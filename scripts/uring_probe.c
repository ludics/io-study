/*
 * io_uring 可用性探针
 *
 * 直接调用 io_uring_setup 系统调用，判断本机是否真的允许使用 io_uring。
 *
 * 退出码：0 = 可用，1 = 不可用，2 = 参数非法（探针自身用法错误的兜底）
 *
 * 关键点（曾经的坑）：
 *   io_uring_setup(entries, params) 的第二个参数是**指向 struct io_uring_params 的指针**，
 *   内核会 copy_from_user 它。如果你的调用里传了 NULL，内核一律返回 **EFAULT (14, Bad address)**
 *   —— 这并不代表 io_uring 不可用，只是探针自己传错了参数。
 *   正确写法必须传一个真实的（零初始化的）struct io_uring_params。
 *
 * 常见 errno 的含义：
 *   0          -> 可用（返回值即 ring fd，探针会关掉它）
 *   EFAULT(14) -> params 指针非法（传 NULL 就是这种情况，说明**内核允许** io_uring）
 *   EPERM(1)   / EACCES(13) -> 被 seccomp / LSM 拦截（容器里最常见）
 *   ENOSYS(38) -> 内核不支持（< 5.1）或编译时未开启
 *   ENOMEM(12) -> 内存不足
 *   EINVAL(22) -> entries 参数非法
 *
 * 编译：gcc -o uring_probe uring_probe.c
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <linux/io_uring.h>

/* __NR_io_uring_setup：x86_64 / aarch64 / riscv64 都是 425（asm-generic 系统调用表）。
 * 优先用 libc 头文件给的值，取不到时再按架构兜底。 */
#ifndef __NR_io_uring_setup
#  if defined(__x86_64__) || defined(__aarch64__) || defined(__riscv)
#    define __NR_io_uring_setup 425
#  else
#    error "未定义 __NR_io_uring_setup，请手动指定该架构的系统调用号"
#  endif
#endif

static void print_kernel(void) {
    struct utsname u;
    if (uname(&u) == 0)
        printf("内核        : %s (%s)\n", u.release, u.machine);
}

int main(void) {
    struct io_uring_params p;
    long r;

    print_kernel();

    memset(&p, 0, sizeof(p));
    errno = 0;

    /* 注意：必须传 &p，不能传 NULL */
    r = syscall(__NR_io_uring_setup, 8, &p);

    if (r >= 0) {
        printf("io_uring    : 可用 ✅  (ring fd=%ld, sq_entries=%u, cq_entries=%u)\n",
               r, p.sq_entries, p.cq_entries);
        close((int)r);
        return 0;
    }

    printf("io_uring    : 不可用 ❌  errno=%d (%s)\n", errno, strerror(errno));
    switch (errno) {
    case EFAULT:
        printf("  -> params 指针非法。你传 NULL 就会走到这里——这恰恰说明**内核允许 io_uring**，\n");
        printf("     是探针参数写法有误（应传 &struct io_uring_params），不是环境被禁用。\n");
        printf("  -> 直接跑 ./bin/echo_io_uring 试试就知道真实情况。\n");
        break;
    case EPERM:
    case EACCES:
        printf("  -> 被 seccomp / LSM 拦截。容器默认策略会拦 io_uring_setup：\n");
        printf("     docker run --security-opt seccomp=unconfined ...\n");
        printf("     K8s: securityContext.seccompProfile.type=Unconfined\n");
        break;
    case ENOSYS:
        printf("  -> 内核未提供该系统调用：内核 < 5.1，或编译内核时关闭了 io_uring。\n");
        break;
    default:
        printf("  -> 参考 check_env.sh 的修复建议，或查看 dmesg。\n");
        break;
    }
    return 1;
}
