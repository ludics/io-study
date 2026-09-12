// 多线程 epoll 版 Echo Server（多 Reactor 模式，SO_REUSEPORT 分流）
//
// 与 echo_epoll.c（单线程 Reactor）对照，回答「同一个 epoll 模型，加线程后能快多少」：
//
//   单线程 Reactor : 1 个 listen fd + 1 个 epoll + 1 个事件循环
//                    → 所有连接的 read/write 都挤在同一个 CPU 上
//   多线程 Reactor : N 个 worker，每个 worker 自己 socket()+SO_REUSEPORT 一份 listen fd，
//                    各自持有独立 epoll 与事件循环
//                    → 内核在 bind 了同一端口的多个 listen fd 之间按四元组哈希分流，
//                      连接从 accept 那一刻就固定归属某个 worker，天然免锁、免惊群
//
// 说明：SO_REUSEPORT 需要 Linux 3.9+。若内核不支持，会退化为「只有第一个 worker 抢到端口」
//       —— 此时程序仍能跑，但只有 1 个核在干活（输出里能看到只有一个 worker 打印就绪）。
//
// 环境变量：
//   ECHO_NODELAY=1|0   是否对连接设置 TCP_NODELAY（默认 1，压测小包的推荐值）
//   ECHO_LT=1|0        1 = 电平触发(LT)，0 = 边沿触发(ET)（默认 0）—— 用于对比 LT/ET
//
// 编译: gcc -O2 -Wall -D_GNU_SOURCE -D_REENTRANT -o echo_epoll_mt echo_epoll_mt.c -lpthread
// 运行: ./echo_epoll_mt [port] [线程数]      # 线程数默认 = CPU 核数
// 测试: printf 'hello' | nc 127.0.0.1 [port]

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_EVENTS 1024
#define BUF_SIZE 4096
#define WRITE_RETRY 100 // 写遇 EAGAIN 时最多重试次数（每次最多 poll 100ms）

typedef struct {
  int port;
  int nodelay;
  int lt; // 1 = LT，0 = ET
  long id;
} worker_arg_t;

static void set_nonblock(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void set_nodelay(int fd) {
  int opt = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
}

// 循环写完 len 字节（非阻塞 fd：遇 EAGAIN 用 poll 等待可写，最多重试 WRITE_RETRY 次）
static int write_all(int fd, const char *buf, int len) {
  int off = 0, retries = 0;
  while (off < len) {
    ssize_t n = write(fd, buf + off, len - off);
    if (n > 0) {
      off += (int)n;
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      if (++retries > WRITE_RETRY) return -1;
      struct pollfd pfd;
      pfd.fd = fd;
      pfd.events = POLLOUT;
      pfd.revents = 0;
      poll(&pfd, 1, 100);
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    return -1;
  }
  return 0;
}

// 每个 worker 自己创建一份 listen fd（SO_REUSEPORT 让内核做连接分流）
static int make_listen_fd(int port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    perror("socket");
    return -1;
  }
  int opt = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_REUSEPORT
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
    perror("setsockopt(SO_REUSEPORT)");
  }
#else
  fprintf(stderr, "[警告] 该平台无 SO_REUSEPORT，多 worker 将无法并行 accept\n");
#endif
  set_nonblock(fd);

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);
  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    perror("bind");
    close(fd);
    return -1;
  }
  if (listen(fd, 512) < 0) {
    perror("listen");
    close(fd);
    return -1;
  }
  return fd;
}

// 处理一个已就绪的连接：循环读到 EAGAIN（ET 的硬性要求，LT 下同样正确）
static void serve_one(int epfd, int fd, char *buf) {
  for (;;) {
    ssize_t r = read(fd, buf, BUF_SIZE);
    if (r > 0) {
      if (write_all(fd, buf, (int)r) < 0) break; // 对端不可写，关闭连接
    } else if (r == 0) {
      break; // 对端关闭
    } else {
      if (errno == EAGAIN || errno == EWOULDBLOCK) return; // 本轮读空，保留连接
      if (errno == EINTR) continue;
      break;
    }
  }
  epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
  close(fd);
}

static void *worker(void *arg) {
  worker_arg_t wa = *(worker_arg_t *)arg;
  free(arg);

  int lfd = make_listen_fd(wa.port);
  if (lfd < 0) return NULL;

  int epfd = epoll_create1(0);
  if (epfd < 0) {
    perror("epoll_create1");
    close(lfd);
    return NULL;
  }

  struct epoll_event ev;
  ev.events = EPOLLIN;
  ev.data.fd = lfd;
  epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &ev);

  char *buf = malloc(BUF_SIZE);
  if (!buf) {
    close(lfd);
    close(epfd);
    return NULL;
  }

  struct epoll_event events[MAX_EVENTS];
  printf("[epoll_mt#%ld] 就绪 port=%d 模式=%s nodelay=%d\n", wa.id, wa.port,
         wa.lt ? "LT" : "ET", wa.nodelay);
  fflush(stdout);

  while (1) {
    int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
    if (n < 0) {
      if (errno == EINTR) continue;
      perror("epoll_wait");
      break;
    }
    for (int i = 0; i < n; i++) {
      int fd = events[i].data.fd;
      if (fd == lfd) {
        // 一次把 accept 队列取空（ET 必需；LT 下也无害）
        for (;;) {
          int cfd = accept(lfd, NULL, NULL);
          if (cfd < 0) break; // EAGAIN 或错误，都退出本轮
          set_nonblock(cfd);
          if (wa.nodelay) set_nodelay(cfd);
          ev.events = EPOLLIN | (wa.lt ? 0 : EPOLLET);
          ev.data.fd = cfd;
          if (epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &ev) < 0) close(cfd);
        }
        continue;
      }
      if (!(events[i].events & EPOLLIN)) continue;
      serve_one(epfd, fd, buf);
    }
  }

  free(buf);
  close(lfd);
  close(epfd);
  return NULL;
}

int main(int argc, char *argv[]) {
  int port = argc > 1 ? atoi(argv[1]) : 19002;
  int nthreads = argc > 2 ? atoi(argv[2]) : 0;
  if (nthreads <= 0) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    nthreads = (n > 0) ? (int)n : 1;
  }

  const char *e = getenv("ECHO_NODELAY");
  int nodelay = e ? atoi(e) : 1;
  e = getenv("ECHO_LT");
  int lt = e ? atoi(e) : 0;

  printf("[epoll_mt] Echo server on port %d (多 Reactor, %d worker, %s)\n", port,
         nthreads, lt ? "LT" : "ET");
  fflush(stdout);

  pthread_t *tids = (pthread_t *)calloc((size_t)nthreads, sizeof(pthread_t));
  if (!tids) {
    perror("calloc");
    return 1;
  }

  int started = 0;
  for (long i = 0; i < nthreads; i++) {
    worker_arg_t *wa = (worker_arg_t *)malloc(sizeof(worker_arg_t));
    if (!wa) break;
    wa->port = port;
    wa->nodelay = nodelay;
    wa->lt = lt;
    wa->id = i;
    if (pthread_create(&tids[i], NULL, worker, wa) != 0) {
      perror("pthread_create");
      free(wa);
      break;
    }
    started++;
  }

  if (started == 0) {
    free(tids);
    fprintf(stderr, "[错误] 没有任何 worker 启动成功\n");
    return 1;
  }

  for (int i = 0; i < started; i++) pthread_join(tids[i], NULL);
  free(tids);
  return 0;
}
