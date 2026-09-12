// ============================================================================
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
// ── 为什么是「每个 worker 一个 listen fd」，而不是「共用一个 + 加锁」───────
// 共用 listen fd 的常见做法有三种，都有代价：
//   1) 多线程都 epoll_wait 同一个 fd → 惊群：一次连接把 N 个线程全叫醒，只有一个 accept 到；
//   2) 加一把 accept 锁 → 所有 worker 在连接建立时串行化，高连接数下锁竞争明显；
//   3) EPOLLEXCLUSIVE → 只唤醒一个线程，但仍要跨线程竞争同一个 accept 队列。
// SO_REUSEPORT 把这件事交给内核：同一端口上挂 N 个独立 socket，
// 每个各有自己的 accept 队列，内核按连接的四元组（源IP/源端口/目的IP/目的端口）哈希分流。
// 结果：连接从建立那一刻就归属某个 worker，之后全程无锁。
//
// 代价：worker 之间负载可能不均（哈希是固定的，连接少的 worker 就闲着），
//       而且没有"跨 worker 迁移连接"的能力 —— 生产框架通常会做 rebalance。
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
// ============================================================================

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

// 传给每个 worker 线程的参数（每个 worker 一份，所以必须独立 malloc）
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
//
// 【为什么必须处理「写不完」】这是新手最容易忽略的一点：
//   write 返回的字节数**可以小于 len**（内核发送缓冲区满了），甚至会返回 EAGAIN。
//   更坑的是：读完一条消息后，如果不把这条消息完整写回去，
//   客户端收到的就是**被截断的半条**，而且因为 TCP 是字节流，它还无从察觉。
//   所以"读完就 write(fd, buf, r)"这种写法在压力下是会错的，必须循环写。
//
// 这里的处理策略：EAGAIN → poll 等可写（最多 100ms 一轮），累计重试 100 次。
// 生产框架更常用的做法是把"待发送数据"挂到连接上、注册 EPOLLOUT，
// 由事件循环驱动继续写 —— 那样不会阻塞当前这个 worker 的其它连接。
// 本实现为了保持 demo 简洁，选择了阻塞式等待。
static int write_all(int fd, const char *buf, int len) {
  int off = 0, retries = 0;
  while (off < len) {
    ssize_t n = write(fd, buf + off, len - off);
    if (n > 0) {
      off += (int)n;      // 只写出了一部分，继续写剩下的
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      if (++retries > WRITE_RETRY) return -1;   // 对端迟迟不可写，放弃这条连接
      struct pollfd pfd;
      pfd.fd = fd;
      pfd.events = POLLOUT;
      pfd.revents = 0;
      poll(&pfd, 1, 100);   // 等可写，最多 100ms
      continue;
    }
    if (n < 0 && errno == EINTR) continue;      // 被信号打断，重来即可
    return -1;                                   // 真错误（对端 RST 等）
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
  // SO_REUSEADDR 与 SO_REUSEPORT 是两件事，且必须**都**设：
  //   SO_REUSEADDR 解决 TIME_WAIT 的 bind 冲突；
  //   SO_REUSEPORT 才允许多个 socket 绑同一端口并做内核分流。
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_REUSEPORT
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
    perror("setsockopt(SO_REUSEPORT)");
  }
#else
  fprintf(stderr, "[警告] 该平台无 SO_REUSEPORT，多 worker 将无法并行 accept\n");
#endif
  // 监听 fd 设非阻塞：下面第 179 行那个 accept 循环靠 EAGAIN 来判断"取空了"
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
//
// 【ET 为什么必须读空】ET 只在"状态变化"时通知一次：数据从无到有 → 通知。
// 如果一次只 read 一次就走人，缓冲区里剩下的数据不会再有通知，
// 这条连接就会永远卡住（除非对端又发来新数据，才可能再次触发通知）。
// 循环读到 EAGAIN 是 ET 的唯一正确姿势。
//
// 返回值/退出语义：函数"返回"表示保留连接（本轮读空），
// 走到函数末尾并 close(fd) 表示这条连接结束了。
static void serve_one(int epfd, int fd, char *buf) {
  for (;;) {
    ssize_t r = read(fd, buf, BUF_SIZE);
    if (r > 0) {
      // 收到的数据在这一轮里同步写完再继续读 —— 单条消息 ping-pong 够用，
      // 但注意这会让 worker 在慢客户端上被拖住（详见 write_all 的注释）。
      if (write_all(fd, buf, (int)r) < 0) break; // 对端不可写，关闭连接
    } else if (r == 0) {
      break; // 对端关闭（EOF）
    } else {
      if (errno == EAGAIN || errno == EWOULDBLOCK) return; // 本轮读空，保留连接
      if (errno == EINTR) continue;
      break; // 其他错误（ECONNRESET 等）
    }
  }
  // 显式 DEL：close() 本身也会把 fd 从 epoll 里摘掉，但显式删除意图更清楚，
  // 而且在"close 之后 fd 号被复用"的场景下能避免误删新连接的注册。
  epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
  close(fd);
}

static void *worker(void *arg) {
  worker_arg_t wa = *(worker_arg_t *)arg;   // 先拷贝出来（下面要 free）
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

  // 每个 worker 一块缓冲区，而不是每条连接一块。
  // 安全性来自 serve_one 的处理方式：读到数据后**立刻同步写完**才回到循环，
  // 所以同一个 worker 的任意两个连接不会同时占用这块内存。
  // 一旦改成"挂起来异步写"，就必须改成每条连接独立缓冲区。
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
    // -1 = 一直等。注意这个 worker 只关心自己 accept 到的连接，
    // 别的 worker 的连接不会出现在它的 epoll 里（SO_REUSEPORT 已分流）。
    int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
    if (n < 0) {
      if (errno == EINTR) continue;   // 被信号打断，重新等
      perror("epoll_wait");
      break;
    }
    for (int i = 0; i < n; i++) {
      int fd = events[i].data.fd;
      if (fd == lfd) {
        // 一次把 accept 队列取空（ET 必需；LT 下也无害）
        // ET 下如果只 accept 一个，剩下已排队的连接就收不到通知了。
        for (;;) {
          int cfd = accept(lfd, NULL, NULL);
          if (cfd < 0) break; // EAGAIN 或错误，都退出本轮
          set_nonblock(cfd);  // 非阻塞：ET 下必须，否则 serve_one 的读循环会卡死
          if (wa.nodelay) set_nodelay(cfd);
          ev.events = EPOLLIN | (wa.lt ? 0 : EPOLLET);
          ev.data.fd = cfd;
          if (epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &ev) < 0) close(cfd);
        }
        continue;
      }
      // 只处理可读事件；其他事件（如 EPOLLHUP/ERR）这里忽略，
      // 真正的错误会在下一次 read 时以返回值的形式暴露出来。
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
  // 默认按 CPU 核数开 worker —— 多 Reactor 的价值就是把多个核用起来，
  // 开超过核数只会引入调度开销（压测里也能看到：worker 数 > 核数时曲线走平甚至下降）。
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
    // 每个线程一份参数：不能传栈上变量的地址（线程启动时机不确定，会踩已释放的栈）
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

  // 主线程只负责等 worker 结束（本程序里 worker 是死循环，等于一直挂着）
  for (int i = 0; i < started; i++) pthread_join(tids[i], NULL);
  free(tids);
  return 0;
}
