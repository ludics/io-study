# ============================================================
#  io-study —— Linux I/O 模型实验项目 统一构建入口
#
#  覆盖三个方向：
#    disk/     磁盘 I/O：sync(pread/pwrite) vs libaio vs io_uring
#    network/  网络 I/O：epoll(Reactor) vs io_uring(Proactor)
#    libco/    协程：libco epoll 封装 + 上下文切换开销
#
#  常用目标：
#    make           编译全部（磁盘 + 网络 + O_NONBLOCK 实验）
#    make check     环境体检（内核/libaio/liburing/io_uring 是否可用）
#    make bench     磁盘 I/O 三方对比（sync / libaio / io_uring）
#    make bench_demo  epoll 为什么必须 O_NONBLOCK 对比实验
#    make bench_net   网络 echo 对比（epoll vs io_uring）
#    make libco     编译 libco（需先 git clone 到 third_party/libco）
#    make clean     清理
#
#  可调参数（命令行覆盖，如 make bench DIRECT=1 TOTAL=65536）：
#    FILE    测试文件路径      默认 /tmp/iodemo_testfile
#    BLOCK   单次 I/O 字节数   默认 4096
#    TOTAL   I/O 总次数        默认 65536
#    DEPTH   异步队列深度      默认 32
#    DIRECT  是否 O_DIRECT     默认 0（1=绕过 page cache，真盘测试）
#    PORT    网络 bench 端口   默认 18800
#    THREADS 压测客户端线程数  默认 4
#    SIZE    单条消息字节数    默认 256
#    SECS    压测时长(秒)      默认 5
# ============================================================

CC       := gcc
CXX      := g++
CFLAGS   := -O2 -Wall -D_GNU_SOURCE -D_REENTRANT
CXXFLAGS := -O2 -Wall -D_GNU_SOURCE -D_REENTRANT -std=c++11

LIBAIO_LIB := -laio
URING_LIB  := -luring
NET_LIBS   := -lpthread -ldl

# ---- 参数 ----
FILE    ?= /tmp/iodemo_testfile
BLOCK   ?= 4096
TOTAL   ?= 65536
DEPTH   ?= 32
DIRECT  ?= 0
PORT    ?= 18800
THREADS ?= 4
SIZE    ?= 256
SECS    ?= 5

# ---- 目录（全部相对，方便整体拷走）----
ROOT     := $(shell pwd)
BINDIR   := $(ROOT)/bin
NETDIR   := $(ROOT)/network
DISKDIR  := $(ROOT)/disk
LIBCODIR := $(ROOT)/libco
TPDIR    := $(ROOT)/third_party
LIBCO_SRC?= $(TPDIR)/libco

# ---- 目标 ----
DISK_TARGETS := $(BINDIR)/io_sync $(BINDIR)/io_libaio $(BINDIR)/io_uring_disk
EPOLL_DEMOS  := $(BINDIR)/epoll_nonblock_demo $(BINDIR)/epoll_starve_demo
NET_TARGETS  := $(BINDIR)/echo_epoll $(BINDIR)/echo_io_uring \
                $(BINDIR)/reactor_server $(BINDIR)/bench_client

.PHONY: all disk net demos libco bench bench_demo bench_net check clean help

all: disk net demos

help:
	@echo "io-study —— Linux I/O 模型实验项目"
	@echo ""
	@echo "  构建："
	@echo "    make            编译全部"
	@echo "    make disk       只编译磁盘 I/O 三版 (sync / libaio / io_uring)"
	@echo "    make net        只编译网络 (echo_epoll / echo_io_uring / reactor_server / bench_client)"
	@echo "    make demos      只编译 epoll O_NONBLOCK 验证 demo"
	@echo "    make libco      编译 libco 与协程 bench（需先 clone 到 third_party/libco）"
	@echo ""
	@echo "  运行："
	@echo "    make check      环境体检：内核/libaio/liburing/io_uring 可用性"
	@echo "    make bench      磁盘 I/O 对比  (make bench DIRECT=1 走真实磁盘)"
	@echo "    make bench_demo epoll 为何必须 O_NONBLOCK 的对比实验"
	@echo "    make bench_net  网络 echo 对比 (epoll vs io_uring)"
	@echo "    make clean      清理 bin/ 与测试文件"
	@echo ""
	@echo "  参数覆盖示例："
	@echo "    make bench TOTAL=65536 BLOCK=4096 DEPTH=32 DIRECT=1"
	@echo "    make bench_net PORT=18800 THREADS=8 SIZE=512 SECS=5"

$(BINDIR):
	@mkdir -p $(BINDIR)

# ================= 磁盘 I/O =================
disk: | $(BINDIR) $(DISK_TARGETS)

$(BINDIR)/io_sync: $(DISKDIR)/io_sync.c $(DISKDIR)/io_common.h | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $<
	@echo "  [OK] io_sync"

$(BINDIR)/io_libaio: $(DISKDIR)/io_libaio.c $(DISKDIR)/io_common.h | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $< $(LIBAIO_LIB)
	@echo "  [OK] io_libaio"

$(BINDIR)/io_uring_disk: $(DISKDIR)/io_uring_disk.c $(DISKDIR)/io_common.h | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $< $(URING_LIB)
	@echo "  [OK] io_uring_disk"

# ================= epoll O_NONBLOCK 验证 demo =================
demos: | $(BINDIR) $(EPOLL_DEMOS)

$(BINDIR)/epoll_nonblock_demo: $(DISKDIR)/epoll_nonblock_demo.c | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $<
	@echo "  [OK] epoll_nonblock_demo"

$(BINDIR)/epoll_starve_demo: $(DISKDIR)/epoll_starve_demo.c | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $<
	@echo "  [OK] epoll_starve_demo"

# ================= 网络 =================
net: | $(BINDIR) $(NET_TARGETS)

$(BINDIR)/echo_epoll: $(NETDIR)/echo_epoll.c | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $<
	@echo "  [OK] echo_epoll"

$(BINDIR)/echo_io_uring: $(NETDIR)/echo_io_uring.c | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $< $(URING_LIB)
	@echo "  [OK] echo_io_uring"

$(BINDIR)/reactor_server: $(NETDIR)/reactor_server.c | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $<
	@echo "  [OK] reactor_server"

$(BINDIR)/bench_client: $(NETDIR)/bench_client.cpp | $(BINDIR)
	$(CXX) $(CXXFLAGS) -o $@ $< $(NET_LIBS)
	@echo "  [OK] bench_client"

# ================= libco =================
libco:
	@if [ ! -d "$(LIBCO_SRC)" ]; then \
		echo "未找到 libco 源码，请先执行："; \
		echo "  mkdir -p third_party && git clone https://github.com/Tencent/libco $(LIBCO_SRC)"; \
		exit 1; \
	fi
	@echo ">>> 编译 libco ..."
	@$(MAKE) -C $(LIBCO_SRC) --no-print-directory 2>&1 | tail -5
	@echo ">>> 编译协程上下文切换 bench ..."
	@LIBCO_A=$$(ls $(LIBCO_SRC)/*.a 2>/dev/null | head -1); \
	if [ -z "$$LIBCO_A" ]; then echo "  [失败] 未找到 libco 静态库"; exit 1; fi; \
	$(CXX) $(CXXFLAGS) -I$(LIBCO_SRC) -o $(BINDIR)/bench_swap $(LIBCODIR)/bench_swap.cpp $$LIBCO_A $(NET_LIBS) \
	  && echo "  [OK] bench_swap"
	@echo ">>> libco 示例 echo server："
	@ls $(LIBCO_SRC)/example_echosvr 2>/dev/null || echo "  (未生成 example_echosvr，请查看 $(LIBCO_SRC) 下 Makefile 输出)"

# ================= 环境体检 =================
check:
	@echo "=========================================="
	@echo "  环境体检"
	@echo "=========================================="
	@echo "内核版本 : $$(uname -r)"
	@echo "发行版   : $$(. /etc/os-release 2>/dev/null && echo $$PRETTY_NAME || echo unknown)"
	@echo "CPU 核数 : $$(nproc)"
	@echo ""
	@printf "libaio 头文件  : "; [ -f /usr/include/libaio.h ] && echo "有" || echo "无 (apt install libaio-dev / yum install libaio-devel)"
	@printf "liburing 头文件: "; [ -f /usr/include/liburing.h ] && echo "有" || echo "无 (apt install liburing-dev / yum install liburing-devel)"
	@echo ""
	@printf "io_uring 运行时可用性 : "
	@if [ -f /proc/sys/kernel/io_uring_disabled ]; then \
		v=$$(cat /proc/sys/kernel/io_uring_disabled); \
		if [ "$$v" = "0" ]; then echo "允许 (io_uring_disabled=0)"; else echo "被禁用 (io_uring_disabled=$$v)"; fi; \
	else echo "内核未导出 io_uring_disabled（<5.12 或已编译时关闭）"; fi
	@printf "io_uring_setup 实测   : "
	@$(CC) -o /tmp/_iour_chk $(ROOT)/scripts/uring_probe.c 2>/dev/null \
	  && (/tmp/_iour_chk || true) || echo "探针编译失败"
	@rm -f /tmp/_iour_chk
	@echo ""
	@echo "提示：io_uring 若不可用，常见原因是容器 seccomp 默认拦截"
	@echo "      docker 需加 --security-opt seccomp=unconfined"

# ================= 磁盘 I/O 性能对比 =================
bench: disk
	@echo ""
	@echo "=========================================="
	@echo "  磁盘 I/O 性能对比"
	@echo "  文件=$(FILE) 块=$(BLOCK) 总数=$(TOTAL) 深度=$(DEPTH) O_DIRECT=$(DIRECT)"
	@echo "=========================================="
	@echo ""
	@echo ">>> [1/3] 同步 I/O (pread/pwrite) —— baseline"
	@$(BINDIR)/io_sync $(FILE) $(BLOCK) $(TOTAL) $(DIRECT) || true
	@echo ""
	@echo ">>> [2/3] libaio (Linux AIO)"
	@$(BINDIR)/io_libaio $(FILE) $(BLOCK) $(TOTAL) $(DEPTH) $(DIRECT) || true
	@echo ""
	@echo ">>> [3/3] io_uring"
	@$(BINDIR)/io_uring_disk $(FILE) $(BLOCK) $(TOTAL) $(DEPTH) $(DIRECT) || \
	  echo "  [跳过] io_uring 在当前环境不可用，代码已编译但无法运行"
	@echo ""
	@echo "=========================================="
	@echo "  对比完成。建议再跑一次 O_DIRECT：make bench DIRECT=1"
	@echo "=========================================="

# ================= epoll O_NONBLOCK 实验 =================
bench_demo: demos
	@echo ""
	@echo "=========================================="
	@echo "  epoll 为什么必须 O_NONBLOCK —— 对比实验"
	@echo "=========================================="
	@echo ""
	@echo ">>> [实验1] 单连接：ET + 非阻塞 fd vs ET + 阻塞 fd"
	@echo "--- 非阻塞（正确写法）---"
	@timeout 20 $(BINDIR)/epoll_nonblock_demo 1 2>&1 | tail -3
	@echo ""
	@echo "--- 阻塞（错误写法：read 挂死整个事件循环）---"
	@timeout 25 $(BINDIR)/epoll_nonblock_demo 0 2>&1 | tail -3
	@echo ""
	@echo ">>> [实验2] 双连接：阻塞 fd 饿死其它连接"
	@echo "--- 非阻塞（正确：两个连接都被及时处理）---"
	@timeout 30 $(BINDIR)/epoll_starve_demo 1 2>&1 | tail -4
	@echo ""
	@echo "--- 阻塞（错误：第 2 个连接被饿死约 3 秒）---"
	@timeout 35 $(BINDIR)/epoll_starve_demo 0 2>&1 | tail -4
	@echo ""
	@echo "=========================================="
	@echo "  实验完成"
	@echo "=========================================="

# ================= 网络 echo 对比 =================
bench_net: net
	@echo ""
	@echo "=========================================="
	@echo "  网络 Echo 对比：epoll vs io_uring"
	@echo "  端口=$(PORT) 线程=$(THREADS) 消息=$(SIZE)B 时长=$(SECS)s"
	@echo "=========================================="
	@echo ""
	@echo ">>> [1/2] epoll (Reactor)"
	@$(BINDIR)/echo_epoll $(PORT) > /tmp/echo_epoll.log 2>&1 & \
	 SRV=$$!; sleep 1; \
	 python3 $(NETDIR)/bench.py $(PORT) $(THREADS) $(SIZE) $(SECS) 2>/dev/null || \
	   echo "  (bench.py 不可用，可改用 bin/bench_client)"; \
	 kill -9 $$SRV 2>/dev/null; sleep 0.3
	@echo ""
	@echo ">>> [2/2] io_uring (Proactor)"
	@$(BINDIR)/echo_io_uring $(PORT) > /tmp/echo_uring.log 2>&1 & \
	 SRV=$$!; sleep 1; \
	 if kill -0 $$SRV 2>/dev/null; then \
	   python3 $(NETDIR)/bench.py $$((PORT+1)) $(THREADS) $(SIZE) $(SECS) 2>/dev/null || true; \
	 else \
	   echo "  [跳过] io_uring 服务端启动失败：$$(cat /tmp/echo_uring.log | head -2)"; \
	 fi; \
	 kill -9 $$SRV 2>/dev/null; sleep 0.3
	@echo ""
	@echo "=========================================="
	@echo "  对比完成"
	@echo "=========================================="

# ================= 清理 =================
clean:
	@rm -rf $(BINDIR)
	@rm -f $(FILE)
	@echo "已清理 bin/ 与 $(FILE)"
