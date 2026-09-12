# io-study —— Linux I/O 模型实验项目

从 `select/poll/epoll` 到 `io_uring`，从 Reactor 到 Proactor，从事件回调到协程 —— 一个**能编译、能跑、能压测**的完整实验集合。

所有代码均经过实际编译运行验证，文中所有性能数据都来自真实测量，而非臆测。

> **关于路径**：项目内所有 Makefile 与 Shell 脚本都使用**相对路径**（基于脚本/文件自身位置推导），
> 整个目录可以拷到任何机器、任何位置直接运行，不需要修改任何路径。

---

## 快速开始（3 分钟）

```bash
tar xzf io-study.tar.gz && cd io-study

bash scripts/check_env.sh    # ① 环境体检（重点看 io_uring 是否可用）
make                         # ② 编译全部
make bench                   # ③ 磁盘 I/O 对比（sync vs libaio vs io_uring）
make bench DIRECT=1          # ④ 再跑一次 O_DIRECT，看真实磁盘差距
make bench_demo              # ⑤ epoll 为什么必须 O_NONBLOCK
```

或者一步到位：

```bash
bash scripts/run_all_bench.sh        # 跑全部实验，结果存到 results/
```

详细的分步说明与预期结果，见 **[运行指引.md](./运行指引.md)**。

---

## 目录结构

```
io-study/
├── Makefile              统一构建入口（根目录）
├── README.md             本文件
├── 运行指引.md            服务器分步运行指引（含预期结果对照表）
│
├── docs/
│   ├── html/             三份分析文档（HTML 版，浏览器打开，含图表）
│   └── md/               三份分析文档（Markdown 版，便于检索/版本管理）
│
├── network/              网络 I/O
│   ├── reactor_server.c      Reactor 模式 echo server（ET + 非阻塞）
│   ├── echo_epoll.c          epoll echo server（用于 strace / 压测对比）
│   ├── echo_io_uring.c       io_uring echo server（Proactor 版）
│   ├── bench_client.cpp      多线程 C++ 压测客户端
│   ├── bench.py              Python 压测客户端（推荐）
│   └── strace_epoll实测.txt   epoll 处理 1 条消息的真实系统调用序列
│
├── disk/                 磁盘 I/O + epoll 验证实验
│   ├── io_common.h           公共工具（计时、对齐内存、开文件、结果打印）
│   ├── io_sync.c             同步 pread/pwrite 基线
│   ├── io_libaio.c           libaio 版本（io_setup/io_submit/io_getevents）
│   ├── io_uring_disk.c       io_uring 版本（SQ/CQ 环形队列）
│   ├── epoll_nonblock_demo.c 实验1：单连接 ET+阻塞 vs ET+非阻塞
│   └── epoll_starve_demo.c   实验2：双连接，证明阻塞 fd 饿死其他连接
│
├── libco/                协程
│   ├── bench_swap.cpp        协程上下文切换开销基准
│   └── compare.sh            epoll vs libco vs io_uring 三方对比
│
├── scripts/
│   ├── check_env.sh          环境体检（内核/依赖/io_uring 可用性）
│   └── run_all_bench.sh      一键跑通全部实验
│
└── third_party/          libco 源码 clone 位置（可选，make libco 时用）
```

---

## Makefile 用法

在项目根目录执行：

| 命令 | 作用 |
| --- | --- |
| `make` / `make all` | 编译全部（磁盘 + 网络 + O_NONBLOCK 实验） |
| `make disk` | 只编译磁盘 I/O 三版 |
| `make net` | 只编译网络（echo_epoll / echo_io_uring / reactor_server / bench_client） |
| `make demos` | 只编译 epoll O_NONBLOCK 验证 demo |
| `make libco` | 编译 libco 与 `bench_swap`（需先 clone 到 `third_party/libco`） |
| `make check` | 环境体检（等价于 `scripts/check_env.sh` 的核心项） |
| `make bench` | 磁盘 I/O 三方对比 |
| `make bench_demo` | epoll O_NONBLOCK 对比实验 |
| `make bench_net` | 网络 echo 对比（epoll vs io_uring） |
| `make clean` | 清理 `bin/` 与测试文件 |
| `make help` | 查看全部目标与参数 |

### 可调参数

```bash
make bench TOTAL=65536 BLOCK=4096 DEPTH=32 DIRECT=1 FILE=/tmp/testfile
make bench_net PORT=18800 THREADS=8 SIZE=512 SECS=5
```

| 参数 | 含义 | 默认值 |
| --- | --- | --- |
| `FILE` | 测试文件路径 | `/tmp/iodemo_testfile` |
| `BLOCK` | 单次 I/O 字节数 | 4096 |
| `TOTAL` | I/O 总次数 | 65536 |
| `DEPTH` | 异步队列深度 | 32 |
| `DIRECT` | 是否 `O_DIRECT`（1=绕过 page cache） | 0 |
| `PORT` | 网络 bench 端口 | 18800 |
| `THREADS` | 压测客户端线程数 | 4 |
| `SIZE` | 单条消息字节数 | 256 |
| `SECS` | 压测时长（秒） | 5 |

---

## 依赖

```bash
# Debian / Ubuntu
sudo apt-get install -y build-essential libaio-dev liburing-dev strace

# RHEL / CentOS / Rocky
sudo yum install -y gcc gcc-c++ make libaio-devel liburing-devel strace
```

- **libaio**：磁盘 libaio 实验需要
- **liburing**：io_uring 实验需要（只装运行时也能跑，开发包用于编译）
- **strace**：系统调用对比实验需要
- **Python 3**：`network/bench.py` 压测需要

> io_uring 还需要 **Linux 5.1+**（建议 5.10+），且不被 seccomp 禁用。
> 容器环境默认 seccomp 会拦截 `io_uring_setup`，需要 `--security-opt seccomp=unconfined`。

---

## 三组实验分别回答什么

### 实验一：磁盘 I/O（sync vs libaio vs io_uring）

| 模式 | 结论 |
| --- | --- |
| buffered（`DIRECT=0`） | 走 page cache，本质是内存拷贝，**异步 I/O 优势不明显，libaio 甚至更慢** |
| O_DIRECT（`DIRECT=1`） | 真落盘，异步 I/O 靠并发深度换吞吐，实测 **libaio 15,791 IOPS vs 同步 572 IOPS（约 27 倍）** |

> io_uring 理论上应 ≥ libaio（更少系统调用 + mmap 零拷贝 + 支持更多操作类型）。

### 实验二：epoll 为什么必须 `O_NONBLOCK`

| 实验 | 非阻塞 | 阻塞 |
| --- | --- | --- |
| 单连接（ET） | ~300 ms | ~5300 ms（`read` 卡到对端关闭） |
| 双连接 | ~350 ms | **~3350 ms**（第 2 个连接被饿死 3 秒） |

结论：ET 模式下非阻塞 fd 是**硬性要求**（一次没读完剩余数据不会再触发）；
即便 LT 模式，一个阻塞 `read` 也会挂起整个事件循环，让所有其他连接挨饿。

### 实验三：Reactor vs Proactor 的系统调用开销

用 `strace` 实测 epoll 处理 **1 条消息**触发了 **约 11 次系统调用**：

```
4× epoll_wait + 1× accept + 2× read + 1× write + 3× epoll_ctl
```

io_uring 通过批量提交，普通模式可降到 1 次 `io_uring_enter`，**SQPOLL 模式 0 次**。
这就是 Proactor「内核代办 I/O」相对 Reactor「应用自己 I/O」的核心收益。

---

## 文档

| 文档 | HTML | Markdown |
| --- | --- | --- |
| Socket 通信与 epoll 多路复用深度解析 | [html](./docs/html/socket_epoll_深度解析.html) | [md](./docs/md/01-socket-epoll-深度解析.md) |
| Reactor 与 Proactor：命名渊源、io_uring 详解 | [html](./docs/html/reactor_proactor_io_uring详解.html) | [md](./docs/md/02-reactor-proactor-io_uring.md) |
| libco 如何使用 epoll（源码 + 汇编级分析） | [html](./docs/html/libco_epoll源码分析.html) | [md](./docs/md/03-libco-epoll-源码分析.md) |

阅读顺序建议：**01 → 02 → 03**，即从「epoll 怎么用」到「为什么这么设计」再到「协程如何封装 epoll」。

---

## 核心知识速查

| 概念 | 一句话 |
| --- | --- |
| **Reactor** | 就绪通知，应用自己 `read`/`write`；`epoll` 是典型代表 |
| **Proactor** | 完成通知，内核代办 I/O；`io_uring` 是典型代表 |
| **LT vs ET** | LT 反复通知（安全），ET 只通知一次（快，必须配非阻塞 + 循环读到 EAGAIN） |
| **epoll 快的本质** | 红黑树存 fd + 就绪链表 + 回调机制，开销只与**活跃连接数**相关 |
| **io_uring 快的本质** | SQ/CQ 双环形队列 mmap 共享，批量提交，SQPOLL 下 0 系统调用 |
| **O_DIRECT** | 绕过 page cache，需 512 字节对齐，是测真实磁盘能力的前提 |
| **协程** | 把 epoll 藏在同步系统调用后面（hook + yield/resume），切换仅 ~15 ns |

---

## 已知限制

- **io_uring 在部分容器/沙箱环境不可用**：seccomp 默认拦截 `io_uring_setup`（返回 `EPERM`）。
  代码可正常编译，运行时会提示 `[环境限制]` 并返回退出码 2。跑 `make check` 可确认。
- **libaio 在 buffered 模式下会退化**：这不是 bug，而是 page cache 让异步开销得不偿失 —— 这本身就是一个值得观察的实验结论。
- **libco 需自行 clone**：`make libco` 前需 `git clone https://github.com/Tencent/libco third_party/libco`。

---

## 数据来源说明

文档中的所有性能数据（QPS、IOPS、ns/次、系统调用次数）均来自实验环境实际测量，
**绝对值会因 CPU、磁盘类型、内核版本、容器限制而显著不同**，请以自己的实测结果为准。
可复现的**相对关系**（如 O_DIRECT 下异步 >> 同步、epoll ~11 次系统调用/消息）才是本项目的核心结论。
