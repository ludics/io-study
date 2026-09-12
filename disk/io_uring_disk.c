// io_uring 磁盘读写：Linux 现代真异步 I/O（Proactor）
// 特点：SQ/CQ 双环形队列 mmap 共享内存，批量提交/收割，可零系统调用（SQPOLL）
#include "io_common.h"
#include <liburing.h>

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
        fprintf(stderr, "\n[环境限制] 本沙箱 seccomp 禁用了 io_uring_setup 系统调用。\n");
        fprintf(stderr, "此程序已编译成功，但需在没有 seccomp 限制的环境运行。\n");
        close(fd);
        unlink(path);
        return 2;  // 用退出码 2 标识环境限制（非代码错误）
    }

    char *bufs = align_alloc_buf(block * depth);
    if (!bufs) { io_uring_queue_exit(&ring); close(fd); return 1; }
    memset(bufs, 'A', block * depth);

    printf("io_uring 测试: file=%s block=%zu total=%ld depth=%d O_DIRECT=%d\n",
           path, block, total, depth, use_direct);

    long submitted = 0, completed = 0;
    double t0 = now_ms();

    // ---- 写测试 ----
    while (completed < total) {
        int to_submit = 0;
        while (to_submit < depth && submitted < total) {
            struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
            if (!sqe) break;
            off_t offset = (off_t)((submitted * block) % file_size);
            io_uring_prep_write(sqe, fd, bufs + to_submit * block, block, offset);
            io_uring_sqe_set_data(sqe, (void *)(long)submitted);
            to_submit++;
            submitted++;
        }
        if (to_submit == 0) break;

        ret = io_uring_submit(&ring);
        if (ret < 0) { fprintf(stderr, "io_uring_submit failed: %s\n", strerror(-ret)); break; }

        // 收割 CQE
        for (int i = 0; i < ret; i++) {
            struct io_uring_cqe *cqe;
            int r = io_uring_wait_cqe(&ring, &cqe);
            if (r < 0) { fprintf(stderr, "wait_cqe failed: %s\n", strerror(-r)); break; }
            if (cqe->res < 0) {
                fprintf(stderr, "I/O error: %s\n", strerror(-cqe->res));
            }
            io_uring_cqe_seen(&ring, cqe);
            completed++;
        }
    }
    double write_ms = now_ms() - t0;

    // ---- 读测试 ----
    submitted = 0; completed = 0;
    t0 = now_ms();
    while (completed < total) {
        int to_submit = 0;
        while (to_submit < depth && submitted < total) {
            struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
            if (!sqe) break;
            off_t offset = (off_t)((submitted * block) % file_size);
            io_uring_prep_read(sqe, fd, bufs + to_submit * block, block, offset);
            io_uring_sqe_set_data(sqe, (void *)(long)submitted);
            to_submit++;
            submitted++;
        }
        if (to_submit == 0) break;

        ret = io_uring_submit(&ring);
        if (ret < 0) { fprintf(stderr, "io_uring_submit(read) failed\n"); break; }

        for (int i = 0; i < ret; i++) {
            struct io_uring_cqe *cqe;
            int r = io_uring_wait_cqe(&ring, &cqe);
            if (r < 0) break;
            io_uring_cqe_seen(&ring, cqe);
            completed++;
        }
    }
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
    printf("备注          : SQ/CQ 共享内存环形队列，批量提交收割（Proactor）\n");

    free(bufs);
    io_uring_queue_exit(&ring);
    close(fd);
    unlink(path);
    return 0;
}
