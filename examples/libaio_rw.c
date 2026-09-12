// libaio 教学示例：异步磁盘读写（iocb / io_submit / io_getevents 三件套）
// 编译: gcc -O2 -Wall -o ex_aio ex_aio.c -laio
// 运行: ./ex_aio [文件] [块大小] [总次数] [深度] [是否O_DIRECT]
//
// 看清三件事：
//   1) iocb 是「请求描述符」，io_submit 把它交给内核，io_getevents 收结果
//   2) 每个在飞的请求必须独占一块缓冲区，否则数据互相覆盖
//   3) 只有 O_DIRECT 才是「真异步」；buffered I/O 在 Linux AIO 上会退化成同步
/* 单独编译需要 _GNU_SOURCE；经 Makefile 编译时用 -D 定义，加保护避免重定义告警 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <libaio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAX_DEPTH 128

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "/tmp/ex_aio.dat";
    size_t block     = argc > 2 ? (size_t)atoi(argv[2]) : 4096;
    long   total     = argc > 3 ? atol(argv[3]) : 4096;
    int    depth     = argc > 4 ? atoi(argv[4]) : 16;
    int    direct    = argc > 5 ? atoi(argv[5]) : 1;   // 默认 O_DIRECT

    if (depth < 1 || depth > MAX_DEPTH) depth = MAX_DEPTH;
    if (direct && block % 512) { fprintf(stderr, "O_DIRECT 要求块大小 512 对齐\n"); return 1; }

    size_t file_sz = block * 1024;        // 文件只开 block×1024，靠偏移回绕跑多次

    int flags = O_RDWR | O_CREAT | O_TRUNC | (direct ? O_DIRECT : 0);
    int fd = open(path, flags, 0644);
    if (fd < 0) { perror("open"); return 1; }
    if (ftruncate(fd, file_sz) < 0) { perror("ftruncate"); return 1; }

    // ---- 建异步 I/O 上下文，容量 = 队列深度 ----
    io_context_t ctx = 0;
    if (io_setup(depth, &ctx) < 0) { perror("io_setup"); return 1; }

    struct iocb  *cbs = calloc(depth, sizeof(struct iocb));
    struct iocb **cbs_p = calloc(depth, sizeof(struct iocb *));
    void *bufs = NULL;
    // O_DIRECT 要求缓冲区地址按块对齐（这一步用 posix_memalign 保证）
    if (posix_memalign(&bufs, 4096, block * depth) != 0) { perror("posix_memalign"); return 1; }
    memset(bufs, 'A', block * depth);

    printf("libaio: file=%s block=%zu total=%ld depth=%d O_DIRECT=%d\n",
           path, block, total, depth, direct);
    printf("  说明：同步方式下每个请求要等上一次完成，深度恒为 1；\n");
    printf("        这里一次把 depth 个请求丢给内核，测的是「并发深度换吞吐」。\n");

    // ---- 写阶段 ----
    long submitted = 0, done = 0;
    double t0 = now_ms();
    while (done < total) {
        int n = 0;
        while (n < depth && submitted < total) {
            struct iocb *cb = &cbs[n];
            off_t off = (off_t)((submitted * block) % file_sz);   // 偏移回绕
            // io_prep_pwrite 内部会填 aio_fildes/aio_buf/aio_nbytes/aio_offset
            io_prep_pwrite(cb, fd, (char *)bufs + (size_t)n * block, block, off);
            cb->data = (void *)(long)submitted;    // 私有字段：完成时能对上号
            cbs_p[n] = cb;
            n++; submitted++;
        }
        if (n == 0) break;

        int r = io_submit(ctx, n, cbs_p);
        if (r < 0) {
            fprintf(stderr, "io_submit: %s%s\n", strerror(-r),
                    (r == -EINVAL && !direct) ? "（buffered I/O 可能不支持 AIO，试试 O_DIRECT=1）" : "");
            break;
        }
        // 【关键】一次 io_getevents 收走整批完成事件（这是 libaio 的批量收割）
        struct io_event evs[MAX_DEPTH];
        int got = io_getevents(ctx, r, r, evs, NULL);
        if (got < 0) { fprintf(stderr, "io_getevents: %s\n", strerror(-got)); break; }
        for (int i = 0; i < got; i++)
            if ((long)evs[i].res < 0) fprintf(stderr, "I/O 错误: %s\n", strerror(-(int)evs[i].res));
        done += got;
    }
    double wms = now_ms() - t0;

    // ---- 读阶段 ----
    submitted = done = 0;
    t0 = now_ms();
    while (done < total) {
        int n = 0;
        while (n < depth && submitted < total) {
            struct iocb *cb = &cbs[n];
            off_t off = (off_t)((submitted * block) % file_sz);
            io_prep_pread(cb, fd, (char *)bufs + (size_t)n * block, block, off);
            cb->data = (void *)(long)submitted;
            cbs_p[n] = cb;
            n++; submitted++;
        }
        if (n == 0) break;
        int r = io_submit(ctx, n, cbs_p);
        if (r < 0) { fprintf(stderr, "io_submit(read): %s\n", strerror(-r)); break; }
        struct io_event evs[MAX_DEPTH];
        int got = io_getevents(ctx, r, r, evs, NULL);
        if (got < 0) { fprintf(stderr, "io_getevents(read): %s\n", strerror(-got)); break; }
        done += got;
    }
    double rms = now_ms() - t0;

    printf("\n写: %8.2f ms | IOPS %8.0f | %.1f MB/s\n", wms, total / (wms / 1000),
           (double)total * block / 1048576.0 / (wms / 1000));
    printf("读: %8.2f ms | IOPS %8.0f | %.1f MB/s\n", rms, total / (rms / 1000),
           (double)total * block / 1048576.0 / (rms / 1000));

    io_destroy(ctx);
    free(cbs); free(cbs_p); free(bufs);
    close(fd); unlink(path);
    return 0;
}
