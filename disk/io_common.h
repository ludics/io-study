// ============================================================================
// 磁盘 I/O 基准测试公共头文件（io_sync / io_libaio / io_uring_disk 三版共用）
//
// 为什么要有这么个头文件？
//   三版测试的**输出格式必须完全一致**，否则脚本没法把它们拼成一张对比表。
//   把「开文件、对齐分配、计时、打印结果」这些与 I/O 模型无关的部分抽出来，
//   三份代码里剩下的就只有各自模型的差异 —— 这正是本实验想让人看清的东西。
// ============================================================================
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
#define DEFAULT_BLOCK 4096       // 每次 I/O 4KB（也是绝大多数文件系统的页大小）
#define DEFAULT_DEPTH 32         // 并发深度（同时在飞的请求数）
#define DEFAULT_TOTAL (1024 * 256) // 总 I/O 次数（256K 次 = 1GB @4KB）

// 计时：用 CLOCK_MONOTONIC 而不是 CLOCK_REALTIME ——
// 后者会被系统时间调整（NTP 校时）影响，测出来的耗时可能为负或跳变
static inline double now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

// 分配一块「按 4096 对齐」的内存。
// 为什么必须对齐：O_DIRECT 绕过 page cache，直接把用户缓冲区交给块设备，
// 内核要求 buffer 地址、文件偏移、长度都按扇区大小（通常 512B，部分设备 4KB）对齐，
// 否则 write/read 会直接返回 EINVAL。malloc 只保证 8/16 字节对齐，不够用。
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
// use_direct: 是否使用 O_DIRECT（测真实磁盘能力的前提；buffered 模式测的其实是 page cache）
// file_size : 预分配大小。用 ftruncate 而不是写零填充 —— 前者只是改 inode 元数据，瞬间完成
static inline int open_file(const char *path, int use_direct, size_t file_size) {
    int flags = O_RDWR | O_CREAT | O_TRUNC;
    if (use_direct) flags |= O_DIRECT;
    int fd = open(path, flags, 0644);
    if (fd < 0) {
        // 常见失败原因：所在文件系统不支持 O_DIRECT（tmpfs/ramfs）、或路径不存在
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

// 统一的输出格式。三个程序都调用它，保证脚本能一视同仁地解析
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

