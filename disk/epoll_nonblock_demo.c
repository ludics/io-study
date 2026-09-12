// ============================================================================
// 实验：验证 epoll 为什么必须搭配 O_NONBLOCK
//
// 两种模式对比：
//   A. ET(边缘触发) + 阻塞 fd  → 会卡死（read 阻塞在最后一次）
//   B. ET(边缘触发) + 非阻塞 fd → 正常（read 返回 EAGAIN 后退出循环）
//
// ── 为什么会卡死，先把道理讲清楚 ──────────────────────────────────────────
// ET（EPOLLET）的语义是「只在状态**变化**时通知一次」：数据从"没有"变成"有"，
// 内核通知你一次，之后不管你读没读完，都不会再通知。
// 所以 ET 下必须**循环 read 直到 EAGAIN**，否则剩下的数据就永远收不到通知了。
//
// 问题就出在这个「循环直到 EAGAIN」上：
//   * fd 是**非阻塞**的 → 数据读完后 read 返回 -1/EAGAIN，循环正常退出；
//   * fd 是**阻塞**的   → 数据读完后 read 不返回，**进程直接睡在这里**。
//     此时该连接既不会被再次通知（ET 不会再发），进程也动不了 —— 死锁。
//
// 演示刻意做的两个设计，都是为了把这个死锁暴露出来：
//   1) BUF_SIZE 故意很小（5 字节），而客户端发 17 字节 → 必然要 read 4 次，
//      第 4 次之后缓冲区就空了，于是走进上面那个"最后一次 read"；
//   2) 客户端**发完不关闭连接**（只 sleep）→ 阻塞的 read 不会因为 EOF 而返回 0，
//      只能一直挂着。（如果客户端立刻 close，read 会返回 0，就看不到卡死了。）
//
// 用法: ./epoll_nonblock_demo <0=阻塞|1=非阻塞>
// 程序内置 4 秒看门狗（SIGALRM）：阻塞模式下会在 4 秒后打印【卡死】并退出。
// 也可以从外面兜一层：timeout 3 ./epoll_nonblock_demo 0
// ============================================================================
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>

#define PORT 19800
#define BUF_SIZE 5   // 故意用小 buffer，强制触发"循环读"（见文件头解释）

// 单调时钟（CLOCK_MONOTONIC）：不受系统时间被调整的影响，测耗时只能用这个
static double now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

// ---------------------------------------------------------------------------
// 卡死检测：SIGALRM 看门狗
//
// 【为什么需要一个看门狗】阻塞模式下进程睡在 read() 里，
// **根本执行不到任何"检查是否超时"的代码** —— 主循环里那句
// `(now_ms() - t_start) < 4000` 永远没机会再被判断。所以只能靠外部打断：
//   * 用信号打断：alarm(4) 到点触发本函数（本文件采用）；
//   * 或者用系统命令：`timeout 3 ./epoll_nonblock_demo 0`（文件头的用法示例）。
//
// 信号处理函数里只能调用**异步信号安全**的函数（printf 不是），
// 所以这里用 write() 直接写 fd 1；_exit() 也是安全的（不能用 exit()，
// 它会跑 atexit/flush，在信号上下文里可能死锁）。
// ---------------------------------------------------------------------------
static pid_t g_child = -1;   // 客户端子进程，卡死退出时顺手收掉，不留孤儿

static void on_alarm(int sig) {
    (void)sig;
    const char *msg =
        "\n>>> 结果: 4 秒内未完成 —— 【卡死】在 read 上（阻塞 fd + ET）\n"
        "    （进程睡在 read 里，只有信号能把它叫醒）\n";
    ssize_t w = write(STDOUT_FILENO, msg, strlen(msg));
    (void)w;
    if (g_child > 0) kill(g_child, SIGKILL);
    _exit(0);
}

// 模拟客户端：发数据后保持连接不关闭
//
// 跑在 fork 出来的子进程里，而不是线程里 —— 因为服务端在阻塞 fd 模式下会
// 挂在 read 上，如果客户端和它在同一个线程里就没法"并发地"制造这个场景了。
static void client_thread() {
    usleep(300000);  // 等 server 起来（bind/listen 需要一点时间）
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect"); close(s); return;
    }
    // 发送 17 字节（BUF_SIZE=5，需要 4 次 read 才能读完：5+5+5+2）
    const char *msg = "HELLO-WORLD-12345";
    if (write(s, msg, strlen(msg)) < 0) {
        perror("write");
    }
    printf("[client] 已发送 %zu 字节，保持连接不关闭\n", strlen(msg));
    fflush(stdout);
    sleep(20);  // 保持连接 20 秒，但不关闭
                // 关键：必须"久到超过看门狗的 4 秒"，否则阻塞的 read 会因为 EOF 提前脱身，
                // 看起来像"没卡死"。（初版这里是 5 秒，刚好在 4 秒看门狗之外，
                //   结果阻塞模式要等 5.4 秒才返回，报的还是"未卡死"—— 见文件末尾说明。）
    close(s);
}

int main(int argc, char *argv[]) {
    int use_nonblock = argc > 1 ? atoi(argv[1]) : 1;

    printf("========== epoll %s fd 实验 ==========\n",
           use_nonblock ? "非阻塞(O_NONBLOCK)" : "阻塞(默认)");
    printf("模式: ET(边缘触发) | buffer=%d 字节\n", BUF_SIZE);
    printf("客户端将发送 17 字节，需要 4 次 read 读完\n\n");
    fflush(stdout);

    // 启动客户端子进程（父进程继续当服务端）
    pid_t pid = fork();
    if (pid == 0) { client_thread(); exit(0); }
    g_child = pid;

    // 装上 4 秒看门狗：阻塞模式下进程会睡在 read 里，靠它才能有输出
    signal(SIGALRM, on_alarm);
    alarm(4);

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    // SO_REUSEADDR：让程序反复运行时不至于被 TIME_WAIT 状态的老连接挡住 bind
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(PORT);
    if (bind(lfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    listen(lfd, 5);
    // 监听 fd 设为非阻塞（避免 accept 阻塞，这不是本次实验重点）
    //
    // 【顺带说一句】生产代码里监听 fd 也应该非阻塞。原因和本实验同一类：
    // accept 在 EAGAIN 时会阻塞，而 epoll 的通知只是"可能有连接"，不保证一定 accept 得到
    // （多进程同时 accept 同一 fd 时会被别人抢走）。
    int fl = fcntl(lfd, F_GETFL, 0);
    fcntl(lfd, F_SETFL, fl | O_NONBLOCK);

    int epfd = epoll_create1(0);
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = lfd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &ev);

    struct epoll_event events[16];
    int handled = 0;
    double t_start = now_ms();

    // 只处理 1 个连接就退出。
    // 注意这里**没有**"超时就报卡死"的判断：主循环自己看不见卡死（进程睡在 read 里），
    // 超时输出由上面的 SIGALRM 看门狗负责。
    while (handled < 1) {
        // 500ms 超时轮询一次：给主循环一个"活着的"机会（也方便 Ctrl-C）
        int n = epoll_wait(epfd, events, 16, 500);
        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            if (fd == lfd) {
                int cfd = accept(lfd, NULL, NULL);
                if (cfd < 0) continue;  // 非阻塞 accept 可能返回 EAGAIN，直接跳过

                // 这里就是本实验的自变量：新连接要不要设成非阻塞
                if (use_nonblock) {
                    int f = fcntl(cfd, F_GETFL, 0);
                    fcntl(cfd, F_SETFL, f | O_NONBLOCK);
                    printf("[server] accept cfd=%d，已设为 O_NONBLOCK\n", cfd);
                } else {
                    printf("[server] accept cfd=%d，保持阻塞模式\n", cfd);
                }
                fflush(stdout);

                ev.events = EPOLLIN | EPOLLET;  // ET 边缘触发（这是卡死的另一个前提）
                ev.data.fd = cfd;
                epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &ev);
            } else {
                // ET 模式：必须循环 read 直到 EAGAIN —— 阻塞 fd 就会卡在这个循环里
                printf("[server] 开始循环 read (ET 模式)\n");
                fflush(stdout);
                char buf[BUF_SIZE + 1];   // +1 方便调试时看字符串（read 不会用它写越界）
                int total = 0;
                int round = 0;
                while (1) {
                    memset(buf, 0, sizeof(buf));
                    ssize_t r = read(fd, buf, BUF_SIZE);
                    round++;
                    if (r > 0) {
                        total += r;
                        printf("[server]   第%d次 read: %zd 字节 (累计%d)\n",
                               round, r, total);
                        fflush(stdout);
                        continue;      // 还有数据，继续读
                    } else if (r == 0) {
                        // read 返回 0 = 对端正常关闭（EOF）。本实验客户端不 close，
                        // 所以走不到这里 —— 阻塞模式下会直接卡在上面的 read 里。
                        printf("[server]   对端关闭\n");
                        break;
                    } else {
                        // r < 0：只有非阻塞 fd 才会走到这里（EAGAIN = 数据读完了）
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            printf("[server]   第%d次 read: EAGAIN —— 数据读完，正常退出\n",
                                   round);
                        } else {
                            printf("[server]   read error: %s\n", strerror(errno));
                        }
                        break;
                    }
                }
                printf("[server] 循环结束，共读 %d 字节\n\n", total);
                fflush(stdout);
                handled++;   // 标记"跑通了"，非阻塞模式下才会执行到这一行
                close(fd);
            }
        }
    }

    alarm(0);   // 跑通了就撤掉看门狗 —— 否则 4 秒后还会打印"卡死"，误导人
    double elapsed = now_ms() - t_start;
    printf(">>> 结果: 正常完成，耗时 %.1f ms —— 未卡死\n", elapsed);
    printf("    对照：改成 ./epoll_nonblock_demo 0（阻塞 fd）会在 4 秒后被看门狗打断，"
           "报【卡死】\n");

    // 客户端子进程还在 sleep，必须收尸，否则会留下僵尸进程
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    close(epfd);
    close(lfd);
    return 0;
}
