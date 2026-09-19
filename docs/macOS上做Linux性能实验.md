# 在 macOS 上做 Linux 性能实验：平台选型与实测

> 问题：**想在 macOS 上测 Linux 的 I/O 性能，最推荐哪种方式？multipass 虚拟机好不好？**
>
> 短答：**要绝对数字就别在本地测**（用真 Linux 机器）；
> **要相对对比和内核行为，本地 VM 完全够用**，multipass 能用但有几个具体短板。
>
> ⚠️ **一个我先说错的更正**：我曾推荐「把 sshfs 换成 `--type native`（virtiofs）」，
> 实测后发现 macOS + QEMU 上那个 `native` 其实是 **virtio-9p，而且比 sshfs 更慢**
> （元数据再慢 2.7x、大块吞吐只有 1/4）。换挂载**不是**性能优化，
> 而是"用速度换语义正确性"的取舍 —— 详见
> [挂载方案对比.md](./挂载方案对比.md)。
>
> 本文所有数字都是在本机（Apple Silicon / 12 核 / macOS 14.6）实测的，
> 不是推断；推断的部分会明确标注「未实测」。

---

## 一、推荐顺序（按"结论可信度"排）

| 排名 | 方案 | 适合什么 | 不适合什么 |
| --- | --- | --- | --- |
| 1 | **真 Linux 机器**（远程服务器 / 云 / 物理机） | 一切，尤其是**绝对数字** | 需要本地反复调试时不便 |
| 2 | **Apple Virtualization.framework 的 VM**（UTM / Lima `--vm-type=vz` / OrbStack） | 本地开发 + 相对对比，比 QEMU 更省更干净 | 绝对数字仍然不可信 |
| 3 | **QEMU VM**（本项目的 multipass） | 内核行为、系统调用、功能验证、相对对比 | 绝对数字、真实网络、PMU 剖析 |
| 4 | **容器**（Docker Desktop / Podman machine） | 跑服务、跑 CI | **最不适合做这个**：seccomp 默认拦 `io_uring_setup`（返回 `EPERM`），共享内核，"磁盘"是 virtiofs/gRPC-FUSE 挂载 |

**一句话**：VM 里的内核是**真的**，所以"内核怎么走这条路"的观察（io-wq、SQPOLL、multishot）完全有效；
但**设备是假的**，所以"这块盘有多快""这块网卡有多快"的结论不可信。

---

## 二、什么实验在 VM 里做是有效的

| | 内容 | 为什么 |
| --- | --- | --- |
| ✅ **完全可信** | API 行为与功能正确性 | 内核是真的 |
| ✅ **完全可信** | 系统调用序列（`strace -f -c`） | 内核是真的 |
| ✅ **完全可信** | 内核实现路径的差异（如 5.15 把 `O_DIRECT` 写 punt 到 io-wq、7.0 不 punt） | 内核是真的，这是**本项目最有价值的发现之一** |
| ✅ **可信（需规范）** | 相对对比：异步 vs 同步、深度换吞吐、零拷贝 ±％、SQPOLL 开关 | 前提是**交替多轮取中位** + **介质正确** |
| ✅ 可信 | 不同内核版本之间的行为对比 | 换内核 ≠ 换设备 |
| ❌ 不可信 | 绝对 IOPS / 带宽 / QPS | 宿主负载、`cache=write back`、vCPU 调度都在影响 |
| ❌ 不可信 | 真实网络 RTT / 吞吐 | 回环没有网卡；虚拟网卡也 ≠ 数据中心网络 |
| ❌ 不可信 | 中断 / 软中断的绝对开销 | 虚拟化会改变中断路径 |
| ❌ 用不了 | PMU 类指标（IPC、cache miss、分支预测） | 见下：VM 里没有 PMU |
| ❌ 用不了 | NUMA、多队列、大页、CPU 亲和性 | 这些在 guest 里看不到/做不到 |

---

## 三、multipass 这台 VM 的实测体检

```
Ubuntu 26.04 / 内核 7.0.0-30 / 8 vCPU / 15.6 GiB RAM / 29 GiB 盘
systemd-detect-virt = qemu        DMI product = QEMU Virtual Machine
```

**设备层面其实不差**（这是常被误解的一点）：

| 部件 | 实测 | 评价 |
| --- | --- | --- |
| 网卡 | `virtio_net` 1.0.0，MTU 1500 | ✅ 不是老掉牙的 e1000 + SLIRP |
| 存储控制器 | `virtio-scsi` + `virtio-blk` | ✅ 正常 |
| 磁盘 | `QEMU HARDDISK`，**cache_type = write back** | ⚠️ **写会被宿主页缓存吸收** |
| 磁盘属性 | `rotational=1`（谎报成机械盘）、`nr_requests=256` | ⚠️ 影响 guest 的调度启发式 |
| PMU | `/sys/bus/event_source/devices/` **只有软件事件** | ❌ `perf stat -e cycles` 直接报 not supported |

**所以问题不在"设备老"，而在下面几件事**：

### 3.1 磁盘 `cache=write back`，且不给你改

写请求被宿主的页缓存吸收后就返回"完成"，所以**绝对 IOPS 偏高且不可复现**。
QEMU 本身支持 `cache=none` / `directsync`，但 multipass 不暴露这个开关。
（这也是为什么本项目文档里反复强调"虚拟盘上只有相对关系可信"。）

### 3.2 没有 PMU

```
$ ls /sys/bus/event_source/devices/
breakpoint  kprobe  software  tracepoint  uprobe     ← 没有 cpu / armv8_pmuv3
```

替代方案：软件事件 `perf record -e cpu-clock -F 3000 --call-graph fp`。
能定位热点函数，但拿不到 IPC / cache miss / 分支预测这类微架构指标。

### 3.3 挂载方式：classic 是 sshfs，`--type native` 是 **9p**（且更慢）

**先纠正一个容易搞错的点**：macOS + QEMU 后端上，`multipass mount --type native`
走的是 **virtio-9p**，不是 virtiofs（换完 `findmnt` 会显示 `9p ... trans=virtio`）。

两种挂载都没有性能优势，只是短板不同（guest 7.0 上同机同时段实测）：

| 操作（相对 VM 本地盘） | native（9p） | classic（sshfs） |
| --- | --- | --- |
| 元数据 `stat` | 慢约 **1 484 倍** | 慢约 545 倍 |
| 小文件创建 | 慢约 290 倍 | 慢约 219 倍 |
| `open`+`close` | 慢约 **2 335 倍** | 慢约 1 035 倍 |
| 4KB 随机读 | 慢约 395 倍 | 慢约 283 倍 |
| 大块顺序读写 | 慢约 **55 倍**（只有 26 MB/s） | 慢约 13 倍（110 MB/s） |
| 目录遍历（目录项多） | 慢约 17 倍 | 慢约 297 倍 |
| 就地编译一个小项目 | 本地盘 2 倍 | 本地盘 2 倍 |

**native（9p）在绝大多数项目上比 sshfs 更慢**（`listdir` 是唯一例外）。
它换来的不是速度，而是：

- ✅ **没有属性缓存** —— 宿主改完文件，guest 立刻看到正确的 `size`/`mtime`
  （sshfs 有约 1 秒窗口，会让 `make` 跳过重编）；实测宿主以 300ms 间隔连写 10 个版本，
  9p 下 guest **10 个全部看到且 `stat` 与内容一致**；
- ✅ POSIX 语义完整（稀疏文件、符号链接、`fallocate` 都可用，sshfs 全都不行）；
- ❌ 代价：**不支持 `mmap`**（clangd/LSP、git packfile、ripgrep 会受影响）、
  **`inotify` 完全不工作**（连 guest 自己改都不触发），大块吞吐只有 sshfs 的 1/4。

另外，**两种挂载上都不能跑磁盘 I/O 实验** —— 把测试文件放进去，
「异步靠深度换吞吐」的结论会完全反过来（本地盘 `libaio` 是同步的 **13.0x**，
9p 上只有 **1.00x**，sshfs 上 **0.37x**）。
完整实测见 [挂载方案对比.md](./挂载方案对比.md)，测量介质审计见
[测量环境与复现.md](./测量环境与复现.md)。

### 3.4 宿主负载会直接乘到结果上

同一台 VM、同一份二进制、同一组参数，**相隔 8 小时**：

| 时刻 | 宿主机 load（12 核） | 8 线程 echo QPS |
| --- | --- | --- |
| 凌晨 01:00 | 低 | **~93,000** |
| 中午 12:35 | **12.75**（打满） | **~48,000** |

**同机同代码差 2 倍。** 这就是"VM 里的绝对值不可信"最直白的证据 ——
你测到的不是程序性能，而是「程序性能 × 宿主机当前心情」。

### 3.5 没有 CPU 亲和性

macOS 上没有 `taskset`。所以在 VM 里无法做"客户端绑 0-3 核、服务端绑 4 号核"这类
公平对比 —— 而本项目在 Linux 真机上正是靠绑核才让数字稳定的。

### 3.6 两台 VM 会互相挤

你现在跑着两台 VM（各 8 vCPU），而 Mac 只有 12 核 —— **总 vCPU 16 > 12**，
本身就是超额分配。做测量时最好只留一台在跑。

---

## 四、一个反直觉的实测：虚拟网卡其实不慢

我原本以为"在 VM 里测网络没意义"，实测打了自己的脸。
用 Mac 上的 C++ 客户端直连 VM 内的服务端（真实虚拟网卡路径：guest virtio-net → QEMU → vmnet → 宿主 IP 栈），
与"客户端也在 VM 里、走回环"交替对比 3 轮：

| | 中位 QPS |
| --- | --- |
| 跨虚机（Mac → VM，走虚拟网卡） | **42,588** |
| 回环（都在 VM 内） | **45,052** |
| 代价 | **慢约 6%**（且在噪声范围内） |

**结论**：256B ping-pong 这种小包场景下，**virtio-net + vmnet 的路径开销很小**，
瓶颈始终在服务端 CPU 上。所以"必须用回环"这个想法在这里不成立 ——
真机之间测反而更接近真实拓扑。

> 但要清楚它的边界：这只说明"虚拟网卡不像想象中那么糟"，
> 不能推出"虚拟网卡能代表真实数据中心网络"。真实网络的 RTT、丢包、拥塞、
> 中断合并、多队列，在虚拟机里通通测不到。

---

## 五、磁盘：宿主真实 SSD vs VM 虚拟盘

| 环境 | 4KB 同步写 IOPS（单线程） |
| --- | --- |
| 宿主机真实 NVMe（macOS，`F_NOCACHE`） | **70,000 ~ 81,000** |
| VM 虚拟盘（Ubuntu，ext4 on `/dev/sda1`） | **8,178 ~ 8,834** |

**约 9 倍**。而且这还是在宿主负载较低时测的。虚拟化的磁盘路径
（guest 文件系统 → virtio-blk → QEMU → 宿主文件 → 宿主文件系统 → 真盘）本身就有多层。

**所以：任何"这块盘有多快"的结论，都必须去真机上测。**

---

## 六、如果继续用 multipass：7 条调优清单

按"性价比"排序：

1. **先想清楚挂载要什么，别按名字选**（实测总结见 [挂载方案对比.md](./挂载方案对比.md)）
   - `--type native` = **virtio-9p，不是 virtiofs，也不更快**：元数据比 sshfs 再慢 2.7x、
     大块吞吐只有 1/4；换来的是**无属性缓存**（`make` 判断准确）和完整 POSIX 语义，
     代价是**没有 `mmap`、没有 `inotify`**。
   - 要 clangd / watch 模式 → 留在 `classic`（sshfs）；要 `make` 判断准确、稀疏文件、
     符号链接 → 用 `native`。
   ```bash
   # 想在这两种之间切换（需要停一次实例）
   multipass stop exact-gerbil
   multipass umount exact-gerbil:/home/ubuntu/workspace
   multipass mount --type classic /Users/ludi/workspace exact-gerbil:/home/ubuntu/workspace
   multipass start exact-gerbil
   ```
   - 两者都慢的时候，最稳的是**方案 B**：`rsync` 源码进 VM 本地盘再编译
     （见 [挂载方案对比.md](./挂载方案对比.md) 第 10 节）。
2. **磁盘测试文件放 VM 本地盘**（`/var/tmp`），**绝不放挂载目录或 `/tmp`**
   —— `bench_matrix.py` 现在会自动挑路径并在报告里标注介质。
3. **压测前先看宿主 load**：`uptime` 的第一/第二/第三个数字都应**远小于**核数。
4. **别让两台 VM 同时跑测量**（16 vCPU / 12 核 = 超额分配）。
5. **交替多轮取中位**，不要一个配置连跑三遍（本项目所有脚本都这么做）。
6. **`perf` 用软件事件**：`-e cpu-clock`，并先加 `-g -fno-omit-frame-pointer` 重编译。
7. **报结论时带上环境**：内核版本 + 介质（`findmnt`）+ 宿主负载 + 是否绑核。
   缺任何一项，别人都无法判断你的数字能不能用。

---

## 七、如果要换平台：UTM / Lima

你机器上**已经装了 UTM**（`/Applications/UTM.app`）。它支持两种模式：

- **Virtualize**（Apple Virtualization.framework）—— 推荐。更省 CPU、启动更快、
  挂载可以走 virtiofs（不需要 sshfs）、网络走 vmnet。
- **Emulate**（QEMU 软件模拟）—— 只有在跑**别的架构**（比如 x86_64 Ubuntu）时才用，
  性能损失很大，不适合做性能实验。

命令行路线（Lima）：
```bash
brew install lima
limactl start --vm-type=vz --mount-type=virtiofs --cpus=8 --memory=16 template://ubuntu
```

**但请对预期保持清醒**：换平台改善的是**干净度与便利性**；
它**不改变**"绝对数字不可信"和"没有 PMU"这两件事 ——
那是虚拟化的本质，不是 multipass 的实现问题。

> 至于**挂载性能**：UTM/Lima 走的是 Apple Virtualization.framework 的 virtiofs，
> 与 multipass 在 macOS+QEMU 上的 9p **不是同一套东西**，理论上应该好得多，
> 但**本项目没有在 UTM/Lima 上实测过**（本文只实测了 multipass 的 9p 与 sshfs）。
> 如果你换过去，建议用 [挂载方案对比.md](./挂载方案对比.md) 第 2~5 节的方法亲自测一遍，
> 别按名字假设。

**如何判断你到底需要换**：
- 只是想验证"我的 epoll/io_uring 代码写得对不对""哪个实现相对更快" → **不用换**，现在的够用。
- 需要"这块盘/这张网卡能跑多少" → **换平台也不够**，得去真机。

---

## 八、结论

| 问题 | 回答 |
| --- | --- |
| 最推荐的方式？ | **绝对数字 → 真 Linux 机器**；相对对比与内核行为 → 本地 VM 即可 |
| multipass 好吗？ | **能用，但不理想**。设备层（virtio）没问题，短板是 `cache=write back`、**没有 PMU**、**挂载慢（两种方式都慢，且 native/9p 比 sshfs 更慢）**、无 CPU 亲和性、宿主负载直接乘到结果上 |
| 最该做的改进？ | **别指望换挂载提升性能** —— 实测 `--type native`（9p）比 sshfs 还慢。真正该做的是**确保测试文件落 VM 本地盘**，以及按需在两种挂载间按语义取舍（见 [挂载方案对比.md](./挂载方案对比.md)） |
| 虚拟网卡慢吗？ | 实测**只慢约 6%**（小包 ping-pong，噪声内）—— 比想象中好 |
| 能替代真机吗？ | **不能**。磁盘差约 9 倍，且同机同代码在宿主负载变化下能差 2 倍 |
