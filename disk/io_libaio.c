// ============================================================================
// Linux AIO (libaio) 磁盘读写 —— 真异步 I/O（Proactor 的早期形态）
//
// 一句话：把 N 个 I/O 请求一次性交给内核（io_submit），内核自己排队、并行执行，
//        完成后统一回收（io_getevents）。提交线程不阻塞在某一次 I/O 上。
//
// 三个角色，理解这三个就算懂了异步 I/O：
//   io_context_t    「异步 I/O 上下文」——一个内核侧的队列，所有请求都挂在这里
//   struct iocb     一个 I/O 控制块，描述「对哪个 fd、在什么偏移、读写多少字节」
//   io_event        内核完成一个请求后回填的结果（类似 io_uring 的 CQE）
//
// 为什么快？（核心就一句：并发深度）
//   同步版每次只让 1 个 I/O 在飞，磁盘队列再深也用不上；
//   这里一次塞 depth 个进去，磁盘才能并行处理。实测 depth=32 时写 IOPS 是同步的 ~27 倍。
//   注意：这个收益来自「深度」，不来自 API 本身 —— 把 depth 设成 1，性能立刻跌回同步水平，
//   这一点在 `scripts/bench_matrix.py disk` 的「维度 A」里看得很清楚。
//
// 两个必须知道的限制：
//   1) 传统 Linux AIO 只对 **O_DIRECT** 真正异步。buffered I/O 要么退化同步执行，
//      要么直接返回 EINVAL（代码里对此专门做了提示）。
//   2) 缓冲区 / 偏移 / 长度都要 512B 对齐，且同一块缓冲区不能同时用于两个在飞的 I/O。
//
// 参数：io_libaio <文件> [块大小] [总次数] [深度] [O_DIRECT]
// ============================================================================
#include "io_common.h"
#include <libaio.h>
#include <sys/eventfd.h>

#define MAX_DEPTH 1024  // events[] 是栈上数组，限制上限避免爆栈

int main(int argc, char *argv[]) {
    const char *path = argc > 1 ? argv[1] : DEFAULT_FILE;
    size_t block     = argc > 2 ? atoi(argv[2]) : DEFAULT_BLOCK;
    long total       = argc > 3 ? atol(argv[3]) : DEFAULT_TOTAL;
    int depth        = argc > 4 ? atoi(argv[4]) : DEFAULT_DEPTH;
    int use_direct   = argc > 5 ? atoi(argv[5]) : 1;  // AIO 默认用 O_DIRECT（见文件头限制 1）

    if (depth > MAX_DEPTH) depth = MAX_DEPTH;
    size_t file_size = block * 1024;  // 与 io_sync.c 一致：小文件循环覆盖

    // O_DIRECT 下必须对齐
    if (use_direct) {
        if (block % 512 != 0) { fprintf(stderr, "block 需 512 对齐\n"); return 1; }
    }

    int fd = open_file(path, use_direct, file_size);
    if (fd < 0) return 1;

    // 建立异步上下文。参数 depth 是「同时在飞的请求数上限」，内核据此分配队列资源。
    io_context_t ctx = 0;
    if (io_setup(depth, &ctx) < 0) {
        fprintf(stderr, "io_setup failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    // 预分配 depth 个 iocb + depth 个缓冲区。
    // 数量与 depth 一致是有原因的：每个在飞的 I/O 必须独占自己的缓冲区和 iocb，
    // 复用一个就会发生「还在后台写，前台已经改掉了数据」的撕裂。
    struct iocb *iocbs = calloc(depth, sizeof(struct iocb));
    struct iocb **iocb_ptrs = calloc(depth, sizeof(struct iocb *));
    char *bufs = align_alloc_buf(block * depth);
    if (!iocbs || !iocb_ptrs || !bufs) {
        fprintf(stderr, "alloc failed\n");
        io_destroy(ctx); close(fd); return 1;
    }
    if (use_direct) {
        // O_DIRECT 要求缓冲区也对齐，align_alloc_buf 已保证首地址对齐；
        // 各段之间按 block 对齐，所以整块连续分配即可
        memset(bufs, 'A', block * depth);
    }

    printf("libaio 测试: file=%s block=%zu total=%ld depth=%d O_DIRECT=%d\n",
           path, block, total, depth, use_direct);

    long submitted = 0, completed = 0;
    double t0 = now_ms();

    // ---- 写测试（批量提交 + 批量收割）----
    // 循环结构：填满提交窗口 → 一次 io_submit → 等这一批全部完成 → 再填下一批。
    // 这是「稳态并发度 = depth」的简单实现。
    while (completed < total) {
        int to_submit = 0;
        // 填满提交窗口：准备 depth 个 iocb
        while (to_submit < depth && submitted < total) {
            struct iocb *cb = &iocbs[to_submit];
            off_t offset = (off_t)((submitted * block) % file_size);
            io_prep_pwrite(cb, fd, bufs + to_submit * block, block, offset);
            cb->data = (void *)(long)submitted;  // 自定义标记，收割时可用于区分是哪个请求
            iocb_ptrs[to_submit] = cb;
            to_submit++;
            submitted++;
        }
        if (to_submit == 0) break;

        // 一次系统调用提交整批 —— 这是相对同步版最关键的差别
        int ret = io_submit(ctx, to_submit, iocb_ptrs);
        if (ret < 0) {
            fprintf(stderr, "io_submit failed: %s\n", strerror(errno));
            // 若 buffered I/O 不支持 AIO，可能返回 EINVAL
            if (errno == EINVAL && !use_direct) {
                fprintf(stderr, "提示: buffered I/O 可能不支持 AIO，尝试加 O_DIRECT(参数5=1)\n");
            }
            break;
        }

        // 收割完成的事件。
        // min_nr = max_nr = ret：意思是「这一批我要等齐」，一次调用收完整批，
        // 不在这里空转轮询（对比 io_uring 版如果用逐事件 wait_cqe，就会退化成 N 次系统调用）。
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
    // 与写完全对称。注意 submitted/completed 要清零，否则循环条件会立刻不成立。
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

    // 输出格式与 io_sync.c / io_uring_disk.c 保持一致，便于拼表对比
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

