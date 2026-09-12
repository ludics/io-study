# Linux 服务器运行指引

在真实 Linux 服务器上复现全部实验（**重点：跑通 io_uring**）。

> 本文所有命令均在服务器上执行，**除特殊说明外都在项目根目录 `io-study/` 下运行**。全程约 15~30 分钟。
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

一键检测：

```bash
cat > /tmp/probe_uring.c <<'EOF'
#include <sys/syscall.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
int main() {
    errno = 0;
    long r = syscall(425 /* __NR_io_uring_setup on x86_64 */, 256, (void*)0);
    printf("io_uring_setup : ret=%ld errno=%d (%s)\n", r, errno, strerror(errno));
    return 0;
}
EOF
gcc -o /tmp/probe_uring /tmp/probe_uring.c && /tmp/probe_uring
```

**判定**：

- `ret=3`（或任意 ≥0）→ io_uring **可用** ✅
- `ret=-1 errno=1 (Operation not permitted)` → **被 seccomp 禁用** ❌

若在容器中遇到 EPERM，解决方式（选一）：

```bash
# Docker：放宽 seccomp
docker run --security-opt seccomp=unconfined ...
# 或使用允许 io_uring 的自定义 profile
docker run --security-opt seccomp=/path/to/io_uring-seccomp.json ...
# K8s：设置 privileged 或自定义 seccompProfile: Unconfined
```

> 我在沙箱里就是卡在这一步（`errno=1 EPERM`），所以 io_uring 只编译未运行。你在物理机上大概率能跑通。

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
│   ├── echo_epoll.c
│   ├── echo_io_uring.c
│   ├── bench_client.cpp
│   ├── bench.py
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
│   └── run_all_bench.sh
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
bin/echo_epoll  bin/echo_io_uring  bin/reactor_server  bin/bench_client
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
strace -f -e trace=epoll_wait,accept,read,write,epoll_ctl -o /tmp/ep_trace.txt ./bin/echo_epoll 19000 &
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
- `third_party/libco/example_echosvr` —— libco 版 echo server

### 5.2 验证回显与切换开销

```bash
# 验证回显
./third_party/libco/example_echosvr 127.0.0.1 19900 10 1 &
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
| buffered vs O_DIRECT | 为什么 libaio 在 page cache 下反而退化？ |
| Echo Server 对比 | 网络场景下 io_uring 相对 epoll 的优势？ |
| strace 系统调用对比 | **Proactor 比 Reactor 省了多少系统调用？** |
| epoll O_NONBLOCK 实验 | 为什么阻塞 fd 会饿死整个事件循环？ |
| libco 协程切换 | 协程切换到底有多快？为什么能「同步写法异步执行」？ |

祝跑通！io_uring 部分是最值得期待的 —— 我在沙箱里被 seccomp 挡住了，你的真机结果正好补上这块拼图。
