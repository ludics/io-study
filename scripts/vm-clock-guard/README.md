# vm-clock-guard —— 虚拟机时钟校正

一句话：**这台 VM 所在网络屏蔽了 NTP，而宿主机一休眠 VM 的时钟就停走，所以 `chrony` 永远修不好。
这个脚本改用 HTTPS 响应头的 `Date` 做时间源，每 5 分钟兜底校正一次。**

---

## 一、病因链（三段，缺一不可）

### 1. 宿主休眠 → VM 时钟停走

VM 跑在 QEMU 里，ARM64 上的 clocksource 是 `arch_sys_counter`（ARM 通用计时器）。
**宿主机 macOS 休眠时 QEMU 的虚拟时钟不推进**，guest 醒来后就落后了「休眠时长」。

实测：Mac 睡了一觉，VM 落后 **2 小时 13 分 41 秒**。

> 判据（别信 `date`，要用第三方权威时间）：
> ```bash
> curl -sI https://www.baidu.com | grep -i '^date:'
> # Sun, 13 Sep 2026 06:56:30 GMT   ← 权威
> # 宿主 Mac : 06:56:31  ✅
> # VM       : 04:42:50  ❌ 慢 2h13m41s
> ```

### 2. 网络屏蔽 NTP → 同步不上

```bash
chronyc sources -v
# MS Name/IP address      Stratum Poll Reach LastRx Last sample
# ^? ntp-nts-2.ps5.canonical.>  2   10    0   176m   ← Reach=0：一个包都没收到
# ^? ntp-nts-3.ps5.canonical.>  2   10    0   167m
# ^? ntp-nts-2.ps6.canonical.>  2   10    0   178m

journalctl -u chrony | tail
# chronyd: Can't synchronise: no selectable sources (5 unreachable sources)
```

逐一手测出口 NTP（UDP 123），**全部超时**：

| 目标 | 结果 |
| --- | --- |
| `ntp.aliyun.com` | ❌ Timeout |
| `time.windows.com` | ❌ Timeout |
| `ntp.ubuntu.com` | ❌ Timeout |
| 网关 `192.168.252.1` | ❌ Timeout |

chrony 用的是 **NTS（TCP 4460）**，同样不通。

### 3. chrony 自己「修不动」

Ubuntu 默认配置是：

```
makestep 1 3
```

含义：**只在启动后的前 3 次时钟更新里**允许「跳跃」（step），之后只能按
`maxslewrate`（默认 83333 ppm ≈ 8.33%）**慢速追赶**。

追 2 小时 13 分要多久？`8012s ÷ 0.0833 ≈ 96000s ≈ 26 小时`。
**而宿主每睡一次就再落后几小时** —— 所以看起来「怎么都修不好」。

> 这解释了为什么换时区、重启 NTP 服务、`ntpdate` 各种尝试都无效：
> **时区只影响显示**（`date -u` 一样错），**服务重启也连不上源**，**慢速追赶追不上**。

### 附：为什么不用 RTC（硬件时钟）

```bash
sudo hwclock -r --utc
# 2026-09-13 04:46:23   ← 和错误的系统时间一模一样
```

QEMU 的 pl031 RTC 虽然底层是宿主时间，但 **chrony 的 `rtcsync` 每 11 分钟把系统时间写进去**，
所以它已经被写脏了，读出来也是错的。（这也是为什么"从 RTC 恢复"这条路走不通。）

---

## 二、方案：用 HTTPS 的 `Date` 头做时间源

唯一在这台 VM 上**实测可达**的权威时间就是 HTTPS 响应的 `Date` 头：

```bash
curl -sI https://www.baidu.com | grep -i '^date:'
curl -sI https://www.cloudflare.com | grep -i '^date:'
# 两者一致，且 TLS 握手本身要校验证书，所以这个值也不能被随便伪造
```

`clock-guard.py` 做的事：

1. 向 3 个 HTTPS 端点各取一次 `Date`（HEAD 请求，几毫秒）
2. **要求 ≥2 个源互相一致**（差 < 3 秒）才动手 —— 避免单个源给出脏数据
3. 与本机时钟比较，**偏差 > 2 秒才跳**（避免无意义的小幅调整）
4. `date -s @<epoch>` 跳到正确时间，再 `hwclock --systohc --utc` 写回 RTC
5. 每次运行都把「权威时间 / 本机时间 / 偏差」打进 journal，便于回看

```bash
clock-guard --dry-run    # 只看偏差，不改时钟
```

---

## 三、安装

```bash
# 在 VM 里执行（要 sudo）
sudo bash scripts/vm-clock-guard/install.sh
```

安装脚本会：

1. 把 `clock-guard.py` 装到 `/usr/local/sbin/clock-guard`
2. 装 systemd unit + timer 并启用（`OnBootSec=30s` + 每 5 分钟）
3. 立刻跑一次，把时钟拨正
4. 把 chrony 的 `makestep 1 3` 改成 **`makestep 1.0 -1`**
   —— 「永远允许跳跃」。平时用不上（网络屏蔽 NTP），但哪天网络变了，
   它一次就能跳到位，而不是花 20 小时慢速追。

验证：

```bash
systemctl list-timers clock-guard.timer
journalctl -u clock-guard -n 20 --no-pager
date -u; timedatectl
```

---

## 三点五、装完之后，有两个「看起来还是不对」其实是对的

### 1. `timedatectl` 仍然显示 `System clock synchronized: no`

```
$ timedatectl
System clock synchronized: no      ← 不是没修好
              NTP service: active
```

这个字段反映的是 **chrony 有没有通过 NTP 同步** —— 而 NTP 在这张网络里根本不通，
所以它永远是 `no`。时钟的正确性由 `clock-guard` 保证，与这个字段无关。
**判断时钟对不对，看 `date -u` 和第三方时间源的对比，不要看这个字段。**

（如果你嫌这个 `no` 碍眼，唯一"修"它的办法是让 chrony 真正拿到一个时间源，
比如在宿主机上跑 NTP 服务 —— 见 §五。）

### 2. 残差固定是 −1.00 秒

`clock-guard` 每次报告 `残差 -1.00 秒`，这是**正常的**，不是没对准：

- HTTP 的 `Date` 头只有**秒级**精度，取到的是整数秒；
- `date -s @<整数>` 也只能设到整秒；
- 所以残差必然落在 −1 ~ 0 秒之间；

因为校正阈值是 2 秒，这个残差**不会触发反复校正**，系统是稳定的。
对 `make`、日志、git 提交来说，1 秒完全够用。

---

## 四、关于时区（这不是时区问题）

`.3` 原本的时区是 `Etc/UTC`：

```
Time zone      : Etc/UTC (UTC, +0000)
RTC in local TZ: no          ← 正确（硬件时钟存 UTC）
```

**这个配置本身没有错。** 但要注意两件事：

1. **时区错误只会造成整 8 小时的偏差**（CST vs UTC），不会造成 2h13m41s ——
   而且时区只影响显示，`date -u` 在任何时区下都该一致。
   所以「2 小时 13 分」这个偏差**与内存时区无关**，是时钟真的停了。
2. **它会让两台 VM 看起来时间不一致**：`.2` 是 `Asia/Shanghai`（显示 CST），
   `.3` 是 `Etc/UTC`，放在一起看就像"差了 8 小时"。

本次已统一为 `Asia/Shanghai`（与 `.2` 和宿主机一致），这样跨 VM 对比日志/报表
时间戳（本项目的 `results/` 日报）才不会混乱：

```bash
sudo timedatectl set-timezone Asia/Shanghai   # 本次执行的就是这句
# 想改回 UTC：sudo timedatectl set-timezone Etc/UTC
```

---

## 四点五、本次实操记录

在 `192.168.252.3` 与 `192.168.252.2` 上各执行了一次，实际偏差与结果：

| 实例 | 校正前偏差 | 校正后 |
| --- | --- | --- |
| `192.168.252.3`（Ubuntu 26.04 / 内核 7.0） | **+8020.1 秒**（2h13m40s） | 与权威时间一致 |
| `192.168.252.2`（Ubuntu 22.04 / 内核 5.15） | **+7528.4 秒**（2h05m28s） | 与权威时间一致 |

**两台 VM 症状完全一样** —— 同一台 Mac、同一套 QEMU、同一张屏蔽 NTP 的网络。

故障演练（确认它真的能自动纠偏）：故意把 `.3` 的时钟拨慢 17 分钟，
然后 `sudo systemctl start clock-guard` → 检测到 `+1019.8 秒` 偏差并自动校正。
`make` 的 `Clock skew detected` 警告随之消失（实测从 15 行降到 **0 行**）。

### 额外发现：两台 VM 的**时区**本来就不一样

修完时钟后核对发现：

| 实例 | 时区 | `date` 显示 |
| --- | --- | --- |
| `192.168.252.2` | `Asia/Shanghai` | 15:02:06 CST |
| `192.168.252.3` | `Etc/UTC` | **07:02:06 UTC** ← 看起来"差 8 小时" |
| 宿主机 Mac | `Asia/Shanghai` | 15:02:07 CST |

**这不是时钟问题，是显示时区不同** —— 两者的 UTC 时间完全一致。
但它很容易被误判成"时间又不对了"（尤其两台 VM 放一起对比时）。

如果跨 VM 对比日志/报表时间戳（本项目的 `results/` 日报就是这样），建议统一：

```bash
sudo timedatectl set-timezone Asia/Shanghai
```

本次已把 `.3` 统一为 `Asia/Shanghai`，与 `.2` 和宿主机一致。
（`RTC in local TZ` 保持 `no` —— 硬件时钟存 UTC 是正确做法，别改。）

---

## 五、其他 VM 上要不要装

同一台 Mac 上的其他 multipass 实例（比如 `192.168.252.2`）**症状完全一样**，
因为它们共享同一套 QEMU + 同一张会屏蔽 NTP 的网络。装法相同：

```bash
sudo bash scripts/vm-clock-guard/install.sh
```

**更彻底的根治办法**（二选一，都需要改宿主机）：

- 让 Mac 在做实验期间不睡：`caffeinate -i` 或改「节能」设置；
- 或者在 Mac 上跑一个 NTP 服务，让 VM 指向它（需要改宿主机配置，
  本项目没有采用，因为改宿主机的影响面更大）。
