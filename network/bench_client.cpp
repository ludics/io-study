#include <arpa/inet.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
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
  signal(SIGINT, do_quit);

  int port = argc >= 2 ? std::atoi(argv[1]) : 8000;
  int thread = argc >= 3 ? std::atoi(argv[2]) : 1;
  int data_size = argc >= 4 ? std::atoi(argv[3]) : 100;
  std::string data(data_size, '\0');
  srand(time(0));
  for (char &ch : data) {
    ch = rand() % 26 + 'A';
  }

  std::atomic<int> qps{0};
  for (int i = 0; i < thread; ++i) {
    std::thread([&] {
      ++alive;
      struct sockaddr_in cli_addr;
      socklen_t cli_len = sizeof(cli_addr);
      memset(&cli_addr, 0, sizeof(cli_addr));
      cli_addr.sin_family = AF_INET;
      cli_addr.sin_addr.s_addr = htonl(INADDR_ANY);
      cli_addr.sin_port = htons(port);
      std::string buffer(data);

      int sock = socket(AF_INET, SOCK_STREAM, 0);
      ZERO_OR_RETURN(connect(sock, (struct sockaddr *)&cli_addr, cli_len));
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
