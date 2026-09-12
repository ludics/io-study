// ============================================================================
//  io_macos.c —— macOS 原生磁盘 I/O 基准（对照 disk/io_sync.c 与 disk/io_libaio.c）
//
//  为什么不能直接用本项目的 io_sync.c / io_libaio.c / io_uring_disk.c？
//    这三个程序依赖的接口在 macOS 上都不存在：
//      O_DIRECT       —— Linux 专有。macOS 的对等物是 fcntl(fd, F_NOCACHE, 1)，
//                        但语义不同：O_DIRECT 要求地址/长度/偏移全部对齐；
//                        F_NOCACHE 只是**提示**内核不要把这个 fd 的页缓存在内存里，
//                        没有对齐要求，也不保证真的完全绕开缓存。
//      libaio/io_uring —— Linux 专有，别的系统上没有。
//    所以本文件用 macOS 上"能拿到的工具"重做同一件事，让你看到：
//      **macOS 没有原生的异步磁盘 I/O 接口，想要并发只能靠多线程。**
//    这正是 libaio / io_uring 存在的意义 —— 它们把"并发"从线程里挪进了内核。
//
//  ── 与 Linux 三个版本的对照 ──────────────────────────────────────────────
//    项目里的实现           macOS 上的对等做法
//    ---------------------  ------------------------------------------------
//    io_sync.c（同步）      本文件的 threads=1
//    io_libaio.c（异步）    本文件的 threads=N —— **只能靠多线程模拟并发深度**
//    io_uring_disk.c        （无对等物；macOS 没有内核级异步 I/O 提交队列）
//
//  ── 用法 ──────────────────────────────────────────────────────────────────
//    ./io_macos <文件> [块大小] [每线程I/O次数] [线程数] [是否F_NOCACHE] [文件MB]
//
//    例：
//      ./io_macos /tmp/iotest.dat 4096 32768 1  1 256    # 单线程（≈ io_sync）
//      ./io_macos /tmp/iotest.dat 4096 32768 32 1 256    # 32 线程（模拟深度 32）
//      ./io_macos /tmp/iotest.dat 4096 32768 32 0 256    # 关掉 F_NOCACHE，看 page cache 的作用
//
//    文件默认 256MB —— 必须远大于内存缓存，否则测出来的是内存不是盘。
//
//  编译（macOS，无需第三方库）：
//    cc -O2 -Wall -Wextra -o io_macos io_macos.c -lpthread
// ============================================================================

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAX_THREADS 256

static int    g_fd      = -1;
static size_t g_block   = 4096;
static long   g_per     = 32768;   // 每个线程做多少次 I/O
static long   g_blocks  = 0;       // 文件里一共有多少块
static int    g_nthreads = 1;      // 当前相位用了几个线程

typedef struct {
    int    tid;
    int    is_write;
    long   done;      // 本线程实际完成的 I/O 次数
    double ms;        // 本线程耗时
} worker_t;

// 单调时钟：不受系统时间被调整的影响，测耗时必须用这个
static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

// ---------------------------------------------------------------------------
// 每个线程干的活：在**整份文件**上按「线程交错」的顺序访问
//
// 为什么用交错（thread i 访问块 i, i+N, i+2N, ...）而不是「每个线程独占一段」？
//   因为我们要比较的是「深度 1」和「深度 N」的吞吐，两者的访问范围必须一致，
//   否则单线程只在小范围里打转（可能整段都待在缓存里），对比就没有意义。
//   交错可以让 N 个线程覆盖同一份文件、同一段地址空间。
// ---------------------------------------------------------------------------
static void *worker(void *arg) {
    worker_t *w = (worker_t *)arg;
    const long stride = g_nthreads;   // 相邻两次访问跨过所有线程各一块

    // 每个线程自己一块对齐的缓冲区（绝不能共享：并发写同一块内存会互相破坏）
    char *buf = NULL;
    if (posix_memalign((void **)&buf, 4096, g_block) != 0 || !buf) {
        fprintf(stderr, "线程 %d 分配缓冲失败\n", w->tid);
        return NULL;
    }
    memset(buf, 'A' + (w->tid % 26), g_block);

    double t0 = now_ms();
    for (long i = 0; i < g_per; i++) {
        long blk = (w->tid + i * stride) % g_blocks;
        off_t off = (off_t)blk * (off_t)g_block;
        ssize_t n;
        if (w->is_write) {
            n = pwrite(g_fd, buf, g_block, off);
        } else {
            n = pread(g_fd, buf, g_block, off);
        }
        if (n < 0) {
            fprintf(stderr, "线程 %d %s 在第 %ld 次失败: %s\n",
                    w->tid, w->is_write ? "写" : "读", i, strerror(errno));
            break;
        }
        w->done++;
    }
    w->ms = now_ms() - t0;

    free(buf);
    return NULL;
}

static void run_phase(const char *name, int is_write, int nthreads) {
    pthread_t th[MAX_THREADS];
    worker_t  ws[MAX_THREADS];

    g_nthreads = nthreads;   // 交错步长要跟当前相位的线程数一致

    double t0 = now_ms();
    for (int i = 0; i < nthreads; i++) {
        ws[i].tid = i;
        ws[i].is_write = is_write;
        ws[i].done = 0;
        ws[i].ms = 0;
        if (pthread_create(&th[i], NULL, worker, &ws[i]) != 0) {
            fprintf(stderr, "创建线程 %d 失败\n", i);
            nthreads = i;
            break;
        }
    }
    long done = 0;
    for (int i = 0; i < nthreads; i++) {
        pthread_join(th[i], NULL);
        done += ws[i].done;
    }
    double ms = now_ms() - t0;

    double sec  = ms / 1000.0;
    double iops = done / sec;
    double mbps = (double)done * g_block / (1024.0 * 1024.0) / sec;
    printf("  %-6s 线程=%-4d 次数=%-8ld 耗时=%8.1f ms  IOPS=%10.0f  带宽=%8.1f MB/s\n",
           name, nthreads, done, ms, iops, mbps);
}

int main(int argc, char *argv[]) {
    const char *path = argc > 1 ? argv[1] : "/tmp/io_macos.dat";
    g_block          = argc > 2 ? (size_t)atol(argv[2]) : 4096;
    g_per            = argc > 3 ? atol(argv[3]) : 32768;
    int nthreads     = argc > 4 ? atoi(argv[4]) : 1;
    int use_nocache  = argc > 5 ? atoi(argv[5]) : 1;
    long size_mb     = argc > 6 ? atol(argv[6]) : 256;

    if (nthreads < 1) nthreads = 1;
    if (nthreads > MAX_THREADS) nthreads = MAX_THREADS;
    if (g_block % 512 != 0) g_block = (g_block / 512 + 1) * 512;   // 取整到 512
    if (size_mb < 1) size_mb = 1;

    // 所有相位都用同一份文件，这样「深度 1」和「深度 N」的访问范围完全一致
    g_blocks = (long)size_mb * 1024 * 1024 / (long)g_block;
    off_t fsize = (off_t)g_blocks * (off_t)g_block;

    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { fprintf(stderr, "open %s 失败: %s\n", path, strerror(errno)); return 1; }

    // ★ macOS 的「O_DIRECT 替身」：F_NOCACHE
    //   注意它只是建议（hint）：内核可以忽略。实测在 APFS 上对本 fd 生效，
    //   能明显看出与带缓存写入的差距。
    if (use_nocache) {
        if (fcntl(fd, F_NOCACHE, 1) != 0) {
            fprintf(stderr, "F_NOCACHE 设置失败（继续跑，数字会含 page cache）: %s\n",
                    strerror(errno));
        }
    }

    if (ftruncate(fd, fsize) != 0) { perror("ftruncate"); close(fd); return 1; }
    g_fd = fd;

    printf("macOS 磁盘基准: file=%s block=%zu per-thread=%ld threads=%d F_NOCACHE=%d\n",
           path, g_block, g_per, nthreads, use_nocache);
    printf("文件大小: %.1f MB\n", (double)fsize / (1024.0 * 1024.0));
    printf("\n");
    run_phase("写", 1, nthreads);
    run_phase("读", 0, nthreads);

    printf("\n备注: macOS 没有 libaio/io_uring 那样的原生异步磁盘接口，\n");
    printf("      「并发深度」只能靠多线程堆出来 —— 这也是那两者存在的意义。\n");

    close(fd);
    unlink(path);
    return 0;
}
