#!/bin/bash
# ============================================================
#  磁盘 I/O 多维度对比压测（矩阵实验）
#
#  回答「不同情况下磁盘 I/O 的性能差多少」，沿三个维度扫描：
#    A. 队列深度   1 → 128，看「用并发深度换吞吐」的收益边界
#    B. 块大小     512B → 256KB，看 IOPS 与带宽的取舍
#    C. O_DIRECT   开/关，看 page cache 到底掩盖了多少真实磁盘行为
#
#  每个维度横跨三种方式：
#    同步 (pread/pwrite) / libaio (Linux AIO) / io_uring (Proactor)
#
#  用法：
#    bash scripts/bench_disk_matrix.sh            # quick（默认）
#    bash scripts/bench_disk_matrix.sh full       # 更细的扫点
#
#  可调环境变量：
#    BYTES=67108864    每个测点的目标 I/O 总量（字节），默认 64MB；full 模式 256MB
#                      每点的 I/O 次数由它推出：TOTAL = BYTES / BLOCK
#    SYNC_TOTAL        同步方式的 I/O 次数（默认取 TOTAL/8，最少 128）
#                      同步比异步慢一到两个数量级，用同样的次数会跑到天荒地老；
#                      因为比较的是「速率」(IOPS/带宽)，次数不同不影响可比性
#    FILE=/tmp/...     测试文件路径。必须落在**真实文件系统**上（tmpfs 不支持 O_DIRECT）
#    TIMEOUT=180       单个测点的超时（秒）
#
#  产物（results/ 下）：
#    disk_matrix_<模式>_<时间戳>.md    对比表格（含自动观察）
#    disk_matrix_<模式>_<时间戳>.csv   原始数据
#    disk_matrix_<模式>_<时间戳>.log   完整控制台日志
#
#  依赖：make disk、python3（生成报表）
#  说明：所有路径由脚本自身位置推导，可整体搬迁
# ============================================================
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

BIN="${PROJECT_ROOT}/bin"
OUTDIR="${PROJECT_ROOT}/results"

MODE="${1:-quick}"
BYTES="${BYTES:-0}"
FILE="${FILE:-/tmp/iodemo_matrix}"
TIMEOUT="${TIMEOUT:-180}"

case "$MODE" in
  quick)
    A_DEPTHS="1 2 8 32";               A_BLOCK=4096; A_DIRECT=1
    B_BLOCKS="512 4096 65536";         B_DEPTH=32;   B_DIRECT=1
    C_DIRECTS="0 1";                   C_BLOCK=4096; C_DEPTH=32
    [ "$BYTES" -gt 0 ] || BYTES=67108864          # 64MB
    ;;
  full)
    A_DEPTHS="1 2 4 8 16 32 64 128";   A_BLOCK=4096; A_DIRECT=1
    B_BLOCKS="512 4096 16384 65536 262144"; B_DEPTH=32; B_DIRECT=1
    C_DIRECTS="0 1";                   C_BLOCK=4096; C_DEPTH=32
    [ "$BYTES" -gt 0 ] || BYTES=268435456         # 256MB
    ;;
  *)
    echo "用法: $0 [quick|full]"
    exit 2
    ;;
esac

mkdir -p "$OUTDIR"
STAMP="$(date +%Y%m%d_%H%M%S)"
CSV="${OUTDIR}/disk_matrix_${MODE}_${STAMP}.csv"
MD="${OUTDIR}/disk_matrix_${MODE}_${STAMP}.md"
LOG="${OUTDIR}/disk_matrix_${MODE}_${STAMP}.log"

# 全部输出同时打到屏幕和日志
exec > >(tee -a "$LOG") 2>&1

echo "============================================================"
echo "  磁盘 I/O 多维度对比压测"
echo "  模式=${MODE}  每点目标 I/O 量=$((BYTES / 1024 / 1024))MB  文件=${FILE}"
echo "  开始时间: $(date '+%F %T')"
echo "  项目目录: ${PROJECT_ROOT}"
echo "============================================================"
echo ""

if ! command -v python3 >/dev/null 2>&1; then
  echo "[错误] 未找到 python3"
  exit 1
fi

# 文件系统检查：tmpfs 不支持 O_DIRECT
FSTYPE="$(df -T "$(dirname "$FILE")" 2>/dev/null | awk 'NR==2 {print $2}')"
echo "测试文件所在文件系统: ${FSTYPE:-unknown} (挂载点 $(dirname "$FILE"))"
case "${FSTYPE:-}" in
  tmpfs|ramfs)
    echo "[警告] 该路径是 ${FSTYPE}，不支持 O_DIRECT，维度 A/B/C 的 DIRECT=1 会失败。"
    echo "        请用 FILE=/path/on/real/disk/xxx 指定真实磁盘上的路径。"
    ;;
esac
echo ""

ORDER="sync libaio uring"

method_label() {
  case "$1" in
    sync)   echo "同步 (pread/pwrite)" ;;
    libaio) echo "libaio (Linux AIO)" ;;
    uring)  echo "io_uring (Proactor)" ;;
    *)      echo "$1" ;;
  esac
}

method_bin() {
  case "$1" in
    sync)   echo "${BIN}/io_sync" ;;
    libaio) echo "${BIN}/io_libaio" ;;
    uring)  echo "${BIN}/io_uring_disk" ;;
    *)      echo "" ;;
  esac
}

for m in $ORDER; do
  b="$(method_bin "$m")"
  [ -x "$b" ] || echo "[提示] 缺少 $(method_label "$m")：$b（该列将标记 N/A）"
done
echo ""

echo "dim,method,block,depth,direct,total,write_iops,read_iops,write_mbps,read_mbps" > "$CSV"

# 按目标字节数推算 I/O 次数
total_for() { # total_for <method> <block>
  local n=$((BYTES / $2))
  if [ "$1" = "sync" ]; then
    n=$((n / 8))
    [ "$n" -lt 128 ] && n=128
  else
    [ "$n" -lt 64 ] && n=64
  fi
  echo "$n"
}

# run_one <维度> <方式> <块大小> <深度> <direct> <总次数>
run_one() {
  local dim="$1" meth="$2" block="$3" depth="$4" direct="$5" total="$6"
  local bin; bin="$(method_bin "$meth")"

  if [ ! -x "$bin" ]; then
    printf "  [%s] %-18s block=%-7s depth=%-4s direct=%s -> N/A (未编译)\n" \
      "$dim" "$(method_label "$meth")" "$block" "$depth" "$direct"
    echo "${dim},${meth},${block},${depth},${direct},${total},0,0,0,0" >> "$CSV"
    return 0
  fi

  local -a args
  case "$meth" in
    sync)   args=("$FILE" "$block" "$total" "$direct") ;;
    *)      args=("$FILE" "$block" "$total" "$depth" "$direct") ;;
  esac

  local out rc
  out="$(timeout "$TIMEOUT" "$bin" "${args[@]}" 2>&1)"
  rc=$?

  local w_io r_io w_bw r_bw
  w_io="$(printf '%s' "$out" | sed -n 's/.*写 IOPS: *\([0-9]*\).*/\1/p')"
  r_io="$(printf '%s' "$out" | sed -n 's/.*读 IOPS: *\([0-9]*\).*/\1/p')"
  w_bw="$(printf '%s' "$out" | sed -n 's/.*写带宽: *\([0-9.]*\).*/\1/p')"
  r_bw="$(printf '%s' "$out" | sed -n 's/.*读带宽: *\([0-9.]*\).*/\1/p')"
  w_io="${w_io:-0}"; r_io="${r_io:-0}"; w_bw="${w_bw:-0}"; r_bw="${r_bw:-0}"

  local tag=""
  if [ "$rc" -eq 2 ]; then
    tag=" [环境限制: io_uring 不可用]"
  elif [ "$rc" -eq 124 ]; then
    tag=" [超时 ${TIMEOUT}s]"
  elif [ "$w_io" -eq 0 ] && [ "$r_io" -eq 0 ]; then
    tag=" [无输出 rc=$rc]"
  fi

  printf "  [%s] %-18s block=%-7s depth=%-4s direct=%s -> 写 %-9s 读 %-9s%s\n" \
    "$dim" "$(method_label "$meth")" "$block" "$depth" "$direct" "$w_io" "$r_io" "$tag"

  echo "${dim},${meth},${block},${depth},${direct},${total},${w_io},${r_io},${w_bw},${r_bw}" >> "$CSV"
  rm -f "$FILE"
}

# ---------------- 维度 A：队列深度 ----------------
echo ">>> 维度 A：队列深度扫描（块大小 ${A_BLOCK}B，O_DIRECT=${A_DIRECT}）"
for d in $A_DEPTHS; do
  for m in $ORDER; do
    run_one A "$m" "$A_BLOCK" "$d" "$A_DIRECT" "$(total_for "$m" "$A_BLOCK")"
  done
done

# ---------------- 维度 B：块大小 ----------------
echo ""
echo ">>> 维度 B：块大小扫描（深度 ${B_DEPTH}，O_DIRECT=${B_DIRECT}）"
for b in $B_BLOCKS; do
  for m in $ORDER; do
    run_one B "$m" "$b" "$B_DEPTH" "$B_DIRECT" "$(total_for "$m" "$b")"
  done
done

# ---------------- 维度 C：O_DIRECT ----------------
echo ""
echo ">>> 维度 C：O_DIRECT 开/关（块大小 ${C_BLOCK}B，深度 ${C_DEPTH}）"
for dr in $C_DIRECTS; do
  for m in $ORDER; do
    run_one C "$m" "$C_BLOCK" "$C_DEPTH" "$dr" "$(total_for "$m" "$C_BLOCK")"
  done
done

echo ""
echo ">>> 生成对比报表 ..."

python3 - "$CSV" "$MD" "$MODE" "$BYTES" "$FILE" "${FSTYPE:-unknown}" <<'PYEOF'
import csv
import sys
from collections import defaultdict

csv_path, md_path, mode, bytes_budget, fpath, fstype = sys.argv[1:7]

rows = list(csv.DictReader(open(csv_path, encoding="utf-8")))

ORDER = ["sync", "libaio", "uring"]
LABEL = {
    "sync": "同步 (pread/pwrite)",
    "libaio": "libaio (Linux AIO)",
    "uring": "io_uring (Proactor)",
}
DIMS = [
    ("A", "队列深度", "depth", "blk=%sB direct=%s"),
    ("B", "块大小", "block", "depth=%s direct=%s"),
    ("C", "O_DIRECT", "direct", "blk=%sB depth=%s"),
]


def num(s):
    try:
        return int(float(s))
    except (TypeError, ValueError):
        return 0


def fnum(s):
    try:
        return float(s)
    except (TypeError, ValueError):
        return 0.0


def fmt_iops(v):
    if v <= 0:
        return "N/A"
    if v >= 1_000_000:
        return "%.2fM" % (v / 1e6)
    if v >= 1000:
        return "%.1fk" % (v / 1e3)
    return str(v)


def fmt_mb(v):
    return "N/A" if v <= 0 else "%.1f" % v


def fmt_x(v):
    return "N/A" if v <= 0 else ("%.1f" % v)


table = defaultdict(lambda: defaultdict(dict))
xkey_of = {d[0]: d[2] for d in DIMS}
for r in rows:
    dim = r["dim"]
    xk = xkey_of.get(dim)
    if not xk:
        continue
    try:
        x = int(r[xk])
    except ValueError:
        continue
    table[dim][r["method"]][x] = r

out = []
out.append("# 磁盘 I/O 多维度性能对比")
out.append("")
out.append("- 模式：`%s`，每个测点目标 I/O 量 %d MB" % (mode, num(bytes_budget) // 1024 // 1024))
out.append("- 测试文件：`%s`（文件系统：%s）" % (fpath, fstype))
out.append("- 指标：写/读 IOPS = 每秒完成的 I/O 次数；带宽 ≈ IOPS × 块大小")
out.append("- 同步方式（pread/pwrite）的 I/O 次数取异步方式的 1/8，因为比较的是**速率**而非总量")
out.append("")

for dim, title, xkey, note_fmt in DIMS:
    methods = [m for m in ORDER if m in table[dim]]
    if not methods:
        continue
    xs = sorted({x for m in methods for x in table[dim][m]})
    if not xs:
        continue

    # 该维度下固定的另外两个参数
    sample = next(iter(table[dim][methods[0]].values()))
    if dim == "A":
        note = note_fmt % (sample["block"], sample["direct"])
    elif dim == "B":
        note = note_fmt % (sample["depth"], sample["direct"])
    else:
        note = note_fmt % (sample["block"], sample["depth"])

    out.append("## 维度 %s：%s" % (dim, title))
    out.append("")
    out.append("_%s_" % note)
    out.append("")

    for metric, mname, formatter in (
        ("write_iops", "写 IOPS", fmt_iops),
        ("read_iops", "读 IOPS", fmt_iops),
        ("write_mbps", "写带宽 (MB/s)", fmt_mb),
    ):
        out.append("**%s**" % mname)
        out.append("")
        out.append("| 方式 | " + " | ".join(str(x) for x in xs) + " |")
        out.append("| --- |" + " --- |" * len(xs))
        for m in methods:
            cells = []
            for x in xs:
                r = table[dim][m].get(x)
                cells.append(formatter(num(r[metric]) if metric.endswith("iops")
                                       else fnum(r[metric])) if r else "-")
            out.append("| %s | %s |" % (LABEL.get(m, m), " | ".join(cells)))
        out.append("")

insights = []

# A：深度收益
for m in ("uring", "libaio"):
    d = table["A"].get(m)
    if not d:
        continue
    xs = sorted(d)
    lo, hi = xs[0], xs[-1]
    q1, q2 = num(d[lo]["write_iops"]), num(d[hi]["write_iops"])
    if q1 > 0 and q2 > 1.5 * q1:
        insights.append("- 维度 A：%s 写 IOPS 从 depth=%d 的 %s 提升到 depth=%d 的 %s（**%.1fx**）"
                        "—— 直观展示「异步靠并发深度换吞吐」。"
                        % (LABEL[m], lo, fmt_iops(q1), hi, fmt_iops(q2), q2 / q1))
        break

# A：异步 vs 同步
if "sync" in table["A"] and table["A"]["sync"]:
    xs = sorted(table["A"]["sync"])
    top = xs[-1]
    s = num(table["A"]["sync"][top]["write_iops"])
    for m in ("libaio", "uring"):
        a = num(table["A"].get(m, {}).get(top, {}).get("write_iops", 0))
        if s > 0 and a > 0:
            insights.append("- 维度 A：depth=%d 时 %s 的写 IOPS 是同步方式的 **%.1fx**（%s vs %s）。"
                            % (top, LABEL[m], a / s, fmt_iops(a), fmt_iops(s)))
            break

# B：块大小 → IOPS 与带宽的取舍
for m in ORDER:
    d = table["B"].get(m)
    if not d:
        continue
    xs = sorted(d)
    if len(xs) < 2:
        continue
    b1, b2 = xs[0], xs[-1]
    i1, i2 = num(d[b1]["write_iops"]), num(d[b2]["write_iops"])
    w1, w2 = fnum(d[b1]["write_mbps"]), fnum(d[b2]["write_mbps"])
    if i1 > 0 and i2 > 0 and w1 > 0 and w2 > 0:
        insights.append("- 维度 B：%s 块大小 %dB→%dB 时，写 IOPS 变为 %.2fx，但写带宽变为 %.2fx"
                        "（%s → %s MB/s）—— 大块牺牲 IOPS 换带宽。"
                        % (LABEL[m], b1, b2, i2 / i1, w2 / w1, fmt_mb(w1), fmt_mb(w2)))
    break

# C：buffered vs O_DIRECT
if table["C"]:
    for m in ORDER:
        d = table["C"].get(m)
        if not d or 0 not in d or 1 not in d:
            continue
        b_io, d_io = num(d[0]["write_iops"]), num(d[1]["write_iops"])
        b_bw, d_bw = fnum(d[0]["write_mbps"]), fnum(d[1]["write_mbps"])
        if b_io > 0 and d_io > 0:
            insights.append("- 维度 C：%s 在 buffered 下写 IOPS %s（%s MB/s），O_DIRECT 下 %s（%s MB/s）"
                            "—— buffered 走 page cache，**数字虚高，不代表真实磁盘能力**。"
                            % (LABEL[m], fmt_iops(b_io), fmt_mb(b_bw),
                               fmt_iops(d_io), fmt_mb(d_bw)))
        break

insights.append("- **读结果的提醒**：若测试文件落在**虚拟磁盘 / qcow2 镜像**上，绝对数值几乎不可信"
                "（宿主机缓存会吸收写入）。可复现的是「同步 vs 异步」「深度换吞吐」这类相对关系；"
                "要拿真实磁盘能力，请在物理机或直通盘上跑。")
insights.append("- 另外注意 io_uring 的 **O_DIRECT 写**可能被内核甩给 io-wq 工作线程池"
                "（`ps -L` 能看到一堆 `iou-wrk-*` 线程），每次 I/O 多一次跨线程交接，"
                "在小 I/O 场景下可能明显慢于 libaio —— 这是本机实测到的现象，"
                "可以用 `ps -o comm= -L -p <pid> | sort | uniq -c` 复现。")

if insights:
    out.append("## 自动观察")
    out.append("")
    out.extend(insights)
    out.append("")

out.append("## 说明与注意事项")
out.append("")
out.append("- `N/A` 表示该方法未编译、io_uring 不可用（容器 seccomp）、或该次运行无输出。")
out.append("- O_DIRECT 要求缓冲区、偏移、长度都按 512B（部分设备 4KB）对齐，本项目代码已用 align_alloc_buf 处理。")
out.append("- buffered（O_DIRECT=0）下异步方式往往**退化甚至更慢**（page cache 让异步开销得不偿失），"
           "这本身就是一个值得观察的结论。")
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
echo "============================================================"
