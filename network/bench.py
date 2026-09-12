#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
网络 echo 压测客户端（io-study）

用于对比「不同情况」下的网络 I/O 性能，相比初版增加：
  * 连接数与线程数解耦（--conns）：每条连接始终保留 1 个在途请求，因此
    「在途请求数 = 连接数」，并发度可被真实控制（QPS ≈ 并发度 / RTT）。
    初版对每条连接串行 send→recv，在途请求数实际只有「线程数」那么多，
    加大 --conns 只会平白增加开销 —— 这是本次重写的主要原因
  * 消息大小、测试时长可调
  * 预热（--warmup）：预热期不计入统计，避免冷启动（页表/分支预测/TCP 慢启动）污染
  * 客户端观测 RTT 分位：p50 / p90 / p99 / p999 / max（1µs 分辨率直方图）
  * 压测端自身 CPU 占用：用来判断「到底是服务端到顶了，还是压测端先饱和」
  * --json / --csv 机读输出，便于 scripts/bench_net_matrix.sh 汇总成表

用法:
    python3 bench.py <port> [线程数] [消息字节] [时长秒] [选项]

选项:
    --conns N        总连接数（默认 = 线程数）。每条连接 1 个在途请求，
                     所以这个值就是并发度；线程只决定用几个核去驱动
    --warmup S       预热秒数（默认 0）
    --nodelay 0|1    是否设置 TCP_NODELAY（默认 1）
    --json           以 JSON 输出（结果走 stdout，进度走 stderr）
    --csv            以单行 CSV 输出，列序见 CSV_COLUMNS

示例:
    python3 bench.py 18800 8 256 5
    python3 bench.py 18800 8 256 5 --conns 256 --warmup 1
    python3 bench.py 18800 4 64  3 --conns 64 --json
"""

import json
import os
import resource
import select
import selectors
import socket
import sys
import threading
import time

# RTT 直方图：0 ~ 20ms 按 1µs 分档，超出部分并入最后一档（另记 max）
HIST_MAX_US = 20000
HIST_LEN = HIST_MAX_US + 1

CSV_COLUMNS = [
    "port", "threads", "conns", "size", "secs",
    "total", "qps", "errors",
    "p50_us", "p90_us", "p99_us", "p999_us", "max_us",
    "MBps", "Gbps",
    "client_cpu_pct", "ncpu",
]


def cpu_seconds():
    """进程累计 CPU 时间（user+sys，含所有线程）——用于判断压测端是否先饱和。"""
    r = resource.getrusage(resource.RUSAGE_SELF)
    return r.ru_utime + r.ru_stime


def parse_args(argv):
    if len(argv) < 2:
        sys.stderr.write(__doc__ + "\n")
        sys.exit(2)

    try:
        port = int(argv[1])
        threads = int(argv[2]) if len(argv) > 2 else 8
        size = int(argv[3]) if len(argv) > 3 else 512
        secs = float(argv[4]) if len(argv) > 4 else 5.0
    except ValueError:
        sys.stderr.write("[错误] 位置参数必须是数字：<port> [线程数] [消息字节] [时长秒]\n")
        sys.exit(2)

    cfg = {
        "conns": None,
        "warmup": 0.0,
        "nodelay": 1,
        "fmt": "text",
    }

    i = 5
    while i < len(argv):
        arg = argv[i]
        key, _, inline = arg.partition("=")

        def take_value():
            if inline:
                return inline
            nonlocal i
            if i + 1 >= len(argv):
                sys.stderr.write("[错误] 参数 %s 缺少取值\n" % key)
                sys.exit(2)
            i += 1
            return argv[i]

        if key == "--conns":
            cfg["conns"] = int(take_value())
        elif key == "--warmup":
            cfg["warmup"] = float(take_value())
        elif key == "--nodelay":
            cfg["nodelay"] = int(take_value())
        elif key == "--json":
            cfg["fmt"] = "json"
        elif key == "--csv":
            cfg["fmt"] = "csv"
        elif key in ("-h", "--help"):
            sys.stderr.write(__doc__ + "\n")
            sys.exit(0)
        else:
            sys.stderr.write("[错误] 未知参数: %s\n" % arg)
            sys.exit(2)
        i += 1

    if cfg["conns"] is None:
        cfg["conns"] = threads

    if port <= 0 or threads <= 0 or size <= 0 or secs <= 0 or cfg["conns"] <= 0:
        sys.stderr.write("[错误] 所有数值参数必须为正数\n")
        sys.exit(2)
    if cfg["conns"] < threads:
        threads = cfg["conns"]  # 连接数少于线程数时收缩线程数

    cfg.update({"port": port, "threads": threads, "size": size, "secs": secs})
    return cfg


def new_hist():
    return [0] * HIST_LEN


def connect_one(cfg):
    """建立一条开启（可选）TCP_NODELAY 的连接；失败返回 None。"""
    s = None
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        if cfg["nodelay"]:
            s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        s.connect(("127.0.0.1", cfg["port"]))
        return s
    except OSError:
        if s is not None:
            try:
                s.close()
            except OSError:
                pass
        return None


def send_all_nb(sock, payload):
    """非阻塞写完整 payload。小消息通常一次写完；真写不下时短暂等待可写。"""
    view = memoryview(payload)
    sent = 0
    total = len(payload)
    while sent < total:
        try:
            n = sock.send(view[sent:])
        except BlockingIOError:
            select.select([], [sock], [], 0.1)
            continue
        except OSError:
            return False
        if n <= 0:
            return False
        sent += n
    return True


def pump_recv(m, size):
    """把该连接上已到达的数据读干；返回 True 表示一个完整响应已收齐。"""
    sock = m["sock"]
    while m["got"] < size:
        try:
            chunk = sock.recv(size - m["got"])
        except BlockingIOError:
            return False
        if not chunk:
            raise ConnectionError("对端关闭")
        m["got"] += len(chunk)
    return True


def worker(tid, n_conns, cfg, shared):
    """每个线程用 selectors 多路复用 n_conns 条连接，且每条连接始终保留 1 个在途请求。

    于是「在途请求数 == 连接数」，并发度由 --conns 真实控制。
    这一点很关键：如果对每条连接串行 send→recv（本脚本的初版写法），线程会阻塞在
    该连接的 RTT 上，在途请求数实际被压到「线程数」——此时加大 --conns 只会白白增加
    每连接的 Python 开销，QPS 反而不升反降（Little's law: QPS ≈ 并发度 / RTT）。
    """
    stats = {"ok": 0, "err": 0, "hist": new_hist(), "max": 0}
    shared["slots"][tid] = stats

    size = cfg["size"]
    payload = b"A" * size
    stop = shared["stop"]
    sel = selectors.DefaultSelector()

    def attach():
        s = connect_one(cfg)
        if s is None:
            return None
        s.setblocking(False)
        m = {"sock": s, "got": 0, "t0": None, "gen": shared["gen"], "idx": -1}
        sel.register(s, selectors.EVENT_READ, m)
        return m

    def drop(m):
        try:
            sel.unregister(m["sock"])
        except (KeyError, ValueError):
            pass
        try:
            m["sock"].close()
        except OSError:
            pass

    conns = [None] * n_conns
    for i in range(n_conns):
        m = attach()
        if m is None:
            stats["err"] += 1
        else:
            m["idx"] = i
            conns[i] = m

    my_gen = shared["gen"]  # 0 = 预热/冷启动期，>=1 = 测量期

    try:
        while not stop.is_set():
            gen = shared["gen"]
            if my_gen != gen:
                # 进入新一轮（测量期开始）：清零预热期数据
                my_gen = gen
                stats["ok"] = 0
                stats["err"] = 0
                stats["hist"] = new_hist()
                stats["max"] = 0

            # ① 给空闲连接挂上新请求，保持「每连接 1 个在途请求」
            for i in range(n_conns):
                if stop.is_set():
                    break
                m = conns[i]
                if m is not None and m["t0"] is not None:
                    continue  # 该连接已有在途请求
                if m is None:
                    m = attach()
                    if m is None:
                        stats["err"] += 1
                        stop.wait(0.001)  # 退避，避免连接失败时空转刷 CPU
                        continue
                    m["idx"] = i
                    conns[i] = m
                try:
                    if not send_all_nb(m["sock"], payload):
                        raise ConnectionError("send 失败")
                    m["got"] = 0
                    m["gen"] = gen
                    m["t0"] = time.perf_counter()
                except OSError:
                    stats["err"] += 1
                    drop(m)
                    conns[i] = None

            # ② 收割已完成的响应
            for key, _ in sel.select(timeout=0.05):
                m = key.data
                idx = m["idx"]
                try:
                    if not pump_recv(m, size):
                        continue  # 还没收全，等下次
                except OSError:
                    stats["err"] += 1
                    drop(m)
                    conns[idx] = None
                    continue

                rtt_us = int((time.perf_counter() - m["t0"]) * 1e6)
                # 只统计「测量期发出、且测量期内收回」的请求（预热期在途的不计入）
                if m["gen"] >= 1 and m["gen"] == gen:
                    stats["ok"] += 1
                    if rtt_us > stats["max"]:
                        stats["max"] = rtt_us
                    stats["hist"][rtt_us if rtt_us < HIST_MAX_US else HIST_MAX_US] += 1
                m["t0"] = None
    finally:
        for m in conns:
            if m is not None:
                drop(m)
        sel.close()


def percentile(hist, total, ratio):
    """直方图求分位（返回 µs 近似值）。"""
    if total <= 0:
        return 0
    target = total * ratio
    cum = 0
    for idx, cnt in enumerate(hist):
        if cnt:
            cum += cnt
            if cum >= target:
                return idx
    return HIST_MAX_US


def summarize(cfg, slots, elapsed):
    total = sum(s["ok"] for s in slots)
    errors = sum(s["err"] for s in slots)

    hist = new_hist()
    max_us = 0
    for s in slots:
        h, m = s["hist"], s["max"]
        if m > max_us:
            max_us = m
        for idx, cnt in enumerate(h):
            if cnt:
                hist[idx] += cnt

    p50 = percentile(hist, total, 0.50)
    p90 = percentile(hist, total, 0.90)
    p99 = percentile(hist, total, 0.99)
    p999 = percentile(hist, total, 0.999)

    denom = elapsed if elapsed > 0 else 1e-9
    qps = int(total / denom)
    mbps = total * cfg["size"] / denom / (1024.0 * 1024.0)

    return {
        "port": cfg["port"],
        "threads": cfg["threads"],
        "conns": cfg["conns"],
        "size": cfg["size"],
        "secs": round(elapsed, 3),
        "total": total,
        "qps": qps,
        "per_thread": [s["ok"] for s in slots],
        "errors": errors,
        "latency_us": {
            "p50": p50, "p90": p90, "p99": p99, "p999": p999,
            "max": max_us,
        },
        "throughput_MBps": round(mbps, 2),
        "throughput_Gbps": round(mbps * 8 / 1024.0, 3),
        "nodelay": cfg["nodelay"],
    }


def print_text(r):
    lat = r["latency_us"]
    print("=== 压测结果 ===")
    print("线程数: %d, 连接数: %d, 数据大小: %d字节, 时长: %.1fs%s"
          % (r["threads"], r["conns"], r["size"], r["secs"],
             "" if r["nodelay"] else ", TCP_NODELAY=off"))
    print("总请求数: %d" % r["total"])
    print("平均 QPS: %d" % r["qps"])
    print("每线程: %s" % (r["per_thread"],))
    print("延迟 RTT(µs): p50=%d p90=%d p99=%d p999=%d max=%d"
          % (lat["p50"], lat["p90"], lat["p99"], lat["p999"], lat["max"]))
    print("吞吐: %.2f MB/s (%.3f Gbps)" % (r["throughput_MBps"], r["throughput_Gbps"]))
    cores = r["client_cpu_pct"] / 100.0
    # Python 有 GIL：单个压测进程的 Python 执行基本只能吃满 ~1 核，
    # 所以「用掉 ≥1 核」就说明压测端可能已经到顶，而不是机器核数被占满
    hint = "，已到单核 Python 执行上限（GIL），瓶颈可能在压测端" if cores >= 0.9 else ""
    print("压测端 CPU: %.2fs (%.0f%% 单核 ≈ %.2f/%d 核%s)"
          % (r["client_cpu_s"], r["client_cpu_pct"], cores, r["ncpu"], hint))
    print("错误: %d" % r["errors"])


def print_csv(r):
    lat = r["latency_us"]
    row = [
        r["port"], r["threads"], r["conns"], r["size"], r["secs"],
        r["total"], r["qps"], r["errors"],
        lat["p50"], lat["p90"], lat["p99"], lat["p999"], lat["max"],
        r["throughput_MBps"], r["throughput_Gbps"],
        r["client_cpu_pct"], r["ncpu"],
    ]
    print(",".join(str(v) for v in row))


def main():
    cfg = parse_args(sys.argv)

    shared = {
        "stop": threading.Event(),
        "gen": 0,
        "slots": [None] * cfg["threads"],
    }

    per_thread = [cfg["conns"] // cfg["threads"]] * cfg["threads"]
    for i in range(cfg["conns"] % cfg["threads"]):
        per_thread[i] += 1

    threads = []
    for tid in range(cfg["threads"]):
        t = threading.Thread(
            target=worker, args=(tid, per_thread[tid], cfg, shared), daemon=True
        )
        threads.append(t)
        t.start()

    if cfg["warmup"] > 0:
        time.sleep(cfg["warmup"])
        sys.stderr.write("[bench] 预热 %.1fs 完成，开始测量\n" % cfg["warmup"])

    shared["gen"] = 1  # 进入测量期，各线程在下一次循环时清零计数
    cpu0 = cpu_seconds()
    t_start = time.perf_counter()
    time.sleep(cfg["secs"])
    elapsed = time.perf_counter() - t_start
    client_cpu = cpu_seconds() - cpu0
    shared["stop"].set()

    stuck = 0
    for t in threads:
        t.join(timeout=5)
        if t.is_alive():
            stuck += 1

    slots = [s for s in shared["slots"] if s is not None]
    result = summarize(cfg, slots, elapsed)

    ncpu = os.cpu_count() or 1
    result["client_cpu_s"] = round(client_cpu, 3)
    # 相对「单核」的占用百分比：>= 100 表示用满了不止一个核
    result["client_cpu_pct"] = round(client_cpu / elapsed * 100, 1) if elapsed > 0 else 0.0
    result["ncpu"] = ncpu

    if result["errors"] == 0 and stuck == 0 and result["total"] == 0:
        sys.stderr.write(
            "[bench] 警告：0 个成功请求。请确认服务端已在 127.0.0.1:%d 监听。\n"
            % cfg["port"]
        )

    if cfg["fmt"] == "json":
        print(json.dumps(result, ensure_ascii=False))
    elif cfg["fmt"] == "csv":
        print_csv(result)
    else:
        print_text(result)

    if stuck:
        sys.stderr.write("[bench] 警告：%d 个线程在 5s 内未退出（服务端可能已断开）\n" % stuck)
    return 0 if result["total"] > 0 else 1


if __name__ == "__main__":
    sys.exit(main())
