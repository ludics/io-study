// ============================================================================
// Reactor 模式的 echo server（最「教科书」的一版，方便对照 libco / io_uring）
//
// Reactor 的核心思想：**内核只通知「就绪」，数据搬运必须由应用自己调 read/write**。
//   事件循环： epoll_wait 阻塞 → 拿到一批就绪的 fd → 逐个处理
//   这和 Proactor（io_uring）的区别是：后者连数据搬运都由内核代办，
//   应用只在完成通知里拿到「已经帮你搬好了」的结果。
//
// 为什么这么写（三个容易写错的地方，都在下面标注了）：
//   1) 必须先设置 O_NONBLOCK（见 accept 之后）。
//      ET 模式下「可读」只会通知一次，如果你不一次把数据读空，剩下的数据
//      再也不会触发新事件 → 连接卡死。要读到 EAGAIN 就得是非阻塞，否则 read 会挂住整个事件循环。
//   2) 事件类型要判断（见 EPOLLIN 检查）。epoll 可能返回 EPOLLHUP/EPOLLERR 等，
//      不判断就 read 可能得到意外的错误。
//   3) fd 关闭前要先 EPOLL_CTL_DEL（见 len == 0 分支）。虽然 close 会自动摘除，
//      但显式删除更清晰，也避免 fd 复用时和旧注册混淆。
//
// 参数：reactor_server <端口>
// ============================================================================
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

// 小工具：表达式非 0 就打印行号并返回（仅用于启动阶段的错误处理）
#define ZERO_OR_RETURN(expr)            \
  do {                                  \
    int ret = (expr);                   \
    if (ret != 0) {                     \
      printf("%d %d\n", __LINE__, ret); \
      return ret;                       \
    }                                   \
  } while (0);

int main(int argc, char *argv[]) {
  if (argc < 2) {
    printf("usage: %s <port>\n", argv[0]);
    return 1;
  }
  struct sockaddr_in serv_addr;
  socklen_t serv_len = sizeof(serv_addr);
  int port = atoi(argv[1]);

  int lfd = socket(AF_INET, SOCK_STREAM, 0);
  memset(&serv_addr, 0, sizeof(serv_addr));
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  serv_addr.sin_port = htons(port);
  ZERO_OR_RETURN(bind(lfd, (struct sockaddr *)&serv_addr, serv_len));
  ZERO_OR_RETURN(listen(lfd, 36));

  struct sockaddr_in client_addr;
  socklen_t cli_len = sizeof(client_addr);

  int epfd = epoll_create(1024);
  struct epoll_event ev;
  // 注意监听 fd 用的是「电平触发」（没加 EPOLLET）：
  // 只要有新连接没被 accept 完，epoll_wait 就会一直返回它，所以这里每次 accept 一个也不会丢。
  // 如果给 lfd 也加上 EPOLLET，就必须循环 accept 到 EAGAIN。
  ev.events = EPOLLIN;
  ev.data.fd = lfd;
  epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &ev);

  struct epoll_event all[1024];
  while (1) {
    // -1 = 没有事件就永久阻塞，不空转烧 CPU
    int ret = epoll_wait(epfd, all, sizeof(all) / sizeof(all[0]), -1);
    for (int i = 0; i < ret; ++i) {
      int fd = all[i].data.fd;
      if (fd == lfd) {
        // 新连接到来
        int cfd = accept(lfd, (struct sockaddr *)&client_addr, &cli_len);
        if (cfd == -1) {
          perror("> Accept Error");
          exit(1);
        }
        // 【易错点 1】非阻塞是 ET 模式的硬性前提，漏了它整个事件循环会被单个连接拖死
        int flag = fcntl(cfd, F_GETFL);
        flag |= O_NONBLOCK;
        fcntl(cfd, F_SETFL, flag);
        struct epoll_event temp;
        temp.events = EPOLLIN | EPOLLET;  // ET：只在状态「变化」时通知一次
        temp.data.fd = cfd;
        epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &temp);
        char ip[64] = {0};
        printf("> New Client [%s:%d] => [%d]\n",
               inet_ntop(AF_INET, &client_addr.sin_addr.s_addr, ip, sizeof(ip)),
               ntohs(client_addr.sin_port), cfd);
      } else {
        // 【易错点 2】先确认确实是「可读」，排除 EPOLLERR 之类的杂事件
        if (!(all[i].events & EPOLLIN)) {
          continue;
        }
        char buffer[5] = {0};  // 故意用 5 字节这种非整块大小的缓冲区，逼出「必须循环读到 EAGAIN」
        int len;
        // 循环读到 -1(EAGAIN) 才停：ET 下必须一次读空，否则剩余数据不会再触发事件
        while ((len = read(fd, buffer, sizeof(buffer))) > 0) {
          if (write(fd, buffer, len) < 0) {  // echo：原样写回
            perror("> Send Error");
            break;
          }
        }
        if (len == -1) {
          if (errno == EAGAIN) {
            // Buffer Data is Finished!;  —— 正常收尾：数据读完了，等下一次事件
          } else {
            perror("> Recv Error");
            exit(1);
          }
        }
        if (len == 0) {
          // read 返回 0 = 对端关闭了连接
          printf("> Client [%d] Disconnected!\n", fd);
          // 【易错点 3】先摘除注册再关闭
          epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
          close(fd);
        }
      }
    }
  }

  close(lfd);
  return 0;
}
