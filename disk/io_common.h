// 磁盘 I/O 基准测试公共头文件（sync / libaio / io_uring 三版共用）
#ifndef __IO_COMMON_H__
#define __IO_COMMON_H__

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>

// 默认参数
#define DEFAULT_FILE "/tmp/iodemo_testfile"
#define DEFAULT_BLOCK 4096       // 每次 I/O 4KB
#define DEFAULT_DEPTH 32         // 并发深度（同时在飞的请求数）
#define DEFAULT_TOTAL (1024 * 256) // 总 I/O 次数（256K 次 = 1GB @4KB）

static inline double now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

// 按 O_DIRECT 要求对齐分配内存
static inline void *align_alloc_buf(size_t size) {
    void *ptr = NULL;
    size_t align = 4096;
    if (posix_memalign(&ptr, align, size) != 0) {
        perror("posix_memalign");
        return NULL;
    }
    memset(ptr, 0, size);
    return ptr;
}

// 创建/打开测试文件
// use_direct: 是否使用 O_DIRECT（真正的异步磁盘 I/O 通常需要）
static inline int open_file(const char *path, int use_direct, size_t file_size) {
    int flags = O_RDWR | O_CREAT | O_TRUNC;
    if (use_direct) flags |= O_DIRECT;
    int fd = open(path, flags, 0644);
    if (fd < 0) {
        fprintf(stderr, "open %s failed: %s (O_DIRECT=%d)\n",
                path, strerror(errno), use_direct);
        return -1;
    }
    // 预分配文件空间
    if (ftruncate(fd, file_size) < 0) {
        perror("ftruncate");
        close(fd);
        return -1;
    }
    return fd;
}

static inline void print_result(const char *name, double ms, long total_ios,
                                 size_t block, const char *note) {
    double sec = ms / 1000.0;
    double iops = total_ios / sec;
    double bw_mb = (double)total_ios * block / (1024.0 * 1024.0) / sec;
    printf("\n===== %s =====\n", name);
    printf("总 I/O 次数   : %ld\n", total_ios);
    printf("块大小        : %zu 字节\n", block);
    printf("总耗时        : %.2f ms\n", ms);
    printf("IOPS          : %.0f\n", iops);
    printf("带宽          : %.2f MB/s\n", bw_mb);
    if (note) printf("备注          : %s\n", note);
}

#endif
