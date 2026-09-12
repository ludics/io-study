// ============================================================================
// io_uring 版 Echo Server（Proactor 模式）—— 「基础版」，与 epoll 版对照
//
// 与 epoll 版（Reactor）对照：
//   - epoll:     epoll_wait 通知"fd 可读" → 应用自己 read/write（同步搬运数据）
//   - io_uring:  应用提交"把 fd 的数据读到 buf" → 内核异步完成 → CQE 通知"已完成"
//
// 这一版刻意只用 io_uring 最朴素的用法，好让它和 epoll 版的差异只来自"模型"：
//   每条消息 = 提交一次 SQE + 立刻 io_uring_enter 一次（批量大小恒为 1）。
// 结果是它**并不比 epoll 快**（实测 0.70x）——这恰好是本项目最重要的结论之一：
//   io_uring 的性能优势来自「批量提交 / 不进内核（SQPOLL）/ 免拷贝」这些用法，
//   而不是"换了个 API"。想看这些特性怎么打开，见 echo_io_uring_adv.c。
//
// ── 关键设计：用 user_data 同时编码 (fd, 操作类型) ──────────────────────────
// 完成事件（CQE）只会告诉你"某个请求完成了"，以及它带回来的 user_data。
// 内核不会告诉你这是哪个 fd、哪种操作 —— 这些必须自己塞进 user_data：
//     低 32 位 = fd，高 32 位 = 操作类型 (0=accept, 1=read, 2=write)
// 必须区分操作类型：read 完成和 write 完成要做完全不同的事
// （read 完成 → 去写；write 完成 → 去读），不区分就会逻辑错乱。
//
// 编译: gcc -O2 -o echo_io_uring echo_io_uring.c -luring
// 运行: ./echo_io_uring [port]
// 测试: printf 'hello' | nc 127.0.0.1 [port]
//
// 环境变量 ECHO_NODELAY=1 可对新连接设置 TCP_NODELAY（默认 0，保持原有行为），
// 供矩阵压测工具的 TCP_NODELAY 维度对比使用。
// ============================================================================

#include <arpa/inet.h>
#include <liburing.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_CONNS 1024
#define ENTRIES 256      // SQ/CQ 环大小：同时最多 256 个请求在飞，够单机 echo 用
#define BUF_SIZE 4096

// 操作类型
#define OP_ACCEPT 0
#define OP_READ 1
#define OP_WRITE 2

// 把 (fd, op) 编码进 user_data：低32位=fd，高32位=op
// 用位运算而不是结构体指针，是为了避免在 hot path 上 malloc/free 一个小对象。
static inline unsigned long encode_data(int fd, int op) {
    return ((unsigned long)op << 32) | (unsigned int)fd;
}
static inline int decode_fd(unsigned long data) { return (int)(data & 0xFFFFFFFF); }
static inline int decode_op(unsigned long data) { return (int)(data >> 32); }

static int listen_fd_global;
static int g_nodelay;

static void set_nodelay(int fd) {
    int opt = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
}

// ---------------------------------------------------------------------------
// 三个"准备请求"的函数
//
// 它们都只做一件事：拿到一个空闲 SQE 槽位，把请求内容填进去。
// **并不会真的提交** —— 真正进内核要等事件循环里的 io_uring_submit()。
// 这样一次循环里准备的所有 SQE 能被合并成一次 io_uring_enter（批量提交）。
// ---------------------------------------------------------------------------

// 提交 accept 请求（user_data 标记 OP_ACCEPT，fd 填 0）
//
// accept 是"单次"的：接受一个连接就消耗掉了，必须再提交一个才能接下一个。
// （多发型 accept 要用 IORING_ACCEPT_MULTISHOT，见 adv 版。）
static void add_accept(struct io_uring *ring) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe) return;   // SQ 环满了：这次准备失败，只能等下次 submit 后重试
    io_uring_prep_accept(sqe, listen_fd_global, NULL, NULL, 0);
    io_uring_sqe_set_data(sqe, (void *)encode_data(0, OP_ACCEPT));
}

// 提交读请求
//
// 注意这里用的是 io_uring_prep_recv（recv 语义）而不是 prep_read：
// recv 直接对应 socket，行为更明确（read 也能用，但对 socket 语义模糊）。
static void add_read(struct io_uring *ring, int fd, void *buf) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe) return;
    io_uring_prep_recv(sqe, fd, buf, BUF_SIZE, 0);
    io_uring_sqe_set_data(sqe, (void *)encode_data(fd, OP_READ));
}

// 提交写请求
static void add_write(struct io_uring *ring, int fd, void *buf, int len) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe) return;
    io_uring_prep_send(sqe, fd, buf, len, 0);
    io_uring_sqe_set_data(sqe, (void *)encode_data(fd, OP_WRITE));
}

int main(int argc, char *argv[]) {
    int port = argc > 1 ? atoi(argv[1]) : 19001;
    const char *nd = getenv("ECHO_NODELAY");
    g_nodelay = nd ? atoi(nd) : 0;

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    // SO_REUSEADDR：避免 TIME_WAIT 状态的老连接挡住 bind，让程序能反复重启
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    if (listen(lfd, 128) < 0) {
        perror("listen");
        return 1;
    }
    listen_fd_global = lfd;

    // 建环。ENTRIES 同时决定 SQ 和 CQ 的容量（liburing 默认 CQ = 2×SQ）
    struct io_uring ring;
    int ret = io_uring_queue_init(ENTRIES, &ring, 0);
    if (ret < 0) {
        fprintf(stderr, "io_uring_queue_init failed: %s\n", strerror(-ret));
        fprintf(stderr, "\n可能原因：\n");
        fprintf(stderr, "  1. 内核 < 5.1（不支持 io_uring）\n");
        fprintf(stderr, "  2. 容器/Docker 的 seccomp 策略禁用了 io_uring_setup\n");
        fprintf(stderr, "  3. 未安装 liburing: apt-get install -y liburing-dev\n");
        return 2;   // 2 = 环境限制（约定：压测脚本据此把该实现标成 N/A）
    }

    // 每个连接独立缓冲区（避免 fd 复用导致数据串扰）
    //
    // 为什么用 static 数组而不是每条连接 malloc？
    //   * 完成事件回来时只能靠 fd 找到缓冲区，所以缓冲区必须"按 fd 可寻址"；
    //   * 这个数组是 BSS 段，进程启动就分配好，hot path 上没有任何分配开销。
    // 代价是固定吃掉 MAX_CONNS × BUF_SIZE = 4MB 内存（用内存换确定性）。
    // 注意 fd 是被内核复用的：连接关闭后同一个 fd 号会给新连接，所以
    // 这里不需要（也不能）"释放"某条连接的缓冲区。
    static char bufs[MAX_CONNS][BUF_SIZE];

    // 初始提交一个 accept，并**立刻提交**：事件循环靠 CQE 驱动，
    // 如果此时 SQ 环里什么都没有，io_uring_wait_cqe 会永远等下去。
    add_accept(&ring);
    io_uring_submit(&ring);

    printf("[io_uring] Echo server on port %d (Proactor)\n", port);
    printf("[io_uring] 用 netcat 测试: printf 'hello' | nc 127.0.0.1 %d\n", port);
    fflush(stdout);

    struct io_uring_cqe *cqe;
    while (1) {
        // 等待完成事件（内核已完成 I/O，包括数据搬运）
        //
        // 这一句就是 Proactor 和 Reactor 的分界：
        //   epoll_wait 返回时数据还在内核缓冲区里，要自己 read 才拿到；
        //   这里返回时**数据已经在 bufs 里了**，内核替我们搬完了。
        ret = io_uring_wait_cqe(&ring, &cqe);
        if (ret < 0) {
            fprintf(stderr, "io_uring_wait_cqe failed: %s\n", strerror(-ret));
            break;
        }

        unsigned long data = (unsigned long)cqe->user_data;
        int res = cqe->res;             // 与系统调用同义：成功=字节数/fd，失败=负 errno
        int fd = decode_fd(data);
        int op = decode_op(data);
        io_uring_cqe_seen(&ring, cqe);  // 告诉内核"这个 CQE 我处理完了"，可以复用槽位

        switch (op) {
        case OP_ACCEPT:
            // accept 完成：res = 新连接的 fd
            if (res >= 0) {
                int conn_fd = res;
                if (g_nodelay) set_nodelay(conn_fd);
                printf("[io_uring] 新连接 fd=%d\n", conn_fd);
                fflush(stdout);
                // 为新连接提交读请求
                add_read(&ring, conn_fd, bufs[conn_fd % MAX_CONNS]);
            }
            // 继续提交下一个 accept（接受一个消耗一个）
            // 这里不看 res 是否成功：即使是 -EAGAIN 之类的错误，也要补一个 accept，
            // 否则监听就断了，服务会静默地不再接受新连接。
            add_accept(&ring);
            break;

        case OP_READ:
            // 读完成：res = 读到的字节数（数据已在 bufs 中）
            if (res > 0) {
                // 提交写请求，回显同样的数据（长度 = res）
                // 注意这里直接复用读进来的那块缓冲区当发送缓冲区 —— 省掉一次拷贝。
                // 代价是"这块缓冲区分不清所有权"：写完之前不能再对它挂 recv。
                // 所以下一句 add_read 是等到 WRITE 完成之后才发的（见下面 OP_WRITE）。
                add_write(&ring, fd, bufs[fd % MAX_CONNS], res);
            } else if (res == 0) {
                // res == 0 表示对端正常关闭（EOF）
                printf("[io_uring] 连接 fd=%d 关闭\n", fd);
                fflush(stdout);
                close(fd);
            } else {
                // res < 0：负 errno（-ECONNRESET 等）
                fprintf(stderr, "[io_uring] read error fd=%d: %s\n", fd, strerror(-res));
                close(fd);
            }
            break;

        case OP_WRITE:
            // 写完成：res = 写出的字节数
            if (res > 0) {
                // 回显完成，缓冲区不再被占用，继续提交下一个读
                add_read(&ring, fd, bufs[fd % MAX_CONNS]);
            } else {
                fprintf(stderr, "[io_uring] write error fd=%d: %s\n", fd, strerror(-res));
                close(fd);
            }
            break;
        }
        // 提交本轮准备的所有 SQE。
        // 这里虽然每次只准备 1~2 个，但接口是"批量"的 —— 想提高吞吐就得
        // 一次准备多个再提交，参见 adv 版的批量提交与 SQPOLL。
        io_uring_submit(&ring);
    }

    io_uring_queue_exit(&ring);
    close(lfd);
    return 0;
}
