// 同步磁盘 I/O（baseline）：用 pwrite/pread 顺序写+读
// 特点：每次 I/O 阻塞等待完成，无并发，作为性能对比的基准线
#include "io_common.h"

int main(int argc, char *argv[]) {
    const char *path   = argc > 1 ? argv[1] : DEFAULT_FILE;
    size_t block       = argc > 2 ? atoi(argv[2]) : DEFAULT_BLOCK;
    long total         = argc > 3 ? atol(argv[3]) : DEFAULT_TOTAL;
    int use_direct     = argc > 4 ? atoi(argv[4]) : 0;  // 默认不用 O_DIRECT

    size_t file_size = block * 1024;  // 文件大小固定为 4MB，循环覆盖写

    int fd = open_file(path, use_direct, file_size);
    if (fd < 0) return 1;

    char *buf = align_alloc_buf(block);
    if (!buf) { close(fd); return 1; }
    memset(buf, 'A', block);

    printf("同步 I/O 测试: file=%s block=%zu total=%ld O_DIRECT=%d\n",
           path, block, total, use_direct);

    // ---- 写测试 ----
    double t0 = now_ms();
    for (long i = 0; i < total; i++) {
        off_t offset = (off_t)((i * block) % file_size);
        ssize_t ret = pwrite(fd, buf, block, offset);
        if (ret != (ssize_t)block) {
            if (ret < 0) {
                fprintf(stderr, "pwrite failed: %s\n", strerror(errno));
                break;
            }
        }
    }
    double write_ms = now_ms() - t0;

    // ---- 读测试 ----
    t0 = now_ms();
    for (long i = 0; i < total; i++) {
        off_t offset = (off_t)((i * block) % file_size);
        ssize_t ret = pread(fd, buf, block, offset);
        if (ret != (ssize_t)block) {
            if (ret < 0) {
                fprintf(stderr, "pread failed: %s\n", strerror(errno));
                break;
            }
        }
    }
    double read_ms = now_ms() - t0;

    printf("\n===== 同步 I/O (baseline) =====\n");
    printf("O_DIRECT      : %d\n", use_direct);
    printf("总 I/O 次数   : %ld (写 %ld + 读 %ld)\n", total * 2, total, total);
    printf("块大小        : %zu 字节\n", block);
    printf("写耗时        : %.2f ms | 写 IOPS: %.0f | 写带宽: %.2f MB/s\n",
           write_ms, total / (write_ms / 1000.0),
           (double)total * block / (1024.0 * 1024.0) / (write_ms / 1000.0));
    printf("读耗时        : %.2f ms | 读 IOPS: %.0f | 读带宽: %.2f MB/s\n",
           read_ms, total / (read_ms / 1000.0),
           (double)total * block / (1024.0 * 1024.0) / (read_ms / 1000.0));
    printf("备注          : 每次 I/O 阻塞等待完成，无并发\n");

    free(buf);
    close(fd);
    unlink(path);
    return 0;
}
