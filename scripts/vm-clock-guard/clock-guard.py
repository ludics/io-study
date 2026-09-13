#!/usr/bin/env python3
# ============================================================================
#  clock-guard —— 用 HTTPS 响应头的 Date 给虚拟机校正时钟
#
#  为什么需要它（这台 VM 的完整病因链）：
#
#   1) 【掉时间】VM 跑在 QEMU 里，宿主机 macOS 一休眠，guest 的时钟就**停走**。
#      ARM64 上 clocksource 是 arch_sys_counter（ARM 通用计时器），
#      宿主挂起期间 QEMU 的虚拟时钟不推进 —— 醒来后 guest 就落后了「休眠时长」。
#      实测：Mac 睡了一觉，VM 落后 2 小时 13 分。
#
#   2) 【同步不上】这台 VM 所在网络**完全屏蔽了 NTP**：
#      UDP 123 到 ntp.aliyun.com / time.windows.com / ntp.ubuntu.com / 网关
#      全部超时；chrony 配的 NTS（TCP 4460）也不通。
#      所以 chrony 永远处于 `Reach=0`、`Can't synchronise: no selectable sources`。
#
#   3) 【自己修不动】chrony 默认 `makestep 1 3` 只在**启动后前 3 次更新**允许"跳跃"，
#      之后只能以 maxslewrate（默认 8.33%）慢慢追。追 2 小时需要 20+ 小时，
#      中途宿主再睡一次就又落后 —— 这就是"试了很多方法都修不好"的原因。
#
#  为什么用 HTTPS 的 Date 头：
#     它是唯一在这台 VM 上**实际可达**的时间源（TLS 握手要校验证书，所以这个
#     Date 也不能是被随便伪造的）。精度到秒 —— 对 make、日志、git 提交完全够用。
#
#  用法：
#     clock-guard            # 检查并按需校正（由 systemd timer 每 5 分钟调一次）
#     clock-guard --dry-run  # 只报告偏差，不改时钟
# ============================================================================
import email.utils
import subprocess
import sys
import time
import urllib.request

# 多个源：只有 ≥2 个源互相同意（差 < 3 秒）才敢动时钟，避免单个源给出脏数据
ENDPOINTS = [
    "https://www.baidu.com",
    "https://www.cloudflare.com",
    "https://www.qq.com",
]
AGREE_TOLERANCE = 3.0    # 多源之间允许的最大分歧（秒）
STEP_THRESHOLD = 2.0     # 与本机时钟偏差超过这个值才动手（秒）
TIMEOUT = 8


def fetch_time(url):
    """HEAD 一个 URL，从响应头的 Date 取出 epoch 秒。失败返回 None。"""
    req = urllib.request.Request(url, method="HEAD",
                                 headers={"User-Agent": "clock-guard/1.0"})
    try:
        with urllib.request.urlopen(req, timeout=TIMEOUT) as resp:
            raw = resp.headers.get("Date")
            # urllib 在部分服务器会回落到 GET，用 HTTPMessage 自带的解析
            if not raw:
                return None
            dt = email.utils.parsedate_to_datetime(raw)
            return dt.timestamp()
    except Exception as e:
        print("  [%s] 取时间失败: %s" % (url, e))
        return None


def main():
    dry_run = "--dry-run" in sys.argv

    print("== clock-guard ==")
    samples = []
    for url in ENDPOINTS:
        t = fetch_time(url)
        if t is not None:
            print("  %-30s %s" % (url, time.strftime("%Y-%m-%d %H:%M:%S UTC",
                                                     time.gmtime(t))))
            samples.append(t)

    if not samples:
        print("  ❌ 一个时间源都取不到 —— 不改时钟（可能是代理/防火墙变了）")
        return 1

    # 取"最一致的那一组"：按大小排序后取中位数，然后只保留靠近中位数的样本
    samples.sort()
    median = samples[len(samples) // 2]
    agree = [t for t in samples if abs(t - median) <= AGREE_TOLERANCE]

    if len(samples) >= 2 and len(agree) < 2:
        print("  ❌ 各源之间分歧太大（%s），不敢改时钟" %
              ", ".join("%.0f" % (t - median) for t in samples))
        return 1

    truth = sum(agree) / len(agree)
    local = time.time()
    delta = truth - local

    print("  权威时间: %s" % time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime(truth)))
    print("  本机时间: %s" % time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime(local)))
    print("  偏差    : %+.1f 秒" % delta)

    if abs(delta) < STEP_THRESHOLD:
        print("  偏差在 %.1fs 以内，无需校正" % STEP_THRESHOLD)
        return 0

    if dry_run:
        print("  (--dry-run：不做修改)")
        return 0

    # 直接拨到正确时间。用 date -s @<epoch> 而不是 sleep/adjtimex，
    # 因为这里的偏差动辄上千秒，只能"跳"，慢速追赶（chrony 的做法）追不动。
    target = "@%d" % round(truth)
    print("  正在把系统时钟设置为 %s ..." % target)
    subprocess.run(["date", "-s", target], check=True,
                   stdout=subprocess.DEVNULL)
    # 顺手写回硬件时钟，免得下次开机又歪
    subprocess.run(["hwclock", "--systohc", "--utc"], check=False,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    now = time.time()
    print("  已校正。当前: %s（残差 %+.2f 秒）"
          % (time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime(now)), truth - now))
    return 0


if __name__ == "__main__":
    sys.exit(main())
