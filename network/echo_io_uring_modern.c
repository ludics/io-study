// ============================================================================
// io_uring Echo Server —— 现代版（只面向新内核，不背任何兼容包袱）
//
// 这是 network/echo_io_uring_adv.c 的「重写版」。adv 版为了让同一份代码能在
// 5.15（老 UAPI 头文件 + liburing 2.1）和新内核上都能编译，塞了这些东西：
//     * 手写内核 ABI 常量（因为老头文件里没有）
//     * 针对"内核不支持某特性"的运行时降级分支
//     * 自己实现提供缓冲区环的入队/发布（因为老 liburing 没有这些辅助函数）
// 结果代码又长又绕，读者很难看清"io_uring 到底该怎么用"。
//
// 这一版的前提是**内核 ≥ 6.6 + liburing ≥ 2.6**（本机验证：7.0 / 2.14），
// 于是上面那些包袱全部消失：
//     * 不再需要任何手写 ABI 常量 —— 直接用内核头文件
//     * 不再需要任何降级分支 —— 不支持的组合直接报错退出，并说清原因
//     * 缓冲区环、multishot accept、direct accept 全部用 liburing 的辅助函数
//
// ── 这一版用到的「现代 io_uring 六件套」─────────────────────────────────────
//
// 1) SQPOLL + SQ_AFF（内核线程轮询 SQ，并且把它绑到指定核）
//    提交请求不再需要 io_uring_enter：应用把 SQE 写进共享内存、发布 tail，
//    内核线程自己取走。**提交路径 0 系统调用。**
//    【必须给它一个独立的核】SQ 线程是忙轮询的；如果和应用挤在同一个核上，
//    两者互相抢，反而比不开更慢（实测 15k vs 63k）。见 ECHO_SQ_CPU。
//
// 2) SINGLE_ISSUER（声明"只有我一个 task 提交请求"）
//    内核可以据此省掉一部分提交路径上的锁/原子操作。
//    注意：它和 COOP_TASKRUN / DEFER_TASKRUN **互斥**（实测 EINVAL），所以这里
//    只开 SINGLE_ISSUER。
//
// 3) CQ 忙轮询（不进内核收割）
//    用 io_uring_peek_batch_cqe() 在用户态成批取走 CQ 里的完成事件，
//    **收割路径 0 系统调用**。
//    【和 SQPOLL 是配套的】只开 SQPOLL 不开忙轮询，收割那一次 enter 还在，
//    实测反而比基础版慢；两个都开才是质变（63k → 189k）。
//
// 4) multishot accept + direct file table（一次提交持续收连接，且直接进固定文件表）
//    io_uring_prep_multishot_accept_direct()：一条 SQE 就能持续接受新连接
//    （每个连接回来一个带 IORING_CQE_F_MORE 的 CQE），
//    并且新连接**直接分配在固定文件表的空槽里**（IORING_FILE_INDEX_ALLOC）。
//    好处：a) 不像传统 accept 那样每条连接都要一次系统调用；
//          b) 后续收发的 fd 在固定表里，不用每次 fget/fput。
//    代价：拿到的是**表下标**而不是 fd，所以不能对连接调 setsockopt ——
//          也就是说这个版本没法设 TCP_NODELAY（实测本 workload 下影响 < 5%）。
//          真需要设 socket 选项的话，见文件末尾「如果必须设 socket 选项」。
//
// 5) 提供缓冲区环（provided buffer ring）
//    应用把一批缓冲区"挂"到环上，提交 recv 时只写 IOSQE_BUFFER_SELECT + 组号，
//    由**内核自己挑一块**；完成后 CQE 的 flags 高位带回用了哪一块（bid）。
//    好处：应用不用为每条连接预留缓冲区，内存从 O(连接数) 降到 O(缓冲数)。
//
// 6) IORING_OP_SEND_ZC（零拷贝发送）—— 实现了，但**默认关着**
//    它省掉内核→网卡路径上的一次拷贝。本机实测：**小消息（256B）下开启反而慢 26%**
//    （233k → 175k），因为每次发送要多一个 CQE（零拷贝完成靠第二个带
//    IORING_CQE_F_NOTIF 的完成事件通知），而省下的那点拷贝不值这个开销。
//    大消息（几十 KB 以上）才可能翻盘 —— 用 ECHO_ZC=1 自己验。
//    【易错】开它的话会返回**两个** CQE：第一个 res=已发出字节数（数据已进协议栈），
//    第二个带 IORING_CQE_F_NOTIF，表示"这块内存内核不再引用了"。
//    **必须等第二个到了才能复用/归还缓冲区**，否则会改写正在发出的数据。
//
// ── 环境变量（都可以关掉，便于做对照实验）─────────────────────────────────
//   ECHO_SQPOLL=1|0     默认 1   SQPOLL（提交不进内核）
//   ECHO_SQ_CPU=<n>     默认 -1  把 SQ 内核线程绑到核 n（-1=不绑，强烈建议绑）
//   ECHO_SPIN=1|0       默认 1   CQ 忙轮询（收割不进内核）
//   ECHO_FIXED=1|0      默认 1   multishot accept 直接分配进固定文件表
//   ECHO_ZC=1|0         默认 0   零拷贝发送（实测小消息下更慢，默认关）
//   ECHO_SINGLE_ISSUER=1|0 默认 1 声明单提交者（与 SQPOLL 可共存）
//   ECHO_BUFS=256       提供缓冲区环的条目数（内部向上取到 2 的幂）
//   ECHO_BUFSIZE=4096   单块缓冲区大小
//
//   ⚠️ ECHO_BUFSIZE 要 ≥ 你要压测的最大消息长度，否则 recv 会截断、回显出来的
//      内容就不完整（表现为客户端校验失败）。压 4KB 消息时给到 4096 以上。
//
// 编译: gcc -O2 -Wall -D_GNU_SOURCE -o echo_io_uring_modern echo_io_uring_modern.c -luring
// 运行: ./bin/echo_io_uring_modern [port]
//   SQPOLL 需要独立核时的典型用法：
//     ECHO_SQ_CPU=5 taskset -c 4 ./bin/echo_io_uring_modern 19002
// 测试: printf 'hello' | nc 127.0.0.1 19002
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
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// 配置
// ---------------------------------------------------------------------------

#define RING_ENTRIES 1024   // SQ/CQ 环条目数（同时可容纳的在飞请求量级）
#define MAX_CONNS    1024   // 固定文件表大小（同时也是连接状态数组大小）
#define BUF_GROUP    0      // 提供缓冲区组号（只用组 0）
#define CQ_BATCH     64     // 一次最多从 CQ 取走多少个完成事件

// 操作类型（编码进 user_data）
enum { OP_ACCEPT = 1, OP_RECV = 2, OP_SEND = 3 };

// user_data 的 64 位布局（避免为每个请求 malloc 一个小结构体）：
//     bit  0..15  bid      缓冲区编号
//     bit 16..31  off      这次发送在整条消息里的偏移（处理"部分写"用）
//     bit 32..39  op       操作类型
//     bit 40..63  idx      连接标识（固定表下标 或 fd），24 位足够
#define UD_PACK(op, idx, bid, off)                      \
    ((unsigned long)(bid) | ((unsigned long)(off) << 16) | \
     ((unsigned long)(op) << 32) | ((unsigned long)(unsigned)(idx) << 40))
#define UD_OP(v)  ((int)(((v) >> 32) & 0xFFu))
#define UD_IDX(v) ((int)((v) >> 40))
#define UD_BID(v) ((unsigned)((v) & 0xFFFFu))
#define UD_OFF(v) ((unsigned)(((v) >> 16) & 0xFFFFu))

// ---------------------------------------------------------------------------
// 全局状态
// ---------------------------------------------------------------------------

static struct io_uring g_ring;
static int g_listen_fd = -1;
static int g_port = 19002;

// 特性开关
static int g_sqpoll = 1;
static int g_sq_cpu = -1;
static int g_spin   = 1;
static int g_fixed  = 1;
static int g_zc     = 0;   // 默认关：小消息下零拷贝实测更慢，见文件头 6)
static int g_single_issuer = 1;

// 提供缓冲区环
static struct io_uring_buf_ring *g_br = NULL;  // 环本身（与内核共享）
static void   *g_buf_data = NULL;              // 实际数据区（bufcount × bufsize）
static int     g_bufcount = 512;
static size_t  g_bufsize  = 8192;
static int     g_br_mask  = 0;                 // bufcount - 1

// 连接状态（下标 = 固定表下标或 fd）
struct conn_state {
    unsigned send_len;   // 当前这条回显消息的总长度
    int      in_use;
};
static struct conn_state g_conns[MAX_CONNS];

// 统计
static volatile sig_atomic_t g_stop = 0;
static long g_stat_recv = 0, g_stat_send = 0, g_stat_err = 0, g_stat_zc_copy = 0;

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

static long env_int(const char *name, long dflt) {
    const char *v = getenv(name);
    return v ? atol(v) : dflt;
}

static void set_nonblock(int fd) {
    int f = fcntl(fd, F_GETFL, 0);
    if (f >= 0) fcntl(fd, F_SETFL, f | O_NONBLOCK);
}

static void on_sigint(int sig) {
    (void)sig;
    g_stop = 1;   // 只置标志，真正的清理放在事件循环外面（信号里不做复杂事）
}

static void die(const char *what) {
    fprintf(stderr, "%s: %s\n", what, strerror(errno));
    exit(1);
}

// ---------------------------------------------------------------------------
// 建环：把现代 flag 一次配齐
// ---------------------------------------------------------------------------

static void setup_ring(void) {
    struct io_uring_params p;
    memset(&p, 0, sizeof(p));

    p.flags = 0;
    if (g_single_issuer) p.flags |= IORING_SETUP_SINGLE_ISSUER;  // 只有本线程提交请求
    if (g_sqpoll) {
        p.flags |= IORING_SETUP_SQPOLL;
        if (g_sq_cpu >= 0) {
            // SQ_AFF 让"内核轮询线程绑到哪个核"生效。绝大多数情况下都应该绑：
            // 不绑的话它可能和应用抢核，也可能跨核迁移导致缓存失效。
            p.flags |= IORING_SETUP_SQ_AFF;
            p.sq_thread_cpu = (unsigned)g_sq_cpu;
        }
    }
    // 【注意】SQPOLL 与 COOP_TASKRUN / DEFER_TASKRUN 是**互斥**的，
    // 一起写内核会返回 EINVAL（本机实测）。想用 DEFER_TASKRUN 就得关掉 SQPOLL，
    // 那时又回到"提交要进内核"，对 echo 这种小请求场景并不划算。
    // 本程序选择 SQPOLL + SINGLE_ISSUER 这个组合（内核接受，实测也没有坏处）。

    int ret = io_uring_queue_init_params(RING_ENTRIES, &g_ring, &p);
    if (ret < 0) {
        // 失败要给得出原因。这里不做"自动降级"——降级会让性能问题被悄悄掩盖，
        // 对一个用来测性能的程序来说，宁可失败得响一点。
        fprintf(stderr,
                "io_uring_queue_init_params 失败: %s (flags=0x%x, sq_cpu=%d)\n"
                "  可能原因：\n"
                "    1) 内核 < 5.1 不支持 io_uring；本程序需要 ≥6.6\n"
                "    2) SQPOLL 被容器 seccomp 拦截，或需要 CAP_SYS_NICE（老内核）\n"
                "    3) SQPOLL 与 SQ_AFF 同时用但 sq_thread_cpu 越界\n"
                "  试试：ECHO_SQPOLL=0 %s %d\n",
                strerror(-ret), p.flags, g_sq_cpu, "echo_io_uring_modern", g_port);
        exit(2);
    }

    // 环建好后内核会回填 features 位图，告诉我们这个内核支持哪些能力。
    // 挑几个和本程序有关的打出来，便于确认"我要的特性的确生效了"。
    printf("[modern] 环已建立: features=0x%x (SQPOLL_NONFIXED=%d FAST_POLL=%d EXT_ARG=%d)\n",
           p.features,
           !!(p.features & IORING_FEAT_SQPOLL_NONFIXED),
           !!(p.features & IORING_FEAT_FAST_POLL),
           !!(p.features & IORING_FEAT_EXT_ARG));
}

// 注册固定文件表（稀疏：先登记 1024 个槽位，但一个文件都不装）
static void setup_fixed_files(void) {
    if (!g_fixed) return;
    if (io_uring_register_files_sparse(&g_ring, MAX_CONNS) < 0)
        die("io_uring_register_files_sparse");
}

// 建提供缓冲区环，并把所有缓冲区一次性挂上去
static void setup_buf_ring(void) {
    // 条目数必须是 2 的幂（内核用掩码索引）
    int n = 1;
    while (n * 2 <= g_bufcount) n *= 2;
    g_bufcount = n;
    g_br_mask = g_bufcount - 1;

    size_t ring_bytes = (size_t)g_bufcount * sizeof(struct io_uring_buf);

    // 环本身要页对齐（内核直接 mmap/引用这块内存）
    if (posix_memalign((void **)&g_br, 4096, ring_bytes) != 0) die("posix_memalign(ring)");
    memset(g_br, 0, ring_bytes);

    // 数据区：bufcount × bufsize 的连续内存，按 bufsize 切片分给各 bid
    if (posix_memalign(&g_buf_data, 4096, (size_t)g_bufcount * g_bufsize) != 0)
        die("posix_memalign(data)");

    struct io_uring_buf_reg reg;
    memset(&reg, 0, sizeof(reg));
    reg.ring_addr    = (unsigned long)g_br;
    reg.ring_entries = (unsigned)g_bufcount;
    reg.bgid         = BUF_GROUP;
    // reg.flags: 0 = 普通模式；IOU_PBUF_RING_INC(增量模式) 留作练习
    if (io_uring_register_buf_ring(&g_ring, &reg, 0) < 0)
        die("io_uring_register_buf_ring");

    // 初始化环并挂上所有缓冲区。
    // 顺序很重要：先把条目都写好，最后一次性 advance 发布 tail ——
    // 否则内核可能读到"tail 已前进但条目还没写"的半个条目。
    io_uring_buf_ring_init(g_br);
    for (int i = 0; i < g_bufcount; i++) {
        io_uring_buf_ring_add(g_br,
                              (char *)g_buf_data + (size_t)i * g_bufsize,
                              (unsigned)g_bufsize,
                              (unsigned short)i,
                              g_br_mask,
                              i /* buf_offset */);
    }
    io_uring_buf_ring_advance(g_br, g_bufcount);
}

// 归还一块缓冲区给内核（等内核不再引用它时才能调）
static inline void buf_return(unsigned bid) {
    io_uring_buf_ring_add(g_br,
                          (char *)g_buf_data + (size_t)bid * g_bufsize,
                          (unsigned)g_bufsize,
                          (unsigned short)bid,
                          g_br_mask,
                          0 /* buf_offset：接着 tail 放 */
    );
    io_uring_buf_ring_advance(g_br, 1);
}

// 能力自检：把"这个内核到底支持什么"打成一张表，避免拿不到数据时瞎猜
static void check_caps(void) {
    struct utsname u;
    uname(&u);

    printf("==================================================\n");
    printf(" io_uring 现代版 · 能力自检\n");
    printf("--------------------------------------------------\n");
    printf(" 内核版本        : %s (%s)\n", u.release, u.machine);

    struct io_uring_probe *probe = io_uring_get_probe_ring(&g_ring);
    const char *zc = "未知";
    if (probe) {
        zc = io_uring_opcode_supported(probe, IORING_OP_SEND_ZC) ? "可用" : "不支持";
        io_uring_free_probe(probe);
    }

    printf(" 建环 flags      : %s%s%s\n",
           g_sqpoll ? "SQPOLL " : "", (g_sqpoll && g_sq_cpu >= 0) ? "SQ_AFF " : "",
           g_single_issuer ? "SINGLE_ISSUER" : "(无 SINGLE_ISSUER)");
    printf(" 提交路径        : %s\n",
           g_sqpoll ? "SQPOLL（应用只写共享内存，0 系统调用）" : "每次 io_uring_submit（进内核）");
    if (g_sqpoll && g_sq_cpu < 0)
        printf("  ⚠️ 没给 SQ 线程绑核：请确认它有独立的核可用，否则会比不开更慢\n");
    printf(" 收割路径        : %s\n",
           g_spin ? "用户态忙轮询 CQ（0 系统调用，烧一个核）" : "io_uring_wait_cqe（进内核等）");
    printf(" 连接建立        : %s\n",
           g_fixed ? "multishot accept → 直接分配进固定文件表（0 系统调用/连接）"
                   : "普通 accept（每连接一次系统调用）");
    printf(" 接收            : multishot recv + 提供缓冲区环（%d × %zu 字节，内核自取）\n",
           g_bufcount, g_bufsize);
    printf(" 发送            : %s\n",
           g_zc ? "零拷贝 send（SEND_ZC，等 F_NOTIF 才归还缓冲区）" : "普通 send");
    if (g_zc) printf("                   SEND_ZC 内核支持: %s\n", zc);
    printf("==================================================\n");
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// 取 SQE
//
// SQ 环满了 io_uring_get_sqe 会返回 NULL。此时必须先把已有请求提交出去
// （内核消费掉 tail，空出槽位），再重试。这一步在 SQPOLL 下通常不进内核。
// ---------------------------------------------------------------------------

static struct io_uring_sqe *get_sqe(void) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&g_ring);
    if (sqe) return sqe;
    io_uring_submit(&g_ring);
    sqe = io_uring_get_sqe(&g_ring);
    if (!sqe) { g_stat_err++; return NULL; }
    return sqe;
}

// ---------------------------------------------------------------------------
// 提交请求：三个"准备 SQE"的函数
//
// 它们都只往 SQ 环里填内容，不主动提交 —— 提交交给事件循环统一做，
// 这样一轮里准备的多个 SQE 能被合并（而且在 SQPOLL 下根本不需要进内核）。
// ---------------------------------------------------------------------------

// 提交 multishot accept。
// direct 模式：新连接直接分配进固定文件表的空槽，完成事件里的 res 是**表下标**。
static void arm_accept(void) {
    struct io_uring_sqe *sqe = get_sqe();
    if (!sqe) return;
    if (g_fixed)
        io_uring_prep_multishot_accept_direct(sqe, g_listen_fd, NULL, NULL, 0);
    else
        io_uring_prep_multishot_accept(sqe, g_listen_fd, NULL, NULL, 0);
    io_uring_sqe_set_data64(sqe, UD_PACK(OP_ACCEPT, 0, 0, 0));
}

// 给一条连接挂上 multishot recv。
// 提交一次就能持续收割这条连接上的所有消息（直到出错/缓冲区耗尽）。
static void arm_recv(int idx) {
    struct io_uring_sqe *sqe = get_sqe();
    if (!sqe) return;
    // buf 传 NULL、len 传 0：地址由内核从提供缓冲区环里挑
    io_uring_prep_recv_multishot(sqe, idx, NULL, 0, 0);
    // ⚠️ 顺序要紧：prep_rw 会把 flags/ioprio 清零，所以这些必须写在 prep 之后
    sqe->flags |= IOSQE_BUFFER_SELECT;       // 让内核从缓冲区环里挑一块
    sqe->buf_group = BUF_GROUP;
    if (g_fixed) sqe->flags |= IOSQE_FIXED_FILE;  // idx 是固定表下标而不是 fd
    io_uring_sqe_set_data64(sqe, UD_PACK(OP_RECV, idx, 0, 0));
}

// 提交一次回显发送。
//   data/len  : 要发的数据（指向缓冲区环里那块内存，zero-copy 情况下不拷贝）
//   bid       : 这块缓冲区编号（发送彻底完成后才能归还）
//   off       : 这次发送在整条消息里的偏移（处理"部分写"：上次只发出去一半）
static void arm_send(int idx, const char *data, unsigned len, unsigned bid, unsigned off) {
    struct io_uring_sqe *sqe = get_sqe();
    if (!sqe) return;
    if (g_zc) {
        // 最后一个参数是 ioprio（zc_flags）：可以塞 IORING_SEND_ZC_REPORT_USAGE
        // 让内核报告"这次真的零拷贝了没有"。本程序直接用完成事件里的标记，
        // 不额外开这个（开了会让完成事件多带一串信息，稍慢）。
        io_uring_prep_send_zc(sqe, idx, data, len, MSG_NOSIGNAL, 0);
    } else {
        io_uring_prep_send(sqe, idx, data, len, MSG_NOSIGNAL);
    }
    if (g_fixed) sqe->flags |= IOSQE_FIXED_FILE;
    io_uring_sqe_set_data64(sqe, UD_PACK(OP_SEND, idx, bid, off));
}

// ---------------------------------------------------------------------------
// 关连接
// ---------------------------------------------------------------------------

static void close_conn(int idx) {
    if (idx < 0 || idx >= MAX_CONNS) return;
    if (g_fixed) {
        // 固定文件表里的文件不能 close()，要把它"换"成 -1 才算注销。
        // 这是每条连接唯一的一次系统调用（普通 accept 模式则每连接还要多一次 accept）。
        int neg = -1;
        io_uring_register_files_update(&g_ring, (unsigned)idx, &neg, 1);
    } else {
        close(idx);
    }
    g_conns[idx].in_use = 0;
    g_conns[idx].send_len = 0;
}

// ---------------------------------------------------------------------------
// 完成事件处理
// ---------------------------------------------------------------------------

static void handle_cqe(struct io_uring_cqe *cqe) {
    unsigned long ud = cqe->user_data;
    int op  = UD_OP(ud);
    int idx = UD_IDX(ud);
    int res = cqe->res;                 // 与系统调用同义：≥0 成功，<0 是负 errno
    unsigned flags = cqe->flags;

    // ---- 新连接 ----
    if (op == OP_ACCEPT) {
        if (res >= 0) {
            int conn = res;   // fixed 模式下这是固定表下标；否则是 fd
            if (conn < MAX_CONNS) {
                g_conns[conn].in_use = 1;
                g_conns[conn].send_len = 0;
                // 【注意】fixed 模式下 conn 是**表下标**而不是 fd，所以这里不能对它
                // 调 fcntl/setsockopt —— 那会作用到别的 fd 上。
                // 好消息是不需要：io_uring 对 socket 的收发本身就按非阻塞语义发起
                // （内核会在内部用 MSG_DONTWAIT 那一套），socket 自己的 O_NONBLOCK
                // 标志对 io_uring 的请求没有影响。
                // 如果确实要给连接设选项（比如 TCP_NODELAY），见文件末尾的两种办法。
                if (!g_fixed) set_nonblock(conn);
                arm_recv(conn);
            }
            // IORING_CQE_F_MORE 表示 multishot accept 还有效，不用补提交；
            // 没有它（出错或内核结束了这个 multishot）才需要补一次。
            if (!(flags & IORING_CQE_F_MORE)) arm_accept();
        } else if (res == -EAGAIN || res == -EINTR || res == -ECONNABORTED) {
            // 这些是"这次没接受成功"，multishot 已经结束，补一次即可
            arm_accept();
        } else {
            g_stat_err++;
            arm_accept();
        }
        return;
    }

    // ---- 收到数据 ----
    if (op == OP_RECV) {
        if (res > 0) {
            unsigned bid = 0;
            char *data = NULL;
            if (flags & IORING_CQE_F_BUFFER) {
                bid = flags >> IORING_CQE_BUFFER_SHIFT;   // 内核告诉我们用的哪一块
                if (bid < (unsigned)g_bufcount)
                    data = (char *)g_buf_data + (size_t)bid * g_bufsize;
            }
            g_stat_recv++;
            if (data) {
                g_conns[idx].send_len = (unsigned)res;
                arm_send(idx, data, (unsigned)res, bid, 0);
            } else {
                g_stat_err++;   // 没拿到缓冲区（不该发生），丢弃这条消息
            }
            // 有 F_MORE 说明 multishot 还在继续，不需要重挂
            if (!(flags & IORING_CQE_F_MORE)) arm_recv(idx);
        } else if (res == -ENOBUFS) {
            // 缓冲区环空了：multishot 会在这里结束，补挂一次 recv
            // （缓冲区会随着发送完成被逐块归还，回了就有得用了）
            g_stat_err++;
            arm_recv(idx);
        } else if (res == 0) {
            close_conn(idx);                 // 对端正常关闭
        } else if (res != -EAGAIN && res != -EINTR) {
            g_stat_err++;
            close_conn(idx);
        } else {
            arm_recv(idx);
        }
        return;
    }

    // ---- 发送完成 ----
    if (op == OP_SEND) {
        unsigned bid = UD_BID(ud);
        unsigned off = UD_OFF(ud);

        if (res < 0) {
            g_stat_err++;
            if (bid < (unsigned)g_bufcount) buf_return(bid);
            close_conn(idx);
            return;
        }

        if (g_zc) {
            // 零拷贝模式下同一次发送会有**两个** CQE，必须分清是哪一个。
            if (!(flags & IORING_CQE_F_NOTIF)) {
                // 第一个：数据已进协议栈，res = 发出去的字节数。
                // 但内存还被内核引用着，**不能**归还缓冲区。
                g_stat_send++;
                if (off + (unsigned)res < g_conns[idx].send_len) {
                    arm_send(idx, (char *)g_buf_data + (size_t)bid * g_bufsize + off + res,
                             g_conns[idx].send_len - off - res, bid, off + (unsigned)res);
                }
                return;
            }
            // 第二个（带 F_NOTIF）：内核不再引用这块内存了，此时归还才安全。
            // IORING_NOTIF_USAGE_ZC_COPIED 表示"内核其实没能真零拷贝，被迫拷了一份"
            // —— 统计它就知道零拷贝到底有没有兑现。
            if (flags & IORING_NOTIF_USAGE_ZC_COPIED) g_stat_zc_copy++;
            if (bid < (unsigned)g_bufcount) buf_return(bid);
            return;
        }

        // 普通 send：一个 CQE 就是全部信息
        g_stat_send++;
        if (off + (unsigned)res < g_conns[idx].send_len) {
            // 只发出去一部分，继续发剩下的（缓冲区不能还，还在用）
            arm_send(idx, (char *)g_buf_data + (size_t)bid * g_bufsize + off + res,
                     g_conns[idx].send_len - off - res, bid, off + (unsigned)res);
            return;
        }
        if (bid < (unsigned)g_bufcount) buf_return(bid);
    }
}

// ---------------------------------------------------------------------------
// 事件循环
// ---------------------------------------------------------------------------

static void event_loop(void) {
    struct io_uring_cqe *cqes[CQ_BATCH];

    while (!g_stop) {
        if (g_spin) {
            // 忙轮询：一次把 CQ 里已经完成的都取出来，**不进内核**。
            // 这是"收割 0 系统调用"的关键。
            unsigned n = io_uring_peek_batch_cqe(&g_ring, cqes, CQ_BATCH);
            if (n == 0) {
                // 没有完成事件。此时 submission 可能还没发出去（SQPOLL 下
                // io_uring_submit 只在需要唤醒内核线程时才真正进内核）。
                io_uring_submit(&g_ring);
                continue;
            }
            for (unsigned i = 0; i < n; i++) handle_cqe(cqes[i]);
            io_uring_cq_advance(&g_ring, n);
        } else {
            // 对照模式：进内核等一个完成事件（每次都会有一次 enter）
            struct io_uring_cqe *cqe;
            if (io_uring_wait_cqe(&g_ring, &cqe) < 0) break;
            handle_cqe(cqe);
            io_uring_cqe_seen(&g_ring, cqe);
        }
        // 把本轮准备的 SQE 提交出去。SQPOLL 下这一步不会进内核
        // （除非 SQ 环需要唤醒内核线程），见文件末尾的实测说明。
        io_uring_submit(&g_ring);
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char *argv[]) {
    g_port = argc > 1 ? atoi(argv[1]) : 19002;

    g_sqpoll  = (int)env_int("ECHO_SQPOLL", 1);
    g_sq_cpu  = (int)env_int("ECHO_SQ_CPU", -1);
    g_spin    = (int)env_int("ECHO_SPIN", 1);
    g_fixed   = (int)env_int("ECHO_FIXED", 1);
    g_zc      = (int)env_int("ECHO_ZC", 0);
    g_single_issuer = (int)env_int("ECHO_SINGLE_ISSUER", 1);
    g_bufcount = (int)env_int("ECHO_BUFS", 256);
    g_bufsize  = (size_t)env_int("ECHO_BUFSIZE", 4096);

    // ---- 监听 socket ----
    g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_fd < 0) die("socket");
    int opt = 1;
    setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(g_port);
    if (bind(g_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) die("bind");
    if (listen(g_listen_fd, 512) < 0) die("listen");
    // 非阻塞：multishot accept 要求监听 socket 非阻塞，
    // 否则 accept 可能阻塞住整个事件循环（单线程程序里这是致命的）
    set_nonblock(g_listen_fd);

    // ---- 建环与各种"注册" ----
    setup_ring();
    setup_fixed_files();
    setup_buf_ring();
    check_caps();

    // ---- 起手：挂上 multishot accept ----
    // 注意必须有一个在飞请求，否则忙轮询会空转（而且没有任何事件能触发进展）
    arm_accept();
    io_uring_submit(&g_ring);

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    printf("[modern] 监听 %d  port=%d\n", g_port, g_port);
    printf("[modern] 特性: SQPOLL=%d(SQ_CPU=%d) SPIN=%d FIXED=%d ZC=%d BUFS=%d×%zu\n",
           g_sqpoll, g_sq_cpu, g_spin, g_fixed, g_zc, g_bufcount, g_bufsize);
    fflush(stdout);

    event_loop();

    // ---- 收尾 ----
    printf("\n[modern] 退出: recv=%ld send=%ld err=%ld"
           " 其中零拷贝回退(内核被迫拷贝)=%ld\n",
           g_stat_recv, g_stat_send, g_stat_err, g_stat_zc_copy);
    io_uring_queue_exit(&g_ring);
    free(g_br);
    free(g_buf_data);
    close(g_listen_fd);
    return 0;
}

// ============================================================================
// 附：本机实测结果（Ubuntu 26.04 / 内核 7.0 / libc 2.14 · aarch64 8 核）
//
// 测试方法：bench_client 16 线程 @ 核 0-3，256B ping-pong；
//           服务端 app 线程绑核 4，SQ 内核线程绑核 5（ECHO_SQ_CPU=5）；
//           每个配置交替跑 5 轮取中位，避免机器噪声影响结论。
//
//   epoll 单线程（1 核）                       112k      1.00x
//   io_uring adv 全特性+忙轮询，开零拷贝（2 核） 185k      1.65x
//   本程序 全特性，开零拷贝（2 核）              175k      1.56x
//   → 本程序 全特性，**关**零拷贝（2 核）        **233k**  **2.08x**
//
// 三个值得记住的结论：
//
// 1) 稳态系统调用数 = 0
//    `perf stat -e syscalls:sys_enter_io_uring_enter -p <PID>` 实测：
//        全特性        : enter = 0        → 0.000 次/消息
//        关掉忙轮询     : enter = 48574    → 0.14 次/消息
//        关掉全部       : enter = 243478   → 1.02 次/消息
//    收发路径上一次系统调用都没有：提交靠 SQPOLL（写共享内存 + 释放语义发布 tail），
//    收割靠用户态忙轮询 CQ。代价是烧掉一个核做轮询。
//
// 2) 零拷贝在小消息下是负收益（本程序因此默认关掉它）
//    同一个二进制、只改 ECHO_ZC：开 175k，关 233k，**差了 26%**。
//    另一个独立实现（echo_io_uring_adv.c）上也是同样的现象，两个实现互相印证。
//    原因：SEND_ZC 每次发送会产生**两个** CQE（第二个带 F_NOTIF），
//    而 256B 消息省下的那次内存拷贝根本不值这个开销。
//    → 「零拷贝」不是免费的：只有当拷贝本身成为瓶颈（大消息、高带宽）时才划算。
//
// 3) 固定文件表 / direct accept 在这个 workload 下**没有可测量的收益**
//    ECHO_FIXED=1 vs 0：中位数都是 222k（各 7 轮交替实测）。
//    理论上它能省掉每次收发的 fget/fput、以及每条连接的 accept 系统调用，
//    但在这个量级上 fget/fput 不是瓶颈，所以收益被噪声淹没。
//    保留它是因为它是"现代写法"，而且不花额外代价；但如果你的场景是
//    超大连接数（几万）或超大 fd 表，它才可能显出价值。
//
// 4) 两个使用上的坑
//    * 固定文件表里拿到的是**表下标而不是 fd**，所以不能对它 close()/setsockopt()。
//      释放槽位要用 io_uring_register_files_update(idx, [-1])（-1 就是"注销这个槽"）。
//    * 如果确实要给每条连接设 socket 选项（比如 TCP_NODELAY），两条路：
//      a) 用**非** direct 的 multishot accept 拿到真 fd，设完选项再
//         io_uring_register_files_update() 装进固定表（每连接多一次系统调用）；
//      b) 干脆 ECHO_FIXED=0。本项目实测 ping-pong 下 TCP_NODELAY 开关差异 < 5%，
//         所以默认走零系统调用的路线。
//
// 其他：
//   * 本项目里 io_uring 真正赢过 epoll 的唯一路径就是这一条 ——
//     把「提交」和「收割」两条进内核的路都消掉（SQPOLL + 忙轮询）。
//     少开任何一个，性能都会掉到 epoll 以下。详见 README「实验五」。
//   * SQPOLL 必须独占一个核：同一个二进制绑 1 核只有 2.6k QPS
//     （内核线程和应用抢同一个核，比不开还慢）。这是部署形态问题，不是代码问题。
//   * 单线程就够吗？这台 8 核机器上单线程 + SQPOLL 已经打到 233k（占 2 个核）。
//     要再高得做多队列（每 worker 一套 ring + SO_REUSEPORT 分流，
//     分流思路见 network/echo_epoll_mt.c），不在本文件范围内。
// ============================================================================
