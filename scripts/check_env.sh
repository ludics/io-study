#!/bin/bash
# ============================================================
#  环境体检：确认本机能否跑通 sync / libaio / io_uring
#
#  用法：  bash scripts/check_env.sh
#
#  说明：所有路径基于脚本自身位置推导，可任意搬迁
# ============================================================
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

OK="\033[32mOK\033[0m"
NO="\033[31mNO\033[0m"
WARN="\033[33mWARN\033[0m"

printf_color() { printf "$1 %b\n" "$2"; }

echo "=========================================="
echo "  环境体检"
echo "=========================================="

# ---------- 基础信息 ----------
echo ""
echo "[系统]"
echo "  内核版本 : $(uname -r)"
echo "  架构     : $(uname -m)"
if [ -f /etc/os-release ]; then
    . /etc/os-release
    echo "  发行版   : ${PRETTY_NAME:-unknown}"
fi
echo "  CPU 核数 : $(nproc)"
echo "  内存     : $(free -h 2>/dev/null | awk '/^Mem:/{print $2}' || echo unknown)"

# 内核主次版本
KVER_MAJ=$(uname -r | cut -d. -f1)
KVER_MIN=$(uname -r | cut -d. -f2 | cut -d- -f1)

# ---------- 工具链 ----------
echo ""
echo "[工具链]"
for t in gcc g++ make python3 git strace; do
    if command -v $t >/dev/null 2>&1; then
        printf "  %-10s : " "$t"; printf_color "$OK" "$(command -v $t)"
    else
        printf "  %-10s : " "$t"; printf_color "$NO" "未安装"
    fi
done

# ---------- 开发库 ----------
echo ""
echo "[开发库]"
if [ -f /usr/include/libaio.h ]; then
    printf "  libaio 头文件   : "; printf_color "$OK" "/usr/include/libaio.h"
else
    printf "  libaio 头文件   : "; printf_color "$NO" "未找到 -> apt install libaio-dev / yum install libaio-devel"
fi

if [ -f /usr/include/liburing.h ]; then
    printf "  liburing 头文件 : "; printf_color "$OK" "/usr/include/liburing.h"
elif [ -f /usr/local/include/liburing.h ]; then
    printf "  liburing 头文件 : "; printf_color "$OK" "/usr/local/include/liburing.h"
else
    printf "  liburing 头文件 : "; printf_color "$NO" "未找到 -> apt install liburing-dev / yum install liburing-devel"
fi

# ---------- io_uring 可用性 ----------
echo ""
echo "[io_uring 可用性]"

# 1) 内核版本门槛：io_uring 5.1+，建议 5.10+
if [ "$KVER_MAJ" -gt 5 ] || { [ "$KVER_MAJ" -eq 5 ] && [ "$KVER_MIN" -ge 1 ]; }; then
    printf "  内核版本门槛   : "; printf_color "$OK" "$(uname -r) (>= 5.1)"
else
    printf "  内核版本门槛   : "; printf_color "$NO" "$(uname -r) < 5.1，不支持 io_uring"
fi

# 2) sysctl 开关（5.12+ 才有）
if [ -f /proc/sys/kernel/io_uring_disabled ]; then
    V=$(cat /proc/sys/kernel/io_uring_disabled)
    printf "  sysctl 开关    : "
    case "$V" in
        0) printf_color "$OK" "io_uring_disabled=0（完全允许）" ;;
        1) printf_color "$WARN" "io_uring_disabled=1（仅 CAP_SYS_ADMIN 可用）" ;;
        2) printf_color "$NO" "io_uring_disabled=2（全局禁用）" ;;
        *) printf_color "$WARN" "io_uring_disabled=$V" ;;
    esac
else
    printf "  sysctl 开关    : "; printf_color "$WARN" "内核未导出（<5.12 或编译期关闭 CONFIG_SYSCTL）"
fi

# 3) 实测 io_uring_setup 系统调用（syscall number 425 on x86_64）
printf "  setup 实测     : "
TMPDIR_CHK="$(mktemp -d /tmp/iourchk.XXXXXX)"
cat > "$TMPDIR_CHK/t.c" <<'EOF'
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/syscall.h>
#ifndef __NR_io_uring_setup
#define __NR_io_uring_setup 425
#endif
int main(void) {
    long r = syscall(__NR_io_uring_setup, 8, 0);
    if (r < 0) {
        printf("不可用 (errno=%d: %s)\n", errno, strerror(errno));
        return 1;
    }
    printf("可用 (ring fd=%ld)\n", r);
    return 0;
}
EOF
if gcc -o "$TMPDIR_CHK/t" "$TMPDIR_CHK/t.c" 2>/dev/null; then
    "$TMPDIR_CHK/t"
    RC=$?
else
    echo "编译失败（无 gcc?）"
    RC=2
fi
rm -rf "$TMPDIR_CHK"

if [ "$RC" -ne 0 ]; then
    echo ""
    echo "  io_uring 不可用的常见原因："
    echo "    1) 容器 seccomp 默认拦截 io_uring_setup"
    echo "       docker run 加 --security-opt seccomp=unconfined"
    echo "    2) 内核 < 5.1 或编译未开 CONFIG_IO_URING"
    echo "    3) /proc/sys/kernel/io_uring_disabled = 2"
    echo "    4) 云厂商定制内核裁剪"
    echo ""
    echo "  不影响：sync / libaio / epoll 全部实验仍可正常进行"
fi

# ---------- 磁盘 ----------
echo ""
echo "[磁盘]"
DF=$(df -h /tmp 2>/dev/null | awk 'NR==2{print $2" total, "$4" avail"}')
echo "  /tmp 空间   : ${DF:-unknown}"
if [ -b /dev/sda ]; then
    echo "  块设备示例 : /dev/sda（O_DIRECT 测试可用真实块设备或文件）"
fi

# ---------- 端口 ----------
echo ""
echo "[网络]"
for p in 18800 19050; do
    if (exec 3<>/dev/tcp/127.0.0.1/$p) 2>/dev/null; then
        exec 3<&- 3>&-
        printf "  端口 %-6s : " "$p"; printf_color "$WARN" "已被占用，压测时请换端口"
    else
        printf "  端口 %-6s : " "$p"; printf_color "$OK" "空闲"
    fi
done

echo ""
echo "=========================================="
echo "  体检完成"
echo "=========================================="
echo ""
echo "下一步："
echo "  make            # 编译全部"
echo "  make bench      # 磁盘 I/O 对比（先 buffered）"
echo "  make bench DIRECT=1   # 再跑 O_DIRECT 真盘对比"
echo "  make bench_demo # epoll O_NONBLOCK 实验"
