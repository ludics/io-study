#!/bin/bash
# ============================================================
#  一键跑通全部实验（按推荐顺序）
#
#  用法：
#    bash scripts/run_all_bench.sh           # 全部实验
#    bash scripts/run_all_bench.sh disk      # 只跑磁盘 I/O
#    bash scripts/run_all_bench.sh demo      # 只跑 epoll O_NONBLOCK 实验
#    bash scripts/run_all_bench.sh net       # 只跑网络 echo 对比
#    bash scripts/run_all_bench.sh matrix    # 只跑网络多维矩阵对比（quick/full 见下）
#
#  说明：所有路径基于脚本自身位置推导，可任意搬迁
# ============================================================
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
OUTDIR="${PROJECT_ROOT}/results"
mkdir -p "$OUTDIR"
STAMP="$(date +%Y%m%d_%H%M%S)"
LOG="${OUTDIR}/bench_${STAMP}.log"

MODE="${1:-all}"

cd "$PROJECT_ROOT"

banner() {
    echo ""
    echo "############################################################"
    echo "# $1"
    echo "############################################################"
}

# 全部输出同时打到屏幕和日志文件
exec > >(tee -a "$LOG") 2>&1

echo "io-study 一键实验"
echo "开始时间 : $(date '+%F %T')"
echo "项目目录 : $PROJECT_ROOT"
echo "结果日志 : $LOG"

# ---------- 0. 环境体检 ----------
if [ "$MODE" = "all" ] || [ "$MODE" = "check" ]; then
    banner "步骤 0 / 6：环境体检"
    bash scripts/check_env.sh
fi

# ---------- 1. 编译 ----------
if [ "$MODE" != "check" ]; then
    banner "步骤 1 / 6：编译"
    make clean >/dev/null 2>&1
    make 2>&1 | tail -20
fi

# ---------- 2. 磁盘 I/O ----------
if [ "$MODE" = "all" ] || [ "$MODE" = "disk" ]; then
    banner "步骤 2 / 6：磁盘 I/O 对比（sync / libaio / io_uring）"

    echo "--- 2.1 buffered I/O（走 page cache）---"
    make bench DIRECT=0 TOTAL="${TOTAL:-65536}" BLOCK="${BLOCK:-4096}" DEPTH="${DEPTH:-32}"

    echo ""
    echo "--- 2.2 O_DIRECT（绕过 page cache，看真实磁盘能力）---"
    make bench DIRECT=1 TOTAL="${TOTAL:-65536}" BLOCK="${BLOCK:-4096}" DEPTH="${DEPTH:-32}"
fi

# ---------- 3. epoll O_NONBLOCK ----------
if [ "$MODE" = "all" ] || [ "$MODE" = "demo" ]; then
    banner "步骤 3 / 6：epoll 为什么必须 O_NONBLOCK"
    make bench_demo
fi

# ---------- 4. 网络 echo ----------
if [ "$MODE" = "all" ] || [ "$MODE" = "net" ]; then
    banner "步骤 4 / 6：网络 Echo 对比（epoll vs io_uring）"
    if [ -x "${PROJECT_ROOT}/bin/echo_io_uring" ]; then
        make bench_net PORT="${PORT:-18800}" THREADS="${THREADS:-4}" SIZE="${SIZE:-256}" SECS="${SECS:-5}"
    else
        echo "未编译 echo_io_uring，跳过（网络对比需要 make net 成功）"
    fi
fi

# ---------- 5. 网络多维矩阵 ----------
if [ "$MODE" = "all" ] || [ "$MODE" = "matrix" ]; then
    banner "步骤 5 / 6：网络 I/O 多维度矩阵对比"
    python3 scripts/bench_matrix.py net --mode "${MATRIX_MODE:-quick}"
fi

# ---------- 6. 磁盘多维矩阵 ----------
if [ "$MODE" = "all" ] || [ "$MODE" = "disk_matrix" ]; then
    banner "步骤 6 / 6：磁盘 I/O 多维度矩阵对比"
    python3 scripts/bench_matrix.py disk --mode "${MATRIX_MODE:-quick}"
fi

echo ""
echo "=========================================="
echo "  全部完成：$(date '+%F %T')"
echo "  日志：$LOG"
echo "=========================================="
