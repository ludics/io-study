// 实验：证明阻塞 fd 会"饿死"epoll 事件循环中的其他连接
//
// 场景：2 个客户端几乎同时发数据
//   - 若 fd 为阻塞：第1个连接的 read 会挂起，第2个连接的数据迟迟得不到处理
//   - 若 fd 非阻塞：两个连接都被及时处理
//
// 用法: ./epoll_starve_demo <0=阻塞|1=非阻塞>
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

#define PORT 19801
#define BUF_SIZE 5

static double now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static double g_t0;

// 客户端：连上后发数据，然后保持连接 HOLD_SEC 秒不关闭
static void client_thread(int id, int hold_sec) {
    usleep(300000);
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect"); close(s); return;
    }
    char msg[32];
    snprintf(msg, sizeof(msg), "CLIENT-%d-DATA", id);
    ssize_t w = write(s, msg, strlen(msg));
    (void)w;
    printf("[client-%d] 已发送，保持连接 %d 秒不关闭\n", id, hold_sec);
    fflush(stdout);
    sleep(hold_sec);
    close(s);
}

int main(int argc, char *argv[]) {
    int use_nonblock = argc > 1 ? atoi(argv[1]) : 1;
    int hold_sec = 3;  // 客户端保持连接 3 秒

    printf("========== 阻塞 fd 饿死其他连接 ==========\n");
    printf("模式: %s | 2 个客户端并发发送\n\n",
           use_nonblock ? "非阻塞(O_NONBLOCK)" : "阻塞(默认)");
    fflush(stdout);

    pid_t p1 = fork();
    if (p1 == 0) { client_thread(1, hold_sec); exit(0); }
    pid_t p2 = fork();
    if (p2 == 0) { usleep(50000); client_thread(2, hold_sec); exit(0); }

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(PORT);
    if (bind(lfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    listen(lfd, 5);
    int fl = fcntl(lfd, F_GETFL, 0);
    fcntl(lfd, F_SETFL, fl | O_NONBLOCK);

    int epfd = epoll_create1(0);
    struct epoll_event ev;
    ev.events = EPOLLIN; ev.data.fd = lfd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &ev);

    struct epoll_event events[16];
    int done = 0;
    g_t0 = now_ms();

    while (done < 2 && (now_ms() - g_t0) < 15000) {
        int n = epoll_wait(epfd, events, 16, 200);
        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            if (fd == lfd) {
                int cfd = accept(lfd, NULL, NULL);
                if (cfd < 0) continue;
                if (use_nonblock) {
                    int f = fcntl(cfd, F_GETFL, 0);
                    fcntl(cfd, F_SETFL, f | O_NONBLOCK);
                }
                printf("[server] accept cfd=%d (%.0f ms)\n", cfd, now_ms() - g_t0);
                fflush(stdout);
                ev.events = EPOLLIN | EPOLLET;
                ev.data.fd = cfd;
                epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &ev);
            } else {
                double t_read_start = now_ms();
                char buf[BUF_SIZE + 1];
                int total = 0;
                // ET: 循环读到 EAGAIN
                while (1) {
                    memset(buf, 0, sizeof(buf));
                    ssize_t r = read(fd, buf, BUF_SIZE);
                    if (r > 0) { total += r; continue; }
                    if (r == 0) break;
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    break;
                }
                double t_read_end = now_ms();
                printf("[server] cfd=%d 读完 %d 字节 | read 占用 %.0f ms | 完成于 %.0f ms\n",
                       fd, total, t_read_end - t_read_start, t_read_end - g_t0);
                fflush(stdout);
                done++;
                close(fd);
            }
        }
    }

    printf("\n>>> 两个连接全部处理完毕，总耗时 %.0f ms\n", now_ms() - g_t0);
    printf(">>> 关键看第2个连接的\"完成于\"时间：越晚说明被第1个阻塞得越久\n");

    kill(p1, SIGKILL); kill(p2, SIGKILL);
    waitpid(p1, NULL, 0); waitpid(p2, NULL, 0);
    close(epfd); close(lfd);
    return 0;
}
