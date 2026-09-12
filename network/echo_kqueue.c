// ============================================================================
//  echo_kqueue.c —— macOS 原生 echo server（kqueue 版）
//
//  为什么需要这个文件？
//    本项目的核心实现（echo_epoll.c / echo_io_uring*.c / io_*aio*）都是 **Linux
//    专属** 的，在 macOS 上连头文件都找不到：
//        <sys/epoll.h>   只有 Linux 有
//        <liburing.h>    io_uring 是 Linux 5.1+ 的内核接口，别的系统没有
//        <libaio.h>      Linux AIO 同理
//    macOS 上与之对等的是 **kqueue**（FreeBSD/macOS 的 IOCP/epoll 同类）。
//    本文件把「单线程 Reactor echo server」用 kqueue 重写一遍，于是同一份压测
//    客户端（network/bench.py 或 bin/bench_client）可以在 macOS 上跑出真实数字，
//    也方便你把两边的**编程模型**放在一起对照学习。
//
//  ── kqueue 与 epoll 的对照表（这张表本身就是最好的学习材料）────────────────
//
//    概念              epoll (Linux)                  kqueue (macOS/BSD)
//    ----------------  -----------------------------  ------------------------------
//    创建句柄          epoll_create1(0)               kqueue()
//    注册/修改/删除    epoll_ctl(ADD/MOD/DEL)         kevent() 提交 changelist
//                                                      （EV_ADD / EV_DELETE / EV_ENABLE…）
//    等待事件          epoll_wait(epfd, evs, n, to)   kevent(kq, NULL, 0, evs, n, to)
//    可读              EPOLLIN                        EVFILT_READ
//    可写              EPOLLOUT                       EVFILT_WRITE
//    水平触发(LT)      默认                           默认（不带 EV_CLEAR）
//    边缘触发(ET)      EPOLLET                        加 EV_CLEAR
//    一次有效          EPOLLONESHOT                   EV_ONESHOT
//    对端关闭/错误     EPOLLHUP | EPOLLERR            EV_EOF（在 fflags 里）
//    用户数据          epoll_event.data.ptr           kevent.udata
//    可读的字节数      —（要自己 read）                EVFILT_READ 的 kevent.data 里
//                                                      直接给出可读字节数（BSD 特性）
//
//    三个最需要注意的差异：
//      1. kqueue 用一个 kevent() 同时干「改注册表」和「等事件」两件事，
//         传参不同而已（changelist != NULL 就是改；eventlist != NULL 就是等）。
//      2. kqueue **没有 LT/ET 开关**，而是用 EV_CLEAR 表示「状态变化后清除」，
//         语义上等价于 ET —— 加了 EV_CLEAR 就必须把 socket 读到 EAGAIN。
//      3. kqueue 的 EVFILT_WRITE 是**水平触发**的：只要 socket 可写，
//         每一轮 kevent 都会报，不处理就是一个 100% CPU 的忙循环。
//         所以本程序的写法是「有数据要发时才注册写事件，发完立刻注销」。
//
//  ── 环境变量 ────────────────────────────────────────────────────────────────
//    ECHO_NODELAY=1   给每条连接设 TCP_NODELAY（关掉 Nagle）
//    ECHO_ET=1        用 EV_CLEAR 走「边缘触发」（须读空 socket）
//    ECHO_KQ=1|0      1=kqueue，0=退化成一个纯阻塞的 accept 循环（对照组）
//
//  编译（macOS，本文件不需要任何第三方库）：
//    cc -O2 -Wall -Wextra -o echo_kqueue echo_kqueue.c
//  运行：
//    ./echo_kqueue [port]          默认 19000
//  测试：
//    printf 'hello' | nc 127.0.0.1 19000
// ============================================================================

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#define MAX_CONNS 4096
#define IN_BUF    8192    // 单次 recv 的缓冲（栈上用）
#define OUT_BUF   65536   // 每条连接待发送数据的缓冲上限

// ---------------------------------------------------------------------------
// 每条连接的状态
//
// 为什么要自己维护一个「待发送缓冲」？
//   因为 socket 可能只接受一部分数据（部分写，部分写 → write 返回 len < 请求长度）。
//   Reactor 里不能阻塞等它变可写，只能把剩下的先存起来，等 EVFILT_WRITE 再继续。
//   这是所有 Reactor 实现里最容易被忽略的一段 —— 只在小包压测时"碰巧"不会出问题。
// ---------------------------------------------------------------------------
typedef struct {
    int    in_use;
    char  *out;        // 待发送缓冲（惰性分配）
    size_t out_off;    // 已发出到哪个位置
    size_t out_len;    // 有效数据总长度
    int    w_reg;      // EVFILT_WRITE 是否已注册（避免重复注册）
    int    r_reg;      // EVFILT_READ  是否已注册（背压时会临时摘掉）
} conn_t;

static conn_t g_conns[MAX_CONNS];

static int kq         = -1;
static int lfd        = -1;
static int g_nodelay  = 0;
static int g_et       = 0;
static int g_use_kq   = 1;

static void on_sigint(int sig) {
    (void)sig;
    // kqueue 版没什么需要清理的状态，直接把监听 fd 关掉让 kevent 返回错误
    if (lfd >= 0) close(lfd);
    _exit(0);
}

static void set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// ---------------------------------------------------------------------------
// kevent 的两个薄封装
//
// kqueue 没有 epoll_ctl 那样的独立函数：注册表变更也是通过 kevent() 提交的，
// 只是把 changelist 传进去、eventlist 传 NULL。
// ---------------------------------------------------------------------------
static int kq_add(int fd, int16_t filter, uint16_t flags, void *udata) {
    struct kevent ev;
    // EV_SET(kev, ident, filter, flags, fflags, data, udata)
    EV_SET(&ev, fd, filter, flags, 0, 0, udata);
    return kevent(kq, &ev, 1, NULL, 0, NULL);
}

static int kq_del(int fd, int16_t filter) {
    struct kevent ev;
    EV_SET(&ev, fd, filter, EV_DELETE, 0, 0, NULL);
    // 删除不存在的注册会返回 ENOENT，这里忽略——调用方不需要关心
    return kevent(kq, &ev, 1, NULL, 0, NULL);
}

// 临时开关一个已注册的过滤器。
//
// 这里用 EV_DISABLE/EV_ENABLE 而不是「删掉再注册」：
//   注册表项保留着，不涉及内核里的哈希表增删，更便宜；
//   而且不会丢失 udata 之类的注册参数。
// （还有一个 EV_DISPATCH：报一次事件后自动禁用，适合「处理完再决定要不要继续收」的
//   场景，等价于 Linux 的 EPOLLONESHOT + 手动重新 arm。）
static int kq_enable(int fd, int16_t filter, int enable) {
    struct kevent ev;
    EV_SET(&ev, fd, filter, enable ? EV_ENABLE : EV_DISABLE, 0, 0, NULL);
    return kevent(kq, &ev, 1, NULL, 0, NULL);
}

// 把这条连接标记成「暂时不要再收数据」——用于输出缓冲写满时的背压。
//
// 关键：水平触发的 EVFILT_READ 只要 socket 里还有数据就会一直报。
// 如果我们因为缓冲满而不处理，它就会每轮都报一次 → 变成 100% CPU 的忙循环。
// 所以必须显式禁用读事件，等缓冲腾空了再启用。
static void conn_pause_read(int fd) {
    if (g_conns[fd].r_reg) {
        kq_enable(fd, EVFILT_READ, 0);
        g_conns[fd].r_reg = 0;
    }
}

static void conn_resume_read(int fd) {
    if (!g_conns[fd].r_reg && g_conns[fd].in_use) {
        kq_enable(fd, EVFILT_READ, 1);
        g_conns[fd].r_reg = 1;
    }
}

static void close_conn(int fd) {
    if (fd >= 0 && fd < MAX_CONNS) {
        // kqueue 在 fd 关闭时会自动摘掉它的注册，不必手动 EV_DELETE
        if (g_conns[fd].out) {
            free(g_conns[fd].out);
            g_conns[fd].out = NULL;
        }
        memset(&g_conns[fd], 0, sizeof(conn_t));
    }
    close(fd);
}

// ---------------------------------------------------------------------------
// 把已缓存的数据尽力发出去；发不完就注册写事件等下次
// ---------------------------------------------------------------------------
static void conn_flush(int fd) {
    conn_t *c = &g_conns[fd];

    while (c->out_off < c->out_len) {
        ssize_t n = send(fd, c->out + c->out_off, c->out_len - c->out_off, 0);
        if (n > 0) {
            c->out_off += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // 内核发送缓冲满了：注册写事件，等可写时继续
            if (!c->w_reg) {
                uint16_t fl = EV_ADD | (g_et ? EV_CLEAR : 0);
                if (kq_add(fd, EVFILT_WRITE, fl, (void *)(intptr_t)fd) == 0) {
                    c->w_reg = 1;
                }
            }
            return;
        }
        if (n < 0 && errno == EINTR) continue;   // 被信号打断，重试
        close_conn(fd);                          // 真错误（EPIPE 等）
        return;
    }

    // 全部发完了：重置缓冲、注销写事件、恢复读事件
    c->out_off = 0;
    c->out_len = 0;
    if (c->w_reg) {
        kq_del(fd, EVFILT_WRITE);
        c->w_reg = 0;
    }
    // 之前因为缓冲满而暂停了读，现在可以恢复了
    if (!c->r_reg) conn_resume_read(fd);
}

// ---------------------------------------------------------------------------
// 处理一个可读事件：读走数据、追加到待发送缓冲、尽力发送
// ---------------------------------------------------------------------------
static void handle_read(int fd) {
    conn_t *c = &g_conns[fd];
    char buf[IN_BUF];

    for (;;) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);

        if (n > 0) {
            // 追加到待发送缓冲。若缓冲满，暂停读事件做背压（见 conn_pause_read）。
            if (!c->out) {
                c->out = malloc(OUT_BUF);
                if (!c->out) { close_conn(fd); return; }
            }
            if (c->out_off > 0 && c->out_off == c->out_len) {
                c->out_off = c->out_len = 0;   // 先压缩一下再用
            }
            size_t room = OUT_BUF - c->out_len;
            if ((size_t)n > room) {
                conn_pause_read(fd);           // 收不下了，先别收了
                n = (ssize_t)room;
            }
            memcpy(c->out + c->out_len, buf, (size_t)n);
            c->out_len += (size_t)n;

            conn_flush(fd);
            if (!g_conns[fd].in_use) return;   // flush 里把它关了

            // 边缘触发：必须读到 EAGAIN 才算处理完，否则事件不会再来了
            if (!g_et && (size_t)n < sizeof(buf)) break;  // 水平触发可提前退出
            if (!g_conns[fd].r_reg) return;    // 已被背压暂停
            continue;
        }

        if (n == 0) { close_conn(fd); return; }        // 对端正常关闭

        if (errno == EAGAIN || errno == EWOULDBLOCK) return;  // 读空了
        if (errno == EINTR) continue;
        close_conn(fd);                                 // 真错误
        return;
    }
}

static void handle_accept(void) {
    for (;;) {
        struct sockaddr_in addr;
        socklen_t len = sizeof(addr);
        int fd = accept(lfd, (struct sockaddr *)&addr, &len);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return;
            perror("accept");
            return;
        }
        if (fd >= MAX_CONNS) { close(fd); continue; }   // 超出连接表容量

        set_nonblock(fd);
        if (g_nodelay) {
            int one = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        }

        memset(&g_conns[fd], 0, sizeof(conn_t));
        g_conns[fd].in_use = 1;

        uint16_t fl = EV_ADD | (g_et ? EV_CLEAR : 0);
        if (kq_add(fd, EVFILT_READ, fl, (void *)(intptr_t)fd) != 0) {
            perror("kevent ADD EVFILT_READ");
            close(fd);
            continue;
        }
        g_conns[fd].r_reg = 1;

        char ip[64] = {0};
        inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
        printf("[kqueue] 新连接 fd=%d %s:%d\n", fd, ip, ntohs(addr.sin_port));
        fflush(stdout);
    }
}

// 对照组：不用 kqueue，纯阻塞 accept + 阻塞读写（一次只能服务一条连接）
static void run_blocking_loop(void) {
    printf("[kqueue] ECHO_KQ=0：走阻塞 accept 循环（一次只能服务一条连接）\n");
    for (;;) {
        int fd = accept(lfd, NULL, NULL);
        if (fd < 0) continue;
        char buf[IN_BUF];
        for (;;) {
            ssize_t n = read(fd, buf, sizeof(buf));
            if (n <= 0) break;
            ssize_t off = 0;
            while (off < n) {
                ssize_t w = write(fd, buf + off, (size_t)(n - off));
                if (w <= 0) break;
                off += w;
            }
        }
        close(fd);
    }
}

int main(int argc, char *argv[]) {
    int port = argc > 1 ? atoi(argv[1]) : 19000;

    // 环境变量解析（用 getenv 而不是 getenv_safe，保持和 Linux 版一致）
    const char *e;
    if ((e = getenv("ECHO_NODELAY"))) g_nodelay = atoi(e);
    if ((e = getenv("ECHO_ET")))      g_et      = atoi(e);
    if ((e = getenv("ECHO_KQ")))      g_use_kq  = atoi(e);

    signal(SIGINT, on_sigint);
    signal(SIGPIPE, SIG_IGN);   // 对端已关闭时 write 会收到 SIGPIPE，默认会杀进程

    lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) { perror("socket"); return 1; }

    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons((uint16_t)port);

    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) != 0) { perror("bind"); return 1; }
    if (listen(lfd, 512) != 0) { perror("listen"); return 1; }
    set_nonblock(lfd);

    printf("[kqueue] 监听 %d（NODELAY=%d ET=%d kqueue=%d）\n",
           port, g_nodelay, g_et, g_use_kq);
    fflush(stdout);

    if (!g_use_kq) run_blocking_loop();

    kq = kqueue();
    if (kq < 0) { perror("kqueue"); return 1; }

    // 监听 fd 只关心可读；用水平触发（不带 EV_CLEAR），
    // 这样 accept 没有全部取走时下一轮还会报，不会漏连接。
    if (kq_add(lfd, EVFILT_READ, EV_ADD, (void *)(intptr_t)lfd) != 0) {
        perror("kevent ADD listen"); return 1;
    }

    struct kevent evs[1024];
    for (;;) {
        // 这里 eventlist 非空 → 语义是「等事件」；changelist 为空。
        // 超时传 NULL 表示永久等待（和 epoll_wait(-1) 同义）。
        int n = kevent(kq, NULL, 0, evs, (int)(sizeof(evs) / sizeof(evs[0])), NULL);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("kevent");
            return 1;
        }

        for (int i = 0; i < n; i++) {
            int fd = (int)(intptr_t)evs[i].udata;

            if (evs[i].flags & EV_ERROR) {      // 注册失败之类
                close_conn(fd);
                continue;
            }
            if (fd == lfd) { handle_accept(); continue; }

            if (!g_conns[fd].in_use) continue;  // 已被关掉的残留事件

            // EV_EOF：对端关闭（也可能是半关闭）。注意 kqueue 会把最后的数据
            // 和 EOF 一起报，所以这里**先读**，读到 0 才关连接。
            if (evs[i].filter == EVFILT_READ) {
                handle_read(fd);
            } else if (evs[i].filter == EVFILT_WRITE) {
                conn_flush(fd);
            }
        }
    }
    return 0;
}
