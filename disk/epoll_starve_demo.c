// ============================================================================
// 实验：证明阻塞 fd 会"饿死"epoll 事件循环中的其他连接
//
// 场景：2 个客户端几乎同时发数据
//   - 若 fd 为阻塞：第1个连接的 read 会挂起，第2个连接的数据迟迟得不到处理
//   - 若 fd 非阻塞：两个连接都被及时处理
//
// ── 和上一个 demo（epoll_nonblock_demo）的区别 ──────────────────────────────
// 上一个 demo 证明的是「阻塞 fd + ET 会把自己卡死」（单连接，死锁）。
// 这一个证明的是**危害面更大的一件事**：事件循环是单线程串行的，
// 一个连接卡在 read 上，**同一条 epoll 上的其他连接全部被饿死** ——
// 数据明明已经到了，内核也准备好了，但没人去处理，因为进程睡在别人的 read 里。
// 这就是"一个慢客户端拖垮整个服务"的经典成因。
//
// ── 怎么读这个实验的输出 ────────────────────────────────────────────────────
// 服务端对每个连接打印两个时间：
//     read 占用    = 这一次读循环花了多久
//     完成于       = 相对于实验开始的绝对时间
// 阻塞模式下，第 2 个连接会晚 3 秒才被处理（= 客户端 hold_sec），
// 因为前 3 秒进程一直挂在第 1 个连接的 read 上。
//
// 用法: ./epoll_starve_demo <0=阻塞|1=非阻塞>
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

#define PORT 19801
#define BUF_SIZE 5   // 又是小 buffer：让"读循环"多转几圈，把阻塞窗口拉长

// 单调时钟，测耗时只认它（见 epoll_nonblock_demo.c 里的说明）
static double now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static double g_t0;   // 实验起点，用来算"完成于 xx ms"

// 客户端：连上后发数据，然后保持连接 HOLD_SEC 秒不关闭
//
// 为什么要 hold 住不关？因为这是制造"饿死"的必要条件：
//   客户端发 15 字节，服务端 BUF_SIZE=5，读 3 次就读完了；
//   第 4 次 read 时缓冲区已空 —— 如果此时客户端已 close，read 会返回 0（EOF），
//   进程立刻脱身，看不出饿死。只有连接还开着，阻塞 fd 才会一直挂在那里。
static void client_thread(int id, int hold_sec) {
    usleep(300000);  // 等服务端 listen 就绪
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
    (void)w;   // 显式忽略返回值：demo 里不处理写失败
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

    // 两个客户端各 fork 一个进程（不用线程：服务端要能被"卡住"，
    // 客户端必须独立于服务端的执行流）
    pid_t p1 = fork();
    if (p1 == 0) { client_thread(1, hold_sec); exit(0); }
    pid_t p2 = fork();
    // 让第 2 个客户端晚 50ms 连上来：这样两个连接**已经有数据在等**，
    // 服务端却还卡在第 1 个连接的 read 里 —— 饿死现象才看得清。
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
    // 监听 fd 必须非阻塞：否则 accept 也会成为卡死点（与本次实验变量无关，先排除掉）
    int fl = fcntl(lfd, F_GETFL, 0);
    fcntl(lfd, F_SETFL, fl | O_NONBLOCK);

    int epfd = epoll_create1(0);
    struct epoll_event ev;
    ev.events = EPOLLIN; ev.data.fd = lfd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &ev);

    struct epoll_event events[16];
    int done = 0;
    g_t0 = now_ms();

    // 15 秒上限 = 防止阻塞模式下真的挂死跑不完，留一条退出路径
    while (done < 2 && (now_ms() - g_t0) < 15000) {
        // 200ms 超时：即使没有事件也能定期回到循环顶部检查退出条件
        int n = epoll_wait(epfd, events, 16, 200);
        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            if (fd == lfd) {
                int cfd = accept(lfd, NULL, NULL);
                if (cfd < 0) continue;   // 非阻塞 accept 的 EAGAIN，跳过即可
                // 自变量：新连接设不设非阻塞
                if (use_nonblock) {
                    int f = fcntl(cfd, F_GETFL, 0);
                    fcntl(cfd, F_SETFL, f | O_NONBLOCK);
                }
                printf("[server] accept cfd=%d (%.0f ms)\n", cfd, now_ms() - g_t0);
                fflush(stdout);
                ev.events = EPOLLIN | EPOLLET;   // ET：只能靠循环读到 EAGAIN 收尾
                ev.data.fd = cfd;
                epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &ev);
            } else {
                double t_read_start = now_ms();
                char buf[BUF_SIZE + 1];
                int total = 0;
                // ET: 循环读到 EAGAIN —— 阻塞 fd 就会永远等不到那个 EAGAIN
                while (1) {
                    memset(buf, 0, sizeof(buf));
                    ssize_t r = read(fd, buf, BUF_SIZE);
                    if (r > 0) { total += r; continue; }       // 有数据就继续读
                    if (r == 0) break;                          // 对端关闭
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;  // 读干净了
                    break;                                      // 其他错误也让出去
                }
                double t_read_end = now_ms();
                // 【这张输出的两个数字就是全部结论】
                //   "read 占用"：本次读循环耗时。阻塞模式 ≈ hold_sec（挂住了）
                //   "完成于"   ：从实验开始到处理完这个连接。第 2 个连接越晚 = 被饿死越久
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

    // 两个客户端进程还在 sleep，必须收尸
    kill(p1, SIGKILL); kill(p2, SIGKILL);
    waitpid(p1, NULL, 0); waitpid(p2, NULL, 0);
    close(epfd); close(lfd);
    return 0;
}
