// epoll 教学示例：单线程 Reactor echo server
// 编译: gcc -O2 -Wall -o ex_epoll ex_epoll.c
// 运行: ./ex_epoll 9000     测试: printf 'hi' | nc 127.0.0.1 9000
//
// 这个文件刻意写得「教科书式正确」：
//   * 监听 fd 与连接 fd 都设非阻塞
//   * ET 模式下循环读到 EAGAIN（不读空就永远收不到下次通知）
//   * 写用 write_all 处理部分写与 EAGAIN（生产代码必做）
//   * 每条连接一块独立缓冲区（这里用固定表，避免 malloc 抖动）
/* 单独编译需要 _GNU_SOURCE；经 Makefile 编译时用 -D 定义，加保护避免重定义告警 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

#define MAX_CONN 1024
#define BUF_SZ   4096
#define MAX_EVT  256

static char  g_buf[MAX_CONN][BUF_SZ];   // 下标即 fd，省掉 malloc（代价是 4MB 常驻）
static char *buf_of(int fd) { return (fd >= 0 && fd < MAX_CONN) ? g_buf[fd] : NULL; }

static void set_nonblock(int fd) {
    int f = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, f | O_NONBLOCK);
}

// 循环写完 len 字节。返回 0 成功，-1 出错（调用方负责关连接）
static int write_all(int fd, const char *p, int len) {
    int off = 0, retry = 0;
    while (off < len) {
        ssize_t n = write(fd, p + off, len - off);
        if (n > 0) { off += (int)n; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (++retry > 100) return -1;
            struct pollfd pfd = { .fd = fd, .events = POLLOUT };
            poll(&pfd, 1, 100);              // 等可写，最多 100ms
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return -1;
    }
    return 0;
}

static void close_conn(int epfd, int fd) {
    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);   // 显式摘除，意图清楚
    close(fd);                                  // close 本身也会从 epoll 移除
}

int main(int argc, char **argv) {
    int port = argc > 1 ? atoi(argv[1]) : 9000;

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    set_nonblock(lfd);                          // 否则 accept 可能阻塞整个事件循环

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    if (bind(lfd, (struct sockaddr *)&a, sizeof(a)) < 0) { perror("bind"); return 1; }
    if (listen(lfd, 512) < 0) { perror("listen"); return 1; }

    int epfd = epoll_create1(0);
    struct epoll_event ev, evs[MAX_EVT];
    ev.events = EPOLLIN;
    ev.data.fd = lfd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &ev);

    printf("epoll echo server on %d\n", port);
    fflush(stdout);

    for (;;) {
        int n = epoll_wait(epfd, evs, MAX_EVT, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }
        for (int i = 0; i < n; i++) {
            int fd = evs[i].data.fd;

            if (fd == lfd) {
                // 一次把 accept 队列取空（ET 必需：只接一个的话剩下的没有通知了）
                for (;;) {
                    int cfd = accept(lfd, NULL, NULL);
                    if (cfd < 0) break;                 // EAGAIN 或错误
                    if (cfd >= MAX_CONN) { close(cfd); continue; }
                    set_nonblock(cfd);
                    ev.events = EPOLLIN | EPOLLET;      // 这里用 ET
                    ev.data.fd = cfd;
                    if (epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &ev) < 0) close(cfd);
                }
                continue;
            }

            if (!(evs[i].events & EPOLLIN)) continue;

            char *b = buf_of(fd);
            if (!b) { close_conn(epfd, fd); continue; }

            int closed = 0;
            for (;;) {                       // ET：必须读到 EAGAIN
                ssize_t r = read(fd, b, BUF_SZ);
                if (r > 0) {
                    if (write_all(fd, b, (int)r) < 0) { closed = 1; break; }
                } else if (r == 0) {
                    closed = 1; break;       // 对端关闭
                } else {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;  // 读空，保留连接
                    if (errno == EINTR) continue;
                    closed = 1; break;
                }
            }
            if (closed) close_conn(epfd, fd);
        }
    }
    close(epfd);
    close(lfd);
    return 0;
}
