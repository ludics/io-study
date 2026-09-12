#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
io-study 矩阵压测工具（网络 / 磁盘通用）

为什么是「矩阵」：单个基准只能回答「A 比 B 快多少」，但真实问题往往是
「在什么条件下 A 比 B 快」。所以这里沿几个**维度**扫描，每个维度横跨多种**实现**，
跑完自动出一张对比表。

用法：
    # 网络：并发连接数 / 消息大小 / TCP_NODELAY × 4 种服务端
    python3 scripts/bench_matrix.py net
    python3 scripts/bench_matrix.py net --mode full
    python3 scripts/bench_matrix.py net --client cpp        # 换 C++ 压测端（无 GIL）

    # 磁盘：队列深度 / 块大小 / O_DIRECT × 3 种方式
    python3 scripts/bench_matrix.py disk
    python3 scripts/bench_matrix.py disk --mode full --bytes 268435456

产物（results/ 下，同名三份）：
    <net|disk>_matrix_<模式>_<时间戳>.md     对比表 + 自动观察
    <net|disk>_matrix_<模式>_<时间戳>.csv    原始数据（可自行画图）
    <net|disk>_matrix_<模式>_<时间戳>.log    完整控制台输出

设计说明（为什么这么写）：
  * 进程管理、采样、统计、报表全在**一个文件、一种语言**里，
    数据以 dataclass 传递，不再有「bash 拼字段串、python 再解析」的跨界约定。
  * 「实现」是声明式的表（见 NET_IMPLS / DISK_IMPLS），加一种实现只要加一行。
  * 每种维度也是声明式的表（见 NET_DIMS / DISK_DIMS），改扫描范围不用动流程代码。
  * 子进程统一用 killpg 收尾，避免压测中途被打断后留下孤儿进程占着端口。
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import re
import signal
import socket
import statistics
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Iterable, Sequence

# ---------------------------------------------------------------------------
# 路径：全部基于本文件位置推导，整个项目可以拷到任何位置运行
# ---------------------------------------------------------------------------
ROOT = Path(__file__).resolve().parent.parent
BIN = ROOT / "bin"
NET_DIR = ROOT / "network"
LIBCO_DIR = ROOT / "third_party" / "libco"
RESULTS = ROOT / "results"


# ===========================================================================
# 第一部分：公共基础设施
# ===========================================================================


class Tee:
    """把输出同时写到终端和日志文件（原来是 bash 的 exec > >(tee -a ...)）"""

    def __init__(self, path: Path):
        self.file = open(path, "w", encoding="utf-8")

    def write(self, text: str = "") -> None:
        print(text, flush=True)
        self.file.write(text + "\n")
        self.file.flush()

    def close(self) -> None:
        self.file.close()


def first_existing(*paths: Path) -> Path | None:
    """返回第一个存在的路径。

    需要它是因为 third_party/libco 有两种常见布局：
      重构版 build/bin/example_echosvr   /  上游原版 example_echosvr
    """
    for p in paths:
        if p.exists():
            return p
    return None


class ChildProc:
    """一个被压测的服务端进程。

    要点：用 start_new_session=True 单独开一个进程组，
    收尾时 killpg 整组杀干净 —— 否则压测脚本被 Ctrl-C 打断后，
    残留的服务端会一直占着端口，下次跑就「启动失败」。
    """

    def __init__(self, argv: Sequence[str], log_path: Path, env: dict | None = None):
        self.argv = list(argv)
        self.log_path = log_path
        self._log = open(log_path, "w", encoding="utf-8")
        run_env = dict(os.environ)
        if env:
            run_env.update(env)
        self.popen = subprocess.Popen(
            self.argv,
            stdout=self._log,
            stderr=subprocess.STDOUT,
            env=run_env,
            start_new_session=True,
        )

    def alive(self) -> bool:
        return self.popen.poll() is None

    def stop(self) -> None:
        if self.popen.poll() is None:
            try:
                os.killpg(os.getpgid(self.popen.pid), signal.SIGKILL)
            except (ProcessLookupError, PermissionError):
                pass
        self.popen.wait(timeout=5)
        self._log.close()

    def tail_log(self, n: int = 2) -> str:
        try:
            lines = self.log_path.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError:
            return ""
        return " | ".join(lines[:n])


def wait_tcp_ready(port: int, timeout: float = 5.0) -> bool:
    """轮询直到端口可连。

    比「sleep 1 再赌它能起来」可靠：机器忙的时候 1 秒可能不够。
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return True
        except OSError:
            time.sleep(0.1)
    return False


@dataclass
class Sample:
    """一次测量的结果。

    metrics 只放数值，格式化（多少 k、多少 M）交给报表层 —— 采样和呈现分开。
    """

    impl: str
    dim: str
    x: int
    metrics: dict = field(default_factory=dict)
    note: str = ""


@dataclass
class Dim:
    """一个扫描维度：维度名 + 扫描值 + 该维度下固定住的其它参数"""

    key: str
    title: str
    values: list
    caption: str


@dataclass
class Impl:
    """一种被压测的实现（一个可执行文件 + 如何启动它）"""

    key: str
    label: str
    path: Callable[[], Path | None]
    # make_runner(binary) -> runner(port, extra) -> argv
    # 用「工厂函数」而不是直接存 argv：因为可执行文件路径要在运行时才确定
    # （libco 有 build/bin/ 和根目录两种布局）
    make_runner: Callable | None = None
    xlabel: Callable | None = None  # 扫描值怎么显示（比如 nodelay 的 0/1）
    # 启动这个实现时要额外设置的环境变量。
    # 典型用途是 echo_io_uring_adv 那类「一堆特性用环境变量开关」的服务端：
    # 在这里写死一组开关，就等于把它固定成"全特性"这一种形态参与对比，
    # 不必为每种开关组合都写一个新 Impl。
    env: dict | None = None


# ---------------------------------------------------------------------------
# 报表渲染：网络和磁盘共用一套「维度 → 指标表」的渲染逻辑
# ---------------------------------------------------------------------------


def fmt_count(v: float) -> str:
    if v <= 0:
        return "N/A"
    if v >= 1_000_000:
        return "%.2fM" % (v / 1e6)
    if v >= 1000:
        return "%.1fk" % (v / 1e3)
    return str(int(v))


def fmt_plain(v: float) -> str:
    return "N/A" if v <= 0 else "%.1f" % v


def fmt_int(v: float) -> str:
    return "N/A" if v <= 0 else str(int(v))


def render_tables(
    dims: Iterable[Dim],
    impls: Sequence[Impl],
    samples: Sequence[Sample],
    metrics: Sequence[tuple],  # [(key, 表头, 格式化函数), ...]
) -> list[str]:
    """把采样结果渲染成「每个维度 × 每个指标 一张表」。

    行 = 实现，列 = 该维度的扫描值。
    """
    # 索引：dim -> impl -> x -> Sample
    index: dict = {}
    for s in samples:
        index.setdefault(s.dim, {}).setdefault(s.impl, {})[s.x] = s

    label_of = {i.key: i.label for i in impls}
    out: list[str] = []

    for dim in dims:
        by_impl = index.get(dim.key)
        if not by_impl:
            continue
        xs = sorted({x for m in by_impl.values() for x in m})
        if not xs:
            continue

        out.append("## 维度 %s：%s" % (dim.key, dim.title))
        out.append("")
        out.append("_%s_" % dim.caption)
        out.append("")

        # 表头里的列名：nodelay 这种 0/1 要显示成「关闭 (0)」
        impl0 = next(iter(by_impl))
        xlabel = None
        for i in impls:
            if i.key == impl0:
                xlabel = i.xlabel
        col_names = [xlabel(x) if xlabel else str(x) for x in xs]

        for mkey, mtitle, mfmt in metrics:
            out.append("**%s**" % mtitle)
            out.append("")
            out.append("| 实现 | " + " | ".join(col_names) + " |")
            out.append("| --- |" + " --- |" * len(xs))
            for impl in impls:
                row = by_impl.get(impl.key)
                if row is None:
                    continue
                cells = []
                for x in xs:
                    s = row.get(x)
                    cells.append(mfmt(s.metrics.get(mkey, 0)) if s else "-")
                out.append("| %s | %s |" % (label_of.get(impl.key, impl.key), " | ".join(cells)))
            out.append("")

    return out


def write_csv(path: Path, samples: Sequence[Sample]) -> None:
    """原始数据落 CSV。列 = impl/dim/x + 所有出现过的 metric（自动求并集）"""
    metric_keys: list[str] = []
    for s in samples:
        for k in s.metrics:
            if k not in metric_keys:
                metric_keys.append(k)

    with open(path, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["dim", "impl", "x"] + metric_keys + ["note"])
        for s in samples:
            w.writerow(
                [s.dim, s.impl, s.x]
                + [s.metrics.get(k, "") for k in metric_keys]
                + [s.note]
            )


def write_report(
    md_path: Path, title: str, meta_lines: list[str], body: list[str], insights: list[str]
) -> None:
    text = ["# " + title, ""]
    text += ["- " + line for line in meta_lines]
    text.append("")
    text += body
    if insights:
        text.append("## 自动观察")
        text.append("")
        text += ["- " + line for line in insights]
        text.append("")
    md_path.write_text("\n".join(text) + "\n", encoding="utf-8")


# ===========================================================================
# 第二部分：网络矩阵
# ===========================================================================


def _impl_paths() -> dict:
    """各网络服务端的可执行文件位置（libco 有两种布局，用 first_existing 兜住）"""
    return {
        "epoll": BIN / "echo_epoll",
        "epoll_mt": BIN / "echo_epoll_mt",
        "uring": BIN / "echo_io_uring",
        "uring_adv": BIN / "echo_io_uring_adv",
        "libco": first_existing(LIBCO_DIR / "build/bin/example_echosvr",
                                LIBCO_DIR / "example_echosvr"),
    }


def _nodelay_xlabel(x: int) -> str:
    return "关闭 (0)" if x == 0 else "开启 (1)"


def make_net_impls() -> list[Impl]:
    """声明式地描述 5 种服务端。

    注意两点：
      * libco 的示例 echo server 不支持 TCP_NODELAY 开关，所以它不参与
        「TCP_NODELAY」维度（否则会产出两条几乎相同的数据，误导读者）。
      * uring_adv 是 io_uring 的「火力全开」形态（SQPOLL + 提供缓冲区环 +
        multishot recv/accept + 零拷贝 + CQ 忙轮询）。它的各个特性本身也能
        用环境变量单独开关，这里固定成"全开"参与对比；要做归因实验请直接
        跑 network/echo_io_uring_adv.c 里列出的那些环境变量组合。
        注意它的 SQPOLL 会额外吃一个核：如果机器核数少，请相应调小
        CLIENT_THREADS，否则压测端和服务端会互相抢核。
    """

    def argv_plain(binary: Path):
        # epoll / epoll_mt / io_uring 都是 <port> [可选 worker 数]
        def build(port: int, extra: dict, **kw) -> list[str]:
            argv = [str(binary), str(port)]
            if binary.name == "echo_epoll_mt" and extra.get("mt_workers"):
                argv.append(str(extra["mt_workers"]))  # 控制 worker 数，默认由服务端取 nproc
            return argv

        return build

    def argv_libco(binary: Path):
        def build(port: int, extra: dict, **kw) -> list[str]:
            # libco 示例的用法：example_echosvr <ip> <port> <协程数> <?>
            return [str(binary), "127.0.0.1", str(port), "10", "1"]

        return build

    p = _impl_paths()
    return [
        Impl("epoll", "epoll 单线程 (Reactor)", lambda: p["epoll"], argv_plain),
        Impl("epoll_mt", "epoll 多线程 (多 Reactor)", lambda: p["epoll_mt"], argv_plain),
        Impl("uring", "io_uring (Proactor)", lambda: p["uring"], argv_plain),
        Impl("uring_adv", "io_uring 全特性 (SQPOLL+缓冲环+multishot+ZC+忙轮询)",
             lambda: p["uring_adv"], argv_plain,
             env={"ECHO_SQPOLL": "1", "ECHO_PBUF": "1", "ECHO_MULTISHOT": "1",
                  "ECHO_ZC": "1", "ECHO_SPIN": "1"}),
        Impl("libco", "libco (协程)", lambda: p["libco"], argv_libco),
    ]


NET_METRICS = [
    ("qps", "QPS", fmt_count),
    ("p99_us", "p99 延迟 (µs)", fmt_int),
]


def net_dims(mode: str) -> tuple[list[Dim], dict]:
    """返回 (维度定义, 每个维度的固定参数)"""
    if mode == "quick":
        conns = [1, 8, 64]
        sizes = [64, 512, 4096]
        base_conns, base_size = 64, 256
    else:
        conns = [1, 2, 8, 32, 128, 512]
        sizes = [16, 64, 256, 1024, 4096, 16384]
        base_conns, base_size = 128, 256

    dims = [
        Dim("A", "并发连接数", conns, "消息 %dB；在途请求数 = 连接数" % base_size),
        Dim("B", "消息大小", sizes, "连接数 %d" % base_conns),
        Dim("C", "TCP_NODELAY", [0, 1], "连接数 %d，消息 64B；libco 示例不支持该开关，未参与" % base_conns),
    ]
    fixed = {
        "A": {"conns": conns, "size": base_size, "nodelay": 1},
        "B": {"conns": base_conns, "size": sizes, "nodelay": 1},
        "C": {"conns": base_conns, "size": 64, "nodelay": [0, 1]},
    }
    return dims, fixed


def _xs_for(dim_key: str, fixed: dict) -> list:
    """该维度要扫的值：dim A 扫 conns、B 扫 size、C 扫 nodelay"""
    return fixed[dim_key][{"A": "conns", "B": "size", "C": "nodelay"}[dim_key]]


def measure_net_py(port: int, conns: int, size: int, args) -> dict:
    """用 network/bench.py 压测。

    它的好处是有延迟分位和压测端 CPU 占用，代价是 Python 有 GIL，
    单进程大约吃到 1 核就到顶 —— 所以看到「压测端 CPU ≈ 1 核」时，
    说明数字可能被压测端卡住了，要换 --client cpp 复核。
    """
    cmd = [
        sys.executable, str(NET_DIR / "bench.py"), str(port),
        str(args.client_threads), str(size), str(args.secs),
        "--conns", str(conns),
        "--warmup", str(args.warmup),
        "--nodelay", str(args.nodelay),
        "--json",
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True,
                          timeout=args.secs + args.warmup + 30)
    line = proc.stdout.strip().splitlines()[-1] if proc.stdout.strip() else "{}"
    d = json.loads(line)
    lat = d.get("latency_us", {})
    return {
        "qps": d.get("qps", 0),
        "p99_us": lat.get("p99", 0),
        "total": d.get("total", 0),
        "client_cpu_pct": d.get("client_cpu_pct", 0),
        "ncpu": d.get("ncpu", 0),
        "errors": d.get("errors", 0),
    }


def measure_net_cpp(port: int, conns: int, size: int, args) -> dict:
    """用 bin/bench_client 压测（C++ 多线程，无 GIL）。

    它每秒打印一行「QPS: N」。bench_client 已用 setvbuf 设成行缓冲，
    所以即使 stdout 是管道，被 kill 前的输出也不会丢。
    取最后 secs 行的中位数（丢掉第一秒作为预热），比单次采样稳。
    """
    binary = BIN / "bench_client"
    if not binary.exists():
        return {"qps": 0, "note": "未编译 bench_client"}

    proc = subprocess.Popen(
        [str(binary), str(port), str(conns), str(size)],
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
        start_new_session=True,
    )
    values: list[int] = []
    try:
        deadline = time.time() + args.secs + 1.5
        while time.time() < deadline:
            line = proc.stdout.readline()
            if not line:
                break
            if line.startswith("QPS:"):
                values.append(int(line.split()[1]))
    finally:
        try:
            os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
        except (ProcessLookupError, PermissionError):
            pass
        proc.wait(timeout=5)

    tail = values[1:] if len(values) > 1 else values  # 丢掉第一秒（预热）
    qps = int(statistics.median(tail[-args.secs:])) if tail else 0
    return {"qps": qps, "p99_us": 0, "total": qps * args.secs}


def run_net(args) -> int:
    dims, fixed = net_dims(args.mode)
    impls = make_net_impls()

    stamp = time.strftime("%Y%m%d_%H%M%S")
    tag = "net_matrix_%s_%s" % (args.mode, stamp)
    RESULTS.mkdir(exist_ok=True)
    md_path = RESULTS / (tag + ".md")
    csv_path = RESULTS / (tag + ".csv")
    log = Tee(RESULTS / (tag + ".log"))

    log.write("=" * 60)
    log.write("  网络 I/O 多维度对比压测")
    log.write("  模式=%s  客户端=%s(%d 线程)  每点 预热%ss+测量%ss"
              % (args.mode, args.client, args.client_threads, args.warmup, args.secs))
    log.write("  开始时间: %s" % time.strftime("%F %T"))
    log.write("=" * 60)

    # 缺哪个实现提前说清楚，不要把 N/A 留到最后让人猜
    for impl in impls:
        path = impl.path()
        if path is None or not Path(path).exists():
            log.write("[提示] 缺少 %s（该列将标记 N/A）：%s" % (impl.label, path))
    log.write("")

    samples: list[Sample] = []
    port = args.base_port

    for dim in dims:
        log.write(">>> 维度 %s：%s 扫描" % (dim.key, dim.title))
        for x in _xs_for(dim.key, fixed):
            for impl in impls:
                # libco 不参与 TCP_NODELAY 维度：它的示例服务端没有这个开关
                if dim.key == "C" and impl.key == "libco":
                    continue

                path = impl.path()
                label = "%-14s" % impl.key
                if path is None or not Path(path).exists():
                    log.write("  [%s] %s -> N/A (未编译)" % (dim.key, label))
                    samples.append(Sample(impl.key, dim.key, x, {}, "not-built"))
                    continue

                # 这一维度下，除扫描值以外的参数都固定
                conns = x if dim.key == "A" else fixed[dim.key]["conns"]
                size = x if dim.key == "B" else fixed[dim.key]["size"]
                nodelay = x if dim.key == "C" else fixed[dim.key]["nodelay"]
                args.nodelay = nodelay

                port += 1
                srv_log = Path("/tmp") / ("net_matrix_%s_%d.log" % (impl.key, port))
                runner = impl.make_runner(Path(path))
                argv = runner(port, {"mt_workers": args.mt_workers})

                # NODELAY 维度要求所有服务端都认这个开关；实现自带的固定环境变量
                # （比如 uring_adv 的 SQPOLL/PBUF 那一组）在这里合并进去。
                srv_env = {"ECHO_NODELAY": str(nodelay)}
                if impl.env:
                    srv_env.update(impl.env)
                child = ChildProc(argv, srv_log, env=srv_env)
                try:
                    if not wait_tcp_ready(port):
                        log.write("  [%s] %s conns=%-4d size=%-5d -> 启动失败: %s"
                                  % (dim.key, label, conns, size, child.tail_log()))
                        samples.append(Sample(impl.key, dim.key, x, {}, "start-failed"))
                        continue

                    if args.client == "cpp":
                        m = measure_net_cpp(port, conns, size, args)
                    else:
                        m = measure_net_py(port, conns, size, args)
                finally:
                    child.stop()
                    srv_log.unlink(missing_ok=True)

                cpu_note = ""
                if m.get("ncpu"):
                    cores = m.get("client_cpu_pct", 0) / 100.0
                    cpu_note = " 压测端%.2f核" % cores
                log.write("  [%s] %s conns=%-4d size=%-5d nodelay=%d -> QPS=%-9s p99=%-8s err=%s%s"
                          % (dim.key, label, conns, size, nodelay,
                             fmt_count(m.get("qps", 0)),
                             (fmt_int(m.get("p99_us", 0)) + "us") if m.get("p99_us") else "N/A",
                             m.get("errors", 0), cpu_note))
                samples.append(Sample(impl.key, dim.key, x, m))
        log.write("")

    write_csv(csv_path, samples)

    # ---------------- 自动观察 ----------------
    insights = derive_net_insights(samples, args)

    meta = [
        "模式：`%s`" % args.mode,
        "每测点：预热 %ss + 测量 %ss，压测端 %s，epoll_mt worker=%s，本机回环 127.0.0.1"
        % (args.warmup, args.secs,
           "network/bench.py（Python）" if args.client == "py" else "bin/bench_client（C++，无 GIL）",
           args.mt_workers or "自动(nproc)"),
        "指标：QPS = 每秒完成的 echo 请求数；p99 = 客户端观测往返延迟的 99 分位",
        "每条连接始终保留 1 个在途请求（严格 ping-pong，不做流水线）",
    ]
    if args.client == "cpp":
        meta.append("**C++ 压测端模式：只采集 QPS，延迟分位不可用**")

    body = render_tables(dims, impls, samples, NET_METRICS)
    body.append("## 读这张表的关键提醒")
    body.append("")
    body.append("- **先确认瓶颈在哪一边**。Python 压测端受 GIL 限制，单进程约 **1 核** 就到顶；"
                "一旦压测端吃满 1 核，各服务端的 QPS 会被压平、差异被掩盖。"
                "报表里的 `client_cpu_pct` 就是这个信号（100% ≈ 1 核）。")
    body.append("- 怀疑压测端到顶时，换 C++ 压测端复核："
                "`python3 scripts/bench_matrix.py net --client cpp`")
    body.append("- `N/A` = 该实现未编译，或运行环境不可用（io_uring 常见于容器 seccomp）。")

    write_report(md_path, "网络 I/O 多维度性能对比", meta, body, insights)

    log.write("=" * 60)
    log.write("  完成：%s" % time.strftime("%F %T"))
    log.write("  报表  : %s" % md_path)
    log.write("  数据  : %s" % csv_path)
    log.write("=" * 60)
    log.close()
    print("\n报表已生成: %s" % md_path)
    return 0


def derive_net_insights(samples: Sequence[Sample], args) -> list[str]:
    """从采样数据里挑几条有信息量的结论（不是复述表格，是给判断）"""
    out: list[str] = []

    def get(dim: str, impl: str, x: int) -> Sample | None:
        for s in samples:
            if s.dim == dim and s.impl == impl and s.x == x:
                return s
        return None

    # 压测端是否到顶：看客户端 CPU 占了多少核
    cpu_cores = [s.metrics.get("client_cpu_pct", 0) / 100.0
                 for s in samples if s.metrics.get("ncpu")]
    if cpu_cores:
        peak = max(cpu_cores)
        if peak >= 0.9:
            out.append("**压测端已用掉 %.2f 核** —— 单进程 Python 受 GIL 限制，约 1 核就是天花板。"
                       "当前数字**很可能被压测端卡住**，请用 `--client cpp` 复核。" % peak)
        else:
            out.append("压测端只用掉 %.2f 核，未触及 GIL 天花板 —— 本轮数字主要反映服务端能力。" % peak)
    else:
        out.append("本轮用 **C++ 压测端（无 GIL）**，不存在压测端单核饱和的干扰，"
                   "数字更接近服务端真实上限；代价是没有延迟分位。")

    # 峰值
    peak = None
    for s in samples:
        if s.dim != "A":
            continue
        q = s.metrics.get("qps", 0)
        if q and (peak is None or q > peak.metrics.get("qps", 0)):
            peak = s
    if peak:
        out.append("维度 A 峰值：**%s** 在 %d 连接时达到 %s QPS。"
                   % (peak.impl, peak.x, fmt_count(peak.metrics["qps"])))

    # ---- 自洽性检查：用 Little's law 反推平均延迟，和实测分位对照 ----
    #
    # 每条连接始终有 1 个在途请求，所以在途请求数 ≈ 连接数，于是：
    #     平均延迟 ≈ 连接数 / QPS
    # 如果这个「反推的平均延迟」比实测 p99 还大好几倍，说明**测量本身不自洽**：
    # 要么在途请求数根本没到连接数（服务端没在真正服务所有连接），
    # 要么客户端统计到的"响应"并不对应它刚发出的请求。
    # 这种数据不能拿来下结论 —— 我第一次看 libco 的 p99 时就被坑过。
    suspicious = []
    for s in samples:
        if s.dim != "A":
            continue
        qps = s.metrics.get("qps", 0)
        p99 = s.metrics.get("p99_us", 0)
        if qps <= 0 or p99 <= 0:
            continue
        avg_us = s.x / qps * 1e6  # 连接数 / QPS → 反推的平均延迟(µs)
        if avg_us > 3 * p99:
            suspicious.append((s, avg_us, p99))

    if suspicious:
        s, avg_us, p99 = suspicious[0]
        out.append("⚠️ **自洽性检查未通过**：`%s` 在 %d 连接时 %s QPS，按 Little's law "
                   "反推平均延迟 ≈ **%.0f µs**，却测出 p99 = %d µs（差了 %.0f 倍）。"
                   "这说明**在途请求数没有达到 %d 条** —— 该行的延迟数字不可信。"
                   "请确认服务端真能并发处理这么多连接（例如 libco 示例的协程池大小、"
                   "epoll 的 listen backlog），或先用更少的连接数复测。"
                   % (s.impl, s.x, fmt_count(s.metrics.get("qps", 0)),
                      avg_us, p99, avg_us / p99, s.x))

    # 多线程 Reactor vs 单线程
    xs = sorted({s.x for s in samples if s.dim == "A" and s.impl == "epoll"
                 and s.metrics.get("qps", 0)})
    for x in reversed(xs):
        a = get("A", "epoll", x)
        b = get("A", "epoll_mt", x)
        if a and b and a.metrics.get("qps") and b.metrics.get("qps"):
            r = b.metrics["qps"] / a.metrics["qps"]
            out.append("维度 A：%d 连接时，多线程 Reactor 相对单线程为 **%.2fx**"
                       "（%s vs %s）。" % (x, r, fmt_count(b.metrics["qps"]),
                                          fmt_count(a.metrics["qps"])))
            break

    # io_uring vs epoll
    for x in reversed(xs):
        a = get("A", "epoll", x)
        b = get("A", "uring", x)
        if a and b and a.metrics.get("qps") and b.metrics.get("qps"):
            out.append("维度 A：%d 连接时 io_uring / epoll 吞吐比 = **%.2fx**（%s vs %s）。"
                       % (x, b.metrics["qps"] / a.metrics["qps"],
                          fmt_count(b.metrics["qps"]), fmt_count(a.metrics["qps"])))
            break

    # 消息大小：QPS 与单位时间字节数的取舍
    bs = sorted({s.x for s in samples if s.dim == "B" and s.impl == "epoll"
                 and s.metrics.get("qps", 0)})
    if len(bs) >= 2:
        s1, s2 = get("B", "epoll", bs[0]), get("B", "epoll", bs[-1])
        if s1 and s2:
            q1, q2 = s1.metrics["qps"], s2.metrics["qps"]
            out.append("维度 B：消息 %dB→%dB 时 QPS 变为 %.2fx（%s → %s），"
                       "但单位时间字节数变为 %.1fx —— 小包受系统调用次数主导，大包受拷贝/带宽主导。"
                       % (bs[0], bs[-1], q2 / q1, fmt_count(q1), fmt_count(q2),
                          (q2 * bs[-1]) / (q1 * bs[0])))

    if any(s.dim == "C" and s.metrics.get("p99_us") for s in samples):
        out.append("维度 C：ping-pong 下单连接只有一个未完成包，Nagle 基本不触发，"
                   "所以开关 TCP_NODELAY 的差异通常很小 —— 若差异明显，"
                   "说明撞上了 Nagle 与延迟确认（delayed ACK）互等的经典症状。")

    return out


# ===========================================================================
# 第三部分：磁盘矩阵
# ===========================================================================

DISK_METRICS = [
    ("write_iops", "写 IOPS", fmt_count),
    ("read_iops", "读 IOPS", fmt_count),
    ("write_mbps", "写带宽 (MB/s)", fmt_plain),
]

_RE_W_IOPS = re.compile(r"写 IOPS:\s*(\d+)")
_RE_R_IOPS = re.compile(r"读 IOPS:\s*(\d+)")
_RE_W_MB = re.compile(r"写带宽:\s*([\d.]+)")
_RE_R_MB = re.compile(r"读带宽:\s*([\d.]+)")


def make_disk_impls() -> list[Impl]:
    return [
        Impl("sync", "同步 (pread/pwrite)", lambda: BIN / "io_sync"),
        Impl("libaio", "libaio (Linux AIO)", lambda: BIN / "io_libaio"),
        Impl("uring", "io_uring (Proactor)", lambda: BIN / "io_uring_disk"),
    ]


def disk_dims(mode: str) -> tuple[list[Dim], dict]:
    if mode == "quick":
        depths = [1, 2, 8, 32]
        blocks = [512, 4096, 65536]
    else:
        depths = [1, 2, 4, 8, 16, 32, 64, 128]
        blocks = [512, 4096, 16384, 65536, 262144]

    dims = [
        Dim("A", "队列深度", depths, "块大小 4096B，O_DIRECT=1"),
        Dim("B", "块大小", blocks, "depth=32，O_DIRECT=1"),
        Dim("C", "O_DIRECT", [0, 1], "块大小 4096B，depth=32"),
    ]
    return dims, {"depths": depths, "blocks": blocks}


def _xs_for_disk(dim_key: str, dims: list[Dim]) -> list:
    for d in dims:
        if d.key == dim_key:
            return d.values
    return []


def run_disk(args) -> int:
    dims, _ = disk_dims(args.mode)
    impls = make_disk_impls()

    stamp = time.strftime("%Y%m%d_%H%M%S")
    tag = "disk_matrix_%s_%s" % (args.mode, stamp)
    RESULTS.mkdir(exist_ok=True)
    md_path = RESULTS / (tag + ".md")
    csv_path = RESULTS / (tag + ".csv")
    log = Tee(RESULTS / (tag + ".log"))

    fstype = "unknown"
    try:
        out = subprocess.run(["df", "-T", str(Path(args.file).parent)],
                             capture_output=True, text=True, timeout=10).stdout
        parts = out.splitlines()[1].split()
        fstype = parts[1]
    except Exception:
        pass

    log.write("=" * 60)
    log.write("  磁盘 I/O 多维度对比压测")
    log.write("  模式=%s  每点目标 I/O 量=%d MB  文件=%s"
              % (args.mode, args.bytes // 1024 // 1024, args.file))
    log.write("  文件系统: %s" % fstype)
    log.write("=" * 60)
    if fstype in ("tmpfs", "ramfs"):
        log.write("[警告] 该路径是 %s，不支持 O_DIRECT，DIRECT=1 的测点会失败。" % fstype)
        log.write("       请用 --file 指定真实磁盘上的路径。")
    log.write("")

    samples: list[Sample] = []

    for dim in dims:
        log.write(">>> 维度 %s：%s 扫描" % (dim.key, dim.title))
        for x in _xs_for_disk(dim.key, dims):
            for impl in impls:
                binary = impl.path()
                if not Path(binary).exists():
                    log.write("  [%s] %-18s -> N/A (未编译)" % (dim.key, impl.label))
                    samples.append(Sample(impl.key, dim.key, x, {}, "not-built"))
                    continue

                block = x if dim.key == "B" else args.block
                depth = x if dim.key == "A" else args.depth
                direct = x if dim.key == "C" else args.direct

                # 按「目标 I/O 字节数」折算次数：这样不同块大小的测点工作量相当。
                # 同步方式再除一个系数 —— 它慢一到两个数量级，用同样次数会跑到天荒地老。
                # 注意比较的是**速率**(IOPS/带宽)，所以次数不同不影响可比性。
                total = max(64, args.bytes // block)
                if impl.key == "sync":
                    total = max(128, total // args.sync_divisor)

                # io_sync 没有 depth 参数（同步执行不存在并发度）
                if impl.key == "sync":
                    argv = [str(binary), args.file, str(block), str(total), str(direct)]
                else:
                    argv = [str(binary), args.file, str(block), str(total),
                            str(depth), str(direct)]

                metrics: dict = {}
                note = ""
                try:
                    proc = subprocess.run(argv, capture_output=True, text=True,
                                          timeout=args.timeout)
                    out = proc.stdout
                    if proc.returncode == 2:
                        note = "io_uring-unavailable"
                    metrics = {
                        "write_iops": int(_RE_W_IOPS.search(out).group(1)) if _RE_W_IOPS.search(out) else 0,
                        "read_iops": int(_RE_R_IOPS.search(out).group(1)) if _RE_R_IOPS.search(out) else 0,
                        "write_mbps": float(_RE_W_MB.search(out).group(1)) if _RE_W_MB.search(out) else 0.0,
                        "read_mbps": float(_RE_R_MB.search(out).group(1)) if _RE_R_MB.search(out) else 0.0,
                        "total": total,
                    }
                except subprocess.TimeoutExpired:
                    note = "timeout"
                    log.write("  [%s] %-18s block=%-7d depth=%-4d direct=%d -> 超时 %ss"
                              % (dim.key, impl.label, block, depth, direct, args.timeout))
                finally:
                    # 每次跑完删掉测试文件：否则下一轮的 ftruncate 复用旧块，
                    # 会把「首次分配」的成本抹掉，不同轮次不可比
                    Path(args.file).unlink(missing_ok=True)

                w = metrics.get("write_iops", 0)
                r = metrics.get("read_iops", 0)
                log.write("  [%s] %-18s block=%-7d depth=%-4d direct=%d -> 写 %-9s 读 %-9s%s"
                          % (dim.key, impl.label, block, depth, direct,
                             fmt_count(w), fmt_count(r),
                             (" [" + note + "]") if note else ""))
                samples.append(Sample(impl.key, dim.key, x, metrics, note))
        log.write("")

    write_csv(csv_path, samples)
    insights = derive_disk_insights(samples, args)

    meta = [
        "模式：`%s`，每个测点目标 I/O 量 %d MB" % (args.mode, args.bytes // 1024 // 1024),
        "测试文件：`%s`（文件系统：%s）" % (args.file, fstype),
        "指标：IOPS = 每秒完成的 I/O 次数；带宽 ≈ IOPS × 块大小",
        "同步方式的 I/O 次数取异步的 1/%d —— 比的是**速率**不是总量，次数不同不影响可比性"
        % args.sync_divisor,
    ]

    body = render_tables(dims, impls, samples, DISK_METRICS)
    body.append("## 说明与注意事项")
    body.append("")
    body.append("- `N/A` = 未编译 / io_uring 不可用（容器 seccomp）/ 该次运行无输出。")
    body.append("- O_DIRECT 要求缓冲区、偏移、长度按 512B（部分设备 4KB）对齐，"
                "本项目代码用 `align_alloc_buf` 处理。")
    body.append("- buffered（O_DIRECT=0）下异步方式可能**退化甚至更慢**："
                "page cache 让异步的额外开销得不偿失，这本身就是一个结论。")
    body.append("- **不要只看绝对值**：若测试文件在虚拟磁盘 / qcow2 镜像上，"
                "宿主机缓存会吸收写入，数值几乎不可信。可信的是相对关系"
                "（同步 vs 异步、深度换吞吐）。")

    write_report(md_path, "磁盘 I/O 多维度性能对比", meta, body, insights)

    log.write("=" * 60)
    log.write("  完成：%s" % time.strftime("%F %T"))
    log.write("  报表  : %s" % md_path)
    log.write("  数据  : %s" % csv_path)
    log.write("=" * 60)
    log.close()
    print("\n报表已生成: %s" % md_path)
    return 0


def derive_disk_insights(samples: Sequence[Sample], args) -> list[str]:
    out: list[str] = []

    def rows(dim: str, impl: str) -> list[Sample]:
        return sorted([s for s in samples if s.dim == dim and s.impl == impl
                       and s.metrics.get("write_iops")], key=lambda s: s.x)

    # 深度收益
    for impl in ("libaio", "uring"):
        r = rows("A", impl)
        if len(r) >= 2 and r[0].metrics["write_iops"] > 0:
            q1, q2 = r[0].metrics["write_iops"], r[-1].metrics["write_iops"]
            if q2 > 1.5 * q1:
                out.append("维度 A：%s 写 IOPS 从 depth=%d 的 %s 提升到 depth=%d 的 %s（**%.1fx**）"
                           "—— 这就是「异步靠并发深度换吞吐」。"
                           % (impl, r[0].x, fmt_count(q1), r[-1].x, fmt_count(q2), q2 / q1))
                break

    # 异步 vs 同步
    sync_rows = rows("A", "sync")
    if sync_rows:
        top = sync_rows[-1].x
        base = next((s for s in sync_rows if s.x == top), None)
        if base and base.metrics["write_iops"] > 0:
            for impl in ("libaio", "uring"):
                other = next((s for s in rows("A", impl) if s.x == top), None)
                if other and other.metrics["write_iops"] > 0:
                    out.append("维度 A：depth=%d 时 %s 的写 IOPS 是同步方式的 **%.1fx**（%s vs %s）。"
                               % (top, impl,
                                  other.metrics["write_iops"] / base.metrics["write_iops"],
                                  fmt_count(other.metrics["write_iops"]),
                                  fmt_count(base.metrics["write_iops"])))
                    break

    # 块大小：IOPS 与带宽的取舍
    rb = rows("B", "sync")
    if len(rb) >= 2:
        if rb[0].metrics["write_iops"] > 0 and rb[-1].metrics["write_iops"] > 0:
            out.append("维度 B：块大小 %dB→%dB 时写 IOPS 变为 %.2fx，写带宽变为 %.1fx"
                       "（%s → %s MB/s）—— 大块牺牲 IOPS 换带宽。"
                       % (rb[0].x, rb[-1].x,
                          rb[-1].metrics["write_iops"] / rb[0].metrics["write_iops"],
                          rb[-1].metrics["write_mbps"] / max(rb[0].metrics["write_mbps"], 1e-9),
                          fmt_plain(rb[0].metrics["write_mbps"]),
                          fmt_plain(rb[-1].metrics["write_mbps"])))

    # buffered vs O_DIRECT
    for impl in ("sync", "libaio", "uring"):
        c = sorted([s for s in samples if s.dim == "C" and s.impl == impl
                    and s.metrics.get("write_iops")], key=lambda s: s.x)
        if len(c) == 2 and c[0].metrics["write_iops"] > 0 and c[1].metrics["write_iops"] > 0:
            out.append("维度 C：%s 在 buffered 下写 IOPS %s（%s MB/s），O_DIRECT 下 %s（%s MB/s）"
                       "—— buffered 走 page cache，**数字虚高，不代表真实磁盘能力**。"
                       % (impl, fmt_count(c[0].metrics["write_iops"]),
                          fmt_plain(c[0].metrics["write_mbps"]),
                          fmt_count(c[1].metrics["write_iops"]),
                          fmt_plain(c[1].metrics["write_mbps"])))
            break

    out.append("另注意：io_uring 的 O_DIRECT 写可能被内核甩给 **io-wq 工作线程池**，"
               "每次 I/O 多一次跨线程交接。可用 `ps -o comm= -L -p <pid> | sort | uniq -c` "
               "复现（会看到一堆 `iou-wrk-*`），读路径不需要 punt，所以通常与 libaio 持平。")
    return out


# ===========================================================================
# 第四部分：命令行入口
# ===========================================================================


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="io-study 矩阵压测工具（网络 / 磁盘）",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="示例：\n"
               "  python3 scripts/bench_matrix.py net\n"
               "  python3 scripts/bench_matrix.py net --mode full --client cpp\n"
               "  python3 scripts/bench_matrix.py disk --bytes 268435456\n",
    )
    sub = p.add_subparsers(dest="target", required=True)

    n = sub.add_parser("net", help="网络 I/O 矩阵：连接数 / 消息大小 / TCP_NODELAY")
    n.add_argument("--mode", choices=["quick", "full"], default="quick")
    n.add_argument("--client", choices=["py", "cpp"], default="py",
                   help="压测端：py=bench.py（有延迟分位，受 GIL 限制）；"
                        "cpp=bench_client（无 GIL，测服务端上限）")
    n.add_argument("--secs", type=float, default=2.0, help="每点测量时长")
    n.add_argument("--warmup", type=float, default=1.0, help="每点预热时长")
    n.add_argument("--client-threads", type=int, default=8, help="py 客户端的线程数")
    n.add_argument("--mt-workers", type=int, default=0,
                   help="echo_epoll_mt 的 worker 数，0=交给服务端取 nproc")
    n.add_argument("--base-port", type=int, default=19200)
    n.add_argument("--nodelay", type=int, default=1, help="内部使用，勿手动指定")

    d = sub.add_parser("disk", help="磁盘 I/O 矩阵：队列深度 / 块大小 / O_DIRECT")
    d.add_argument("--mode", choices=["quick", "full"], default="quick")
    d.add_argument("--bytes", type=int, default=0,
                   help="每个测点的目标 I/O 字节数（默认 quick=64MB, full=256MB）")
    d.add_argument("--file", default="/tmp/iodemo_matrix",
                   help="测试文件路径，必须落在真实文件系统上（tmpfs 不支持 O_DIRECT）")
    d.add_argument("--sync-divisor", type=int, default=8,
                   help="同步方式的 I/O 次数 = 异步 / 该系数")
    d.add_argument("--timeout", type=int, default=180, help="单个测点超时（秒）")
    d.add_argument("--block", type=int, default=4096, help="维度 A/C 固定的块大小")
    d.add_argument("--depth", type=int, default=32, help="维度 B/C 固定的队列深度")
    d.add_argument("--direct", type=int, default=1, help="维度 A/B 固定的 O_DIRECT")
    return p


def main() -> int:
    args = build_parser().parse_args()

    if args.target == "net":
        for binary in ("echo_epoll", "echo_epoll_mt", "echo_io_uring"):
            if not (BIN / binary).exists():
                print("[提示] 缺少 bin/%s，请先 make net" % binary)
        return run_net(args)

    if args.bytes == 0:
        args.bytes = 67108864 if args.mode == "quick" else 268435456
    for binary in ("io_sync", "io_libaio", "io_uring_disk"):
        if not (BIN / binary).exists():
            print("[提示] 缺少 bin/%s，请先 make disk" % binary)
    return run_disk(args)


if __name__ == "__main__":
    sys.exit(main())
