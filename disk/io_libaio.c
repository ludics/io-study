// Linux AIO (libaio) 磁盘读写：真异步 I/O（Proactor 早期形态）
// 特点：一次 io_submit 提交批量请求，内核异步完成后用 io_getevents 收割
// 注意：Linux AIO 对 buffered I/O 支持有限（可能退化同步），通常需要 O_DIRECT
#include "io_common.h"
#include <libaio.h>
#include <sys/eventfd.h>

#define MAX_DEPTH 1024

int main(int argc, char *argv[]) {
    const char *path = argc > 1 ? argv[1] : DEFAULT_FILE;
    size_t block     = argc > 2 ? atoi(argv[2]) : DEFAULT_BLOCK;
    long total       = argc > 3 ? atol(argv[3]) : DEFAULT_TOTAL;
    int depth        = argc > 4 ? atoi(argv[4]) : DEFAULT_DEPTH;
    int use_direct   = argc > 5 ? atoi(argv[5]) : 1;  // AIO 默认用 O_DIRECT

    if (depth > MAX_DEPTH) depth = MAX_DEPTH;
    size_t file_size = block * 1024;

    // O_DIRECT 下必须对齐
    if (use_direct) {
        if (block % 512 != 0) { fprintf(stderr, "block 需 512 对齐\n"); return 1; }
    }

    int fd = open_file(path, use_direct, file_size);
    if (fd < 0) return 1;

    io_context_t ctx = 0;
    if (io_setup(depth, &ctx) < 0) {
        fprintf(stderr, "io_setup failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    // 分配 io 控制块与缓冲区
    struct iocb *iocbs = calloc(depth, sizeof(struct iocb));
    struct iocb **iocb_ptrs = calloc(depth, sizeof(struct iocb *));
    char *bufs = align_alloc_buf(block * depth);
    if (!iocbs || !iocb_ptrs || !bufs) {
        fprintf(stderr, "alloc failed\n");
        io_destroy(ctx); close(fd); return 1;
    }
    if (use_direct) {
        // O_DIRECT 要求缓冲区也对齐，align_alloc_buf 已保证首地址对齐
        memset(bufs, 'A', block * depth);
    }

    printf("libaio 测试: file=%s block=%zu total=%ld depth=%d O_DIRECT=%d\n",
           path, block, total, depth, use_direct);

    long submitted = 0, completed = 0;
    double t0 = now_ms();

    // ---- 写测试（批量提交 + 收割）----
    while (completed < total) {
        int to_submit = 0;
        // 填满提交窗口
        while (to_submit < depth && submitted < total) {
            struct iocb *cb = &iocbs[to_submit];
            off_t offset = (off_t)((submitted * block) % file_size);
            io_prep_pwrite(cb, fd, bufs + to_submit * block, block, offset);
            cb->data = (void *)(long)submitted;
            iocb_ptrs[to_submit] = cb;
            to_submit++;
            submitted++;
        }
        if (to_submit == 0) break;

        int ret = io_submit(ctx, to_submit, iocb_ptrs);
        if (ret < 0) {
            fprintf(stderr, "io_submit failed: %s\n", strerror(errno));
            // 若 buffered I/O 不支持 AIO，可能返回 EINVAL
            if (errno == EINVAL && !use_direct) {
                fprintf(stderr, "提示: buffered I/O 可能不支持 AIO，尝试加 O_DIRECT(参数5=1)\n");
            }
            break;
        }

        // 收割完成的事件
        struct io_event events[MAX_DEPTH];
        int n = io_getevents(ctx, ret, ret, events, NULL);
        if (n < 0) {
            fprintf(stderr, "io_getevents failed: %s\n", strerror(errno));
            break;
        }
        completed += n;
    }
    double write_ms = now_ms() - t0;

    // ---- 读测试 ----
    submitted = 0; completed = 0;
    t0 = now_ms();
    while (completed < total) {
        int to_submit = 0;
        while (to_submit < depth && submitted < total) {
            struct iocb *cb = &iocbs[to_submit];
            off_t offset = (off_t)((submitted * block) % file_size);
            io_prep_pread(cb, fd, bufs + to_submit * block, block, offset);
            cb->data = (void *)(long)submitted;
            iocb_ptrs[to_submit] = cb;
            to_submit++;
            submitted++;
        }
        if (to_submit == 0) break;

        int ret = io_submit(ctx, to_submit, iocb_ptrs);
        if (ret < 0) { fprintf(stderr, "io_submit(read) failed: %s\n", strerror(errno)); break; }

        struct io_event events[MAX_DEPTH];
        int n = io_getevents(ctx, ret, ret, events, NULL);
        if (n < 0) { fprintf(stderr, "io_getevents(read) failed: %s\n", strerror(errno)); break; }
        completed += n;
    }
    double read_ms = now_ms() - t0;

    printf("\n===== libaio (Linux AIO) =====\n");
    printf("O_DIRECT      : %d | 并发深度: %d\n", use_direct, depth);
    printf("总 I/O 次数   : %ld (写 %ld + 读 %ld)\n", total * 2, total, total);
    printf("块大小        : %zu 字节\n", block);
    printf("写耗时        : %.2f ms | 写 IOPS: %.0f | 写带宽: %.2f MB/s\n",
           write_ms, total / (write_ms / 1000.0),
           (double)total * block / (1024.0 * 1024.0) / (write_ms / 1000.0));
    printf("读耗时        : %.2f ms | 读 IOPS: %.0f | 读带宽: %.2f MB/s\n",
           read_ms, total / (read_ms / 1000.0),
           (double)total * block / (1024.0 * 1024.0) / (read_ms / 1000.0));
    printf("备注          : 批量提交 + 内核异步完成 + 收割（Proactor）\n");

    free(iocbs); free(iocb_ptrs); free(bufs);
    io_destroy(ctx);
    close(fd);
    unlink(path);
    return 0;
}
