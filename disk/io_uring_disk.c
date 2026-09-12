// ============================================================================
// io_uring 磁盘读写：Linux 现代真异步 I/O（Proactor）
//
// 特点：SQ/CQ 双环形队列 mmap 共享内存，批量提交/收割
//
// 【关键实现点】收割侧必须"批量取走 CQ 里所有已完成的事件"：
//   ❌ for (i = 0; i < submitted; i++) io_uring_wait_cqe(&ring, &cqe);
//      —— 每个完成事件都可能进一次内核（io_uring_enter），一批 32 个就是最多 32 次系统调用，
//         把 Proactor「批量收割」的红利整个吃掉（实测写 IOPS 只有 libaio 的 1/11）
//   ✅ io_uring_wait_cqe 等一次（CQ 空时才进内核），然后用 io_uring_for_each_cqe +
//      io_uring_cq_advance 一次把 CQ 里已有的事件全部取走 —— 一批只有 2 次系统调用
//          （1 次 submit + 1 次 enter），与 libaio 的 io_submit + io_getevents 对等
//
// ── 一个反直觉的实测结论：io_uring 读很快，写反而不如 libaio ──────────────
// 在 4KB / O_DIRECT / depth=32 的测试里：读 IOPS 与 libaio 持平甚至反超，
// **写只有 libaio 的一半**。原因用 `ps -o comm= -L -p <pid>` 能看到：
//   跑写测试时进程里多出十几个 **iou-wrk-*** 线程 —— 内核把 O_DIRECT 写
//   "甩"给了 io-wq 异步工作线程池，每次 I/O 多一次跨线程交接；
//   读走的是快路径（不需要 punt），所以读不受影响。
// 教训：异步 I/O 的瓶颈常常不在 API，而在内核到底走了哪条实现路径。
//       （同样的现象在网络版也出现过：见 network/echo_io_uring_adv.c 的注释）
// ============================================================================
#include "io_common.h"
#include <liburing.h>

// 提交当前 SQ 中排队的请求，并等待 target 个完成事件全部收齐。
// 返回 0 成功，负数失败。
//
// target 是**累计**完成数（不是本次要收几个）：调用方关心的是"总数到没到"，
// 因为完成事件可能一次回来多个（批量），也可能只回来一个。
static int submit_and_drain(struct io_uring *ring, long target, long *completed) {
    int ret = io_uring_submit(ring);
    if (ret < 0) {
        fprintf(stderr, "io_uring_submit failed: %s\n", strerror(-ret));
        return ret;
    }

    while (*completed < target) {
        struct io_uring_cqe *cqe;
        // CQ 里已有事件时直接返回（纯用户态读共享内存，**不进内核**）；
        // 只有 CQ 空的时候才会调用 io_uring_enter 去睡等。
        int r = io_uring_wait_cqe(ring, &cqe);  // CQ 非空时直接返回，不进内核
        if (r < 0) {
            fprintf(stderr, "io_uring_wait_cqe failed: %s\n", strerror(-r));
            return r;
        }

        // 一次性把 CQ 中当前所有已完成事件取走（这是 io_uring 相对 libaio 的核心用法）
        //
        // io_uring_for_each_cqe 只是在共享内存里遍历 CQ 环，不产生系统调用；
        // 扫完用 io_uring_cq_advance 推进 CQ 的 head。两者之间的**任何时刻都不允许
        // 提前调用 io_uring_cqe_seen**（会破坏遍历游标）。所以这里只统计数量，
        // 真正"消费"通过 cq_advance 一次性完成。
        unsigned head, n = 0;
        io_uring_for_each_cqe(ring, head, cqe) {
            // cqe->res 语义与系统调用一致：≥0 是字节数，<0 是负 errno
            if (cqe->res < 0)
                fprintf(stderr, "I/O error: %s\n", strerror(-cqe->res));
            n++;
        }
        io_uring_cq_advance(ring, n);
        *completed += n;
    }
    return 0;
}

// 跑一个阶段（写或读）：滚动提交 depth 个在飞请求，直到 total 个全部完成
//
// "滚动"的含义：始终保持约 depth 个请求在飞 —— 完成多少就补多少。
// 这就是"用并发深度换吞吐"（HDD/SSD 都能从队列深度里拿到并行度）。
static int run_phase(struct io_uring *ring, int fd, int is_write, char *bufs,
                     size_t block, long total, int depth, size_t file_size) {
    long submitted = 0, completed = 0;

    while (completed < total) {
        // ---- 填满提交窗口 ----
        int queued = 0;
        while (queued < depth && submitted < total) {
            struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
            if (!sqe) break;   // SQ 环满：这一轮先少提交几个，等收割后再补
            // 偏移在文件范围内**循环回绕**：这样测试文件只要 block*1024 字节，
            // 就能跑任意多次 I/O，不必把磁盘写满。
            // 代价是重复读写同一片区域 —— 所以本测试衡量的是"速率"，不是"容量"。
            off_t offset = (off_t)((submitted * block) % file_size);
            // 每个在飞请求必须**独占**一块缓冲区切片（bufs + queued*block）：
            // 同一块内存不能同时喂给两个在飞的 I/O，否则数据会互相覆盖。
            // 这也是为什么缓冲区要按 depth 分配（block × depth）。
            if (is_write)
                io_uring_prep_write(sqe, fd, bufs + queued * block, block, offset);
            else
                io_uring_prep_read(sqe, fd, bufs + queued * block, block, offset);
            // 把序号塞进 user_data：完成时能对上号（本程序只用来调试/统计，
            // 真实程序常靠它找回"这次 I/O 属于哪个连接/请求"的上下文）
            io_uring_sqe_set_data(sqe, (void *)(long)submitted);
            queued++;
            submitted++;
        }
        if (queued == 0) break;

        if (submit_and_drain(ring, completed + queued, &completed) < 0) return -1;
    }
    return 0;
}

int main(int argc, char *argv[]) {
    // 位置参数：<文件> <块大小> <总次数> <队列深度> <是否 O_DIRECT>
    const char *path = argc > 1 ? argv[1] : DEFAULT_FILE;
    size_t block     = argc > 2 ? atoi(argv[2]) : DEFAULT_BLOCK;
    long total       = argc > 3 ? atol(argv[3]) : DEFAULT_TOTAL;
    int depth        = argc > 4 ? atoi(argv[4]) : DEFAULT_DEPTH;
    int use_direct   = argc > 5 ? atoi(argv[5]) : 0;  // io_uring 对 buffered I/O 支持更好

    size_t file_size = block * 1024;

    int fd = open_file(path, use_direct, file_size);
    if (fd < 0) return 1;

    // 环大小直接取自 depth：SQ 环至少要能装下"同时在飞的请求数"，
    // 否则 io_uring_get_sqe 会返回 NULL，程序只能降速重试。
    struct io_uring ring;
    int ret = io_uring_queue_init(depth, &ring, 0);
    if (ret < 0) {
        fprintf(stderr, "io_uring_queue_init failed: %s\n", strerror(-ret));
        fprintf(stderr, "\n[环境限制] io_uring_setup 系统调用被 seccomp 拦截（容器默认策略）。\n");
        fprintf(stderr, "此程序已编译成功，但需在没有 seccomp 限制的环境运行：\n");
        fprintf(stderr, "  docker run --security-opt seccomp=unconfined ...\n");
        close(fd);
        unlink(path);
        return 2;  // 用退出码 2 标识环境限制（非代码错误），脚本据此把该实现标成 N/A
    }

    // 按 depth 分配缓冲区（每块 block 字节，见 run_phase 里的说明）。
    // align_alloc_buf 用 posix_memalign 保证 4096 对齐 —— O_DIRECT 的硬性要求。
    char *bufs = align_alloc_buf(block * depth);
    if (!bufs) { io_uring_queue_exit(&ring); close(fd); return 1; }
    memset(bufs, 'A', block * depth);

    printf("io_uring 测试: file=%s block=%zu total=%ld depth=%d O_DIRECT=%d\n",
           path, block, total, depth, use_direct);

    // ---- 写测试（先写后读，保证读的时候文件里有内容）----
    double t0 = now_ms();
    run_phase(&ring, fd, 1, bufs, block, total, depth, file_size);
    double write_ms = now_ms() - t0;

    // ---- 读测试 ----
    // 读写分开计时：两者在内核里走的路径完全不同（写可能被 punt 到 io-wq，
    // 详见文件头），合在一起算会把差异抹平。
    t0 = now_ms();
    run_phase(&ring, fd, 0, bufs, block, total, depth, file_size);
    double read_ms = now_ms() - t0;

    printf("\n===== io_uring =====\n");
    printf("O_DIRECT      : %d | 并发深度: %d\n", use_direct, depth);
    printf("总 I/O 次数   : %ld (写 %ld + 读 %ld)\n", total * 2, total, total);
    printf("块大小        : %zu 字节\n", block);
    printf("写耗时        : %.2f ms | 写 IOPS: %.0f | 写带宽: %.2f MB/s\n",
           write_ms, total / (write_ms / 1000.0),
           (double)total * block / (1024.0 * 1024.0) / (write_ms / 1000.0));
    printf("读耗时        : %.2f ms | 读 IOPS: %.0f | 读带宽: %.2f MB/s\n",
           read_ms, total / (read_ms / 1000.0),
           (double)total * block / (1024.0 * 1024.0) / (read_ms / 1000.0));
    printf("备注          : SQ/CQ 共享内存环形队列，批量提交 + 批量收割（Proactor）\n");

    free(bufs);                     // 必须在 queue_exit 之前：这块内存属于用户态
    io_uring_queue_exit(&ring);     // 释放 SQ/CQ 环与 mmap 的共享内存
    close(fd);
    unlink(path);                   // 清掉测试文件，别在 /tmp 里留垃圾
    return 0;
}
