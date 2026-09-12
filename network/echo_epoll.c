#include <arpa/inet.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

// epoll 版 Echo Server（Reactor 模式）
// 每个 I/O 事件：epoll_wait 通知就绪 → 应用自己 read/write（同步系统调用）

static void set_nonblock(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int main(int argc, char *argv[]) {
  int port = argc > 1 ? atoi(argv[1]) : 19000;
  int lfd = socket(AF_INET, SOCK_STREAM, 0);
  int opt = 1;
  setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  set_nonblock(lfd);

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);
  bind(lfd, (struct sockaddr *)&addr, sizeof(addr));
  listen(lfd, 128);

  int epfd = epoll_create1(0);
  struct epoll_event ev = {.events = EPOLLIN, .data.fd = lfd};
  epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &ev);

  struct epoll_event events[64];
  char buf[4096];
  printf("[epoll] Echo server on port %d (Reactor)\n", port);
  fflush(stdout);

  while (1) {
    int n = epoll_wait(epfd, events, 64, -1);   // 系统调用
    for (int i = 0; i < n; i++) {
      int fd = events[i].data.fd;
      if (fd == lfd) {
        int cfd = accept(lfd, NULL, NULL);       // 系统调用
        set_nonblock(cfd);
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = cfd;
        epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &ev); // 系统调用
      } else {
        int r = read(fd, buf, sizeof(buf));       // 系统调用（同步数据搬运）
        if (r > 0) {
          if (write(fd, buf, r) < 0) {            // 系统调用（同步数据搬运）
            perror("write");
          }
        } else {
          epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
          close(fd);
        }
      }
    }
  }
  return 0;
}
