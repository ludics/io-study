/*
 * io_uring 可用性探针
 *
 * 直接调用 io_uring_setup 系统调用，判断本机是否真的允许使用 io_uring。
 * 返回码：0 = 可用，1 = 不可用（被 seccomp / 内核版本 / sysctl 禁用）
 *
 * 编译：gcc -o uring_probe uring_probe.c
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/syscall.h>

#ifndef __NR_io_uring_setup
#  if defined(__x86_64__)
#    define __NR_io_uring_setup 425
#  elif defined(__aarch64__)
#    define __NR_io_uring_setup 425
#  else
#    error "未定义 __NR_io_uring_setup，请手动指定该架构的系统调用号"
#  endif
#endif

int main(void)
{
    long r = syscall(__NR_io_uring_setup, 8, (void *)0);
    if (r < 0) {
        printf("不可用 (errno=%d: %s)\n", errno, strerror(errno));
        return 1;
    }
    printf("可用 (ring fd=%ld)\n", r);
    return 0;
}
