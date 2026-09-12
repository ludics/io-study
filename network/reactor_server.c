#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

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
  ev.events = EPOLLIN;
  ev.data.fd = lfd;
  epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &ev);

  struct epoll_event all[1024];
  while (1) {
    int ret = epoll_wait(epfd, all, sizeof(all) / sizeof(all[0]), -1);
    for (int i = 0; i < ret; ++i) {
      int fd = all[i].data.fd;
      if (fd == lfd) {
        int cfd = accept(lfd, (struct sockaddr *)&client_addr, &cli_len);
        if (cfd == -1) {
          perror("> Accept Error");
          exit(1);
        }
        int flag = fcntl(cfd, F_GETFL);
        flag |= O_NONBLOCK;
        fcntl(cfd, F_SETFL, flag);
        struct epoll_event temp;
        temp.events = EPOLLIN | EPOLLET;
        temp.data.fd = cfd;
        epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &temp);
        char ip[64] = {0};
        printf("> New Client [%s:%d] => [%d]\n",
               inet_ntop(AF_INET, &client_addr.sin_addr.s_addr, ip, sizeof(ip)),
               ntohs(client_addr.sin_port), cfd);
      } else {
        if (!(all[i].events & EPOLLIN)) {
          continue;
        }
        char buffer[5] = {0};
        int len;
        while ((len = read(fd, buffer, sizeof(buffer))) > 0) {
          if (write(fd, buffer, len) < 0) {
            perror("> Send Error");
            break;
          }
        }
        if (len == -1) {
          if (errno == EAGAIN) {
            // Buffer Data is Finished!;
          } else {
            perror("> Recv Error");
            exit(1);
          }
        }
        if (len == 0) {
          printf("> Client [%d] Disconnected!\n", fd);
          epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
          close(fd);
        }
      }
    }
  }

  close(lfd);
  return 0;
}
