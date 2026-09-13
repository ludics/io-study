# io-study —— Linux I/O 模型实验项目

从 `select/poll/epoll` 到 `io_uring`，从 Reactor 到 Proactor，从事件回调到协程 —— 一个**能编译、能跑、能压测**的完整实验集合。

> Linux 上是 epoll / io_uring / libaio 三套实现；macOS 上另有一组**对等实现**
> （`kqueue` 版 echo server、`F_NOCACHE` 版磁盘基准），压测客户端两边通用 → 见 [macOS 上怎么用](#macos-上怎么用)。

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

**在 macOS 上**（Linux 专有接口不存在，会自动跳过并给出提示）：

```bash
make macos                   # 编译 kqueue 版 echo + F_NOCACHE 磁盘基准 + 压测客户端
./bin/echo_kqueue 19000 &    # kqueue 版服务端
./bin/bench_client 19000 8 256
./bin/io_macos /tmp/t.dat 4096 8192 1 1 256
```

详细的分步说明与预期结果，见 **[Instruction.md](./Instruction.md)**（服务器分步运行指引）。
**先确认"你测的是什么介质"** —— 见 **[测量环境与复现.md](./docs/测量环境与复现.md)**。

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
│   ├── md/               分析文档 + 三篇编程指南 + 结论总结（Markdown 版）
│   ├── 测量环境与复现.md  每个数字测在什么介质上 + macOS 能测什么（重要，先读这份）
│   ├── macOS上做Linux性能实验.md  平台选型：真机 / UTM / multipass / 容器 的实测对比
│   └── 挂载方案对比.md    sshfs（multipass 默认）vs virtiofs 的实测对比与规避办法
│
├── examples/             教学示例（与 docs/md/04~06 三篇编程指南配套，make examples）
│   ├── epoll_echo.c          epoll 版 echo，教科书式正确（ET 读空 / 循环写 / 非阻塞）
│   ├── io_uring_echo.c       io_uring 版 echo，刻意不用 SQPOLL 以便看清提交/收割
│   └── libaio_rw.c           libaio 异步读写，含 O_DIRECT 对齐与批量收割
│
├── network/              网络 I/O
│   ├── reactor_server.c      Reactor 模式 echo server（ET + 非阻塞）
│   ├── echo_epoll.c          epoll echo server，单线程 Reactor（用于 strace / 压测对比）
│   ├── echo_epoll_mt.c       多线程 Reactor（SO_REUSEPORT 分流，N worker 各自 epoll）
│   ├── echo_io_uring.c       io_uring echo server（Proactor 基础版）
│   ├── echo_io_uring_adv.c   io_uring 火力全开版（SQPOLL/缓冲区环/multishot/ZC）
│   ├── echo_io_uring_modern.c io_uring 现代版（新内核专用写法，无兼容包袱，最快）
│   ├── echo_kqueue.c         macOS 对等实现：kqueue 版 Reactor echo server
│   ├── bench_client.cpp      多线程 C++ 压测客户端（无 GIL）
│   ├── bench.py              Python 压测客户端（推荐；支持连接数/预热/延迟分位/JSON）
│   └── strace_epoll实测.txt   epoll 处理 1 条消息的真实系统调用序列
│
├── disk/                 磁盘 I/O + epoll 验证实验
│   ├── io_common.h           公共工具（计时、对齐内存、开文件、结果打印）
│   ├── io_sync.c             同步 pread/pwrite 基线
│   ├── io_libaio.c           libaio 版本（io_setup/io_submit/io_getevents）
│   ├── io_uring_disk.c       io_uring 版本（SQ/CQ 环形队列）
│   ├── io_macos.c            macOS 对等实现：F_NOCACHE + 多线程模拟并发深度
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
│   ├── bench_matrix.py       矩阵压测工具（网络 + 磁盘，纯 Python，产出 results/ 报表）
│   └── vm-clock-guard/      虚拟机时钟校正（NTP 被墙 + 宿主休眠导致 VM 掉时间的兜底）
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
| `make net` | 编译网络部分（含 `echo_io_uring_adv` 与 `echo_io_uring_modern`） |
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

## macOS 上怎么用

本项目的核心实现都是 **Linux 专有接口**（`epoll` / `io_uring` / `libaio` / `O_DIRECT`），
在 macOS 上**连头文件都找不到**，编不过也跑不了。所以 macOS 侧单独给了一组**对等实现**：

| macOS 实现 | 对应 Linux 的 | 用什么替代 |
| --- | --- | --- |
| `network/echo_kqueue.c` | `network/echo_epoll.c` | `kqueue`（含 `EV_CLEAR` = 边缘触发） |
| `disk/io_macos.c` | `disk/io_sync.c` / `io_libaio.c` | `fcntl(F_NOCACHE)` + 多线程模拟并发深度 |
| `network/bench_client.cpp` | 同一个文件 | 只用 POSIX socket，**两个平台通用** |
| `network/bench.py` | 同一个文件 | 纯 Python，**两个平台通用** |

```bash
make macos                                 # 编 echo_kqueue / io_macos / bench_client
./bin/echo_kqueue 19000 &
./bin/bench_client 19000 8 256             # C++ 客户端（实测 ~96k QPS）
bin/io_macos /tmp/t.dat 4096 8192 1 1 256  # F_NOCACHE 磁盘基准

# 也可以「客户端在 Mac、服务端在 Linux 虚拟机里」（跨机器压测）：
./bin/bench_client --host 192.168.252.3 19000 8 256
```

> **平台选型**（想在 macOS 上测 Linux 性能，该用真机 / UTM / multipass / 容器？）
> 见 **[macOS上做Linux性能实验.md](./docs/macOS上做Linux性能实验.md)**：
> 实测 VM 的磁盘比宿主 NVMe 慢约 9 倍、宿主负载变化能让同机同代码差 2 倍，
> 而虚拟网卡只比回环慢约 6%。

`make disk` / `make net` 这类 Linux 目标在 macOS 上会打印一句提示并跳过，不会报一堆头文件错误。

> **宿主机实测**（Apple Silicon，回环 + APFS SSD）：kqueue 单线程 **~96k QPS**；
> 真实 NVMe 单线程 4KB 同步写 **7.0~8.1 万 IOPS**（是 VM 虚拟盘的约 9 倍）。
> 有意思的是：macOS 上**多线程写反而越并发越慢**（1 线程 7~8 万 → 32 线程 1.5~2.1 万），
> 因为没有内核级异步提交队列，「并发深度」只能靠线程堆 —— 这恰好说明 libaio / io_uring 的价值。

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

## 五组实验分别回答什么

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

**三篇编程指南**（讲「怎么用」：API 语义 + 内核行为 + 完整示例 + 陷阱清单 + 实测数字）：

| 文档 | 内容概要 |
| --- | --- |
| [深入浅出 epoll 编程](./docs/md/04-epoll-编程指南.md) | 四个 API 逐个讲、LT/ET 的内核实现差异、回调路径、11 条陷阱、每条消息 3.0 次系统调用的由来 |
| [深入浅出 io_uring 编程](./docs/md/05-io_uring-编程指南.md) | SQ/CQ 三个内存结构、提交与收割的三种风格、SQPOLL/提供缓冲区/固定文件表/零拷贝、如何把系统调用降到 0 |
| [深入浅出 libaio 编程](./docs/md/06-libaio-编程指南.md) | iocb/io_submit/io_getevents 三件套、为什么只有 `O_DIRECT` 是真异步、深度换吞吐 25 倍、与 io_uring 的取舍 |

**两份总结**：

| 文档 | 内容概要 |
| --- | --- |
| [结论：网络 I/O 与磁盘 I/O 谁快谁慢](./docs/md/07-结论-网络IO与磁盘IO.md) | 8 个实现、两台 VM、十几个维度后的结论；跨网络/磁盘都成立的 5 条第一性原理；选型指南 |
| [挂载方案对比：sshfs vs virtiofs](./docs/挂载方案对比.md) | multipass 两种挂载的实测差异（元数据差 400 倍）、属性缓存对 `make` 的影响、三种规避方案 |

另外还有一份 **[学习路径.md](./docs/学习路径.md)**：写给刚接触 Linux I/O 的人 ——
三根概念轴、前置知识清单、按顺序的动手实验、延伸阅读、五个常见误区，以及「怎么算学明白了」的自测题。

阅读顺序建议：**01 → 02 → 03**（原理），然后 **04 → 05 → 06**（编程），最后 **07**（结论）。
只想快速看答案的话，直接读 07。

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

## 实验五：io_uring 的极限在哪（`echo_io_uring_adv` / `echo_io_uring_modern`）

基础版 io_uring（`echo_io_uring.c`）每条消息提交一次 SQE、立刻 `io_uring_enter` 一次，
把 io_uring 最值钱的特性全浪费了，结果反而**慢于 epoll**（0.70x）。
`network/echo_io_uring_adv.c` 把高级特性都实现出来，**并且每个都能用环境变量独立开关**，
这样可以量化各自的贡献：

| 特性 | 作用 | 内核要求 | 5.15 | 7.0 |
| --- | --- | --- | --- | --- |
| `IORING_SETUP_SQPOLL` | 内核线程轮询 SQ，**提交不进内核** | ≥5.1（非特权需 ≥5.13） | ✅ | ✅ |
| 提供缓冲区环 `IORING_REGISTER_PBUF_RING` | 缓冲区交给内核自取，内存 O(连接数)→O(缓冲数) | ≥5.19 | ❌ | ✅ |
| `IORING_RECV_MULTISHOT` | 提交一次 recv 持续收割（**注意：是 flag 不是 opcode**） | ≥5.19 | ❌ | ✅ |
| multishot accept `IORING_ACCEPT_MULTISHOT` | 一次提交持续接受新连接 | ≥5.19 | ❌ | ✅ |
| `IORING_OP_SEND_ZC` | 零拷贝发送（返回两个 CQE，要等 `IORING_CQE_F_NOTIF`） | ≥6.0 | ❌ | ✅ |

```bash
make net
ECHO_SQPOLL=1 ./bin/echo_io_uring_adv 19002      # 启动时打印「能力自检」表
ECHO_SPIN=1   ./bin/echo_io_uring_adv 19002      # 再加 CQ 忙轮询
```

### 全特性实测（Ubuntu 26.04 / 内核 7.0，所有特性都能跑）

16 客户端线程 × 256B ping-pong，客户端绑核 0-3，交替跑 3 轮：

| 服务端 | 核数 | QPS（3 轮） |
| --- | --- | --- |
| epoll 单线程 | 1 | 107k, 110k |
| io_uring 基础版 | 1 | 74.7k, 74.6k, 74.8k |
| adv 全特性（SQPOLL+缓冲环+multishot+ZC） | 2 | 63.4k, 64.3k, 63.5k |
| **adv 全特性 + CQ 忙轮询** | 2 | **189.0k, 189.4k, 185.0k** |

**全特性 + 忙轮询 = 189k，是 epoll 单线程（107k）的 1.77 倍**，而且只用了 1 个业务核
（另 1 个核被 SQPOLL 内核线程占用）。这是项目里 io_uring 第一次明确超过 epoll。

### 归因：到底是哪个特性在起作用

| 配置 | QPS | io_uring_enter / 条 |
| --- | --- | --- |
| io_uring 基础版 | 74.7k | 2.01 |
| adv +SQPOLL +PBUF +MULTISHOT +ZC | 63.4k | 0.95 |
| adv 同上 + CQ 忙轮询 | **189.0k** | ≈0 |

四条结论：

1. **批量提交能把系统调用减半**（2.01 → 0.95 次/条），但**吞吐几乎不动** ——
   说明在这个场景里系统调用次数根本不是瓶颈，「省系统调用」本身不产生性能。
2. **SQPOLL 单独用甚至会变慢**（63k < 74.7k）：内核轮询线程要吃掉一个核，
   而应用仍要为「收割」进内核。**必须配上 CQ 忙轮询**（用 `io_uring_peek_cqe`
   在用户态轮询 CQ，不进内核）才见效 —— 一加就是 **3.0x**。
   > 所以「什么时候 io_uring 才有优势」的准确答案是：
   > **当「提交」和「收割」两条进内核的路径都被消掉时**（SQPOLL + 忙轮询）。
3. **SQPOLL 必须给它留一个核**：同一个二进制，服务端只绑 1 个核时 15.4k，
   给 2 个核（应用 + SQ 线程各一个）63k。若把进程 `taskset` 到单核，
   SQ 内核线程会落在同一个核上互相抢，**比不开 SQPOLL 还慢**。
   这是部署形态问题，不是代码问题。
4. **「什么时候挂下一条 recv」比想象中重要得多**（这条最容易漏）：
   adv 默认在 recv 完成时立刻续挂下一条（保持 socket 常驻可读），
   基础版则是**等回显真正发完才挂**。实测两者差 **1.55x**：
   | 挂 recv 的时机 | QPS |
   | --- | --- |
   | 收到 recv 完成就续挂（常驻可读） | 38.9k, 39.4k |
   | 等 send 完成后再挂（基础版形态） | 61.9k, 59.5k |
   原因见下面的 perf 采样：回环下「发完再挂」时对端回复往往**已经在 socket 缓冲区里**，
   recv 能在提交系统调用里直接完成；而「常驻可读」时 recv 十有八九要登记**异步轮询**，
   等数据到了再唤醒进程重新提交 —— 省下的系统调用远抵不过唤醒的开销。
   用 `ECHO_RECV_LATE=1` 可以切换这两种形态自己做对照。
   > 注意：这不是说「常驻 recv」写法有问题。真实网络下对端回复要几十上百微秒才到，
   > 常驻 recv 才是低延迟的正确姿势；这里的 1.55x 是回环（内核内同步投递）
   > 这个特殊环境放大的效应。测出来的是「环境」而不是「写法」。

### 怎么用 perf 定位到这些的（方法本身可复用）

```bash
# 1) 确认 perf 能用：虚拟机里通常没有 PMU，cycles/instructions 会报 not supported
sudo perf stat -e cycles -- /bin/true
# 2) 退而用软件事件采样（cpu-clock 靠时钟中断，不依赖 PMU）
gcc -O2 -g -fno-omit-frame-pointer -o /tmp/adv_prof network/echo_io_uring_adv.c -luring
sudo perf record -e cpu-clock -F 3000 --call-graph fp -p <服务端PID> -- sleep 6
sudo perf report -i perf.data --stdio --no-collapse --percent-limit 1
```

采样结果（adv、SQPOLL=0、全关高级特性）：

```
41.7%  __wake_up_sync_key          ← 唤醒等待队列（异步轮询完事后的唤醒）
27.4%  queued_spin_lock_slowpath   ← 等待队列哈希锁竞争
52.8%  io_send → tcp_sendmsg → __dev_queue_xmit → __local_bh_enable_ip → do_softirq
```

`do_softirq` 出现在发送路径里，是因为**回环设备的发送会在发送方进程上下文里内联执行
RX 软中断**：自己发出去的回显，马上由同一个线程"收到"并唤醒对端 socket。
再叠加上「常驻 recv」带来的异步轮询，就形成了那 41.7% + 27.4% 的开销。
**这两项加起来接近 70% 的 CPU，全花在"等与唤醒"上，而不是花在搬数据上。**


### 现代版：把兼容包袱全部删掉（`echo_io_uring_modern`）

上面那个 `adv` 版为了同一份代码能在 5.15（老 UAPI 头文件 + liburing 2.1）和新内核上
都编得过，塞进了三样东西：手写的内核 ABI 常量、"内核不支持就降级"的分支、
以及自己实现的缓冲区环入队函数。代价是代码又长又绕，读者很难看清 io_uring 该怎么用。

`network/echo_io_uring_modern.c` 是一份**只面向新环境**（内核 ≥6.6 + liburing ≥2.6）的
重写版，上面三样全部删掉，所有高级特性都用 liburing 现成的辅助函数：

| 技术 | 用到的 API | 收益 |
| --- | --- | --- |
| SQPOLL + SQ_AFF | `io_uring_queue_init_params` + `sq_thread_cpu` | 提交路径 **0 系统调用** |
| CQ 忙轮询 | `io_uring_peek_batch_cqe` + `io_uring_cq_advance` | 收割路径 **0 系统调用** |
| multishot accept + 固定文件表 | `io_uring_prep_multishot_accept_direct` + `io_uring_register_files_sparse` | 连接建立 **0 系统调用**，收发免 fget/fput |
| 提供缓冲区环 | `io_uring_register_buf_ring` + `io_uring_buf_ring_add/advance` | 内存 O(连接数) → O(缓冲数) |
| multishot recv | `io_uring_prep_recv_multishot` | 每条连接只提交一次 recv |
| 零拷贝发送 | `io_uring_prep_send_zc` | 省一次拷贝（**但默认关着，见下**）|

```bash
make net
# SQPOLL 的内核线程是忙轮询的，必须给它一个独立的核：
ECHO_SQ_CPU=5 taskset -c 4 ./bin/echo_io_uring_modern 19002
```

**实测（Ubuntu 26.04 / 内核 7.0，16 线程 × 256B，各配置交替 5 轮取中位）**：

| 服务端 | 核数 | QPS | 相对 epoll |
| --- | --- | --- | --- |
| epoll 单线程 | 1 | 112k | 1.00x |
| adv 全特性 + 忙轮询（开零拷贝） | 2 | 185k | 1.65x |
| modern 全特性（开零拷贝） | 2 | 175k | 1.56x |
| **modern 全特性（关零拷贝）** | 2 | **233k** | **2.08x** |

**稳态系统调用数实测 0**（`perf stat -e syscalls:sys_enter_io_uring_enter`）：

| 配置 | enter 次数 | 每条消息 |
| --- | --- | --- |
| 全特性（SQPOLL + 忙轮询） | **0** | **0.000** |
| 关掉忙轮询 | 48,574 | 0.14 |
| 关掉两者 | 243,478 | 1.02 |

三个反直觉的结论（都用另一份独立实现交叉验证过）：

1. **零拷贝在小消息下是负收益**：同一个二进制只改 `ECHO_ZC`，233k → 175k（**-26%**）。
   `SEND_ZC` 每次发送会产生**两个** CQE（第二个带 `IORING_CQE_F_NOTIF`），
   256B 消息省下的那次拷贝根本不值这个开销。所以现代版**默认关闭**它
   （`ECHO_ZC=1` 可打开验证）。**「零拷贝」不是免费的午餐 —— 只有拷贝本身成为瓶颈时才划算。**
2. **固定文件表 / direct accept 在这个量级没有可测量的收益**：`ECHO_FIXED=1` vs `0`
   各跑 7 轮，中位数都是 222k。理论收益（省 fget/fput、免 accept 系统调用）在
   这个并发量下不是瓶颈。保留它是因为它是现代写法且不额外花代价；
   要到几万连接、fd 表很大时才可能显出价值。
3. **`COOP_TASKRUN` / `DEFER_TASKRUN` 与 `SQPOLL` 互斥**（实测一起写内核返回 `EINVAL`），
   但 `SINGLE_ISSUER` 可以和 SQPOLL 共存。想要 SQPOLL 就得放弃前两个。

> 顺便回答一个常见疑问：**「少用系统调用」和「更高的吞吐」不是一回事**。
> 本项目的对照实验里，把每条消息的系统调用从 1.02 次降到 0 次，吞吐是 72k → 233k；
> 而 adv 版把系统调用从 2.01 降到 0.95 时吞吐几乎没动。差别在于**省掉的是哪一次**：
> 省掉"等待完成"（收割）才会真的快，因为那一次进内核意味着进程要睡下去再被唤醒。


---

## 已知限制

- **测量介质会改变结论（最重要的一条）**：磁盘实验的测试文件必须落在**真实本地磁盘**上。
  放到 sshfs（FUSE over SFTP，本质是网络文件系统）里时，「异步靠深度换吞吐」会**完全反过来** ——
  实测同一台 VM、同一份代码：本地盘 libaio 是同步的 **23.8x**，sshfs 上只有 **0.61x**；
  更阴的是 `O_DIRECT` 在 sshfs 上**打开成功但被忽略**，不报任何错。
  另外 `/tmp` **不一定**是磁盘（有的发行版是 tmpfs 内存盘，同步写能测出 163 万 IOPS）。
  现在 `bench_matrix.py` 会自动挑真实本地盘、并在报告里标注介质、对危险介质给强警告。
  完整对照实验见 **[测量环境与复现.md](./docs/测量环境与复现.md)**。
- **虚拟机时钟会掉（NTP 被墙 + 宿主休眠）**：VM 跑在 QEMU 里，宿主 macOS 一休眠 guest 时钟就停走；
  本环境还**完全屏蔽了 NTP**（UDP 123 全部超时），chrony 的 `makestep 1 3` 又只允许启动后前 3 次跳跃、
  之后只能慢速追赶 —— 所以时间越掉越多、怎么修都修不好。实测两台 VM 分别落后 2h13m / 2h05m，
  每次 `make` 都报 `Clock skew detected`。
  **解法**：[`scripts/vm-clock-guard/`](./scripts/vm-clock-guard/README.md) 用 HTTPS 的 `Date` 头
  做时间源，每 5 分钟兜底校正（`sudo bash scripts/vm-clock-guard/install.sh`）。
  顺带注意：两台 VM 的**时区**原本也不一致（`.2` 是 CST、`.3` 是 UTC），会让时间戳跨机不可比。
- **多台机器共用一份 `bin/` 会互相覆盖**：工作区挂载进多台 VM 时，`bin/` 是共享的，
  后 `make` 的机器会盖掉先 `make` 的产物（症状：`libaio.so.1t64: cannot open shared object file`）。
  现在 `bin/.build-stamp` 会记下编译主机，`bench_matrix.py` 开跑前核对，不匹配直接拒绝执行。
- **io_uring 在部分容器/沙箱环境不可用**：seccomp 默认拦截 `io_uring_setup`（返回 `EPERM`）。
  代码可正常编译，运行时会提示 `[环境限制]` 并返回退出码 2。跑 `make check` 可确认。
- **libaio 在 buffered 模式下会退化**：这不是 bug，而是 page cache 让异步开销得不偿失 —— 这本身就是一个值得观察的实验结论。
- **libco 需自行提供源码**：`make libco` 依赖 `third_party/libco`。支持两种布局
  （重构版 `build/lib/libcolib.a` + `build/bin/`，或上游原版根目录 `libco.a`）。
  注意构建产物**不能跨平台复用** —— 在 macOS 上编译出的 `build/` 拿到 Linux 上跑不了，需在目标机重新 `make`。
- **Python 压测端可能先饱和**：`network/bench.py` 是多线程 Python，受 GIL 限制单进程约 1 核到顶，
  会把各服务端的差异压平。矩阵工具的「自动观察」会给出判断；要测服务端上限请换
  `--client cpp`（`bin/bench_client`，C++ 无 GIL）。
- **工作区挂载（multipass sshfs）的元数据很慢**：在挂载目录里做小文件密集操作（`git status`、
  大量小文件、文件监听）会明显变慢 —— 实测元数据比 VM 本地盘慢约 400 倍、小文件创建慢 171 倍。
  更要注意：sshfs 有**约 1 秒的属性缓存**，宿主刚改完文件时 `make` 可能因为 mtime 是旧值而
  **跳过重编**（文件*内容*读取不受影响）。规避办法：编译用 `make -B`，
  或把产物落到本地盘 `make BINDIR=/home/ubuntu/iobin`。
  完整实测与三种方案见 **[挂载方案对比.md](./docs/挂载方案对比.md)**。

---

## 数据来源说明

文档中的所有性能数据（QPS、IOPS、ns/次、系统调用次数）均来自实验环境实际测量，
**绝对值会因 CPU、磁盘类型、内核版本、容器限制而显著不同**，请以自己的实测结果为准。
**每个数字的测量介质（VM 本地盘 / 回环 / 宿主机 SSD）与完整对照实验见
[测量环境与复现.md](./docs/测量环境与复现.md)** —— 例如 io_uring 的磁盘写在
内核 5.15 上只有 libaio 的 0.23x（被 punt 到 io-wq），在 7.0 上已追到 0.89x。
可复现的**相对关系**（如 O_DIRECT 下异步 >> 同步、epoll ~11 次系统调用/消息）才是本项目的核心结论。
