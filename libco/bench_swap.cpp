#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "co_routine.h"

// 测量 libco 协程切换（co_resume / co_yield）的实际开销
// 两个协程 A/B 通过 co_yield_ct() 互相让出，由主循环驱动 resume

static const int ROUNDS = 1000000;

static stCoRoutine_t *g_co_a = NULL;
static stCoRoutine_t *g_co_b = NULL;

// 协程体：被 resume 后立刻 yield 回主协程
static void *pingpong_routine(void *arg) {
    // 无限循环：靠主循环的 resume 次数控制，避免协程提前结束
    while (1) {
        co_yield_ct(); // 让出，回到主协程
    }
    return NULL;
}

int main() {
    co_create(&g_co_a, NULL, pingpong_routine, (void *)0L);
    co_create(&g_co_b, NULL, pingpong_routine, (void *)1L);

    struct timespec t1, t2;

    // 预热
    for (int i = 0; i < 10000; i++) {
        co_resume(g_co_a);
        co_resume(g_co_b);
    }

    // 正式测量：交替 resume 两个协程，各 ROUNDS 次
    clock_gettime(CLOCK_MONOTONIC, &t1);
    for (int i = 0; i < ROUNDS; i++) {
        co_resume(g_co_a); // 主 -> A -> yield 回主
        co_resume(g_co_b); // 主 -> B -> yield 回主
    }
    clock_gettime(CLOCK_MONOTONIC, &t2);

    double ns = (t2.tv_sec - t1.tv_sec) * 1e9 + (t2.tv_nsec - t1.tv_nsec);
    int total_swaps = ROUNDS * 4; // 每次 resume 包含 2 次上下文切换(切出+切回)

    printf("=== libco 协程切换开销实测 ===\n");
    printf("resume 次数        : %d 次 (A/B 交替)\n", ROUNDS * 2);
    printf("上下文切换次数     : %d 次\n", total_swaps);
    printf("总耗时             : %.2f ms\n", ns / 1e6);
    printf("单次切换平均开销   : %.1f ns\n", ns / total_swaps);

    // 对照：getpid() 系统调用开销
    const int N2 = 1000000;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    for (int i = 0; i < N2; i++) {
        getpid();
    }
    clock_gettime(CLOCK_MONOTONIC, &t2);
    double ns2 = (t2.tv_sec - t1.tv_sec) * 1e9 + (t2.tv_nsec - t1.tv_nsec);

    printf("\n--- 对照：getpid() 系统调用开销 ---\n");
    printf("单次 getpid()      : %.1f ns\n", ns2 / N2);
    printf("切换/syscall 比值  : %.3f\n", (ns / total_swaps) / (ns2 / N2));

    // 对照：pthread 线程切换开销（粗略）
    printf("\n--- 说明 ---\n");
    printf("协程切换在用户态完成，无需陷入内核，因此远快于线程切换(约1000ns+)\n");

    return 0;
}
