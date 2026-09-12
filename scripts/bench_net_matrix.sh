#!/bin/bash
# ============================================================
#  网络 I/O 多维度对比压测（矩阵实验）
#
#  回答「不同情况下网络 I/O 的性能差多少」，沿三个维度扫描：
#    A. 并发连接数   连接数 1 → 数百，看单/多 Reactor 的扩展性
#    B. 消息大小     16B → 16KB，看小包/大包的差异（系统调用 vs 内存拷贝主导）
#    C. TCP_NODELAY  开/关，看 Nagle 聚合对小包 RTT 的影响
#
#  每个维度都横跨全部可用服务端：
#    epoll 单线程(Reactor) / epoll 多线程(多 Reactor) / io_uring(Proactor) / libco(协程)
#
#  用法：
#    bash scripts/bench_net_matrix.sh            # quick（默认，约 3~5 分钟）
#    bash scripts/bench_net_matrix.sh full       # full（更细的扫点，约 10~20 分钟）
#
#  可调环境变量：
#    SECS=2            每个测点的有效测量时长（秒），默认 2
#    WARMUP=1          每个测点的预热时长（秒），默认 1
#    CLIENT_THREADS=8  压测客户端线程数，默认 8
#    CLIENT=py|cpp     压测客户端：py = network/bench.py（有延迟分位/CPU 占用，但受 Python GIL
#                      限制，单进程约 1 核就到顶，可能掩盖服务端差异）；
#                      cpp = bin/bench_client（C++ 多线程，无 GIL，测服务端上限更可靠，
#                      但只有 QPS，没有延迟分位）。默认 py
#    MT_WORKERS=4      echo_epoll_mt 的 worker 数（默认交给服务端自己取 nproc）
#                      注意：核数不多时，「客户端线程 + 服务端 worker」超过核数会互相抢 CPU，
#                      对比会失真。8 核机器建议 CLIENT_THREADS=4 MT_WORKERS=4
#                      更彻底的做法是用 taskset 把两侧绑到不相交的核上
#    BASE_PORT=19200   起始端口
#
#  产物（results/ 下）：
#    net_matrix_<模式>_<时间戳>.md    对比表格（含自动观察）
#    net_matrix_<模式>_<时间戳>.csv   原始数据
#    net_matrix_<模式>_<时间戳>.log   完整控制台日志
#
#  依赖：make net（编译服务端）、python3（压测客户端 + 报表）
#  说明：所有路径由脚本自身位置推导，可整体搬迁
# ============================================================
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

BIN="${PROJECT_ROOT}/bin"
NET="${PROJECT_ROOT}/network"
TP="${PROJECT_ROOT}/third_party"
BENCH="${NET}/bench.py"
CPP_BIN="${BIN}/bench_client"
OUTDIR="${PROJECT_ROOT}/results"
LIBCO_ECHO="${TP}/libco/example_echosvr"

# bench_client 的输出若直接进管道会被全缓冲，timeout 杀掉进程时缓冲区丢失，故强制行缓冲
STDBUF=""
command -v stdbuf >/dev/null 2>&1 && STDBUF="stdbuf -oL"

MODE="${1:-quick}"
SECS="${SECS:-2}"
WARMUP="${WARMUP:-1}"
CLIENT_THREADS="${CLIENT_THREADS:-8}"
CLIENT_MODE="${CLIENT:-py}"
MT_WORKERS="${MT_WORKERS:-}"
BASE_PORT="${BASE_PORT:-19200}"

if [ "$CLIENT_MODE" != "py" ] && [ "$CLIENT_MODE" != "cpp" ]; then
  echo "[错误] CLIENT 只能是 py 或 cpp（当前：$CLIENT_MODE）"
  exit 2
fi

PORT="$BASE_PORT"
SERVER_PID=""
LOGDIR="$(mktemp -d /tmp/net_matrix.XXXXXX)"
STAMP="$(date +%Y%m%d_%H%M%S)"
mkdir -p "$OUTDIR"
CSV="${OUTDIR}/net_matrix_${MODE}_${STAMP}.csv"
MD="${OUTDIR}/net_matrix_${MODE}_${STAMP}.md"
LOG="${OUTDIR}/net_matrix_${MODE}_${STAMP}.log"

# ---- 维度定义：每行是 "<值1> <值2> ..." ----
case "$MODE" in
  quick)
    A_CONNS="1 8 64";            A_SIZE=256
    B_SIZES="64 512 4096";       B_CONNS=64
    C_NDS="0 1";                 C_CONNS=64; C_SIZE=64
    ;;
  full)
    A_CONNS="1 2 8 32 128 512";  A_SIZE=256
    B_SIZES="16 64 256 1024 4096 16384"; B_CONNS=128
    C_NDS="0 1";                 C_CONNS=128; C_SIZE=64
    ;;
  *)
    echo "用法: $0 [quick|full]"
    exit 2
    ;;
esac

# ---- 服务端登记 ----
SERVER_ORDER="epoll epoll_mt uring libco"

server_bin() {
  case "$1" in
    epoll)    echo "${BIN}/echo_epoll" ;;
    epoll_mt) echo "${BIN}/echo_epoll_mt" ;;
    uring)    echo "${BIN}/echo_io_uring" ;;
    libco)    echo "${LIBCO_ECHO}" ;;
    *)        echo "" ;;
  esac
}

server_label() {
  case "$1" in
    epoll)    echo "epoll 单线程 (Reactor)" ;;
    epoll_mt) echo "epoll 多线程 (多 Reactor)" ;;
    uring)    echo "io_uring (Proactor)" ;;
    libco)    echo "libco (协程)" ;;
    *)        echo "$1" ;;
  esac
}

# 控制台用纯 ASCII 名，避免 CJK 宽度导致 printf 对不齐
server_short() {
  case "$1" in
    epoll)    echo "epoll-single" ;;
    epoll_mt) echo "epoll-multi" ;;
    uring)    echo "io_uring" ;;
    libco)    echo "libco" ;;
    *)        echo "$1" ;;
  esac
}

# 全部输出同时打到屏幕和日志
exec > >(tee -a "$LOG") 2>&1

echo "============================================================"
echo "  网络 I/O 多维度对比压测"
echo "  模式=${MODE}  每点时长=${SECS}s  预热=${WARMUP}s  客户端=${CLIENT_MODE}(${CLIENT_THREADS} 线程)  epoll_mt worker=${MT_WORKERS:-自动(nproc)}"
echo "  开始时间: $(date '+%F %T')"
echo "  项目目录: ${PROJECT_ROOT}"
echo "============================================================"
echo ""

for k in $SERVER_ORDER; do
  b="$(server_bin "$k")"
  [ -x "$b" ] || echo "[提示] 缺少 $(server_label "$k")：$b（该列将标记 N/A）"
done
echo ""

if ! command -v python3 >/dev/null 2>&1; then
  echo "[错误] 未找到 python3"
  exit 1
fi
if [ ! -f "$BENCH" ]; then
  echo "[错误] 未找到压测脚本 $BENCH"
  exit 1
fi

cleanup() {
  [ -n "${SERVER_PID}" ] && kill -9 "${SERVER_PID}" 2>/dev/null
  return 0
}
trap cleanup EXIT

start_server() { # start_server <server-key> <port> <nodelay> <logfile>
  local key="$1" port="$2" nd="$3" log="$4" bin
  bin="$(server_bin "$key")"
  case "$key" in
    libco)
      env ECHO_NODELAY="$nd" "$bin" 127.0.0.1 "$port" 10 1 >"$log" 2>&1 &
      ;;
    epoll_mt)
      if [ -n "${MT_WORKERS}" ]; then
        env ECHO_NODELAY="$nd" "$bin" "$port" "$MT_WORKERS" >"$log" 2>&1 &
      else
        env ECHO_NODELAY="$nd" "$bin" "$port" >"$log" 2>&1 &
      fi
      ;;
    *)
      env ECHO_NODELAY="$nd" "$bin" "$port" >"$log" 2>&1 &
      ;;
  esac
  SERVER_PID=$!
}

stop_server() {
  [ -n "${SERVER_PID}" ] && kill -9 "${SERVER_PID}" 2>/dev/null
  wait "${SERVER_PID}" 2>/dev/null
  SERVER_PID=""
  sleep 0.2
  return 0
}

# 等端口可连（就绪探测），最多 5 秒。用 python3 探测以兼容各平台 shell
# （部分 macOS 自带 bash 3.2 不支持 /dev/tcp）
wait_port() {
  local port="$1" i
  for i in $(seq 1 50); do
    if python3 -c "
import socket, sys
s = socket.socket()
s.settimeout(0.2)
try:
    s.connect(('127.0.0.1', $port))
except OSError:
    sys.exit(1)
else:
    sys.exit(0)
finally:
    s.close()
" 2>/dev/null; then
      return 0
    fi
    sleep 0.1
  done
  return 1
}

echo "dim,server,conns,size,nodelay,total,qps,p50_us,p99_us,max_us,errors,client_cpu_pct,ncpu" > "$CSV"

# measure_cpp <port> <conns> <size> —— 用 C++ 客户端测，输出与 bench.py 相同列序的字段串。
# 只有 QPS（取每秒 QPS 的中位数，丢掉第 1 秒作为预热），延迟列填 0（报表里显示 N/A）。
measure_cpp() {
  local port="$1" conns="$2" size="$3" lines qps total
  if [ ! -x "$CPP_BIN" ]; then
    echo "0 0 0 0 0 -4 0 0"
    return 0
  fi
  lines="$(timeout $((SECS + 3)) $STDBUF "$CPP_BIN" "$port" "$conns" "$size" 2>/dev/null \
             | awk '/^QPS:/ {print $2}' | tail -n "$SECS")"
  qps="$(printf '%s\n' "$lines" | grep -E '^[0-9]+$' | sort -n \
         | awk '{a[NR]=$1} END {if (NR==0) print 0; else print a[int((NR+1)/2)]}')"
  [ -n "$qps" ] || qps=0
  total=$((qps * SECS))
  echo "$total $qps 0 0 0 0 0 0"
}

# run_one <维度> <服务端key> <连接数> <消息字节> <nodelay>
run_one() {
  local dim="$1" key="$2" conns="$3" size="$4" nd="$5"
  local bin; bin="$(server_bin "$key")"

  if [ ! -x "$bin" ]; then
    printf "  [%s] %-14s conns=%-4s size=%-5s nodelay=%s -> N/A (未编译)\n" \
      "$dim" "$(server_short "$key")" "$conns" "$size" "$nd"
    echo "${dim},${key},${conns},${size},${nd},0,0,0,0,0,-1,0,0" >> "$CSV"
    return 0
  fi

  PORT=$((PORT + 1))
  local slog="${LOGDIR}/srv_${dim}_${key}_${conns}_${size}_${nd}.log"
  start_server "$key" "$PORT" "$nd" "$slog"

  if ! wait_port "$PORT"; then
    printf "  [%s] %-14s conns=%-4s size=%-5s nodelay=%s -> 启动失败\n" \
      "$dim" "$(server_short "$key")" "$conns" "$size" "$nd"
    sed -n '1,2p' "$slog" | sed 's/^/        /'
    stop_server
    echo "${dim},${key},${conns},${size},${nd},0,0,0,0,0,-2,0,0" >> "$CSV"
    return 0
  fi

  local out fields
  if [ "$CLIENT_MODE" = "cpp" ]; then
    fields="$(measure_cpp "$PORT" "$conns" "$size")"
  else
    out="$(python3 "$BENCH" "$PORT" "$CLIENT_THREADS" "$size" "$SECS" \
             --conns "$conns" --warmup "$WARMUP" --nodelay "$nd" --json 2>/dev/null)"
    fields="$(printf '%s' "$out" | python3 -c '
import sys, json
try:
    d = json.load(sys.stdin)
except Exception:
    print("0 0 0 0 0 -3 0 0"); raise SystemExit
lat = d["latency_us"]
print("%d %d %d %d %d %d %s %s" % (d["total"], d["qps"], lat["p50"], lat["p99"],
                                   lat["max"], d["errors"],
                                   d.get("client_cpu_pct", 0), d.get("ncpu", 0)))
')"
  fi
  stop_server

  local total qps p50 p99 mx err cpu ncpu
  total="$(echo "$fields" | awk '{print $1}')"
  qps="$(echo "$fields" | awk '{print $2}')"
  p50="$(echo "$fields" | awk '{print $3}')"
  p99="$(echo "$fields" | awk '{print $4}')"
  mx="$(echo "$fields"  | awk '{print $5}')"
  err="$(echo "$fields" | awk '{print $6}')"
  cpu="$(echo "$fields" | awk '{print $7}')"
  ncpu="$(echo "$fields" | awk '{print $8}')"

  printf "  [%s] %-14s conns=%-4s size=%-5s nodelay=%s -> QPS=%-9s p99=%-7s cpu=%-6s err=%s\n" \
    "$dim" "$(server_short "$key")" "$conns" "$size" "$nd" "$qps" "${p99}us" "${cpu}%" "$err"

  echo "${dim},${key},${conns},${size},${nd},${total},${qps},${p50},${p99},${mx},${err},${cpu},${ncpu}" >> "$CSV"
}

# ---------------- 维度 A：并发连接数 ----------------
echo ""
echo ">>> 维度 A：并发连接数扫描（消息 ${A_SIZE}B）"
for c in $A_CONNS; do
  for k in $SERVER_ORDER; do
    run_one A "$k" "$c" "$A_SIZE" 1
  done
done

# ---------------- 维度 B：消息大小 ----------------
echo ""
echo ">>> 维度 B：消息大小扫描（连接数 ${B_CONNS}）"
for s in $B_SIZES; do
  for k in $SERVER_ORDER; do
    run_one B "$k" "$B_CONNS" "$s" 1
  done
done

# ---------------- 维度 C：TCP_NODELAY ----------------
# libco 的示例 echo server 不支持 TCP_NODELAY 开关，参与该维度只会得到两条几乎相同的数据，
# 反而误导读者，因此排除（其余三个服务端都支持 ECHO_NODELAY 环境变量）
echo ""
echo ">>> 维度 C：TCP_NODELAY 开/关（连接数 ${C_CONNS}，消息 ${C_SIZE}B；libco 不支持该开关，已排除）"
for nd in $C_NDS; do
  for k in $SERVER_ORDER; do
    [ "$k" = "libco" ] && continue
    run_one C "$k" "$C_CONNS" "$C_SIZE" "$nd"
  done
done

echo ""
echo ">>> 生成对比报表 ..."

python3 - "$CSV" "$MD" "$MODE" "$SECS" "$WARMUP" "$CLIENT_THREADS" "$MT_WORKERS" "$CLIENT_MODE" <<'PYEOF'
import csv
import sys
from collections import defaultdict

csv_path, md_path, mode, secs, warmup, cthreads, mt_workers, client_mode = sys.argv[1:9]
has_latency = (client_mode != "cpp")

rows = list(csv.DictReader(open(csv_path, encoding="utf-8")))

SERVER_ORDER = ["epoll", "epoll_mt", "uring", "libco"]
LABEL = {
    "epoll": "epoll 单线程 (Reactor)",
    "epoll_mt": "epoll 多线程 (多 Reactor)",
    "uring": "io_uring (Proactor)",
    "libco": "libco (协程)",
}
DIMS = [
    ("A", "并发连接数", "conns", "并发连接数", "消息固定 256B"),
    ("B", "消息大小", "size", "消息大小 (B)", "连接数固定"),
    ("C", "TCP_NODELAY", "nodelay", "TCP_NODELAY",
     "连接数固定、小包；libco 示例服务端不支持该开关，未参与本维度"),
]


def xlabel(xkey, x):
    if xkey == "nodelay":
        return "关闭 (0)" if x == 0 else "开启 (1)"
    return str(x)


def num(s):
    try:
        return int(float(s))
    except (TypeError, ValueError):
        return 0


def fnum(s, default=0.0):
    try:
        return float(s)
    except (TypeError, ValueError):
        return default


def fmt_qps(v):
    if v <= 0:
        return "N/A"
    if v >= 1_000_000:
        return "%.2fM" % (v / 1e6)
    if v >= 1000:
        return "%.1fk" % (v / 1e3)
    return str(v)


def fmt_us(v):
    return "N/A" if v <= 0 else str(v)


# 建立 dim -> server -> {x值: row} 的索引
table = defaultdict(lambda: defaultdict(dict))
xkey_of = {d[0]: d[2] for d in DIMS}
for r in rows:
    dim = r["dim"]
    xkey = xkey_of.get(dim)
    if not xkey:
        continue
    try:
        x = int(r[xkey])
    except ValueError:
        continue
    table[dim][r["server"]][x] = r

out = []
out.append("# 网络 I/O 多维度性能对比")
out.append("")
out.append("- 模式：`%s`" % mode)
out.append("- 每测点：预热 %ss + 测量 %ss，客户端 %s（%s 线程），epoll_mt worker=%s，本机回环（127.0.0.1）"
           % (warmup, secs,
              "network/bench.py（Python）" if client_mode == "py" else "bin/bench_client（C++）",
              cthreads, mt_workers if mt_workers else "自动(nproc)"))
out.append("- 指标：QPS = 完成的 echo 请求数/秒；p99 = 客户端观测往返延迟的 99 分位（µs）")
if not has_latency:
    out.append("- **本模式用 C++ 客户端，只采集 QPS，延迟分位不可用（表中显示 N/A）**")
out.append("- 服务端均为单条消息严格 ping-pong（收到一条回一条），未做流水线")
out.append("")

for dim, title, xkey, _xlab, note in DIMS:
    servers = [s for s in SERVER_ORDER if s in table[dim]]
    if not servers:
        continue
    xs = sorted({x for s in servers for x in table[dim][s]})
    if not xs:
        continue

    out.append("## 维度 %s：%s" % (dim, title))
    out.append("")
    out.append("_%s_" % note)
    out.append("")

    metrics = [("qps", "QPS", fmt_qps)]
    if has_latency:
        metrics.append(("p99_us", "p99 延迟 (µs)", fmt_us))
    for metric, mname, formatter in metrics:
        out.append("**%s**" % mname)
        out.append("")
        out.append("| 服务端 | " + " | ".join(xlabel(xkey, x) for x in xs) + " |")
        out.append("| --- |" + " --- |" * len(xs))
        for s in servers:
            cells = []
            for x in xs:
                r = table[dim][s].get(x)
                cells.append(formatter(num(r[metric])) if r else "-")
            out.append("| %s | %s |" % (LABEL.get(s, s), " | ".join(cells)))
        out.append("")

# ---------------- 自动观察 ----------------
insights = []

# A 维度：各服务端峰值
best = []
for s in SERVER_ORDER:
    d = table["A"].get(s)
    if not d:
        continue
    peak_x, peak_q = 0, 0
    for x, r in d.items():
        q = num(r["qps"])
        if q > peak_q:
            peak_x, peak_q = x, q
    if peak_q > 0:
        best.append((s, peak_x, peak_q))
if best:
    top = max(best, key=lambda t: t[2])
    insights.append("- 维度 A 峰值：**%s** 在 %d 连接时达到最高 QPS %s。"
                    % (LABEL.get(top[0], top[0]), top[1], fmt_qps(top[2])))

    # 压测端是否先饱和：看它自己实测吃掉了多少 CPU，而不是靠结果接近程度猜
    cpu_samples = [(fnum(r["client_cpu_pct"]), num(r["ncpu"]))
                   for r in rows if num(r["ncpu"]) > 0]
    if cpu_samples:
        peak_cpu = max(c for c, _ in cpu_samples)
        ncpu = cpu_samples[0][1]
        cores = peak_cpu / 100.0   # 压测端实际吃掉的核数
        # Python 有 GIL：单压测进程的 Python 执行基本只能吃满 ~1 核，
        # 因此判据是「用掉 ≥1 核」而不是「占满整机」
        if cores >= 0.9:
            insights.append("- **注意**：压测端已用掉 %.2f 核（整机 %d 核）—— 单进程 Python 受 GIL 限制，"
                            "约 1 核就是常见天花板。**当前数字很可能被压测端卡住，而不是服务端到顶。**"
                            "请改用 `bin/bench_client`（C++ 多线程，无 GIL）复核，例如："
                            "`./bin/bench_client <port> 16 256`。" % (cores, ncpu))
        else:
            insights.append("- 压测端只用掉 %.2f 核（整机 %d 核），远未触及 GIL 天花板 —— "
                            "本轮数字主要反映服务端与虚机的处理能力。" % (cores, ncpu))
    else:
        # C++ 客户端模式（csv 里 ncpu=0）——不存在 Python 单核瓶颈
        insights.append("- 本轮用 **C++ 客户端（bin/bench_client，无 GIL）**，不存在「压测端单核饱和」"
                        "这一干扰，数字更接近服务端真实上限；代价是没有延迟分位。")

# 多线程 vs 单线程（对比相同的最大连接数）
if "epoll" in table["A"] and "epoll_mt" in table["A"]:
    common = sorted(set(table["A"]["epoll"]) & set(table["A"]["epoll_mt"]), reverse=True)
    for x in common:
        a = num(table["A"]["epoll"][x]["qps"])
        b = num(table["A"]["epoll_mt"][x]["qps"])
        if a > 0 and b > 0:
            insights.append("- 维度 A：%d 连接时，多线程 epoll 相对单线程 epoll 的吞吐为 "
                            "**%.2fx**（%s vs %s）。" % (x, b / a, fmt_qps(b), fmt_qps(a)))
            break

# 消息大小：小包 vs 大包（QPS 与单位时间字节数）
for s in ("epoll", "epoll_mt", "uring"):
    d = table["B"].get(s)
    if not d:
        continue
    xs = sorted(d)
    if len(xs) >= 2:
        small, big = xs[0], xs[-1]
        qs, qb = num(d[small]["qps"]), num(d[big]["qps"])
        if qs > 0 and qb > 0:
            insights.append("- 维度 B：%s 消息从 %dB 增到 %dB 时，QPS 变为 %.2fx（%s → %s），"
                            "对应的单位时间字节数变为 %.1fx。"
                            % (LABEL.get(s, s), small, big, qb / qs, fmt_qps(qs), fmt_qps(qb),
                               (qb * big) / (qs * small)))
        break

# TCP_NODELAY：最多列 2 个服务端，避免结论区过长
nd_lines = []
for s in ("epoll", "epoll_mt", "uring", "libco"):
    d = table["C"].get(s)
    if d and 0 in d and 1 in d:
        off_p, on_p = num(d[0]["p99_us"]), num(d[1]["p99_us"])
        off_q, on_q = num(d[0]["qps"]), num(d[1]["qps"])
        if off_p > 0 and on_p > 0:
            nd_lines.append("- 维度 C：%s 关闭 TCP_NODELAY 时 p99=%dµs / QPS=%s，"
                            "开启后 p99=%dµs / QPS=%s。"
                            % (LABEL.get(s, s), off_p, fmt_qps(off_q), on_p, fmt_qps(on_q)))
insights.extend(nd_lines[:2])

# io_uring vs epoll
if "epoll" in table["A"] and "uring" in table["A"]:
    common = sorted(set(table["A"]["epoll"]) & set(table["A"]["uring"]), reverse=True)
    for x in common:
        a = num(table["A"]["epoll"][x]["qps"])
        b = num(table["A"]["uring"][x]["qps"])
        if a > 0 and b > 0:
            insights.append("- 维度 A：%d 连接时 io_uring / epoll 吞吐比 = **%.2fx**"
                            "（%s vs %s）。" % (x, b / a, fmt_qps(b), fmt_qps(a)))
            break

if insights:
    out.append("## 自动观察")
    out.append("")
    out.extend(insights)
    out.append("")

out.append("## 说明与注意事项")
out.append("")
out.append("- 数据为本机回环实测，**绝对值随 CPU/内核/容器限制不同**，可复现的是相对关系。")
out.append("- `N/A` 表示该服务端未编译或运行环境不可用（io_uring 常见于容器 seccomp 拦截）。")
out.append("- 单条消息 ping-pong 下，单连接的 QPS 上限 ≈ 1/RTT；因此**连接数**是提升吞吐的主要手段，"
           "这也正是维度 A 想说明的问题。")
out.append("- **压测端可能比服务端先到顶，务必先看瓶颈归属**：")
out.append("  - `CLIENT=py`（默认）：`client_cpu_pct` 是压测端实测 CPU 占用（相对单核的百分比），"
           "`ncpu` 是机器核数。Python 有 GIL，单进程大约吃到 **1 核** 就到顶 —— 达到这个量级时，"
           "各服务端的 QPS 会被压平，**差异会被掩盖**。")
out.append("  - `CLIENT=cpp`：改用 `bin/bench_client`（C++ 多线程，无 GIL），测服务端上限更可靠，"
           "但没有延迟分位。两者都要跑一遍再下结论最稳。")
out.append("- 原始数据见同名 `.csv`，完整日志见同名 `.log`。")

open(md_path, "w", encoding="utf-8").write("\n".join(out) + "\n")
print("报表已生成: %s" % md_path)
PYEOF

echo ""
echo "============================================================"
echo "  完成：$(date '+%F %T')"
echo "  报表  : $MD"
echo "  数据  : $CSV"
echo "  日志  : $LOG"
echo "  服务端日志目录: $LOGDIR"
echo "============================================================"

echo ""
echo "以下为报表预览："
echo ""
cat "$MD"
