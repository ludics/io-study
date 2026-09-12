// io_uring 教学示例：最朴素的 Proactor echo server（不用 SQPOLL，便于看清提交/收割）
// 编译: gcc -O2 -Wall -o ex_uring ex_uring.c -luring
// 运行: ./ex_uring 9001
//
// 想看清的三件事：
//   1) 应用只负责「描述请求」（填 SQE），数据搬运由内核完成
//   2) user_data 是我们自己塞的上下文 —— 内核原样带回，必须自己编码 (fd, 操作)
//   3) 完成事件（CQE）里数据已经在缓冲区里了（Proactor 与 Reactor 的分界）
/* 单独编译需要 _GNU_SOURCE；经 Makefile 编译时用 -D 定义，加保护避免重定义告警 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <liburing.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ENTRIES  256
#define MAX_CONN 1024
#define BUF_SZ   4096

enum { OP_ACCEPT = 1, OP_RECV, OP_SEND };

// 把 (op, fd, off, len) 打包进 64 位 user_data。
//   内核只负责原样带回 user_data，请求的上下文（哪个 fd、什么操作、
//   这条消息的哪个片段）必须我们自己编码 —— 这是 io_uring 编程里最容易漏的一点。
//   布局: op(8) | fd(24) | off(16) | len(16)
//   off/len 各 16 位，支持到 64KB 的单条消息（echo 场景足够）
static unsigned long pack(int op, int fd, unsigned off, unsigned len) {
    return (unsigned long)(unsigned)op
         | ((unsigned long)(unsigned)fd  << 8)
         | ((unsigned long)(off & 0xFFFF) << 32)
         | ((unsigned long)(len & 0xFFFF) << 48);
}
static int      unpack_op(unsigned long v)  { return (int)(v & 0xFF); }
static int      unpack_fd(unsigned long v)  { return (int)((v >> 8) & 0xFFFFFF); }
static unsigned unpack_off(unsigned long v) { return (unsigned)((v >> 32) & 0xFFFF); }
static unsigned unpack_len(unsigned long v) { return (unsigned)((v >> 48) & 0xFFFF); }

static struct io_uring g_ring;
static int g_lfd;
static char g_buf[MAX_CONN][BUF_SZ];

static char *buf_of(int fd) { return (fd >= 0 && fd < MAX_CONN) ? g_buf[fd] : NULL; }

static void set_nonblock(int fd) {
    int f = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, f | O_NONBLOCK);
}

// 准备一个请求 = 拿一个空闲 SQE 槽位、把内容填进去。注意这里**不提交**。
static struct io_uring_sqe *new_sqe(void) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&g_ring);
    if (!sqe) {                 // SQ 环满：先提交腾出槽位再重试
        io_uring_submit(&g_ring);
        sqe = io_uring_get_sqe(&g_ring);
    }
    return sqe;
}

static void arm_accept(void) {
    struct io_uring_sqe *sqe = new_sqe();
    if (!sqe) return;
    // 传 NULL 拿不到对端地址，但换来省一次内存写；要地址就传 &addr/&len
    io_uring_prep_accept(sqe, g_lfd, NULL, NULL, 0);
    io_uring_sqe_set_data64(sqe, pack(OP_ACCEPT, 0, 0, 0));
}

static void arm_recv(int fd) {
    struct io_uring_sqe *sqe = new_sqe();
    if (!sqe) return;
    // 注意是 recv 而不是 read：对 socket 语义更明确
    io_uring_prep_recv(sqe, fd, buf_of(fd), BUF_SZ, 0);
    io_uring_sqe_set_data64(sqe, pack(OP_RECV, fd, 0, 0));
}

// 发这条消息的 [off, off+len) 这一段。off 非 0 说明是「部分写」剩下的尾巴。
static void arm_send(int fd, unsigned off, unsigned len) {
    struct io_uring_sqe *sqe = new_sqe();
    if (!sqe) return;
    io_uring_prep_send(sqe, fd, buf_of(fd) + off, len, MSG_NOSIGNAL);
    io_uring_sqe_set_data64(sqe, pack(OP_SEND, fd, off, len));
}

int main(int argc, char **argv) {
    int port = argc > 1 ? atoi(argv[1]) : 9001;

    g_lfd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(g_lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    if (bind(g_lfd, (struct sockaddr *)&a, sizeof(a)) < 0) { perror("bind"); return 1; }
    if (listen(g_lfd, 512) < 0) { perror("listen"); return 1; }
    set_nonblock(g_lfd);      // io_uring 内部也按非阻塞发起，这里设上更保险

    int ret = io_uring_queue_init(ENTRIES, &g_ring, 0);
    if (ret < 0) {
        fprintf(stderr, "io_uring_queue_init: %s\n", strerror(-ret));
        fprintf(stderr, "需要内核 >= 5.1，且不要跑在默认 seccomp 的容器里\n");
        return 2;
    }

    arm_accept();
    io_uring_submit(&g_ring);      // 必须有一次在飞请求，否则循环无事可做

    printf("io_uring echo server on %d\n", port);
    fflush(stdout);

    for (;;) {
        struct io_uring_cqe *cqe;
        // CQ 非空时直接返回（纯用户态读共享内存）；空的时候才进内核睡等
        if (io_uring_wait_cqe(&g_ring, &cqe) < 0) break;

        unsigned long ud  = cqe->user_data;
        int           op  = unpack_op(ud);
        int           fd  = unpack_fd(ud);
        unsigned      off = unpack_off(ud);
        unsigned      len = unpack_len(ud);
        int           res = cqe->res;        // 与系统调用同义：<0 是负 errno
        io_uring_cqe_seen(&g_ring, cqe);     // 交还 CQE 槽位

        switch (op) {
        case OP_ACCEPT:
            if (res >= 0) {
                set_nonblock(res);
                arm_recv(res);
            }
            // accept 是「单次」的：接受一个就消耗掉了，必须补一个
            arm_accept();
            break;

        case OP_RECV:
            if (res > 0) {
                arm_send(fd, 0, (unsigned)res);   // 数据已在 buf_of(fd) 里
            } else if (res == 0) {
                close(fd);                        // 对端关闭
            } else {
                fprintf(stderr, "recv fd=%d: %s\n", fd, strerror(-res));
                close(fd);
            }
            break;

        case OP_SEND:
            if (res < 0) { close(fd); break; }
            // 【易错】res 可能小于我们请求的 len（写缓冲区满 → 只发出去一部分）。
            // 必须从 off+res 继续把剩下那截发完，否则客户端收到的是残缺数据。
            if ((unsigned)res < len) {
                arm_send(fd, off + (unsigned)res, len - (unsigned)res);
            } else {
                arm_recv(fd);                     // 回显完成，续挂下一轮接收
            }
            break;
        }
        io_uring_submit(&g_ring);              // 提交本轮准备的所有 SQE
    }
    io_uring_queue_exit(&g_ring);
    close(g_lfd);
    return 0;
}
