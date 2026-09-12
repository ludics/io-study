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
#    make bench_net_matrix  网络 I/O 多维度对比（服务端 × 连接数 × 消息大小 × NODELAY）
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
#    MODE    bench_*_matrix 的扫描档位  默认 quick（可选 full）
#    CLIENT  bench_net_matrix 压测端     默认 py（可选 cpp，见 README 实验四）
# ============================================================

CC       := gcc
CXX      := g++
CFLAGS   := -O2 -Wall -D_GNU_SOURCE -D_REENTRANT
CXXFLAGS := -O2 -Wall -D_GNU_SOURCE -D_REENTRANT -std=c++11

LIBAIO_LIB := -laio
URING_LIB  := -luring
NET_LIBS   := -lpthread -ldl

# ---- 平台检测 ----
# 本项目的核心实现（epoll / io_uring / libaio）都是 **Linux 专有** 的：
# macOS 上既没有 <sys/epoll.h>，也没有 io_uring / libaio 这两套内核接口。
# 所以 macOS 侧单独提供了一组「对等物」实现：
#     network/echo_kqueue.c   kqueue 版 Reactor echo server（≈ echo_epoll.c）
#     disk/io_macos.c         F_NOCACHE + 多线程模拟并发深度（≈ io_sync / io_libaio）
# 压测客户端（network/bench.py、network/bench_client.cpp）两边通用，无需改动。
UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
  CC         := cc
  CXX        := c++
  CFLAGS     := -O2 -Wall -Wextra
  CXXFLAGS   := -O2 -Wall -Wextra -std=c++17
  NET_LIBS   := -lpthread
  LIBAIO_LIB :=
  URING_LIB  :=
  # 注意用 =（递归展开）而不是 := —— BINDIR 在下面才定义，:= 会立刻展开成空
  MAC_TARGETS = $(BINDIR)/echo_kqueue $(BINDIR)/io_macos $(BINDIR)/bench_client
endif

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
MODE    ?= quick
CLIENT  ?= py

# ---- 目录（全部相对，方便整体拷走）----
ROOT     := $(shell pwd)
BINDIR   := $(ROOT)/bin
NETDIR   := $(ROOT)/network
DISKDIR  := $(ROOT)/disk
LIBCODIR := $(ROOT)/libco
EXDIR    := $(ROOT)/examples
TPDIR    := $(ROOT)/third_party
LIBCO_SRC?= $(TPDIR)/libco

# ---- 目标 ----
DISK_TARGETS := $(BINDIR)/io_sync $(BINDIR)/io_libaio $(BINDIR)/io_uring_disk
EPOLL_DEMOS  := $(BINDIR)/epoll_nonblock_demo $(BINDIR)/epoll_starve_demo
EXAMPLES     := $(BINDIR)/ex_epoll_echo $(BINDIR)/ex_io_uring_echo $(BINDIR)/ex_libaio_rw
NET_TARGETS  := $(BINDIR)/echo_epoll $(BINDIR)/echo_epoll_mt $(BINDIR)/echo_io_uring \
                $(BINDIR)/echo_io_uring_adv \
                $(BINDIR)/reactor_server $(BINDIR)/bench_client

# echo_io_uring_modern 是「只面向新环境」的写法，依赖 liburing 较新的 API：
#   direct accept / 稀疏固定文件表 / 批量取 CQE（liburing ≥ 2.6）
# 老发行版（例如 Ubuntu 22.04 自带的 2.1）没有这些符号，硬编会**整个构建失败**。
# 所以这里在解析阶段探一下头文件，缺符号就把它从目标列表里摘掉，并给出提示。
# io_uring_sqe_set_data64() 是 liburing 2.2 才加入的（Ubuntu 22.04 自带的 2.1 没有）。
# 探测一下，让 examples/io_uring_echo.c 自动走对分支。
URING_DATA64 := $(shell \
  grep -q io_uring_sqe_set_data64 /usr/include/liburing.h 2>/dev/null && echo yes || echo no)

URING_MODERN_OK := $(shell \
  grep -q io_uring_prep_multishot_accept_direct /usr/include/liburing.h 2>/dev/null && \
  grep -q io_uring_register_files_sparse          /usr/include/liburing.h 2>/dev/null && \
  grep -q io_uring_peek_batch_cqe                 /usr/include/liburing.h 2>/dev/null \
  && echo yes || echo no)

ifeq ($(URING_MODERN_OK),yes)
NET_TARGETS += $(BINDIR)/echo_io_uring_modern
endif

# ---- macOS：清空 Linux 专有目标列表 ----
# epoll / io_uring / libaio 都是 Linux 内核接口，macOS 上连头文件都没有。
# 这里把目标列表清空，规则本身保留（Linux 上照常工作），
# 于是 make disk / net / demos / examples 在 macOS 上不会去编译注定失败的文件。
# 另外把 URING_MODERN_OK 置为 yes，避免 net 目标里那段「跳过提示」被当成 recipe。
ifeq ($(UNAME_S),Darwin)
  DISK_TARGETS    :=
  EPOLL_DEMOS     :=
  EXAMPLES        :=
  NET_TARGETS     :=
  URING_MODERN_OK := yes
endif

.PHONY: all disk net demos examples libco bench bench_demo bench_net bench_net_matrix \
        bench_disk_matrix macos check clean help FORCE

all: disk net demos

help:
	@echo "io-study —— Linux I/O 模型实验项目"
	@echo ""
	@echo "  构建："
	@echo "    make            编译全部"
	@echo "    make disk       只编译磁盘 I/O 三版 (sync / libaio / io_uring)"
	@echo "    make net        只编译网络 (echo_epoll / echo_epoll_mt / echo_io_uring[_adv|_modern] / reactor_server / bench_client)"
	@echo "                    _modern 需要 liburing >= 2.6，老环境会自动跳过并提示"
	@echo "    make demos      只编译 epoll O_NONBLOCK 验证 demo"
	@echo "    make examples   只编译 docs/md/04~06 三篇编程指南的配套示例"
	@if [ "$(UNAME_S)" = "Darwin" ]; then \
	  echo "    make macos      编译 macOS 对等实现 (echo_kqueue / io_macos / bench_client)"; \
	fi
	@echo "    make libco      编译 libco 与协程 bench（需先 clone 到 third_party/libco）"
	@echo ""
	@echo "  运行："
	@echo "    make check      环境体检：内核/libaio/liburing/io_uring 可用性"
	@echo "    make bench      磁盘 I/O 对比  (make bench DIRECT=1 走真实磁盘)"
	@echo "    make bench_demo epoll 为何必须 O_NONBLOCK 的对比实验"
	@echo "    make bench_net  网络 echo 对比 (epoll vs io_uring)"
	@echo "    make bench_net_matrix  网络多维矩阵对比 (make bench_net_matrix MODE=full CLIENT=cpp)"
	@echo "    make bench_disk_matrix 磁盘多维矩阵对比 (连接数无关；深度/块大小/O_DIRECT)"
	@echo "    make clean      清理 bin/ 与测试文件"
	@echo ""
	@echo "  参数覆盖示例："
	@echo "    make bench TOTAL=65536 BLOCK=4096 DEPTH=32 DIRECT=1"
	@echo "    make bench_net PORT=18800 THREADS=8 SIZE=512 SECS=5"
	@echo "    make bench_net_matrix MODE=full"

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
ifeq ($(URING_MODERN_OK),no)
	@echo "  [跳过] echo_io_uring_modern：本机 liburing 缺少所需 API"
	@echo "         （需要 direct accept / 稀疏固定文件表 / 批量取 CQE，liburing ≥ 2.6）"
	@echo "         想编译它请升级 liburing；其余目标不受影响。"
endif

$(BINDIR)/echo_epoll: $(NETDIR)/echo_epoll.c | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $<
	@echo "  [OK] echo_epoll"

$(BINDIR)/echo_epoll_mt: $(NETDIR)/echo_epoll_mt.c | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $< $(NET_LIBS)
	@echo "  [OK] echo_epoll_mt"

# io_uring 火力全开版：SQPOLL + 提供缓冲区环 + multishot recv + zerocopy send
# 各特性可用环境变量独立开关（见文件头注释），并会在启动时打印能力自检表
$(BINDIR)/echo_io_uring_adv: $(NETDIR)/echo_io_uring_adv.c | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $< $(URING_LIB)
	@echo "  [OK] echo_io_uring_adv"

# io_uring 现代版：只面向新内核（≥6.6）+ 新 liburing（≥2.6），没有任何兼容包袱。
# 用上 multishot accept + direct file table + 提供缓冲区环 + SQPOLL + CQ 忙轮询，
# 实测稳态系统调用为 0、吞吐约 2x epoll（详见文件头与 README「实验五」）。
# 用法示例（SQPOLL 内核线程单独绑一个核）：
#   ECHO_SQ_CPU=5 taskset -c 4 ./bin/echo_io_uring_modern 19002
$(BINDIR)/echo_io_uring_modern: $(NETDIR)/echo_io_uring_modern.c | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $< $(URING_LIB)
	@echo "  [OK] echo_io_uring_modern"

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
# third_party/libco 支持两种布局（本仓库用的是「重构版」）：
#   重构版：build/lib/libcolib.a + build/bin/example_echosvr + src/ 下放头文件
#   上游原版：根目录 libco.a + example_echosvr + 头文件在根目录
libco:
	@if [ ! -d "$(LIBCO_SRC)" ]; then \
		echo "未找到 libco 源码，请先执行："; \
		echo "  mkdir -p third_party && git clone https://github.com/Tencent/libco $(LIBCO_SRC)"; \
		exit 1; \
	fi
	@echo ">>> 编译 libco ..."
	@$(MAKE) -C $(LIBCO_SRC) --no-print-directory 2>&1 | tail -5
	@set -e; \
	LIBCO_A=$$(ls $(LIBCO_SRC)/build/lib/libcolib.a $(LIBCO_SRC)/*.a 2>/dev/null | head -1); \
	if [ -z "$$LIBCO_A" ]; then echo "  [失败] 未找到 libco 静态库（build/lib/libcolib.a 或 libco.a）"; exit 1; fi; \
	if [ -f "$(LIBCO_SRC)/src/co_routine.h" ]; then LIBCO_INC="$(LIBCO_SRC)/src"; else LIBCO_INC="$(LIBCO_SRC)"; fi; \
	echo "  静态库: $$LIBCO_A"; \
	echo "  头文件: $$LIBCO_INC"; \
	$(CXX) $(CXXFLAGS) -I$$LIBCO_INC -o $(BINDIR)/bench_swap $(LIBCODIR)/bench_swap.cpp $$LIBCO_A $(NET_LIBS) \
	  && echo "  [OK] bench_swap"
	@echo ">>> libco 示例 echo server："
	@ECHO_BIN=$$(ls $(LIBCO_SRC)/build/bin/example_echosvr $(LIBCO_SRC)/example_echosvr 2>/dev/null | head -1); \
	 if [ -n "$$ECHO_BIN" ]; then echo "  $$ECHO_BIN"; \
	 else echo "  (未生成 example_echosvr，请查看 $(LIBCO_SRC) 的 Makefile 输出)"; fi

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
	@$(BINDIR)/echo_io_uring $$((PORT+1)) > /tmp/echo_uring.log 2>&1 & \
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

# ================= 网络多维度矩阵对比 =================
#   make bench_net_matrix                  quick 模式，Python 压测端
#   make bench_net_matrix MODE=full        更细的扫点
#   make bench_net_matrix CLIENT=cpp       换 C++ 压测端（无 GIL，测服务端上限）
#
# 实现见 scripts/bench_matrix.py（网络和磁盘共用一个工具，纯 Python，无 shell 嵌套）
bench_net_matrix: net
	@python3 $(ROOT)/scripts/bench_matrix.py net --mode $(MODE) --client $(CLIENT)

# ================= 磁盘多维度矩阵对比 =================
#   make bench_disk_matrix                 quick 模式
#   make bench_disk_matrix MODE=full       更细的扫点
#
# BYTES / FILE 等参数请直接调脚本（更好读）：
#   python3 scripts/bench_matrix.py disk --bytes 268435456 --file /data/iodemo
bench_disk_matrix: disk
	@python3 $(ROOT)/scripts/bench_matrix.py disk --mode $(MODE)

# ================= 教学示例 =================
# 与 docs/md/04~06 三篇编程指南配套的最小可运行示例。刻意写得「教科书式正确」：
# 非阻塞、ET 读空、循环写、对齐、批量收割等该做的都做了，适合当模板抄。
examples: | $(BINDIR) $(EXAMPLES)

$(BINDIR)/ex_epoll_echo: $(EXDIR)/epoll_echo.c | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $<
	@echo "  [OK] ex_epoll_echo"

$(BINDIR)/ex_io_uring_echo: $(EXDIR)/io_uring_echo.c | $(BINDIR)
	$(CC) $(CFLAGS) $(if $(filter yes,$(URING_DATA64)),-DHAVE_SQE_DATA64=1,) -o $@ $< $(URING_LIB)
	@echo "  [OK] ex_io_uring_echo"

$(BINDIR)/ex_libaio_rw: $(EXDIR)/libaio_rw.c | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $< $(LIBAIO_LIB)
	@echo "  [OK] ex_libaio_rw"

# ================= 清理 =================
clean:
	@rm -rf $(BINDIR)
	@rm -f $(FILE)
	@echo "已清理 bin/ 与 $(FILE)"

# ================= 构建戳（防止多台机器共用一份挂载目录时互相覆盖）=================
#
# 背景：工作区通过 sshfs 挂载进虚拟机时，bin/ 是**所有虚拟机共享**的。
# 两台机器（比如 Ubuntu 22.04 与 26.04）各自 make 一次就会互相覆盖对方编出来的
# 可执行文件 —— 后编的那台的动态库依赖（glibc/libaio 版本）会让先编的那台跑不起来。
# 这个戳记下「bin/ 里的产物是哪台机器、哪个内核、什么时候编的」，
# scripts/bench_matrix.py 会在压测前核对它，不匹配就拒绝跑（而非默默测错东西）。
$(BINDIR)/.build-stamp: FORCE | $(BINDIR)
	@printf '%s|%s|%s|%s\n' "$$(hostname)" "$$(uname -r)" "$$(uname -m)" "$$(date '+%F %T')" > $@

FORCE:

all disk net demos examples: $(BINDIR)/.build-stamp

# ================= macOS 平台段 =================
#
# 关键：下面只给 Linux 专有目标**加一句提示**，不重定义它们的 recipe ——
# 两个 recipe 定义同一个目标会让 make 报 "overriding commands for target ..." 警告。
# 只加前置依赖（prerequisite）是合法的，会与原 recipe 合并。
ifeq ($(UNAME_S),Darwin)

linux-guard:
	@echo "  ───────────────────────────────────────────────────────────────"
	@echo "  [跳过] 该目标依赖 Linux 专有的 epoll / io_uring / libaio，macOS 上"
	@echo "         没有对应的内核接口，无法编译也无法运行。"
	@echo "         macOS 请用：make macos"
	@echo "         对等实现：network/echo_kqueue.c（kqueue）、disk/io_macos.c（F_NOCACHE）"
	@echo "  ───────────────────────────────────────────────────────────────"

disk net demos examples libco: linux-guard
bench bench_demo bench_net bench_net_matrix bench_disk_matrix: linux-guard

# ---- macOS 上真正能编的东西 ----
#   echo_kqueue   kqueue 版单线程 Reactor echo server  （Linux 上对应 echo_epoll.c）
#   io_macos      F_NOCACHE + 多线程模拟并发深度        （Linux 上对应 io_sync.c / io_libaio.c）
#   bench_client  纯 POSIX socket，两个平台通用（规则在上面的网络段里，这里不重定义）
macos: $(BINDIR)/.build-stamp
macos: | $(BINDIR) $(MAC_TARGETS)

$(BINDIR)/echo_kqueue: $(NETDIR)/echo_kqueue.c | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $<
	@echo "  [OK] echo_kqueue   （kqueue，对应 Linux 的 echo_epoll）"

$(BINDIR)/io_macos: $(DISKDIR)/io_macos.c | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $< -lpthread
	@echo "  [OK] io_macos      （F_NOCACHE，对应 Linux 的 io_sync / io_libaio）"

endif
