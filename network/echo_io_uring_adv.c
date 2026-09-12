// ============================================================================
// io_uring 进阶版 echo server —— 把「批量提交 / 免进内核 / 零拷贝」全用上
//
// 这是 echo_io_uring.c 的「火力全开」版本，用来回答一个问题：
//   **io_uring 的极限在哪？相对 epoll 到底能拉开多少？**
//
// 基础版（echo_io_uring.c）只用了最朴素的用法：每条消息提交一次 SQE、立刻 enter 一次，
// 结果反而略慢于 epoll。这一版会把 io_uring 真正值钱的四个特性全部打开，
// 并且**每个特性都能独立开关**，这样可以量化「每个特性各贡献了多少」。
//
// ── 四个特性 ───────────────────────────────────────────────────────────────
//
// 1) IORING_SETUP_SQPOLL（内核轮询 SQ）
//    建环时带上这个 flag，内核会起一个内核线程持续轮询 SQ 环。
//    应用提交请求时**不需要调用 io_uring_enter**（除非内核线程睡了要唤醒它）。
//    收益：干掉「提交」那次系统调用。代价：内核线程常驻占一个核。
//    需要内核 ≥ 5.1；非特权用户需要 ≥ 5.13（IORING_FEAT_SQPOLL_NONFIXED）。
//
// 2) 提供缓冲区环（provided buffer ring）+ IOSQE_BUFFER_SELECT
//    传统做法：每个请求都要指定「数据读到哪块内存」，所以每连接得常驻一块缓冲区。
//    提供缓冲区：应用先把一批缓冲区「挂」到一个 buffer group 上，
//    提交 recv 时只写 `IOSQE_BUFFER_SELECT` + `buf_group`，让**内核自己挑一块**用，
//    完成后在 CQE 的 flags 里告诉我们用的是哪一块（bid）。
//    收益：应用不用为每条连接预留缓冲区，内存占用从 O(连接数) 降到 O(可复用缓冲数)。
//
// 3) IORING_RECV_MULTISHOT（多发型 recv）
//    【易错】它不是独立 opcode，而是 IORING_OP_RECV 上的一个标志位
//            （sqe->ioprio |= IORING_RECV_MULTISHOT），这点和网上很多说法不一样。
//    普通 recv 收完一条消息就结束了，要收下一条得再提交一次 SQE（每条消息一次提交）。
//    multishot 提交一次就能持续收割，CQE 的 IORING_CQE_F_MORE 表示「还有下次」。
//    收益：把「每条消息一次提交」变成「每条连接一次提交」，这是质变。
//    需要内核 ≥ 5.19。缓冲区耗尽时内核返回 -ENOBUFS，补缓冲区后重新提交即可。
//
// 4) IORING_OP_SEND_ZC（零拷贝发送）
//    【易错】SEND_ZC 会返回**两个** CQE：
//      第一个 res = 实际发出的字节数（数据已进协议栈）；
//      第二个带 IORING_CQE_F_NOTIF，表示「这块内存内核已经不再引用了」。
//    **必须等第二个（NOTIF）到了才能复用/归还缓冲区**，否则会改写正在发送的数据。
//    收益：省掉一次内核→网卡路径上的内存拷贝。需要内核 ≥ 6.0。
//
// ── 环境变量（都可独立开关，便于做归因实验）────────────────────────────────
//   ECHO_SQPOLL=1|0     默认 1
//   ECHO_PBUF=1|0       默认 1   （提供缓冲区环）
//   ECHO_MULTISHOT=1|0  默认 1   （依赖 ECHO_PBUF）
//   ECHO_ZC=1|0         默认 0   （需要内核 ≥6.0，默认关）
//   ECHO_SPIN=1|0       默认 0   （忙轮询 CQ 而不进内核等，烧一个核换延迟）
//   ECHO_BUFS=256       提供缓冲区的数量
//   ECHO_BUFSIZE=4096   单块大小
//
// 用法：./bin/echo_io_uring_adv [port]
// 测试：printf 'hello' | nc 127.0.0.1 [port]
// ============================================================================

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <liburing.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <unistd.h>

// liburing 2.1 没有导出 io_uring_register() 这个符号，所以这里直接用系统调用。
// 参数顺序与内核一致：io_uring_register(ring_fd, opcode, arg, nr_args)
#ifndef __NR_io_uring_register
#define __NR_io_uring_register 427 /* x86_64 / aarch64 相同（asm-generic 表） */
#endif

static int uring_register(int ring_fd, unsigned opcode, const void *arg, unsigned nr_args) {
    return (int)syscall(__NR_io_uring_register, ring_fd, opcode, arg, nr_args);
}

// ---------------------------------------------------------------------------
// 补齐旧 uapi 头文件缺失的内核 ABI 定义
//
// 有些发行版（例如 Ubuntu 22.04 的 linux-libc-dev）自带的 <linux/io_uring.h>
// 版本偏旧，缺少 5.19 / 6.0 才加入的定义。下面这些数值取自内核 UAPI 头文件
// （include/uapi/linux/io_uring.h），是**稳定的 ABI 常量，不会随版本变化**，
// 因此在这里补齐是安全的。
//
// 注意：定义齐了不代表内核支持 —— 内核不支持时会返回 -EINVAL/-ENOSYS，
// 我们在运行时捕获并自动降级（见 capability_check / degrade_*）。
// ---------------------------------------------------------------------------
#ifndef IORING_REGISTER_PBUF_RING
#define IORING_REGISTER_PBUF_RING 22 /* 内核 5.19 */
#endif
#ifndef IORING_RECV_MULTISHOT
#define IORING_RECV_MULTISHOT (1U << 1) /* 内核 5.19；注意它是个 flag，不是 opcode */
#endif
#ifndef IORING_OP_SEND_ZC
#define IORING_OP_SEND_ZC 47 /* 内核 6.0 */
#endif
#ifndef IORING_ACCEPT_MULTISHOT
#define IORING_ACCEPT_MULTISHOT (1U << 0) /* 内核 5.19 */
#endif
#ifndef IORING_CQE_F_NOTIF
#define IORING_CQE_F_NOTIF (1U << 3) /* 内核 6.0，零拷贝的第二个通知 */
#endif

// 提供缓冲区的三个结构体（内核 5.19）。字段顺序必须与内核一致，不能改。
#ifndef __IO_URING_BUF_DEFINED
struct io_uring_buf {
    __u64 addr; /* 数据缓冲区地址 */
    __u32 len;  /* 这块缓冲区能装多少字节 */
    __u16 bid;  /* buffer id：完成后 CQE 会告诉我们用了哪一块 */
    __u16 resv;
};

struct io_uring_buf_ring {
    union {
        // 前 16 字节与第一个 bufs[] 条目重叠，其中最后 2 字节是 ring 的 tail 游标。
        // 这是内核故意设计的省内存技巧，所以填充缓冲区时必须"先写条目、后更新 tail"。
        struct {
            __u64 resv1;
            __u32 resv2;
            __u16 resv3;
            __u16 tail;
        };
        struct io_uring_buf bufs[1];
    };
};

struct io_uring_buf_reg {
    __u64 ring_addr;   /* 缓冲区环的地址（我们自己分配的） */
    __u32 ring_entries;/* 环里有多少个条目，必须是 2 的幂 */
    __u16 bgid;        /* buffer group id */
    __u16 flags;       /* IOU_PBUF_RING_MMAP 等 */
    __u64 resv[3];
};
#define __IO_URING_BUF_DEFINED
#endif

// ---------------------------------------------------------------------------
// 配置与全局状态
// ---------------------------------------------------------------------------

#define MAX_CONNS 1024
#define RING_ENTRIES 512
#define MAX_BUFS 4096

// 操作类型编码进 user_data，完成时靠它区分是哪类请求
enum { OP_ACCEPT = 0, OP_RECV = 1, OP_SEND = 2 };

// user_data 的打包格式（不用额外分配内存，一个 u64 装下全部上下文）：
//   bit  0..31  fd
//   bit 32..39  op
//   bit 40..55  bid（仅 SEND_ZC 需要：等 NOTIF 回来时要知道归还哪块缓冲区）
#define PACK(fd, op, bid) \
    (((unsigned long)(unsigned)(fd)) | ((unsigned long)(op) << 32) | ((unsigned long)(bid) << 40))
#define UNPACK_FD(v) ((int)((v) & 0xFFFFFFFFu))
#define UNPACK_OP(v) ((int)(((v) >> 32) & 0xFFu))
#define UNPACK_BID(v) ((unsigned)(((v) >> 40) & 0xFFFFu))

static struct io_uring g_ring;
static int g_listen_fd = -1;
static int g_port = 19002;

// 特性开关（由环境变量初始化，运行时可能被自动降级改写）
static int g_use_sqpoll = 1;
static int g_use_pbuf = 1;
static int g_use_multishot = 1;
static int g_use_zc = 0;
static int g_use_spin = 0;

static int g_bufcount = 256;
static int g_bufsize = 4096;

// 提供缓冲区环
static struct io_uring_buf_ring *g_buf_ring = NULL;
static void *g_buf_mem = NULL;         // 真正的数据区（bufcount × bufsize 连续内存）
static unsigned g_br_tail = 0;         // 我们已发布到环里的位置
static unsigned char g_buf_state[MAX_BUFS]; // 每块缓冲区的状态，防止重复归还
static int g_bufs_free = 0;            // 当前可被内核取用的缓冲区数

enum { BUF_KERNEL = 0, BUF_APP = 1 };  // 内核持有 / 应用持有（回显中或待归还）

// 连接表（下标即 fd）
static struct conn_state {
    int in_use;
    int recv_active;   // 是否已有一个 recv 在飞（单次模式下避免重复提交）
    char *rxbuf;       // 关闭提供缓冲区时，每条连接自己的固定接收缓冲区
} g_conns[MAX_CONNS];

static long g_stat_recv = 0, g_stat_send = 0, g_stat_err = 0;
static int g_multishot_warned = 0;

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

static void set_nonblock(int fd) {
    int f = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, f | O_NONBLOCK);
}

static long env_flag(const char *name, long dflt) {
    const char *v = getenv(name);
    return v ? atol(v) : dflt;
}

// 把一块缓冲区挂到提供缓冲区环上。
// 顺序很关键：先填 bufs[] 条目，全部填完后再一次性更新 tail，
// 否则内核可能看到一个「tail 已前进但条目还没写好」的半个条目。
static void pbuf_add(void *addr, unsigned len, unsigned bid) {
    unsigned idx = g_br_tail & (unsigned)(g_bufcount - 1);
    g_buf_ring->bufs[idx].addr = (__u64)(unsigned long)addr;
    g_buf_ring->bufs[idx].len = len;
    g_buf_ring->bufs[idx].bid = (__u16)bid;
    g_buf_ring->bufs[idx].resv = 0;
    g_br_tail++;
    if (bid < MAX_BUFS) {
        g_buf_state[bid] = BUF_KERNEL;
        g_bufs_free++;
    }
}

// 把 tail 一次性发布给内核（release 语义：保证上面的写入对内核可见）
static void pbuf_publish(void) {
    io_uring_smp_store_release(&g_buf_ring->tail, g_br_tail);
}

// 回显完成后把缓冲区还给内核，让它继续用于后续 recv
static void pbuf_return(unsigned bid) {
    if (bid >= MAX_BUFS) return;
    if (g_buf_state[bid] != BUF_APP) return;  // 已经在内核手里，别重复归还
    pbuf_add((char *)g_buf_mem + (size_t)bid * g_bufsize, (unsigned)g_bufsize, bid);
    pbuf_publish();
}

// ---------------------------------------------------------------------------
// 提交请求
// ---------------------------------------------------------------------------

// 提交一个 accept。若内核支持 multishot accept，一次提交就能持续接受新连接。
static void submit_accept(void) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&g_ring);
    if (!sqe) return;
    memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = IORING_OP_ACCEPT;
    sqe->fd = g_listen_fd;
    sqe->addr = 0;
    sqe->addr2 = 0;
    // multishot accept：一次提交，之后每个新连接都产生一个 CQE（带 IORING_CQE_F_MORE）。
    // 内核 < 5.19 不支持这个 flag，会直接返回 -EINVAL，我们在完成路径里降级。
    sqe->ioprio |= IORING_ACCEPT_MULTISHOT;
    sqe->user_data = PACK(0, OP_ACCEPT, 0);
}

// 为一条连接提交 recv。
// 开启 ECHO_PBUF 时用「缓冲区选择」，让内核从 buffer group 里自取一块；
// 否则退回固定缓冲区（和基础版一样）。
static void submit_recv(int fd) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&g_ring);
    if (!sqe) return;
    memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = IORING_OP_RECV;
    sqe->fd = fd;
    sqe->user_data = PACK(fd, OP_RECV, 0);

    if (g_use_pbuf) {
        sqe->addr = 0;                       // 地址交给内核选
        sqe->len = 0;
        sqe->flags |= IOSQE_BUFFER_SELECT;   // 「从 buffer group 里挑一块」
        sqe->buf_group = 0;                  // 我们只用 group 0
        if (g_use_multishot) {
            // 关键：multishot 是 RECV 上的一个 ioprio 标志位，不是独立 opcode
            sqe->ioprio |= IORING_RECV_MULTISHOT;
        }
    } else {
        // 没有提供缓冲区时只能自己指定地址：退化成"每条连接一块固定内存"。
        // 注意必须在连接状态里记住这块内存 —— 完成时要靠它找到回显数据。
        if (fd < MAX_CONNS && !g_conns[fd].rxbuf) {
            g_conns[fd].rxbuf = malloc(g_bufsize);
        }
        if (fd >= MAX_CONNS || !g_conns[fd].rxbuf) { g_stat_err++; return; }
        sqe->addr = (unsigned long)g_conns[fd].rxbuf;
        sqe->len = g_bufsize;
    }
    if (fd < MAX_CONNS) g_conns[fd].recv_active = 1;
}

// 提交回显。地址/长度来自那个被内核选中的缓冲区。
static void submit_send(int fd, void *data, unsigned len, unsigned bid) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&g_ring);
    if (!sqe) return;
    memset(sqe, 0, sizeof(*sqe));
    sqe->fd = fd;
    sqe->addr = (unsigned long)data;
    sqe->len = len;
    sqe->msg_flags = MSG_NOSIGNAL;  // 对端已关闭时不要发 SIGPIPE 把进程杀掉
    sqe->user_data = PACK(fd, OP_SEND, bid);

    if (g_use_zc) {
        sqe->opcode = IORING_OP_SEND_ZC;
        // 让内核在第一个完成事件里报告是否真的走了零拷贝（可选，便于观察）
        // sqe->ioprio |= IORING_SEND_ZC_REPORT_USAGE;
    } else {
        sqe->opcode = IORING_OP_SEND;
    }
}

// ---------------------------------------------------------------------------
// 能力自检：把「这个内核到底支持什么」明确打印出来
// ---------------------------------------------------------------------------

static void capability_check(void) {
    struct utsname u;
    uname(&u);

    printf("==================================================\n");
    printf(" io_uring 能力自检\n");
    printf("--------------------------------------------------\n");
    printf(" 内核版本          : %s (%s)\n", u.release, u.machine);

    // 用 IORING_REGISTER_PROBE 问内核支持哪些 opcode（内核 5.6+）
    struct io_uring_probe *probe = io_uring_get_probe_ring(&g_ring);
    int zc_ok = 0;
    if (probe) {
        zc_ok = io_uring_opcode_supported(probe, IORING_OP_SEND_ZC);
    }

    printf(" SQPOLL            : %s\n",
           g_use_sqpoll ? "✅ 已启用（提交不进内核）" : "➖ 未启用（ECHO_SQPOLL=0）");

    if (g_buf_ring) {
        printf(" 提供缓冲区环      : ✅ 已启用（%d 块 × %d 字节，内核自取）\n",
               g_bufcount, g_bufsize);
        printf(" multishot recv    : %s\n",
               g_use_multishot
                   ? "✅ 已提交（若内核 <5.19 会立刻返回 -EINVAL 并自动降级）"
                   : "➖ 未启用（ECHO_MULTISHOT=0）");
    } else {
        printf(" 提供缓冲区环      : ❌ 不可用（需内核 ≥5.19 的 IORING_REGISTER_PBUF_RING）\n");
        printf(" multishot recv    : ❌ 不可用（依赖提供缓冲区环）\n");
    }
    printf(" multishot accept  : %s（需内核 ≥5.19）\n",
           g_use_multishot ? "尝试启用" : "未启用");
    printf(" zerocopy send     : %s\n",
           zc_ok ? (g_use_zc ? "✅ 已启用" : "可用但未启用（ECHO_ZC=0）")
                 : "❌ 内核不支持（需 ≥6.0）");
    printf(" CQ 忙轮询         : %s\n",
           g_use_spin ? "✅ 已启用（烧一核换延迟）" : "➖ 未启用");
    if (probe) io_uring_free_probe(probe);
    printf("==================================================\n");
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// 完成事件处理
// ---------------------------------------------------------------------------

static void close_conn(int fd) {
    if (fd >= 0 && fd < MAX_CONNS) {
        g_conns[fd].in_use = 0;
        g_conns[fd].recv_active = 0;
        if (g_conns[fd].rxbuf) { free(g_conns[fd].rxbuf); g_conns[fd].rxbuf = NULL; }
    }
    close(fd);
}

// 处理一个 CQE
static void handle_cqe(struct io_uring_cqe *cqe) {
    unsigned long ud = cqe->user_data;
    int fd = UNPACK_FD(ud);
    int op = UNPACK_OP(ud);
    int res = cqe->res;
    unsigned flags = cqe->flags;

    // ---- accept ----
    if (op == OP_ACCEPT) {
        if (res >= 0) {
            int cfd = res;
            set_nonblock(cfd);
            if (cfd < MAX_CONNS) { g_conns[cfd].in_use = 1; g_conns[cfd].recv_active = 0; }
            submit_recv(cfd);
            // IORING_CQE_F_MORE 表示 multishot accept 仍然有效，不用重新提交；
            // 没有这个标志（单次 accept 或 multishot 结束）才需要补一次。
            if (!(flags & IORING_CQE_F_MORE)) submit_accept();
        } else if (res == -EINVAL) {
            // 内核不支持 IORING_ACCEPT_MULTISHOT（<5.19）：去掉标志重试一次
            static int warned = 0;
            if (!warned) {
                fprintf(stderr, "[提示] 内核不支持 multishot accept（需 ≥5.19），已自动降级\n");
                warned = 1;
            }
            struct io_uring_sqe *sqe = io_uring_get_sqe(&g_ring);
            if (sqe) {
                memset(sqe, 0, sizeof(*sqe));
                sqe->opcode = IORING_OP_ACCEPT;
                sqe->fd = g_listen_fd;
                sqe->user_data = PACK(0, OP_ACCEPT, 0);
            }
        } else {
            g_stat_err++;
        }
        return;
    }

    // ---- recv ----
    if (op == OP_RECV) {
        if (fd < MAX_CONNS) g_conns[fd].recv_active = 0;

        if (res > 0) {
            // 用缓冲区选择时，CQE 的 flags 高位告诉我们内核用了哪一块缓冲区
            unsigned bid = 0;
            void *data = NULL;
            if ((flags & IORING_CQE_F_BUFFER) && g_buf_ring) {
                bid = flags >> IORING_CQE_BUFFER_SHIFT;
                if (bid < (unsigned)g_bufcount) {
                    data = (char *)g_buf_mem + (size_t)bid * g_bufsize;
                    g_buf_state[bid] = BUF_APP;  // 现在归应用所有
                    g_bufs_free--;
                }
            } else if (fd < MAX_CONNS) {
                // 没走缓冲区选择：数据收在连接自己的固定缓冲区里
                data = g_conns[fd].rxbuf;
                bid = 0;
            }
            g_stat_recv++;
            if (data) {
                // 数据在 bufs[bid] 里，长度是 res —— 直接拿它当发送缓冲区回显，省一次拷贝
                submit_send(fd, data, (unsigned)res, bid);
            } else {
                g_stat_err++;  // 没拿到缓冲区（配置问题），丢弃
            }

            // multishot：有 F_MORE 就说明这次 recv 还在继续，不用重新提交
            if (!(flags & IORING_CQE_F_MORE)) {
                if (g_use_multishot && !g_multishot_warned) {
                    // 走到这里通常是内核不支持 multishot（返回 -EINVAL 后我们降级重试）
                    g_multishot_warned = 1;
                }
                submit_recv(fd);
            }
        } else if (res == -EINVAL || res == -EOPNOTSUPP) {
            // 内核不支持 multishot recv（需 ≥5.19）→ 关掉它重来一次
            if (g_use_multishot && !g_multishot_warned) {
                fprintf(stderr, "[提示] 内核不支持 multishot recv（需 ≥5.19），"
                                "已自动降级为「单次 recv + 提供缓冲区」\n");
                g_multishot_warned = 1;
                g_use_multishot = 0;
            }
            submit_recv(fd);
        } else if (res == -ENOBUFS) {
            // 缓冲区被用完了：补一块回去再重新提交
            g_stat_err++;
            submit_recv(fd);
        } else if (res == 0) {
            close_conn(fd);  // 对端正常关闭
        } else if (res != -EAGAIN && res != -EINTR) {
            g_stat_err++;
            close_conn(fd);
        } else {
            submit_recv(fd);
        }
        return;
    }

    // ---- send ----
    if (op == OP_SEND) {
        unsigned bid = UNPACK_BID(ud);
        if (res < 0) {
            g_stat_err++;
            if (bid < (unsigned)g_bufcount) {
                // 发送失败也要把缓冲区还回去，否则缓冲区会被慢慢耗光
                pbuf_return(bid);
            }
            close_conn(fd);
            return;
        }
        g_stat_send++;
        // 零拷贝模式：第一个 CQE 只代表数据进了协议栈，必须等带 F_NOTIF 的第二个 CQE
        // 才能确认内核不再引用这块内存，此时才可归还缓冲区。
        if (g_use_zc && !(flags & IORING_CQE_F_NOTIF)) {
            return;
        }
        // 只有用了提供缓冲区才需要归还；固定缓冲区属于连接自己，不用还
        if (g_buf_ring && (!g_use_zc || (flags & IORING_CQE_F_NOTIF))) {
            pbuf_return(bid);
        }
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char *argv[]) {
    g_port = argc > 1 ? atoi(argv[1]) : 19002;
    g_use_sqpoll    = (int)env_flag("ECHO_SQPOLL", 1);
    g_use_pbuf      = (int)env_flag("ECHO_PBUF", 1);
    g_use_multishot = (int)env_flag("ECHO_MULTISHOT", 1);
    g_use_zc        = (int)env_flag("ECHO_ZC", 0);
    g_use_spin      = (int)env_flag("ECHO_SPIN", 0);
    g_bufcount      = (int)env_flag("ECHO_BUFS", 256);
    g_bufsize       = (int)env_flag("ECHO_BUFSIZE", 4096);
    if (g_bufcount > MAX_BUFS) g_bufcount = MAX_BUFS;
    if (g_bufcount < 2) g_bufcount = 2;
    // 环大小必须是 2 的幂（内核要求），不满足就向下取整
    {
        int p = 1;
        while (p * 2 <= g_bufcount) p *= 2;
        g_bufcount = p;
    }

    // ---- 监听 socket ----
    g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(g_port);
    if (bind(g_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(g_listen_fd, 512) < 0) { perror("listen"); return 1; }
    set_nonblock(g_listen_fd);

    // ---- 建环 ----
    unsigned flags = 0;
    if (g_use_sqpoll) flags |= IORING_SETUP_SQPOLL;
    int ret = io_uring_queue_init(RING_ENTRIES, &g_ring, flags);
    if (ret < 0 && g_use_sqpoll) {
        // SQPOLL 需要权限（老内核要求 CAP_SYS_NICE）或内核 ≥5.13 的非特权支持。
        // 建环失败不致命：去掉 SQPOLL 重来一次。
        fprintf(stderr, "[提示] SQPOLL 建环失败(%s)，退回普通模式\n", strerror(-ret));
        g_use_sqpoll = 0;
        ret = io_uring_queue_init(RING_ENTRIES, &g_ring, 0);
    }
    if (ret < 0) {
        fprintf(stderr, "io_uring_queue_init failed: %s\n", strerror(-ret));
        fprintf(stderr, "  1) 内核 <5.1   2) 容器 seccomp 拦截 io_uring_setup   3) 未装 liburing\n");
        return 2;  // 2 = 环境限制（与基础版约定一致）
    }

    // ---- 提供缓冲区环 ----
    if (g_use_pbuf) {
        size_t ring_bytes = (size_t)g_bufcount * sizeof(struct io_uring_buf);
        size_t align = 4096;
        // 缓冲区环本身要按页对齐
        if (posix_memalign((void **)&g_buf_ring, align, ring_bytes) != 0 || !g_buf_ring) {
            g_use_pbuf = 0;
        } else {
            memset(g_buf_ring, 0, ring_bytes);
            // 数据区：bufcount 块连成一片，第 i 块就是 bufs[i]
            if (posix_memalign(&g_buf_mem, align, (size_t)g_bufcount * g_bufsize) != 0) {
                g_use_pbuf = 0;
            } else {
                struct io_uring_buf_reg reg;
                memset(&reg, 0, sizeof(reg));
                reg.ring_addr = (unsigned long)g_buf_ring;
                reg.ring_entries = (unsigned)g_bufcount;
                reg.bgid = 0;
                // 把环注册给内核。内核 <5.19 会返回 -EINVAL（register opcode 22 不存在）。
                if (uring_register(g_ring.ring_fd, IORING_REGISTER_PBUF_RING, &reg, 1) < 0) {
                    fprintf(stderr, "[提示] 注册提供缓冲区环失败（errno=%d %s）；"
                                    "需内核 ≥5.19，退回固定缓冲区\n", errno, strerror(errno));
                    g_use_pbuf = 0;
                    g_use_multishot = 0;  // multishot 依赖提供缓冲区
                    free(g_buf_ring); free(g_buf_mem);
                    g_buf_ring = NULL; g_buf_mem = NULL;
                } else {
                    g_bufs_free = 0;
                    for (int i = 0; i < g_bufcount; i++) {
                        g_buf_state[i] = BUF_APP;
                        pbuf_add((char *)g_buf_mem + (size_t)i * g_bufsize,
                                 (unsigned)g_bufsize, (unsigned)i);
                    }
                    pbuf_publish();
                }
            }
        }
    }
    if (!g_use_pbuf) g_use_multishot = 0;

    capability_check();

    // ---- 起手：提交监听与 accept ----
    struct io_uring_sqe *sqe = io_uring_get_sqe(&g_ring);
    memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = IORING_OP_ACCEPT;
    sqe->fd = g_listen_fd;
    sqe->ioprio |= IORING_ACCEPT_MULTISHOT;
    sqe->user_data = PACK(0, OP_ACCEPT, 0);
    io_uring_submit(&g_ring);

    printf("[adv] 监听 %d（SQPOLL=%d PBUF=%d MULTISHOT=%d ZC=%d SPIN=%d）\n",
           g_port, g_use_sqpoll, g_use_pbuf, g_use_multishot, g_use_zc, g_use_spin);
    fflush(stdout);

    // ---- 事件循环 ----
    struct io_uring_cqe *cqe;
    for (;;) {
        if (g_use_spin) {
            // 忙轮询：不进内核等，代价是吃满一个核。
            // peek 拿不到就先 submit（SQPOLL 下这是空操作），继续转。
            if (io_uring_peek_cqe(&g_ring, &cqe) != 0) {
                io_uring_submit(&g_ring);
                continue;
            }
        } else {
            // 常规做法：CQ 空时才进内核等待。这是本实现里"收割"那一次系统调用的来源。
            if (io_uring_wait_cqe(&g_ring, &cqe) < 0) break;
        }

        handle_cqe(cqe);
        io_uring_cqe_seen(&g_ring, cqe);
        // 提交本次处理过程中新准备的 SQE。
        // SQPOLL 下内核线程会自己取走，这一步不需要进内核（liburing 会判断是否需要唤醒）。
        io_uring_submit(&g_ring);
    }

    fprintf(stderr, "退出：recv=%ld send=%ld err=%ld\n", g_stat_recv, g_stat_send, g_stat_err);
    io_uring_queue_exit(&g_ring);
    close(g_listen_fd);
    return 0;
}
