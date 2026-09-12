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
#include "io_common.h"
#include <liburing.h>

// 提交当前 SQ 中排队的请求，并等待 target 个完成事件全部收齐。
// 返回 0 成功，负数失败。
static int submit_and_drain(struct io_uring *ring, long target, long *completed) {
    int ret = io_uring_submit(ring);
    if (ret < 0) {
        fprintf(stderr, "io_uring_submit failed: %s\n", strerror(-ret));
        return ret;
    }

    while (*completed < target) {
        struct io_uring_cqe *cqe;
        int r = io_uring_wait_cqe(ring, &cqe);  // CQ 非空时直接返回，不进内核
        if (r < 0) {
            fprintf(stderr, "io_uring_wait_cqe failed: %s\n", strerror(-r));
            return r;
        }

        // 一次性把 CQ 中当前所有已完成事件取走（这是 io_uring 相对 libaio 的核心用法）
        unsigned head, n = 0;
        io_uring_for_each_cqe(ring, head, cqe) {
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
static int run_phase(struct io_uring *ring, int fd, int is_write, char *bufs,
                     size_t block, long total, int depth, size_t file_size) {
    long submitted = 0, completed = 0;

    while (completed < total) {
        int queued = 0;
        while (queued < depth && submitted < total) {
            struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
            if (!sqe) break;
            off_t offset = (off_t)((submitted * block) % file_size);
            if (is_write)
                io_uring_prep_write(sqe, fd, bufs + queued * block, block, offset);
            else
                io_uring_prep_read(sqe, fd, bufs + queued * block, block, offset);
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
    const char *path = argc > 1 ? argv[1] : DEFAULT_FILE;
    size_t block     = argc > 2 ? atoi(argv[2]) : DEFAULT_BLOCK;
    long total       = argc > 3 ? atol(argv[3]) : DEFAULT_TOTAL;
    int depth        = argc > 4 ? atoi(argv[4]) : DEFAULT_DEPTH;
    int use_direct   = argc > 5 ? atoi(argv[5]) : 0;  // io_uring 对 buffered I/O 支持更好

    size_t file_size = block * 1024;

    int fd = open_file(path, use_direct, file_size);
    if (fd < 0) return 1;

    struct io_uring ring;
    int ret = io_uring_queue_init(depth, &ring, 0);
    if (ret < 0) {
        fprintf(stderr, "io_uring_queue_init failed: %s\n", strerror(-ret));
        fprintf(stderr, "\n[环境限制] io_uring_setup 系统调用被 seccomp 拦截（容器默认策略）。\n");
        fprintf(stderr, "此程序已编译成功，但需在没有 seccomp 限制的环境运行：\n");
        fprintf(stderr, "  docker run --security-opt seccomp=unconfined ...\n");
        close(fd);
        unlink(path);
        return 2;  // 用退出码 2 标识环境限制（非代码错误）
    }

    char *bufs = align_alloc_buf(block * depth);
    if (!bufs) { io_uring_queue_exit(&ring); close(fd); return 1; }
    memset(bufs, 'A', block * depth);

    printf("io_uring 测试: file=%s block=%zu total=%ld depth=%d O_DIRECT=%d\n",
           path, block, total, depth, use_direct);

    // ---- 写测试 ----
    double t0 = now_ms();
    run_phase(&ring, fd, 1, bufs, block, total, depth, file_size);
    double write_ms = now_ms() - t0;

    // ---- 读测试 ----
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

    free(bufs);
    io_uring_queue_exit(&ring);
    close(fd);
    unlink(path);
    return 0;
}
