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
make bench_disk_matrix       # ⑥ 磁盘 I/O 多维度对比（深度/块大小/O_DIRECT）
make bench_net_matrix        # ⑦ 网络 I/O 多维度对比（连接数/消息大小/NODELAY）
```

或者一步到位：

```bash
bash scripts/run_all_bench.sh        # 跑全部实验，结果存到 results/
```

详细的分步说明与预期结果，见 **[Instruction.md](./Instruction.md)**（服务器分步运行指引）。

---

## 目录结构

```
io-study/
├── Makefile              统一构建入口（根目录）
├── README.md             本文件
├── Instruction.md        服务器分步运行指引（含预期结果对照表）
│
├── docs/
│   ├── html/             三份分析文档（HTML 版，浏览器打开，含图表）
│   └── md/               三份分析文档（Markdown 版，便于检索/版本管理）
│
├── network/              网络 I/O
│   ├── reactor_server.c      Reactor 模式 echo server（ET + 非阻塞）
│   ├── echo_epoll.c          epoll echo server，单线程 Reactor（用于 strace / 压测对比）
│   ├── echo_epoll_mt.c       多线程 Reactor（SO_REUSEPORT 分流，N worker 各自 epoll）
│   ├── echo_io_uring.c       io_uring echo server（Proactor 版）
│   ├── bench_client.cpp      多线程 C++ 压测客户端
│   ├── bench.py              Python 压测客户端（推荐；支持连接数/预热/延迟分位/JSON）
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
│   ├── run_all_bench.sh      一键跑通全部实验
│   ├── uring_probe.c         io_uring 可用性探针（会翻译 errno）
│   └── bench_matrix.py       矩阵压测工具（网络 + 磁盘，纯 Python，产出 results/ 报表）
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
| `make bench_net` | 网络 echo 对比（epoll vs io_uring，单场景） |
| `make bench_disk_matrix` | 磁盘 I/O 多维度对比（队列深度 × 块大小 × O_DIRECT），产出 `results/*.md` + `.csv` |
| `make bench_net_matrix` | 网络 I/O 多维度对比（服务端 × 连接数 × 消息大小 × TCP_NODELAY），产出 `results/*.md` + `.csv` |
| `make clean` | 清理 `bin/` 与测试文件 |
| `make help` | 查看全部目标与参数 |

### 可调参数

```bash
make bench TOTAL=65536 BLOCK=4096 DEPTH=32 DIRECT=1 FILE=/tmp/testfile
make bench_net PORT=18800 THREADS=8 SIZE=512 SECS=5
make bench_net_matrix MODE=full        # quick（默认）| full
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
| `MODE` | `bench_net_matrix` 模式（quick / full） | quick |

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

## 四组实验分别回答什么

### 实验一：磁盘 I/O（sync vs libaio vs io_uring）

| 模式 | 结论 |
| --- | --- |
| buffered（`DIRECT=0`） | 走 page cache，本质是内存拷贝，**异步 I/O 优势不明显，libaio 甚至更慢** |
| O_DIRECT（`DIRECT=1`） | 真落盘，异步 I/O 靠并发深度换吞吐，实测 **libaio 15,791 IOPS vs 同步 572 IOPS（约 27 倍）** |

> io_uring 理论上应 ≥ libaio（更少系统调用 + mmap 零拷贝 + 支持更多操作类型）。

#### 实验一 · 扩展：磁盘 I/O 多维度对比（`make bench_disk_matrix`）

`make bench` 只跑一组参数，看不出「什么条件下谁更快」。矩阵实验沿三个维度扫描，
每个维度都横跨同步 / libaio / io_uring：

| 维度 | 扫描范围 | 想回答的问题 |
| --- | --- | --- |
| **A. 队列深度** | 1 → 128 | 「异步靠并发深度换吞吐」的收益边界在哪？ |
| **B. 块大小** | 512B → 256KB | IOPS 与带宽如何取舍？ |
| **C. O_DIRECT** | 开 / 关 | page cache 到底掩盖了多少真实磁盘行为？ |

```bash
make bench_disk_matrix                 # quick
make bench_disk_matrix MODE=full       # 更细的扫点
python3 scripts/bench_matrix.py disk --mode full --bytes 268435456 --file /data/iodemo
```

产物与网络矩阵一致（`results/disk_matrix_*.md` / `.csv` / `.log`）。

**实测一组**（4KB / O_DIRECT / 64MB I/O 量，Ubuntu 22.04 aarch64 虚拟机）：

| 写 IOPS | depth=1 | depth=2 | depth=8 | depth=32 |
| --- | --- | --- | --- | --- |
| 同步 (pread/pwrite) | 8.2k | 8.4k | 8.6k | 7.8k |
| libaio | 8.3k | 16.7k | 64.4k | **213.3k** |
| io_uring | 8.6k | 16.6k | 39.0k | **103.6k** |

三个结论：

1. **同步 I/O 完全不吃深度**（8k 一路平），异步则近乎线性放大 —— libaio 在 depth=32 拿到
   **27.4x**，这就是「用并发深度换吞吐」。
2. **io_uring 只爬到 libaio 的一半**（103.6k vs 213.3k）。原因不是 API，而是内核把
   **O_DIRECT 写甩给了 io-wq 工作线程池**（`ps -o comm= -L -p <pid>` 能看到一堆
   `iou-wrk-*`），每次 I/O 多一次跨线程交接。**读路径不需要 punt，所以 io_uring 读
   （240k）与 libaio（238k）完全持平。**
3. **buffered 模式数字虚高**：同步 buffered 写 1.46M IOPS（5.7 GB/s）vs O_DIRECT 8.4k
   —— 走 page cache 的本质是内存拷贝，这也是 `make bench` 里 libaio 在 buffered 下退化的原因。

> ⚠️ 若测试文件落在**虚拟磁盘 / qcow2 镜像**上，绝对数值几乎不可信（宿主机缓存会吸收写入）。
> 可复现的是「同步 vs 异步」「深度换吞吐」「buffered vs O_DIRECT」这类**相对关系**；
> 要拿真实磁盘能力，请在物理机或直通盘上跑。

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

### 实验四：网络 I/O 多维度对比（`make bench_net_matrix`）

前面的 `bench_net` 只跑了**一个**场景，看不出「什么条件下谁更快」。这个矩阵实验把网络 I/O
沿三个维度扫描，每个维度都横跨全部可用服务端：

| 维度 | 扫描范围 | 想回答的问题 |
| --- | --- | --- |
| **A. 并发连接数** | 1 → 512 | 单线程 Reactor 能扛多少连接？加线程（多 Reactor）值不值？ |
| **B. 消息大小** | 16B → 16KB | 小包场景是系统调用主导，大包场景是内存拷贝主导，拐点在哪？ |
| **C. TCP_NODELAY** | 开 / 关 | Nagle 聚合对小包 RTT 的影响有多大？ |

服务端覆盖四种模型：

| 服务端 | 模型 | 代码 |
| --- | --- | --- |
| `echo_epoll` | 单线程 Reactor（1 个事件循环） | `network/echo_epoll.c` |
| `echo_epoll_mt` | 多 Reactor（N worker + SO_REUSEPORT 分流） | `network/echo_epoll_mt.c` |
| `echo_io_uring` | Proactor | `network/echo_io_uring.c` |
| `example_echosvr` | 协程（libco） | 需先 `make libco` |

三个 C 服务端都支持 `ECHO_NODELAY=1`（对每条新连接设置 TCP_NODELAY）与
`echo_epoll_mt` 的 `ECHO_LT=1`（切到电平触发），矩阵脚本会自动注入这些环境变量。
libco 的示例服务端两者都不支持，因此**不参与维度 C**（避免产出两条几乎相同的数据误导判断）。

跑完自动产出：

```
results/net_matrix_<模式>_<时间戳>.md    对比表（QPS 表 + p99 表 + 自动观察）
results/net_matrix_<模式>_<时间戳>.csv   原始数据，可自行画图
results/net_matrix_<模式>_<时间戳>.log   完整日志
```

想手动跑单个测点：

```bash
# 多线程 Reactor，4 个 worker，64 连接，512B 消息
ECHO_NODELAY=1 ./bin/echo_epoll_mt 19000 4 &
python3 network/bench.py 19000 8 512 5 --conns 64 --warmup 1

# 看 LT 与 ET 的差别
ECHO_LT=1 ./bin/echo_epoll_mt 19001 4 &
python3 network/bench.py 19001 8 512 5 --conns 64 --json
```

压测客户端有两种，`CLIENT` 环境变量切换（默认 `py`）：

| 模式 | 客户端 | 优点 | 缺点 |
| --- | --- | --- | --- |
| `CLIENT=py` | `network/bench.py` | 有延迟分位 p50/p90/p99/p999、有压测端 CPU 占用 | **受 Python GIL 限制，单进程约 1 核到顶** |
| `CLIENT=cpp` | `bin/bench_client` | C++ 多线程无 GIL，能测出服务端真实上限 | 只有 QPS，没有延迟分位 |

```bash
python3 scripts/bench_matrix.py net --client cpp --secs 3 --mt-workers 4
```

`network/bench.py` 相比初版：**连接数与线程数解耦**（`--conns`，每条连接 1 个在途请求，
所以连接数就是并发度）、预热（`--warmup`）、RTT 分位、压测端 CPU 占用、机读输出
（`--json`/`--csv`）。旧的位置参数用法完全兼容。

### 实测一组（aarch64 / Ubuntu 22.04 / 8 vCPU 虚拟机）

64 连接、256B、ping-pong：

| 客户端 | epoll 单线程 | epoll 多线程 (4 worker) | io_uring |
| --- | --- | --- | --- |
| `CLIENT=py` | 17.5k | 21.3k | 23.4k |
| **`CLIENT=cpp`** | **76.6k** | **308.1k** | **73.6k** |

同一台机器、同一批服务端，**只换压测客户端，结论就完全反过来了**。Python 有 GIL，
单进程约 1 核到顶，会把服务端差异整个压平 —— 所以务必以 `CLIENT=cpp` 那一遍为准。

从这个表能读出两件反直觉的事：

1. **多线程 Reactor 接近线性扩展**（4 worker → 4.02x），说明单线程版确实卡在一个核上；
2. **io_uring 并没有更快**（73.6k vs 76.6k，甚至略低）。因为本项目的 io_uring echo 版
   每处理一条消息都要提交一次 SQE 并立刻 `io_uring_enter`，**没吃到批量提交的红利**。
   这反而正面印证了项目的主结论：**io_uring 的优势来自批量提交 / SQPOLL，而不是「换个 API 就更快」**。

> **诚实提示**：单条消息严格 ping-pong 时，单连接的 QPS 上限 ≈ 1/RTT，**连接数**才是提升吞吐的
> 主要手段。读结果的第一步永远是先确认瓶颈在哪一边：看报表「自动观察」里压测端吃了多少核，
> 别把压测端的天花板当成服务端的能力上限。

---

## 文档

| 文档 | HTML | Markdown |
| --- | --- | --- |
| Socket 通信与 epoll 多路复用深度解析 | [html](./docs/html/socket_epoll_深度解析.html) | [md](./docs/md/01-socket-epoll-深度解析.md) |
| Reactor 与 Proactor：命名渊源、io_uring 详解 | [html](./docs/html/reactor_proactor_io_uring详解.html) | [md](./docs/md/02-reactor-proactor-io_uring.md) |
| libco 如何使用 epoll（源码 + 汇编级分析） | [html](./docs/html/libco_epoll源码分析.html) | [md](./docs/md/03-libco-epoll-源码分析.md) |

另外还有一份 **[学习路径.md](./docs/学习路径.md)**：写给刚接触 Linux I/O 的人 ——
三根概念轴、前置知识清单、按顺序的动手实验、延伸阅读、五个常见误区，以及「怎么算学明白了」的自测题。

阅读顺序建议：**01 → 02 → 03**，即从「epoll 怎么用」到「为什么这么设计」再到「协程如何封装 epoll」。

---

## 核心知识速查

| 概念 | 一句话 |
| --- | --- |
| **Reactor** | 就绪通知，应用自己 `read`/`write`；`epoll` 是典型代表 |
| **Proactor** | 完成通知，内核代办 I/O；`io_uring` 是典型代表 |
| **多 Reactor** | 每线程一个 epoll + 一份自己的 listen fd，连接天然归属某个线程，免锁免惊群 |
| **SO_REUSEPORT** | 多个 socket 绑同一端口，由内核按四元组哈希分流（Linux 3.9+） |
| **LT vs ET** | LT 反复通知（安全），ET 只通知一次（快，必须配非阻塞 + 循环读到 EAGAIN） |
| **epoll 快的本质** | 红黑树存 fd + 就绪链表 + 回调机制，开销只与**活跃连接数**相关 |
| **io_uring 快的本质** | SQ/CQ 双环形队列 mmap 共享，批量提交，SQPOLL 下 0 系统调用 —— **API 本身不带来性能，批量与免进内核才是** |
| **异步 I/O 的前提** | 真异步靠**并发深度**换吞吐；serial 提交下 libaio/io_uring 和同步没区别 |
| **io-wq 代价** | io_uring 在无法就地完成时会 punt 给内核工作线程池（`iou-wrk-*`），小 I/O 场景下这笔跨线程开销可能吃光收益 |
| **O_DIRECT** | 绕过 page cache，需 512 字节对齐，是测真实磁盘能力的前提 |
| **协程** | 把 epoll 藏在同步系统调用后面（hook + yield/resume），切换仅 ~15 ns |

---

## 已知限制

- **io_uring 在部分容器/沙箱环境不可用**：seccomp 默认拦截 `io_uring_setup`（返回 `EPERM`）。
  代码可正常编译，运行时会提示 `[环境限制]` 并返回退出码 2。跑 `make check` 可确认。
- **libaio 在 buffered 模式下会退化**：这不是 bug，而是 page cache 让异步开销得不偿失 —— 这本身就是一个值得观察的实验结论。
- **libco 需自行提供源码**：`make libco` 依赖 `third_party/libco`。支持两种布局
  （重构版 `build/lib/libcolib.a` + `build/bin/`，或上游原版根目录 `libco.a`）。
  注意构建产物**不能跨平台复用** —— 在 macOS 上编译出的 `build/` 拿到 Linux 上跑不了，需在目标机重新 `make`。
- **Python 压测端可能先饱和**：`network/bench.py` 是多线程 Python，受 GIL 限制单进程约 1 核到顶，
  会把各服务端的差异压平。矩阵工具的「自动观察」会给出判断；要测服务端上限请换
  `--client cpp`（`bin/bench_client`，C++ 无 GIL）。

---

## 数据来源说明

文档中的所有性能数据（QPS、IOPS、ns/次、系统调用次数）均来自实验环境实际测量，
**绝对值会因 CPU、磁盘类型、内核版本、容器限制而显著不同**，请以自己的实测结果为准。
可复现的**相对关系**（如 O_DIRECT 下异步 >> 同步、epoll ~11 次系统调用/消息）才是本项目的核心结论。
