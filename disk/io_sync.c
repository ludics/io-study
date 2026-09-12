// ============================================================================
// 同步磁盘 I/O —— 三方对比的 baseline（基准线）
//
// 一句话：每次 pread/pwrite 都会**阻塞到数据搬完**才返回，所以整个进程
//        任何时刻只有 1 个 I/O 在飞 —— 这就是「同步」的含义。
//
// 为什么需要这个 baseline？
//   只有先知道「串行做有多慢」，才能量化「异步 + 并发深度」到底赚了多少。
//   实测在 4KB / O_DIRECT 下同步约 8k IOPS，libaio 开 depth=32 能到 200k+。
//
// 为什么这么写（几个容易踩的点）：
//   1) 缓冲区必须自己对齐 —— O_DIRECT 要求 buffer/offset/length 都按块设备扇区
//      （通常 512B，部分 4KB）对齐，普通 malloc 不保证，所以用 align_alloc_buf()。
//   2) 偏移用 (i * block) % file_size 循环回绕 —— 这是故意的：让文件保持很小
//      （block*1024），避免把磁盘写满，同时保证每次 I/O 都落在真实位置。
//      page cache 命中率会随文件变小而升高，所以看相对趋势就好。
//   3) 写和读分开计时 —— 两者在介质上的行为完全不同（写有分配/日志开销，
//      读可能被 cache 命中），混在一起会互相掩盖。
//
// 参数：io_sync <文件> [块大小] [总次数] [O_DIRECT]
// ============================================================================
#include "io_common.h"

int main(int argc, char *argv[]) {
    const char *path   = argc > 1 ? argv[1] : DEFAULT_FILE;
    size_t block       = argc > 2 ? atoi(argv[2]) : DEFAULT_BLOCK;
    long total         = argc > 3 ? atol(argv[3]) : DEFAULT_TOTAL;
    int use_direct     = argc > 4 ? atoi(argv[4]) : 0;  // 默认不用 O_DIRECT（走 page cache）

    // 文件大小故意与块大小成固定比例（约 1024 块），保证「循环覆盖」不会写爆磁盘。
    // 注意：文件小 → page cache 容易整体命中 → buffered 模式下数字会虚高，属正常现象。
    size_t file_size = block * 1024;

    int fd = open_file(path, use_direct, file_size);
    if (fd < 0) return 1;

    // 只分配 1 块缓冲区：同步执行下永远只有一个 I/O 在飞，多分配没有意义
    char *buf = align_alloc_buf(block);
    if (!buf) { close(fd); return 1; }
    memset(buf, 'A', block);

    printf("同步 I/O 测试: file=%s block=%zu total=%ld O_DIRECT=%d\n",
           path, block, total, use_direct);

    // ---- 写测试 ----
    // 每一次 pwrite 都要等这次写完才进入下一次循环，这就是同步的本质。
    // 想加速只能靠并发（多个 I/O 同时在飞），而这正是后面 libaio / io_uring 要解决的问题。
    double t0 = now_ms();
    for (long i = 0; i < total; i++) {
        off_t offset = (off_t)((i * block) % file_size);  // 循环回绕，见文件头注释 (2)
        ssize_t ret = pwrite(fd, buf, block, offset);
        if (ret != (ssize_t)block) {
            // 短写（ret < block）在普通文件上极少见；出错则打印并退出本次测量，
            // 但不直接 return —— 让读测试也能跑，便于对比
            if (ret < 0) {
                fprintf(stderr, "pwrite failed: %s\n", strerror(errno));
                break;
            }
        }
    }
    double write_ms = now_ms() - t0;

    // ---- 读测试 ----
    // 与写完全对称。buffered 模式下这里读的很可能就是刚才写的、还在 page cache 里的数据，
    // 所以会快得离谱（百万级 IOPS）—— 那不是磁盘能力，是内存拷贝能力。
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

    // 输出格式与另外两版保持完全一致，方便脚本横向解析、拼成对比表
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
    unlink(path);  // 收尾删掉测试文件，避免残留大文件
    return 0;
}
