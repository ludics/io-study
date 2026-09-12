// 实验：验证 epoll 为什么必须搭配 O_NONBLOCK
//
// 两种模式对比：
//   A. ET(边缘触发) + 阻塞 fd  → 会卡死（read 阻塞在最后一次）
//   B. ET(边缘触发) + 非阻塞 fd → 正常（read 返回 EAGAIN 后退出循环）
//
// 用法: ./epoll_nonblock_demo <0=阻塞|1=非阻塞>
// 用 timeout 命令可检测是否卡死：timeout 3 ./epoll_nonblock_demo 0
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
#define BUF_SIZE 5   // 故意用小 buffer，强制触发"循环读"

static double now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

// 模拟客户端：发数据后保持连接不关闭
static void client_thread() {
    usleep(300000);  // 等 server 起来
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect"); close(s); return;
    }
    // 发送 20 字节（BUF_SIZE=5，需要 4 次 read 才能读完）
    const char *msg = "HELLO-WORLD-12345";
    if (write(s, msg, strlen(msg)) < 0) {
        perror("write");
    }
    printf("[client] 已发送 %zu 字节，保持连接不关闭\n", strlen(msg));
    fflush(stdout);
    sleep(5);  // 保持连接 5 秒，但不关闭
    close(s);
}

int main(int argc, char *argv[]) {
    int use_nonblock = argc > 1 ? atoi(argv[1]) : 1;

    printf("========== epoll %s fd 实验 ==========\n",
           use_nonblock ? "非阻塞(O_NONBLOCK)" : "阻塞(默认)");
    printf("模式: ET(边缘触发) | buffer=%d 字节\n", BUF_SIZE);
    printf("客户端将发送 17 字节，需要 4 次 read 读完\n\n");
    fflush(stdout);

    // 启动客户端子进程
    pid_t pid = fork();
    if (pid == 0) { client_thread(); exit(0); }

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
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

    // 只处理有限次事件，然后退出（避免无限循环）
    while (handled < 1 && (now_ms() - t_start) < 4000) {
        int n = epoll_wait(epfd, events, 16, 500);
        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            if (fd == lfd) {
                int cfd = accept(lfd, NULL, NULL);
                if (cfd < 0) continue;

                if (use_nonblock) {
                    int f = fcntl(cfd, F_GETFL, 0);
                    fcntl(cfd, F_SETFL, f | O_NONBLOCK);
                    printf("[server] accept cfd=%d，已设为 O_NONBLOCK\n", cfd);
                } else {
                    printf("[server] accept cfd=%d，保持阻塞模式\n", cfd);
                }
                fflush(stdout);

                ev.events = EPOLLIN | EPOLLET;  // ET 边缘触发
                ev.data.fd = cfd;
                epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &ev);
            } else {
                // ET 模式：必须循环 read 直到 EAGAIN
                printf("[server] 开始循环 read (ET 模式)\n");
                fflush(stdout);
                char buf[BUF_SIZE + 1];
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
                        continue;
                    } else if (r == 0) {
                        printf("[server]   对端关闭\n");
                        break;
                    } else {
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
                handled++;
                close(fd);
            }
        }
    }

    double elapsed = now_ms() - t_start;
    if (handled == 0) {
        printf("\n>>> 结果: 4 秒内未完成 —— 【卡死】在 read 上（阻塞 fd + ET）\n");
    } else {
        printf(">>> 结果: 正常完成，耗时 %.1f ms —— 未卡死\n", elapsed);
    }

    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    close(epfd);
    close(lfd);
    return 0;
}
