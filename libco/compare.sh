#!/bin/bash
# ============================================================
#  网络 Echo 三方对比：epoll(裸 Reactor) vs libco(协程) vs io_uring(Proactor)
#
#  ⚠️ 本脚本是早期版本，功能已被 scripts/bench_matrix.py 覆盖（那个更全：
#     4 种服务端 × 3 个维度，还带延迟分位和压测端 CPU 占用）。
#     保留它是因为它足够短、适合当「读代码的入口」。要正经测数据请用：
#         python3 scripts/bench_matrix.py net --mode full
#
#  用法：
#    ./compare.sh                 # 默认 4 线程 / 256B / 3 秒
#    ./compare.sh 8 512 5         # 自定义：线程数 消息大小 秒数
#
#  前置条件（在项目根目录执行）：
#    make net      # 编译 bin/echo_epoll、bin/echo_io_uring
#    make libco    # 编译 third_party/libco 并产出 example_echosvr
#
#  说明：所有路径均由脚本自身位置推导，不含任何绝对路径，项目可整体搬迁
# ============================================================
set -u

# ---- 路径推导：基于脚本自身位置，与当前工作目录无关 ----
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

BIN="${PROJECT_ROOT}/bin"
NET="${PROJECT_ROOT}/network"
BENCH="${NET}/bench.py"

# libco 有两种布局：重构版在 build/bin/ 下，上游原版在根目录
LIBCO_ECHO="${PROJECT_ROOT}/third_party/libco/build/bin/example_echosvr"
[ -x "$LIBCO_ECHO" ] || LIBCO_ECHO="${PROJECT_ROOT}/third_party/libco/example_echosvr"

EPOLL_BIN="${BIN}/echo_epoll"
URING_BIN="${BIN}/echo_io_uring"

# ---- 参数 ----
THREADS="${1:-4}"
SIZE="${2:-256}"
SECS="${3:-3}"

BASE_PORT="${BASE_PORT:-19050}"
PORT=$BASE_PORT
LOGDIR="$(mktemp -d /tmp/compare_echo.XXXXXX)"
SERVER_PID=""

cleanup() {
    [ -n "${SERVER_PID}" ] && kill -9 "${SERVER_PID}" 2>/dev/null
    return 0
}
trap cleanup EXIT

have() { command -v "$1" >/dev/null 2>&1; }

start_server() {
    local log="$1"; shift
    "$@" > "$log" 2>&1 &
    SERVER_PID=$!
    sleep 1.0
    kill -0 "${SERVER_PID}" 2>/dev/null
}

stop_server() {
    [ -n "${SERVER_PID}" ] && kill -9 "${SERVER_PID}" 2>/dev/null
    wait "${SERVER_PID}" 2>/dev/null
    SERVER_PID=""
    sleep 0.3
    return 0
}

# run_test <名称> <服务端命令...>   命令中的 {PORT} 会被替换为实际端口
run_test() {
    local name="$1"; shift
    local log="${LOGDIR}/srv_${name}.log"
    local blog="${LOGDIR}/bench_${name}.log"
    local args=()
    local a

    PORT=$((PORT + 1))
    for a in "$@"; do args+=("${a//\{PORT\}/$PORT}"); done

    if ! start_server "$log" "${args[@]}"; then
        echo "--- $name : [跳过] 服务端启动失败 ---"
        sed -n '1,2p' "$log" | sed 's/^/      /'
        SERVER_PID=""
        return 0
    fi

    python3 "$BENCH" "$PORT" "$THREADS" "$SIZE" "$SECS" > "$blog" 2>&1
    stop_server

    echo "--- $name (port $PORT) ---"
    if [ -s "$blog" ]; then
        grep -E "总请求数|平均 QPS|QPS|requests" "$blog" | sed 's/^/    /' \
          || sed -n '1,5p' "$blog" | sed 's/^/    /'
    else
        echo "    (无输出)"
    fi
    return 0
}

# ================= 主流程 =================
echo "======================================"
echo "  Echo 三方对比"
echo "  epoll(Reactor) vs libco(协程) vs io_uring(Proactor)"
echo "  线程=${THREADS}  消息=${SIZE}B  时长=${SECS}s"
echo "======================================"
echo ""
echo "项目根目录: ${PROJECT_ROOT}"

if ! have python3; then
    echo "[错误] 未找到 python3，bench.py 无法运行"
    exit 1
fi
if [ ! -f "$BENCH" ]; then
    echo "[错误] 未找到压测脚本 $BENCH"
    exit 1
fi

MISSING=0
[ -x "$EPOLL_BIN" ]  || { echo "[提示] 缺少 $EPOLL_BIN  -> 请先执行 make net";  MISSING=1; }
[ -x "$URING_BIN" ]  || { echo "[提示] 缺少 $URING_BIN -> 请先执行 make net";   MISSING=1; }
[ -x "$LIBCO_ECHO" ] || { echo "[提示] 缺少 $LIBCO_ECHO -> 请先执行 make libco"; MISSING=1; }
[ "$MISSING" -eq 1 ] && echo ""
echo ""

echo "【场景1】2 线程 / 128 字节 / ${SECS}s"
[ -x "$EPOLL_BIN" ]  && run_test epoll "$EPOLL_BIN" "{PORT}"
[ -x "$LIBCO_ECHO" ] && run_test libco "$LIBCO_ECHO" 127.0.0.1 "{PORT}" 10 1
[ -x "$URING_BIN" ]  && run_test uring "$URING_BIN" "{PORT}"

echo ""
echo "【场景2】4 线程 / 256 字节 / ${SECS}s"
[ -x "$EPOLL_BIN" ]  && run_test epoll2 "$EPOLL_BIN" "{PORT}"
[ -x "$LIBCO_ECHO" ] && run_test libco2 "$LIBCO_ECHO" 127.0.0.1 "{PORT}" 10 1
[ -x "$URING_BIN" ]  && run_test uring2 "$URING_BIN" "{PORT}"

echo ""
echo "【场景3】8 线程 / 512 字节 / ${SECS}s"
[ -x "$EPOLL_BIN" ]  && run_test epoll3 "$EPOLL_BIN" "{PORT}"
[ -x "$LIBCO_ECHO" ] && run_test libco3 "$LIBCO_ECHO" 127.0.0.1 "{PORT}" 10 1
[ -x "$URING_BIN" ]  && run_test uring3 "$URING_BIN" "{PORT}"

echo ""
echo "完成。服务端日志目录：$LOGDIR"
