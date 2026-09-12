// ============================================================================
// epoll 版 Echo Server（Reactor 模式）—— 单线程最小实现
//
// 每个 I/O 事件：epoll_wait 通知就绪 → 应用自己 read/write（同步系统调用）
//
// ── Reactor 与 Proactor 的分界（对照 echo_io_uring.c 看）──────────────────
//   Reactor : epoll_wait 只说"fd 可读了"，**数据还在内核缓冲区里**；
//             要拿到数据必须自己 read() 一次（同步系统调用 + 一次内存拷贝）。
//   Proactor: 提交"把数据读到 buf"，完成事件回来时**数据已经在 buf 里了**，
//             数据搬运由内核代劳。
//   文件的注释里标了每一处 "// 系统调用"，方便数清楚一条消息要进几次内核。
//
// ── 本文件刻意保持"最小实现"，因此有三处简化（生产代码不能这么写）───────
//   1) ET 下只 read 一次，没有循环读到 EAGAIN。
//      严格来说这是**不严谨的**：ET 只在状态变化时通知一次，如果一次没读完，
//      剩下的数据就再也收不到通知了。正确写法见 echo_epoll_mt.c 的 serve_one()。
//      （压测里每条连接只有 1 个在途请求，不会触发这个问题，所以数字仍可用。）
//   2) write 不处理"只写出去一部分"（返回值 < r）和 EAGAIN。
//      正确做法见 echo_epoll_mt.c 的 write_all()。
//   3) 所有连接共用一块 buf —— 只在"读到就立刻写完、全程同步"时才安全。
//
// 环境变量 ECHO_NODELAY=1 可对每条连接设置 TCP_NODELAY（默认 0，保持原有行为），
// 供矩阵压测工具的 TCP_NODELAY 维度对比使用。
// ============================================================================
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

// 设为非阻塞。ET（EPOLLET）下这是**必须**的：
// 事件循环是单线程串行的，一旦 read/write 阻塞，整个服务（所有连接）都停摆。
// 详见 disk/epoll_nonblock_demo.c 与 disk/epoll_starve_demo.c 两个实验。
static void set_nonblock(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void set_nodelay(int fd) {
  int opt = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
}

int main(int argc, char *argv[]) {
  int port = argc > 1 ? atoi(argv[1]) : 19000;
  const char *nd = getenv("ECHO_NODELAY");
  int nodelay = nd ? atoi(nd) : 0;
  int lfd = socket(AF_INET, SOCK_STREAM, 0);
  int opt = 1;
  // SO_REUSEADDR：免去 TIME_WAIT 对 bind 的阻挡，方便反复重启压测
  setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  // 监听 fd 也要非阻塞：accept 在 EAGAIN 时会阻塞（epoll 的通知只代表"可能有连接"，
  // 多进程/多线程 accept 同一个 fd 时连接可能已被别人取走）
  set_nonblock(lfd);

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);   // 监听所有网卡
  addr.sin_port = htons(port);                // 端口要用网络字节序
  bind(lfd, (struct sockaddr *)&addr, sizeof(addr));
  listen(lfd, 128);                           // 128 = 已完成连接队列上限（backlog）

  int epfd = epoll_create1(0);
  // 监听 fd 注册为「可读」：有新连接到来时变成可读
  struct epoll_event ev = {.events = EPOLLIN, .data.fd = lfd};
  epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &ev);

  // events 是"本次有哪些 fd 就绪"的输出数组；给 64 意味着一次最多处理 64 个就绪 fd，
  // 超出的部分留在内核里，下一轮 epoll_wait 会继续吐出。
  struct epoll_event events[64];
  // 所有连接共用一块 buf：仅在"读到就立刻同步写完"时成立（见文件头简化说明 3）
  char buf[4096];
  printf("[epoll] Echo server on port %d (Reactor)\n", port);
  fflush(stdout);

  while (1) {
    // -1 = 没有事件就一直睡（不占 CPU）。这是 Reactor 的"等待"环节。
    int n = epoll_wait(epfd, events, 64, -1);   // 系统调用
    for (int i = 0; i < n; i++) {
      int fd = events[i].data.fd;
      if (fd == lfd) {
        // 走到这里 = 有新连接。注意没有循环 accept（简化说明 1 的同类问题：
        // 一次事件可能积压多个连接，正确做法是循环 accept 到 EAGAIN）
        int cfd = accept(lfd, NULL, NULL);       // 系统调用
        set_nonblock(cfd);
        if (nodelay) set_nodelay(cfd);
        // EPOLLET = 边沿触发。注意 LT/ET 只影响"通知时机"，不改变正确性要求：
        // 无论哪种都要把数据读干净，ET 下尤其必须（否则永远等不到下次通知）。
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = cfd;
        epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &ev); // 系统调用
      } else {
        // 已连接的客户端有数据可读。r = 实际读到的字节数。
        int r = read(fd, buf, sizeof(buf));       // 系统调用（同步数据搬运）
        if (r > 0) {
          // 立刻回显。注意这里只 write 一次，没有处理"部分写"（简化说明 2）。
          // 同样地，write 在这条消息里也是一次同步的数据搬运。
          if (write(fd, buf, r) < 0) {            // 系统调用（同步数据搬运）
            perror("write");
          }
        } else {
          // r == 0 → 对端正常关闭；r < 0（非 EAGAIN）→ 出错。
          // 两者都直接收摊。close() 本身会把 fd 从 epoll 里摘掉，
          // 这里显式 DEL 一下让意图更清楚。
          epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
          close(fd);
        }
      }
    }
  }
  return 0;
}
