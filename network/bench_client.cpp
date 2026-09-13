// 多线程 echo 压测客户端（C++，无 GIL）
//
// 用法:
//   ./bench_client [--host <地址>] <端口> [线程数] [消息字节数]
//
//     --host   对端地址，默认 127.0.0.1。
//              跨机器压测时用得上 —— 例如「客户端在 macOS 上、服务端在虚拟机里」：
//                ./bench_client --host 192.168.252.3 19000 8 256
//              注意 --host 必须写在最前面（位置参数保持向后兼容）。
//     端口     必填
//     线程数   默认 1
//     消息字节 默认 100
//
// 输出: 每秒一行 `QPS: <n>`（行缓冲，被 kill 也不会丢最后几行）
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

#define ZERO_OR_RETURN(expr)            \
  do {                                  \
    int ret = (expr);                   \
    if (ret != 0) {                     \
      printf("%d %d\n", __LINE__, ret); \
      return ret;                       \
    }                                   \
  } while (0);

// 连接失败时说清楚「连谁失败了、为什么」——
// 原来只打印 "行号 返回值"（如 "62 -1"），排错时完全看不出是端口没监听还是地址不可达。
static void die_connect(const char *host, int port) {
  fprintf(stderr, "连接 %s:%d 失败: %s\n", host, port, strerror(errno));
  fprintf(stderr, "  排查: 服务端监听了吗（ss -ltn / netstat）？地址可达吗？\n");
}


std::atomic<bool> quit{false};
std::atomic<int> alive{0};

void do_quit(int) {
  quit = true;
  while (alive > 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
  }
  exit(0);
}

int main(int argc, char *argv[]) {
  // 关掉 stdout 的全缓冲：被压测脚本用管道接住时，printf 默认按 4KB 缓冲，
  // 如果脚本到点直接 kill 掉本进程，缓冲区里还没刷出的 QPS 行就全丢了
  // （表现为「跑了 5 秒一行输出都没有」）。设成行缓冲后每行立刻可见。
  setvbuf(stdout, nullptr, _IOLBF, 0);

  signal(SIGINT, do_quit);

  // 参数：[--host <地址>] <端口> [线程数] [消息字节数]
  int argi = 1;
  std::string host = "127.0.0.1";
  if (argc >= 3 && std::string(argv[1]) == "--host") {
    host = argv[2];
    argi = 3;
  }
  if (argc <= argi) {
    fprintf(stderr, "用法: %s [--host <地址>] <端口> [线程数] [消息字节数]\n", argv[0]);
    return 1;
  }
  int port = std::atoi(argv[argi]);
  int thread = (argc > argi + 1) ? std::atoi(argv[argi + 1]) : 1;
  int data_size = (argc > argi + 2) ? std::atoi(argv[argi + 2]) : 100;

  // 先把地址解析出来（用 getaddrinfo，IP 和域名都支持），再开线程 —— 
  // 避免每个线程各解析一次。
  struct addrinfo hints;
  struct addrinfo *res = nullptr;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  char portstr[16];
  snprintf(portstr, sizeof(portstr), "%d", port);
  int gerr = getaddrinfo(host.c_str(), portstr, &hints, &res);
  if (gerr != 0 || !res) {
    fprintf(stderr, "解析地址 %s:%d 失败: %s\n", host.c_str(), port, gai_strerror(gerr));
    return 1;
  }
  struct sockaddr_in cli_addr;
  memcpy(&cli_addr, res->ai_addr, sizeof(cli_addr));
  freeaddrinfo(res);
  printf("目标: %s:%d  线程=%d  消息=%dB\n", host.c_str(), port, thread, data_size);
  std::string data(data_size, '\0');
  srand(time(0));
  for (char &ch : data) {
    ch = rand() % 26 + 'A';
  }

  std::atomic<int> qps{0};
  for (int i = 0; i < thread; ++i) {
    std::thread([&] {
      ++alive;
      socklen_t cli_len = sizeof(cli_addr);
      std::string buffer(data);

      int sock = socket(AF_INET, SOCK_STREAM, 0);
      if (connect(sock, (struct sockaddr *)&cli_addr, cli_len) != 0) {
        die_connect(host.c_str(), port);
        close(sock);
        --alive;
        return 1;
      }
      while (!quit) {
        size_t writen = 0;
        while (writen < data.size()) {
          ssize_t w = write(sock, &data[writen], data.size() - writen);
          if (w <= 0) {
            // 写失败(r==0)或出错(r<0)：跳出，避免 writen 倒退导致越界
            break;
          }
          writen += w;
        }
        if (writen != data.size()) {
          break;
        }

        size_t readed = 0;
        while (readed < data.size()) {
          ssize_t r = read(sock, &buffer[readed], data.size() - readed);
          if (r <= 0) {
            // 连接被关闭(r==0)或出错(r<0)：标记失败并跳出，避免死循环/越界
            readed = 0;
            break;
          }
          readed += r;
        }
        if (readed == data.size() && data == buffer) {
          qps += 1;
        }
      }

      ZERO_OR_RETURN(close(sock));
      --alive;
      return 0;
    }).detach();
  }

  while (true) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    printf("QPS: %d\n", qps.load());
    qps = 0;
  }
}
