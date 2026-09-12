# Linux 服务器运行指引

在真实 Linux 服务器上复现全部实验（**重点：跑通 io_uring**）。

> 本文所有命令均在服务器上执行，**除特殊说明外都在项目根目录 `io-study/` 下运行**。全程约 20~40 分钟（含网络多维矩阵实验）。
>
> 关键前提：项目内所有 Makefile / 脚本均使用**相对路径**，整个目录可以拷到任何位置运行。

---

## 第 0 步：环境检查（最重要）

### 0.1 一键体检（推荐）

```bash
cd io-study
bash scripts/check_env.sh
```

它会检查：内核版本、gcc/g++/make/python3、libaio / liburing 开发库、
`io_uring_disabled` sysctl、**实际调用一次 `io_uring_setup` 验证可用性**，并给出修复建议。

### 0.2 手工检查内核版本

io_uring 需要 **Linux 5.1+**（建议 5.10+ 以获得完整特性）。

```bash
uname -r
```

- 若 < 5.1：io_uring 无法使用，其余实验（epoll/libaio/同步）不受影响。
- 若 ≥ 5.1：继续。

### 0.3 检查是否受 seccomp / 容器限制

**这是最常见的坑**。即使在物理机上，如果程序跑在 Docker/K8s 容器里，默认 seccomp 策略也会**禁用 io_uring**。

一键检测（直接用项目自带探针，它会把 errno 翻译成人话）：

```bash
gcc -O2 -o /tmp/uring_probe scripts/uring_probe.c && /tmp/uring_probe
```

**判定**：

| 结果 | 含义 |
|------|------|
| `可用 ✅` | 一切正常，继续 |
| `不可用 ❌ errno=1 (Operation not permitted)` | 被 seccomp / LSM 拦截 |
| `不可用 ❌ errno=38 (Function not implemented)` | 内核 < 5.1 或编译内核时关掉了 io_uring |
| `不可用 ❌ errno=14 (Bad address)` | **探针参数写法有误**，不是环境问题（见下方说明） |

> ⚠️ **不要用「传 NULL 当 params」的写法做探测**。常见的一段错误探针是这样的：
>
> ```c
> // ❌ 错误示例：第二个参数必须是 struct io_uring_params *，传 NULL 内核一律返回 EFAULT(14)
> long r = syscall(425 /* __NR_io_uring_setup */, 256, (void*)0);
> ```
>
> `io_uring_setup(entries, params)` 的 `params` 是指向 `struct io_uring_params` 的**指针**，
> 内核会对它做 `copy_from_user`。传 `NULL` 时内核返回 `EFAULT (14, Bad address)` ——
> **这恰恰说明内核允许 io_uring、也执行到了该逻辑**，只是探针自己传错了参数。
> 正确写法必须传一个零初始化的结构体（`scripts/uring_probe.c` 就是这么做的）。
>
> 系统调用号在 `x86_64 / aarch64 / riscv64` 上都是 **425**（asm-generic 表），
> 但更稳妥的是直接用 `<sys/syscall.h>` 里的 `__NR_io_uring_setup`。

若在容器中遇到 EPERM，解决方式（选一）：

```bash
# Docker：放宽 seccomp
docker run --security-opt seccomp=unconfined ...
# 或使用允许 io_uring 的自定义 profile
docker run --security-opt seccomp=/path/to/io_uring-seccomp.json ...
# K8s：设置 privileged 或自定义 seccompProfile: Unconfined
```

> 我在沙箱里就是卡在这一步（`errno=1 EPERM`），所以当时 io_uring 只编译未运行。
> 本项目已在 **aarch64 / Ubuntu 22.04 / 内核 5.15.0-generic** 上实测跑通：探针返回可用，
> `echo_io_uring` 回显正常 —— 说明 **aarch64 与 x86_64 一样可用**，系统调用号同为 425。

### 0.4 安装依赖

```bash
# Debian / Ubuntu
sudo apt-get update
sudo apt-get install -y build-essential libaio-dev liburing-dev strace

# RHEL / CentOS / Rocky
sudo yum install -y gcc gcc-c++ make libaio-devel liburing-devel strace
```

验证库已装：

```bash
ls /usr/include/libaio.h /usr/include/liburing.h
```

### 0.5 上传代码

```bash
scp io-study.tar.gz user@your-server:~/
ssh user@your-server
tar xzf io-study.tar.gz && cd io-study
```

目录结构（**请保持完整**）：

```
io-study/
├── Makefile            # 统一构建入口（根目录）
├── README.md           # 项目说明
├── 运行指引.md          # 本文
├── docs/
│   ├── html/           # 三份分析文档（HTML，浏览器打开）
│   └── md/             # 三份分析文档（Markdown）
├── network/            # 网络 I/O：epoll / io_uring Echo Server
│   ├── reactor_server.c
│   ├── echo_epoll.c          # 单线程 Reactor
│   ├── echo_epoll_mt.c       # 多线程 Reactor（SO_REUSEPORT）
│   ├── echo_io_uring.c
│   ├── bench_client.cpp
│   ├── bench.py              # 压测客户端（连接数/预热/延迟分位/JSON）
│   └── strace_epoll实测.txt
├── disk/               # 磁盘 I/O：sync / libaio / io_uring + epoll 验证实验
│   ├── io_common.h
│   ├── io_sync.c
│   ├── io_libaio.c
│   ├── io_uring_disk.c
│   ├── epoll_nonblock_demo.c
│   └── epoll_starve_demo.c
├── libco/              # libco 协程切换基准 + 三方对比脚本
│   ├── bench_swap.cpp
│   └── compare.sh
├── scripts/            # 环境体检 + 一键实验
│   ├── check_env.sh
│   ├── run_all_bench.sh
│   ├── uring_probe.c         # io_uring 可用性探针（会翻译 errno）
│   └── bench_matrix.py       # 矩阵压测工具（网络 + 磁盘，产出 results/ 报表）
└── third_party/        # libco 源码 clone 到这里（可选，make libco 时用）
```

---

## 第 1 步：编译

```bash
cd io-study
make            # 编译全部
make help       # 查看所有目标
```

预期输出若干个 `[OK]`。产物统一在 `bin/`：

```
bin/io_sync  bin/io_libaio  bin/io_uring_disk
bin/echo_epoll  bin/echo_epoll_mt  bin/echo_io_uring  bin/reactor_server  bin/bench_client
bin/epoll_nonblock_demo  bin/epoll_starve_demo
```

若报 `cannot find -luring`，回到 0.4 装依赖。

---

## 第 2 步：磁盘 I/O 三方对比（核心，含 io_uring）

### 2.1 先跑 buffered 模式（快速验证链路通）

```bash
make bench TOTAL=65536 BLOCK=4096 DEPTH=32 DIRECT=0
```

**预期结果**：

| 方式 | 写 IOPS | 读 IOPS |
|------|---------|---------|
| 同步 | ~600K+ | ~2.8M |
| libaio | ~600K | ~1.9M |
| io_uring | 与上两者接近或略高 | |

**结论**：buffered 模式下数据命中 page cache，本质是内存拷贝，异步 I/O **优势不明显甚至更慢**（libaio 在 buffered 下会退化）。这一步主要验证三个程序都能跑通。

### 2.2 再跑 O_DIRECT 模式（真实磁盘 I/O，关键）

```bash
make bench TOTAL=4096 BLOCK=4096 DEPTH=32 DIRECT=1
```

> `TOTAL` 调小是因为 O_DIRECT 直落磁盘，速度慢很多。

**预期结果**（我在沙箱实测，供参考）：

| 方式 | 写 IOPS | 写带宽 |
|------|---------|--------|
| 同步 | ~572 | ~2.2 MB/s |
| libaio | ~15,791 | ~61.7 MB/s |
| **io_uring** | **≥ libaio** | 通常更高 |

**结论**：

1. 异步 I/O（libaio / io_uring）比同步快 **20~30 倍** —— 并发深度让磁盘请求并行化。
2. **io_uring 应 ≥ libaio**，因为 io_uring 减少了系统调用、共享内存零拷贝、支持更多操作类型。
3. 若你的磁盘是 NVMe SSD，差距会更明显（HDD 上 IOPS 上限受机械延迟限制）。

### 2.3 单独运行 io_uring（确认它真的跑起来了）

```bash
./bin/io_uring_disk /tmp/uring_test 4096 4096 32 1
```

- 正常：输出 IOPS / 带宽数据。
- 若输出 `[环境限制]` 并返回退出码 2 → seccomp 问题，回到 0.3。

### 2.4 进阶：测不同并发深度的影响（可选）

```bash
for d in 1 4 16 32 64 128; do
    echo "--- depth=$d ---"
    ./bin/io_uring_disk /tmp/t_$d 4096 8192 $d 1 2>&1 | grep "写 IOPS"
done
```

**预期**：IOPS 随 depth 上升，到某点后趋于平缓（磁盘队列饱和）。这直观展示「异步 I/O 靠并发深度换吞吐」。

### 2.5 磁盘 I/O 多维度矩阵对比（推荐重点跑）

2.1~2.4 都是单点验证。矩阵实验沿三个维度扫描，每个维度横跨 同步 / libaio / io_uring：

```bash
make bench_disk_matrix                 # quick，约 1~2 分钟
make bench_disk_matrix MODE=full       # 更细的扫点

# 需要指定真实磁盘路径 / 调整 I/O 量时直接调脚本：
python3 scripts/bench_matrix.py disk --mode full --bytes 268435456 --file /data/iodemo
```

| 维度 | 扫描范围 | 固定参数 |
|------|---------|---------|
| **A. 队列深度** | 1 → 128 | 4KB / O_DIRECT |
| **B. 块大小** | 512B → 256KB | depth=32 / O_DIRECT |
| **C. O_DIRECT** | 开 / 关 | 4KB / depth=32 |

产物在 `results/` 下：`disk_matrix_<模式>_<时间戳>.md`（表 + 自动观察）、`.csv`、`.log`。

> ⚠️ **测试文件必须落在真实文件系统上**，tmpfs/ramfs 不支持 O_DIRECT。
> 脚本会自动检测并警告（`df -T`）。默认路径 `/tmp/iodemo_matrix`。

**预期结果**（括号内是 aarch64 Ubuntu 22.04 虚拟机实测，仅看相对关系）：

1. **同步方式完全不吃深度**：depth 1→32，同步写 IOPS 一直在 8k 附近。
   而 **libaio 从 8.3k 涨到 213.3k（27.4x）** —— 异步的吞吐来自并发深度。
2. **io_uring 写只爬到 libaio 的一半**（103.6k vs 213.3k），**但读完全持平**（240k vs 238k）。
   原因是内核把 **O_DIRECT 写甩给了 io-wq 工作线程池**，每次 I/O 多一次跨线程交接；
   读不需要 punt，所以没有这笔开销。可以这样复现：
   ```bash
   ./bin/io_uring_disk /tmp/t 4096 65536 32 1 & sleep 0.2
   ps -o comm= -L -p $(pgrep -n io_uring_disk) | sort | uniq -c   # 会看到一堆 iou-wrk-*
   ```
3. **buffered 数字虚高**：同步 buffered 写 1.46M IOPS（5.7 GB/s）vs O_DIRECT 8.4k
   —— page cache 把真实磁盘行为整个盖住了，量级差 100 倍以上。
4. **块大小是 IOPS 与带宽的取舍**：512B→64KB 时写 IOPS 基本不变，但写带宽涨了 113 倍。

> 另：`disk/io_uring_disk.c` 已改为**批量收割**（一次 `io_uring_wait_cqe` 等待 +
> `io_uring_for_each_cqe` 一次取走 CQ 里所有完成事件）。如果按「每个完成事件调用一次
> `io_uring_wait_cqe`」写，一批 32 个 I/O 就要进内核最多 32 次，把 Proactor 的红利全吃掉。

---

## 第 3 步：网络 Echo Server —— epoll vs io_uring

### 3.1 单独验证 io_uring Echo Server

```bash
# 终端 1
./bin/echo_io_uring 19001

# 终端 2
printf 'Hello io_uring' | nc 127.0.0.1 19001
```

**预期**：终端 2 回显 `Hello io_uring` —— 证明 io_uring 网络 I/O 工作正常。

> 这是我在沙箱**没能验证**的部分（seccomp 限制），请重点确认。

### 3.2 压测对比（epoll vs io_uring）

```bash
# 终端 1：启动 epoll 版
./bin/echo_epoll 19000 &

# 终端 2：压测（8 线程 / 512 字节 / 5 秒）
python3 network/bench.py 19000 8 512 5

# 换成 io_uring 版
./bin/echo_io_uring 19001 &
python3 network/bench.py 19001 8 512 5
```

`bench.py` 参数：`python3 network/bench.py <port> <线程数> <数据大小> <时长秒>`

也可以用 Makefile 一键跑：

```bash
make bench_net PORT=19000 THREADS=8 SIZE=512 SECS=5
```

**预期**：两者 QPS 在同一量级；io_uring 在高并发小包场景通常更优（系统调用更少）。

### 3.3 strace 对比系统调用（最能体现差异）

```bash
# epoll 版
strace -f -e trace=epoll_pwait,epoll_wait,accept,accept4,read,write,epoll_ctl -o /tmp/ep_trace.txt ./bin/echo_epoll 19000 &
sleep 1
printf 'X' | nc 127.0.0.1 19000
kill %1
cat /tmp/ep_trace.txt

# io_uring 版
strace -f -e trace=io_uring_setup,io_uring_enter,io_uring_register -o /tmp/uring_trace.txt ./bin/echo_io_uring 19001 &
sleep 1
printf 'X' | nc 127.0.0.1 19001
kill %1
cat /tmp/uring_trace.txt
```

**预期对比**：

| 版本 | 处理 1 条消息的系统调用 |
|------|----------------------|
| epoll | ~11 次（4×epoll_wait + accept + 2×read + write + 3×epoll_ctl） |
| **io_uring** | **显著更少**（批量提交，SQPOLL 下可降至 0） |

我在沙箱实测的 epoll 序列（供对照，也保存在 `network/strace_epoll实测.txt`）：

```
epoll_ctl(4, EPOLL_CTL_ADD, 3, {EPOLLIN}) = 0
epoll_wait(4, [{fd=3}], 64, -1) = 1
accept(3, NULL, NULL) = 5
epoll_ctl(4, EPOLL_CTL_ADD, 5, {EPOLLIN|EPOLLET}) = 0
epoll_wait(4, [{fd=5}], 64, -1) = 1
read(5, "X", 4096) = 1
write(5, "X", 1) = 1
epoll_wait(4, [{fd=5}], 64, -1) = 1
read(5, "", 4096) = 0
epoll_ctl(4, EPOLL_CTL_DEL, 5, NULL) = 0
```

**这是本次验证最有说服力的证据** —— 请重点对比这两个 trace 的行数。

### 3.4 网络 I/O 多维度矩阵对比（推荐重点跑）

3.2 只跑了**一个**场景，无法回答「什么条件下谁更快」。矩阵实验沿三个维度扫描
（并发连接数 / 消息大小 / TCP_NODELAY），每个维度横跨全部可用服务端：

```bash
make bench_net_matrix              # quick 模式，约 3~5 分钟
make bench_net_matrix MODE=full    # 更细的扫点，约 10~20 分钟

# 或者直接调脚本，参数更灵活：
python3 scripts/bench_matrix.py net --secs 3

# ⭐ 测服务端真实上限：换用 C++ 客户端（无 GIL）
python3 scripts/bench_matrix.py net --client cpp --secs 3 --mt-workers 4
```

> **为什么一定要跑 CLIENT=cpp 那一遍？**
> `network/bench.py` 是 Python 多线程，受 **GIL** 限制，单进程大约吃到 **1 核** 就到顶。
> 在一台 8 核机器上实测：Python 客户端下 epoll 与 io_uring 都只有 ~33k QPS、看起来「性能相当」；
> 换成 C++ 客户端后，同一个 io_uring 服务端能跑到 ~74k，多线程 epoll 更是到 ~308k。
> **结论完全相反** —— Python 客户端的上限会把服务端的差异整个压平。
> 报表的「自动观察」会据此提示瓶颈归属（看 `client_cpu_pct` 是否已到 1 核）。

服务端覆盖：`echo_epoll`（单线程 Reactor）、`echo_epoll_mt`（多 Reactor，SO_REUSEPORT）、
`echo_io_uring`（Proactor）、`example_echosvr`（libco 协程，需先 `make libco`）。
未编译的服务端会标记为 `N/A`，不影响其余部分。

> `echo_epoll` / `echo_epoll_mt` / `echo_io_uring` 都认 `ECHO_NODELAY=1`（对新连接设置 TCP_NODELAY），
> `echo_epoll_mt` 另认 `ECHO_LT=1`（切电平触发）——矩阵脚本会自动注入。libco 示例服务端不支持，
> 故不参与维度 C，报表中会注明。

跑完在 `results/` 下产出三个文件：

| 文件 | 内容 |
|------|------|
| `net_matrix_<模式>_<时间戳>.md` | 对比表（每个维度一张 QPS 表 + 一张 p99 表）+ 自动观察 |
| `net_matrix_<模式>_<时间戳>.csv` | 原始数据（dim, server, conns, size, nodelay, total, qps, p50, p99, max, errors） |
| `net_matrix_<模式>_<时间戳>.log` | 完整控制台日志 |

**预期结果**（下面括号里是 aarch64 / Ubuntu 22.04 / 8 vCPU 虚拟机的实测值，供对照）：

1. **维度 A（连接数）**：并发度 = 连接数（每条连接 1 个在途请求），所以 QPS 随连接数
   近似线性上升，直到某个资源饱和。实测 ^1：1 连接时三者都 ~23k（受单次 RTT 限制），
   64 连接时 epoll 单线程 76.6k、io_uring 73.6k、**epoll 多线程(4 worker) 308.1k**。
2. **维度 B（消息大小）**：小包（64B）系统调用次数主导、大包（4KB）内存拷贝/带宽主导。
   实测 epoll 单线程 77.5k → 74.1k（QPS 略降），但单位时间字节数涨了约 **61 倍**。
3. **维度 C（TCP_NODELAY）**：ping-pong 下单连接只有一个未完成包，Nagle 基本不触发，
   所以开/关差异通常很小；若差异明显，说明撞上了 Nagle 与延迟确认（delayed ACK）互等的经典症状。
4. **多线程 Reactor vs 单线程**：连接数大时应接近线性扩展。实测 **4.02x**（4 worker），
   与核数吻合 —— 说明单线程版确实卡在一个核上。
5. **io_uring vs epoll**：本项目的 io_uring echo 版每条消息都要提交 SQE 并立刻
   `io_uring_enter` 一次，**没吃到批量提交的红利**，所以在这种 ping-pong 微基准里
   与 epoll 单线程基本持平（73.6k vs 76.6k，甚至略低）。这恰好反向印证了项目的主结论：
   **io_uring 的优势来自批量提交与 SQPOLL，而不是「换个 API 就更快」**。

^1 均为 C++ 客户端（`--client cpp`）实测；Python 客户端下三者都会被压到 ~33k，看不出差异。

> **读结果的第一原则：先确认瓶颈在哪一边。**
> 如果所有服务端的 QPS 都挤在一起，先怀疑**压测端到顶**，而不是「服务端能力相当」。
> Python 压测端看 `client_cpu_pct` 是否已到 ~100%（1 核 = GIL 天花板）；
> 报表的「自动观察」会自动给出这个判断，并建议改用 `--client cpp`。

### 3.6 io_uring 到底省了多少系统调用？（附实测）

「io_uring 更少系统调用」这句话要有数字支撑才有意义。用固定 2000 条消息 +
`strace -c -f` 统计服务端的系统调用总量：

```bash
# 服务端在 strace 下跑（-c 只输出汇总；-f 跟踪线程）
strace -c -f -e trace=epoll_pwait,epoll_wait,epoll_ctl,accept,read,write \
       ./bin/echo_epoll 19701 &
# 另一个终端灌固定条数的消息，然后 Ctrl-C 让 strace 打印汇总
```

**实测（2000 条消息）**：

| 服务端 | 主要系统调用 | 总计 | 每条消息 |
|--------|------------|------|---------|
| epoll 单线程 | read 2002 + write 2001 + **epoll_pwait 2002** + epoll_ctl 3 | 6009 | **3.0 次** |
| io_uring | **io_uring_enter 4004**（提交 1 + 收割 1）+ setup 1 | 4005 | **2.0 次** |

结论分两层：

1. io_uring **确实少用 1/3 的系统调用**（2.0 vs 3.0）。
2. **但吞吐并没有赢**（73.6k vs 76.6k）。所以在这个场景里，系统调用次数**不是瓶颈** ——
   瓶颈在单核 CPU、内存拷贝和虚机网络路径上。**「省系统调用」只有在系统调用本身成为瓶颈时才有价值。**

> ⚠️ **架构坑**：aarch64（以及 riscv64）上**没有 `epoll_wait` 这个系统调用**，
> glibc 的 `epoll_wait()` 实际走 `epoll_pwait`。所以 `strace -e trace=epoll_wait` 在 ARM 上
> 会一条都抓不到（我第一遍就踩了这个坑，统计出来的 epoll 只有 4007 次，少了两千次）。
> x86_64 上才叫 `epoll_wait`。写 strace 过滤器时两个都带上最稳。

### 3.7 io_uring 的高级特性：极限在哪（`echo_io_uring_adv`）

上面的结论是「基础版 io_uring 打不过 epoll」。但基础版每条消息只提交 1 个 SQE、
立刻 `io_uring_enter` 一次，把 io_uring 值钱的特性全浪费了。
`network/echo_io_uring_adv.c` 把高级特性都实现了，**每个都能用环境变量独立开关**，
方便做归因：

```bash
make net
ECHO_SQPOLL=1 ECHO_PBUF=1 ECHO_MULTISHOT=1 ECHO_ZC=1 ECHO_SPIN=1 \
    ./bin/echo_io_uring_adv 19002      # 启动时打印「能力自检」表
```

| 环境变量 | 特性 | 内核要求 |
|---|---|---|
| `ECHO_SQPOLL` | 内核线程轮询 SQ，提交不进内核 | ≥5.1（非特权 ≥5.13） |
| `ECHO_PBUF` | 提供缓冲区环，缓冲区由内核自取 | ≥5.19 |
| `ECHO_MULTISHOT` | multishot recv（**是 flag 不是 opcode**） | ≥5.19 |
| `ECHO_ZC` | 零拷贝发送（两个 CQE，要等 `IORING_CQE_F_NOTIF`） | ≥6.0 |
| `ECHO_SPIN` | CQ 忙轮询，收割也不进内核 | 无 |
| `ECHO_RECV_LATE` | 改成「发完再挂 recv」（诊断用，见下） | 无 |

**实测（Ubuntu 26.04 / 内核 7.0，全部特性可用；16 线程 × 256B，客户端绑核 0-3）**：

| 服务端 | 核数 | QPS |
|---|---|---|
| epoll 单线程 | 1 | 107k / 110k |
| io_uring 基础版 | 1 | 74.7k ×3 |
| adv 全特性（SQPOLL+缓冲环+multishot+ZC） | 2 | 63.4k ×3 |
| **adv 全特性 + CQ 忙轮询** | 2 | **189.0k / 189.4k / 185.0k** |

**189k vs 107k = 1.77 倍**，这是项目里 io_uring 第一次明确赢过 epoll。三条要点：

1. **SQPOLL 单独用会更慢**（63k < 74.7k）：内核轮询线程占一个核，而收割仍要进内核。
   **必须配 CQ 忙轮询**才见效 —— 一加就是 3.0x。
   「什么时候 io_uring 有优势」的准确答案是：**提交和收割两条进内核的路径都被消掉时**。
2. **SQPOLL 必须独占一个核**：同一个二进制，服务端绑 1 个核只有 15.4k，给 2 个核 63k。
   把进程 `taskset` 到单核会让 SQ 内核线程和它抢同一个核，比不开还慢 —— 这是部署形态问题。
3. **「什么时候挂下一条 recv」差 1.55x**：`ECHO_RECV_LATE=0`（默认，收到就续挂）38.9k，
   `ECHO_RECV_LATE=1`（等回显发完再挂，即基础版形态）61.9k。
   原因是回环下「发完再挂」时对端回复常已在 socket 缓冲区里，recv 能在提交时**立即完成**；
   而「常驻可读」时 recv 往往要登记**异步轮询**，等数据到了再唤醒进程重新提交。
   perf 采样显示这类开销占了近 70% 的 CPU（`__wake_up_sync_key` 41.7% +
   `queued_spin_lock_slowpath` 27.4%）。注意这是**回环环境放大的效应**，
   真实网络下常驻 recv 才是低延迟的正确姿势。

> ⚠️ 这些特性需要**内核 ≥6.0**（全特性在 7.0 上验证通过）。Ubuntu 22.04 可装
> `linux-generic-hwe-22.04`（6.8）解锁。代码会在内核不支持时**自动降级**并说明原因。

> 🔧 **perf 用法**：虚拟机里通常没有 PMU（`cycles` 会报 not supported），
> 改用软件事件采样：`sudo perf record -e cpu-clock -F 3000 --call-graph fp -p <PID> -- sleep 6`，
> 再用 `sudo perf report --stdio --percent-limit 1` 看热点。
> 建议先用 `-g -fno-omit-frame-pointer` 重编译，否则看不到调用栈。

---

## 第 4 步：epoll 为什么必须 O_NONBLOCK（实验验证）

```bash
make bench_demo
```

或手动跑：

```bash
# 实验1：单连接，ET 模式
./bin/epoll_nonblock_demo 1   # 非阻塞（正确）
./bin/epoll_nonblock_demo 0   # 阻塞（错误）

# 实验2：双连接，阻塞饿死其他连接
./bin/epoll_starve_demo 1     # 非阻塞
./bin/epoll_starve_demo 0     # 阻塞
```

**预期结果**：

| 实验 | 非阻塞 | 阻塞 |
|------|--------|------|
| 单连接耗时 | ~300 ms | **~5300 ms**（read 卡到对端关闭） |
| 双连接：连接2 完成时间 | ~350 ms | **~3350 ms**（被饿死 3 秒） |

**结论**：ET 模式下非阻塞 fd 是**硬性要求**；即便 LT 模式，一个阻塞 `read` 也会挂起整个事件循环，让所有其他连接挨饿。

---

## 第 5 步：libco 协程（可选，需联网 clone）

### 5.1 clone 并编译

```bash
mkdir -p third_party
git clone --depth 1 https://github.com/Tencent/libco.git third_party/libco
make libco
```

> 注意：libco 用 `make` 而非 `make -j4`（并行有依赖时序问题）。
> 若新版 gcc 编译报错，尝试在 libco 的 Makefile 中 `CXXFLAGS` 加 `-std=c++11 -w`。

`make libco` 会产出：

- `bin/bench_swap` —— 协程切换开销基准
- `third_party/libco/build/bin/example_echosvr` —— libco 版 echo server

### 5.2 验证回显与切换开销

```bash
# 验证回显
./third_party/libco/build/bin/example_echosvr 127.0.0.1 19900 10 1 &
printf 'Hello libco' | nc 127.0.0.1 19900

# 协程切换开销
./bin/bench_swap
```

**预期**：单次协程切换约 **15~30 ns**，远快于系统调用（~50ns）和线程切换（~1000ns+）。

### 5.3 三方对比（epoll vs libco vs io_uring）

```bash
./libco/compare.sh            # 默认 4 线程 / 256B / 3 秒
./libco/compare.sh 8 512 5    # 自定义：线程数 消息大小 秒数
```

脚本会依次启动三个服务端并压测，输出各场景 QPS。所有路径均由脚本自身位置推导，可在任意目录执行。

---

## 第 6 步：一键跑通全部（懒人版）

```bash
bash scripts/run_all_bench.sh        # 全部实验，结果存到 results/
bash scripts/run_all_bench.sh disk   # 只跑磁盘 I/O
bash scripts/run_all_bench.sh demo   # 只跑 epoll O_NONBLOCK 实验
bash scripts/run_all_bench.sh net    # 只跑网络 echo 对比
```

---

## 第 7 步：汇总你的实测结论

跑完后，建议把结果填入这张表（括号内是我在沙箱的值，供对照）：

| 实验 | 指标 | 沙箱值 | **你的服务器** |
|------|------|--------|---------------|
| 磁盘 sync (O_DIRECT) | 写 IOPS | 572 | |
| 磁盘 libaio (O_DIRECT) | 写 IOPS | 15,791 | |
| 磁盘 io_uring (O_DIRECT) | 写 IOPS | *环境禁用* | **← 重点** |
| 磁盘 buffered 三者 | 写 IOPS | 同步≈libaio | |
| epoll 1 条消息 | 系统调用次数 | ~11 | |
| io_uring 1 条消息 | 系统调用次数 | *环境禁用* | **← 重点** |
| epoll 双连接（阻塞fd） | 连接2 延迟 | 3000 ms | |
| libco 协程切换 | ns/次 | 15.6 ns | |
| 网络 单线程 epoll（64 连接） | QPS | — | |
| 网络 多线程 epoll（64 连接） | QPS | — | |
| 网络 io_uring（64 连接） | QPS | — | |
| 网络 4KB/64B 消息 | 单位时间字节数比 | — | |

---

## 常见问题排查

### Q1: `cannot find -luring` / `liburing.h: No such file`
未装 liburing 开发包，回到 0.4。

### Q2: io_uring 程序输出 `[环境限制]` 或退出码 2
seccomp 禁用。用 0.3 的检测程序确认；若在容器里，需要 `--security-opt seccomp=unconfined`。

### Q3: O_DIRECT 报 `Invalid argument`
可能原因：

- 块大小/缓冲区/offset 未按 512 字节对齐（代码已用 `posix_memalign` 对齐）；
- 文件系统不支持 O_DIRECT（如 tmpfs、某些 overlayfs）。
  → 换到 ext4/xfs 的真实磁盘路径测试，或改用 `DIRECT=0`。

### Q4: 磁盘测试结果波动大
- 确保测试文件落在真实磁盘而非 tmpfs（`df -T /tmp` 查看）；
- 多次运行取平均；
- 避免测试时有其他 I/O 负载。

### Q5: strace 输出为空
某些环境限制了 ptrace。可改用 `perf stat -e syscalls:sys_enter_*` 统计，或直接对比 QPS。

### Q6: `make libco` 提示未找到源码
需要先 clone：`git clone https://github.com/Tencent/libco third_party/libco`。

### Q7: 端口被占用
`make bench_net PORT=18888` 换端口；`check_env.sh` 会提示常用端口占用情况。

---

## 一句话总结各实验目的

| 实验 | 回答的问题 |
|------|-----------|
| 磁盘三方对比 | io_uring / libaio 比同步快多少？异步 I/O 价值在哪？ |
| 磁盘多维矩阵 | 队列深度、块大小、O_DIRECT 各自带来多大变化？为什么 io_uring 写反而没赢？ |
| buffered vs O_DIRECT | 为什么 libaio 在 page cache 下反而退化？ |
| Echo Server 对比 | 网络场景下 io_uring 相对 epoll 的优势？ |
| 网络多维矩阵 | 连接数、消息大小、TCP_NODELAY 各自带来多大变化？单线程 vs 多线程 Reactor 差多少？ |
| strace 系统调用对比 | **Proactor 比 Reactor 省了多少系统调用？** |
| epoll O_NONBLOCK 实验 | 为什么阻塞 fd 会饿死整个事件循环？ |
| libco 协程切换 | 协程切换到底有多快？为什么能「同步写法异步执行」？ |

祝跑通！io_uring 部分是最值得期待的 —— 我在沙箱里被 seccomp 挡住了，你的真机结果正好补上这块拼图。
