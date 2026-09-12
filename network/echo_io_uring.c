// io_uring 版 Echo Server（Proactor 模式）
//
// 与 epoll 版（Reactor）对照：
//   - epoll:     epoll_wait 通知"fd 可读" → 应用自己 read/write（同步搬运数据）
//   - io_uring:  应用提交"把 fd 的数据读到 buf" → 内核异步完成 → CQE 通知"已完成"
//
// 关键设计：用 user_data 同时编码 (fd, 操作类型)
//   低 32 位 = fd，高 32 位 = 操作类型 (0=accept, 1=read, 2=write)
//   必须区分操作类型，否则 read 完成和 write 完成无法区分（会导致逻辑错乱）
//
// 编译: gcc -O2 -o echo_io_uring echo_io_uring.c -luring
// 运行: ./echo_io_uring [port]
// 测试: printf 'hello' | nc 127.0.0.1 [port]

#include <arpa/inet.h>
#include <liburing.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_CONNS 1024
#define ENTRIES 256
#define BUF_SIZE 4096

// 操作类型
#define OP_ACCEPT 0
#define OP_READ 1
#define OP_WRITE 2

// 把 (fd, op) 编码进 user_data：低32位=fd，高32位=op
static inline unsigned long encode_data(int fd, int op) {
    return ((unsigned long)op << 32) | (unsigned int)fd;
}
static inline int decode_fd(unsigned long data) { return (int)(data & 0xFFFFFFFF); }
static inline int decode_op(unsigned long data) { return (int)(data >> 32); }

static int listen_fd_global;

// 提交 accept 请求（user_data 标记 OP_ACCEPT，fd 填 0）
static void add_accept(struct io_uring *ring) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe) return;
    io_uring_prep_accept(sqe, listen_fd_global, NULL, NULL, 0);
    io_uring_sqe_set_data(sqe, (void *)encode_data(0, OP_ACCEPT));
}

// 提交读请求
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

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
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

    struct io_uring ring;
    int ret = io_uring_queue_init(ENTRIES, &ring, 0);
    if (ret < 0) {
        fprintf(stderr, "io_uring_queue_init failed: %s\n", strerror(-ret));
        fprintf(stderr, "\n可能原因：\n");
        fprintf(stderr, "  1. 内核 < 5.1（不支持 io_uring）\n");
        fprintf(stderr, "  2. 容器/Docker 的 seccomp 策略禁用了 io_uring_setup\n");
        fprintf(stderr, "  3. 未安装 liburing: apt-get install -y liburing-dev\n");
        return 2;
    }

    // 每个连接独立缓冲区（避免 fd 复用导致数据串扰）
    static char bufs[MAX_CONNS][BUF_SIZE];

    // 初始提交一个 accept
    add_accept(&ring);
    io_uring_submit(&ring);

    printf("[io_uring] Echo server on port %d (Proactor)\n", port);
    printf("[io_uring] 用 netcat 测试: printf 'hello' | nc 127.0.0.1 %d\n", port);
    fflush(stdout);

    struct io_uring_cqe *cqe;
    while (1) {
        // 等待完成事件（内核已完成 I/O，包括数据搬运）
        ret = io_uring_wait_cqe(&ring, &cqe);
        if (ret < 0) {
            fprintf(stderr, "io_uring_wait_cqe failed: %s\n", strerror(-ret));
            break;
        }

        unsigned long data = (unsigned long)cqe->user_data;
        int res = cqe->res;
        int fd = decode_fd(data);
        int op = decode_op(data);
        io_uring_cqe_seen(&ring, cqe);

        switch (op) {
        case OP_ACCEPT:
            // accept 完成：res = 新连接的 fd
            if (res >= 0) {
                int conn_fd = res;
                printf("[io_uring] 新连接 fd=%d\n", conn_fd);
                fflush(stdout);
                // 为新连接提交读请求
                add_read(&ring, conn_fd, bufs[conn_fd % MAX_CONNS]);
            }
            // 继续提交下一个 accept
            add_accept(&ring);
            break;

        case OP_READ:
            // 读完成：res = 读到的字节数（数据已在 bufs 中）
            if (res > 0) {
                // 提交写请求，回显同样的数据（长度 = res）
                add_write(&ring, fd, bufs[fd % MAX_CONNS], res);
            } else if (res == 0) {
                // 对端关闭
                printf("[io_uring] 连接 fd=%d 关闭\n", fd);
                fflush(stdout);
                close(fd);
            } else {
                fprintf(stderr, "[io_uring] read error fd=%d: %s\n", fd, strerror(-res));
                close(fd);
            }
            break;

        case OP_WRITE:
            // 写完成：res = 写出的字节数
            if (res > 0) {
                // 回显完成，继续提交下一个读
                add_read(&ring, fd, bufs[fd % MAX_CONNS]);
            } else {
                fprintf(stderr, "[io_uring] write error fd=%d: %s\n", fd, strerror(-res));
                close(fd);
            }
            break;
        }
        io_uring_submit(&ring);
    }

    io_uring_queue_exit(&ring);
    close(lfd);
    return 0;
}
