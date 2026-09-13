#!/bin/bash
# ============================================================
#  安装 clock-guard 到本机（要 sudo）
#
#  用法（在 VM 里执行）：
#     sudo bash scripts/vm-clock-guard/install.sh
#
#  做的事：
#     1. 装脚本到 /usr/local/sbin/clock-guard
#     2. 装 systemd unit + timer 并启用（每 5 分钟一次）
#     3. 立刻跑一次，把时钟拨正
#     4. 顺手把 chrony 的 makestep 调成「永远允许跳跃」
#        （默认 `makestep 1 3` 只在启动后前 3 次允许跳跃，之后只能慢速追赶）
# ============================================================
set -euo pipefail

[ "$(id -u)" -eq 0 ] || { echo "需要 root：sudo bash $0"; exit 1; }

SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo ">>> 1/4 安装脚本到 /usr/local/sbin/clock-guard"
install -m 0755 "$SRC/clock-guard.py" /usr/local/sbin/clock-guard

echo ">>> 2/4 安装 systemd unit 与 timer"
install -m 0644 "$SRC/clock-guard.service" /etc/systemd/system/clock-guard.service
install -m 0644 "$SRC/clock-guard.timer"   /etc/systemd/system/clock-guard.timer
systemctl daemon-reload
systemctl enable --now clock-guard.timer

echo ">>> 3/4 立刻校正一次"
/usr/local/sbin/clock-guard || true

echo ">>> 4/4 让 chrony 允许「随时跳跃」"
# 这台 VM 所在网络屏蔽了 NTP，chrony 平时同步不上；
# 但如果哪天网络变了，让它一次跳到位，而不是花 20 小时慢速追赶。
if [ -f /etc/chrony/chrony.conf ]; then
    if grep -qE '^\s*makestep\s+1\s+3\s*$' /etc/chrony/chrony.conf; then
        sed -i -E 's|^\s*makestep\s+1\s+3\s*$|makestep 1.0 -1|' /etc/chrony/chrony.conf
        echo "    已把 'makestep 1 3' 改为 'makestep 1.0 -1'"
        systemctl restart chrony
    else
        echo "    (makestep 配置不是默认值，未改动：)"
        grep -E '^\s*makestep' /etc/chrony/chrony.conf | sed 's/^/      /' || echo "      (没有 makestep 行)"
    fi
fi

echo
echo "完成。检查状态："
echo "    systemctl list-timers clock-guard.timer"
echo "    journalctl -u clock-guard -n 20 --no-pager"
echo "    date -u; timedatectl"
