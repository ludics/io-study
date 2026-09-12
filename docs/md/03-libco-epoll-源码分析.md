# libco 如何使用 epoll —— 源码级分析 + 实测

> 本文基于 [Tencent/libco](https://github.com/Tencent/libco) 源码（已 clone 并编译运行），回答一个核心问题：**libco 是如何把 epoll 藏在同步系统调用后面的？**
>
> 包含三部分：① 三层架构源码分析；② 与裸 epoll 的实测性能对比；③ `coctx_swap.S` 汇编级协程切换逐行剖析。
> 对应源码：`libco/bench_swap.cpp`、`libco/compare.sh`

---

## 1. 核心结论（先说结论）

libco 的 epoll 使用方式可概括为**三层结构**：

| 层 | 文件 | 职责 |
| --- | --- | --- |
| ① 薄封装层 | `co_epoll.cpp` | epoll API 跨平台封装（Linux 透传，macOS/BSD 用 kqueue 模拟） |
| ② 协程调度层 | `co_routine.cpp` | **核心**：`co_eventloop` + `co_poll_inner`，把「就绪事件」与「协程恢复」绑定 |
| ③ Hook 层 | `co_hook_sys_call.cpp` | 拦截 `read`/`write`/`connect`/`poll`，让**同步代码自动 yield** |

**一句话总结**：libco 不是在 epoll 上套了层新 API，而是**把 epoll 藏在了同步系统调用后面** —— 用户写普通 `read()`，底层自动 poll → 注册 epoll → yield 让出协程 → epoll 就绪后 resume 恢复。

### 完整调用链路

```
用户代码 read(fd)
  → (被 hook 劫持)
    poll(fd, POLLIN)
      → (poll 也被 hook)
        co_poll_inner()
          → epoll_ctl(ADD, fd)      把 fd 注册进 epoll
          → co_yield_env()          当前协程让出 CPU，挂起
              ↓
        co_eventloop 的 epoll_wait()  事件循环统一等待
              ↓ (fd 就绪)
        co_resume(协程)             恢复之前挂起的协程
          → 回到 read() 继续执行     用户无感知
```

---

## 2. 第 1 层：`co_epoll.cpp` —— 薄封装

最外层，**Linux 下几乎就是直接透传**：

```c
int co_epoll_wait(int epfd, struct co_epoll_res *events, int maxevents, int timeout)
{
    return epoll_wait(epfd, events->events, maxevents, timeout);   // 直接透传
}
int co_epoll_ctl(int epfd, int op, int fd, struct epoll_event *ev)
{
    return epoll_ctl(epfd, op, fd, ev);                            // 直接透传
}
int co_epoll_create(int size)
{
    return epoll_create(size);                                     // 直接透传
}
```

**关键设计**：

1. `co_epoll_res` 结构体：把 `epoll_event` 数组和结果缓存封装在一起，`co_epoll_res_alloc` 用 `calloc` 预分配，避免每次 `epoll_wait` 重新分配内存。
2. **跨平台**：macOS/FreeBSD 下用 kqueue 模拟 epoll 接口（`kevent` 实现 `EPOLL_CTL_ADD/DEL/MOD`、`EPOLLIN/EPOLLOUT`），并用二维数组 `clsFdMap`（1024×1024，可存百万 fd）做 fd→事件映射。

这一层**没有协程逻辑**，纯粹是「换个名字 + 跨平台适配」。真正的魔法在下面两层。

---

## 3. 第 2 层：`co_routine.cpp` —— 协程调度核心

### 3.1 核心数据结构 `stCoEpoll_t`

```c
struct stCoEpoll_t {
    int  iEpollFd;                              // epoll 的 fd
    static const int _EPOLL_SIZE = 1024 * 10;   // 每线程最多 10240 个 fd
    struct stTimeout_t *pTimeout;               // 时间轮（处理超时）
    struct stTimeoutItemLink_t *pstTimeoutList; // 超时链表
    struct stTimeoutItemLink_t *pstActiveList;  // 就绪链表
    co_epoll_res *result;                       // epoll_wait 结果缓存
};
```

- `_EPOLL_SIZE = 1024 * 10`：**每个线程一个 epoll 实例**（`stCoRoutineEnv_t.pEpoll` 是 thread-local）。
- `pTimeout` 是**时间轮**：`AddTimeout` 按 diff 取模放到 `pItems[diff]` 槽位，与 epoll 就绪事件**合并处理**。

### 3.2 `co_eventloop` —— 事件循环（心脏）

```c
void co_eventloop(stCoEpoll_t *ctx, pfn_co_eventloop_t pfn, void *arg)
{
    co_epoll_res *result = ctx->result;
    for (;;) {
        // ① 等 1ms，返回就绪事件（关键：timeout=1，非无限等待！）
        int ret = co_epoll_wait(ctx->iEpollFd, result, _EPOLL_SIZE, 1);

        // ② 遍历就绪事件，把每个事件的 item 加入 active 链表
        for (int i = 0; i < ret; i++) {
            stTimeoutItem_t *item = (stTimeoutItem_t*)result->events[i].data.ptr;
            if (item->pfnPrepare) {
                item->pfnPrepare(item, result->events[i], active);  // 预处理
            } else {
                AddTail(active, item);
            }
        }

        // ③ 检查时间轮，取出超时的 item
        unsigned long long now = GetTickMS();
        TakeAllTimeout(ctx->pTimeout, now, timeout);
        // 标记 bTimeout = true ...

        // ④ 合并 active 和 timeout 链表
        Join(active, timeout);

        // ⑤ 遍历处理每个就绪/超时 item，调用 pfnProcess
        while (lp) {
            PopHead(active);
            if (lp->pfnProcess) lp->pfnProcess(lp);   // → co_resume 恢复协程
            lp = active->head;
        }
        if (pfn) { if (-1 == pfn(arg)) break; }
    }
}
```

**三个精妙设计点**：

1. `epoll_wait(..., 1)` 的 timeout 是 **1ms 而非 -1**。因为 libco 还有**时间轮超时**要处理，必须周期性醒来检查。1ms 是折中：既响应及时，又驱动时间轮。
2. `data.ptr` 存的是 **`stTimeoutItem_t` 指针**（不是 fd！）。这是与朴素 Reactor 的关键区别 —— 通过它能找到要恢复的协程。
3. **事件源统一抽象**：epoll 就绪事件 + 时间轮超时事件，都抽象成 `stTimeoutItem_t`，统一进链表，统一调 `pfnProcess`。

### 3.3 `co_poll_inner` —— 协程化的 poll（核心中的核心）

```c
int co_poll_inner(stCoEpoll_t *ctx, struct pollfd fds[], nfds_t nfds,
                  int timeout, poll_pfn_t pollfunc)
{
    int epfd = ctx->iEpollFd;

    // ① 构造 arg（stPoll_t），记录要 poll 的 fds
    arg.pfnProcess = OnPollProcessEvent;   // 就绪后调这个
    arg.pArg = GetCurrCo(...);             // 记录当前协程

    // ② 把每个 fd 注册进 epoll
    for (i = 0; i < nfds; i++) {
        ev.data.ptr = arg.pPollItems + i;  // ← 存 item 指针（不是 fd）
        ev.events   = PollEvent2Epoll(fds[i].events);
        co_epoll_ctl(epfd, EPOLL_CTL_ADD, fds[i].fd, &ev);
    }

    // ③ 加入时间轮（超时管理）
    arg.ullExpireTime = now + timeout;
    AddTimeout(ctx->pTimeout, &arg, now);

    // ④ 让出当前协程！这是分水岭
    co_yield_env(co_get_curr_thread_env());
    // ↑ 协程在这里挂起，CPU 让给其他协程
    // ↓ epoll 就绪后，co_resume 从这里恢复

    // ⑤ 恢复后，清理 epoll 状态
    for (i = 0; i < nfds; i++) {
        co_epoll_ctl(epfd, EPOLL_CTL_DEL, fds[i].fd, ...);   // 从 epoll 摘除
        fds[i].revents = arg.fds[i].revents;
    }
    return iRaiseCnt;
}
```

**关键点**：

- `data.ptr = arg.pPollItems + i`：把「该 fd 对应的 poll item」存进 epoll 事件。这样 `epoll_wait` 返回时，通过 ptr 直接找到是哪个 fd、哪个协程在等。
- **`co_yield_env` 是分水岭**：调用前代码在「当前协程」执行；调用后协程挂起，控制权交调度器。等 epoll 就绪，`co_eventloop` 调 `pfnProcess`（= `OnPollProcessEvent`），最终 `co_resume(co)` 恢复协程，代码**从 `co_yield_env` 的下一行继续执行**。
- **恢复后立即 `EPOLL_CTL_DEL`**：libco 的 epoll 是**一次性**的 —— 每次 poll 临时注册，处理完就摘除。避免长连接长期占用 epoll 红黑树节点（与朴素 Reactor「注册一次长期持有」不同）。

```c
// 就绪回调：恢复等待的协程
void OnPollProcessEvent(stTimeoutItem_t *ap) {
    stCoRoutine_t *co = (stCoRoutine_t*)ap->pArg;
    co_resume(co);
}
```

---

## 4. 第 3 层：`co_hook_sys_call.cpp` —— Hook 层

这一层让「用户无感知」成为可能。原理是 **dlsym 符号劫持**：用 `RTLD_NEXT` 拿到真正的系统调用函数指针，然后**用同名函数覆盖**。

```c
static read_pfn_t  g_sys_read_func  = (read_pfn_t)dlsym(RTLD_NEXT, "read");
static __poll_pfn_t g_sys___poll_func = (__poll_pfn_t)dlsym(RTLD_NEXT, "__poll");
```

### 4.1 `read` 的 hook 逻辑

```c
ssize_t read(int fd, void *buf, size_t nbyte)
{
    if (!co_is_enable_sys_hook()) return g_sys_read_func(...);   // 未开启则直连
    rpchook_t *lp = get_by_fd(fd);
    if (!lp || (O_NONBLOCK & lp->user_flag)) {
        return g_sys_read_func(...);                             // 非协程 fd 直连
    }
    // 走到这里：fd 由协程管理，且是"阻塞"语义
    struct pollfd pf = {0};
    pf.fd = fd;
    pf.events = (POLLIN | POLLERR | POLLHUP);
    int pollret = poll(&pf, 1, timeout);          // ← 调 poll，但 poll 也被 hook 了！
    ssize_t readret = g_sys_read_func(fd, buf, nbyte);
    return readret;
}
```

**精妙之处**：`read` 里调的 `poll` **本身也被 hook**（`poll` → `co_poll_inner` → `co_yield_env`）。完整链路：

```
用户 read()
  → hook 的 read()
    → hook 的 poll()
      → co_poll_inner()
        → epoll_ctl(ADD) + co_yield_env()      （协程挂起）
        → [epoll 就绪] co_resume()
      → 返回 poll
    → 真正 g_sys_read_func()（数据已就绪，立即读到）
  → 返回用户
```

用户看到的是「`read()` 阻塞等待，数据来了就返回」，但实际上这期间协程已让出 CPU 去干别的事了。

### 4.2 socket hook 的细节

```c
int socket(int domain, int type, int protocol)
{
    int fd = g_sys_socket_func(domain, type, protocol);
    rpchook_t *lp = alloc_by_fd(fd);                        // 登记这个 fd，纳入协程管理
    lp->domain = domain;
    fcntl(fd, F_SETFL, g_sys_fcntl_func(fd, F_GETFL, 0));   // 设为非阻塞
    return fd;
}
```

**关键**：libco 的 socket 一律设成**非阻塞**，但通过 hook 让 `read`/`write` **对外表现为阻塞语义**。这正是 epoll + Reactor 的标准做法（非阻塞 fd + epoll），只不过 libco 用 hook 把「非阻塞的复杂度」藏起来了。

---

## 5. 实测：libco vs 裸 epoll 性能对比

编译运行了两者，用**同一压测脚本**（多线程并发 echo）公平对比：

- 裸 epoll：`echo_epoll`（Reactor 模式）
- libco：`example_echosvr`（协程模式，10 个协程 × 1 进程）

| 场景 | 裸 epoll QPS | libco QPS | 差距 |
| --- | --- | --- | --- |
| 2 线程 / 128 字节 | 53,511 | 51,900 | **-3.0%** |
| 4 线程 / 256 字节 | 80,818 | 72,440 | **-10.4%** |
| 8 线程 / 512 字节 | 41,560 | 41,327 | **-0.6%** |

**实测结论**：

- libco 吞吐**略低于**裸 epoll，差距在 **0.5% ~ 10%** 之间，符合预期。
- 差距来源：hook 层有额外开销（每次 `read`/`write` 都要先 poll 一次，即多一次 `epoll_ctl` + yield/resume 往返）。
- 但换来的是**编程模型的巨大简化**：从「写事件回调」变成「写顺序代码」。这就是 libco 的核心价值 —— **用少量性能换取巨大的开发效率**。

---

## 6. 汇编级：`coctx_swap.S` 协程切换剖析

### 6.1 寄存器布局（`coctx.cpp` 定义）

`coctx_t` 是 `void *regs[14]`，x86_64 下每个 8 字节。布局与汇编偏移**精确对应**：

| regs[i] | 寄存器 | 偏移(字节) | 汇编中出现 |
| --- | --- | --- | --- |
| regs[0] | `%r15` | 0 | `movq %r15, (%rdi)` |
| regs[1] | `%r14` | 8 | `movq %r14, 8(%rdi)` |
| regs[2] | `%r13` | 16 | `movq %r13, 16(%rdi)` |
| regs[3] | `%r12` | 24 | `movq %r12, 24(%rdi)` |
| regs[4] | `%r9` | 32 | `movq %r9, 32(%rdi)` |
| regs[5] | `%r8` | 40 | `movq %r8, 40(%rdi)` |
| regs[6] | `%rbp` | 48 | `movq %rbp, 48(%rdi)` |
| regs[7] | `%rdi` | 56 | `movq %rdi, 56(%rdi)` |
| regs[8] | `%rsi` | 64 | `movq %rsi, 64(%rdi)` |
| regs[9] | 返回地址 | 72 | `movq 0(%rax), %rax` → `72(%rdi)` |
| regs[10] | `%rdx` | 80 | `movq %rdx, 80(%rdi)` |
| regs[11] | `%rcx` | 88 | `movq %rcx, 88(%rdi)` |
| regs[12] | `%rbx` | 96 | `movq %rbx, 96(%rdi)` |
| regs[13] | `%rsp` | 104 | `leaq (%rsp),%rax` → `104(%rdi)` |

### 6.2 保存阶段（当前协程 curr → `%rdi`）

```asm
coctx_swap:
    leaq (%rsp),%rax        ; rax = 当前 rsp
    movq %rax, 104(%rdi)    ; regs[13] = rsp  ← 保存栈指针
    movq %rbx, 96(%rdi)     ; regs[12] = rbx
    movq %rcx, 88(%rdi)     ; regs[11] = rcx
    movq %rdx, 80(%rdi)     ; regs[10] = rdx
    movq 0(%rax), %rax      ; rax = *rsp —— 栈顶存的是"返回地址"
    movq %rax, 72(%rdi)     ; regs[9] = 返回地址 ★
    movq %rsi, 64(%rdi)     ; regs[8]  = rsi
    movq %rdi, 56(%rdi)     ; regs[7]  = rdi
    movq %rbp, 48(%rdi)     ; regs[6]  = rbp
    movq %r8,  40(%rdi)     ; regs[5]  = r8
    movq %r9,  32(%rdi)     ; regs[4]  = r9
    movq %r12, 24(%rdi)     ; regs[3]  = r12
    movq %r13, 16(%rdi)     ; regs[2]  = r13
    movq %r14, 8(%rdi)      ; regs[1]  = r14
    movq %r15, (%rdi)       ; regs[0]  = r15
    xorq %rax, %rax         ; rax = 0（返回值 0）
```

**关键技巧**：`movq 0(%rax), %rax` 这行 —— `rax` 此时是 rsp，`0(%rax)` 就是**栈顶的值（返回地址）**。因为 `coctx_swap` 是被 `call` 进来的，栈顶必然存着返回地址。把它存进 `regs[9]`，恢复时就能「跳回」原执行点。

### 6.3 恢复阶段（目标协程 pending → `%rsi`）

```asm
    movq 48(%rsi), %rbp     ; 恢复 rbp
    movq 104(%rsi), %rsp    ; ★ 恢复 rsp —— 切换到新协程的栈！
    movq (%rsi), %r15       ; 恢复 r15
    movq 8(%rsi),  %r14
    movq 16(%rsi), %r13
    movq 24(%rsi), %r12
    movq 32(%rsi), %r9
    movq 40(%rsi), %r8
    movq 56(%rsi), %rdi     ; 恢复 rdi（第1个参数 s）
    movq 80(%rsi), %rdx
    movq 88(%rsi), %rcx
    movq 96(%rsi), %rbx
    leaq 8(%rsp), %rsp      ; rsp += 8（跳过返回地址槽位）
    pushq 72(%rsi)          ; ★ 把 pending 的返回地址压栈
    movq 64(%rsi), %rsi     ; 最后恢复 rsi（因为一直用它做基址）
    ret                     ; ★ 弹出返回地址并跳转
```

**最精妙的三行**（手工构造 call/ret）：

1. `movq 104(%rsi), %rsp` —— rsp 指向新栈顶（`coctx_make` 里 `*ret_addr = pfn`，栈顶存的是入口函数地址）。
2. `leaq 8(%rsp), %rsp` + `pushq 72(%rsi)` —— 先上移 8 字节，再把返回地址压到新栈顶。
3. `ret` —— 弹出返回地址并跳转，**等价于一次手工的「调用」**。

### 6.4 为什么保存 rdi/rsi 等 caller-saved 寄存器？

x86_64 System V ABI 规定 `rbx, rbp, r12-r15, rsp` 是 **callee-saved**，理论上只需保存这些。但 libco 还保存了 `rdi, rsi, rdx, rcx, r8, r9`（caller-saved），原因在 `coctx_make`：

```c
int coctx_make(coctx_t* ctx, coctx_pfn_t pfn, const void* s, const void* s1) {
    char* sp = ctx->ss_sp + ctx->ss_size - sizeof(void*);
    sp = (char*)((unsigned long)sp & -16LL);      // 16 字节对齐

    memset(ctx->regs, 0, sizeof(ctx->regs));
    void** ret_addr = (void**)(sp);
    *ret_addr = (void*)pfn;                        // 栈顶放入口函数地址

    ctx->regs[kRSP]     = sp;                      // regs[13] = 栈指针
    ctx->regs[kRETAddr] = (char*)pfn;              // regs[9]  = 入口函数
    ctx->regs[kRDI]     = (char*)s;                // regs[7]  = 第1个参数 ★
    ctx->regs[kRSI]     = (char*)s1;               // regs[8]  = 第2个参数 ★
    return 0;
}
```

**答案**：恢复时 `movq 56(%rsi), %rdi` 和 `movq 64(%rsi), %rsi` 分别把 `s` 和 `s1` 放入 rdi/rsi —— 这正好是 C 函数调用的**前两个参数**！所以 `ret` 跳转后，`pfn(s, s1)` 就能被正确调用。这是协程入口**参数传递的机制**。

### 6.5 实测：协程切换开销

基准测试（`libco/bench_swap.cpp`）：两个协程通过 `co_yield_ct()` 互相切换。

```
=== libco 协程切换开销实测 ===
resume 次数        : 2000000 次 (A/B 交替)
上下文切换次数      : 4000000 次
总耗时             : 62.48 ms
单次切换平均开销    : 15.6 ns

--- 对照：getpid() 系统调用开销 ---
单次 getpid()      : 49.5 ns
切换/syscall 比值  : 0.316
```

| 指标 | 数值 |
| --- | --- |
| 单次协程切换 | **15.6 ns** |
| 单次 `getpid()` 系统调用 | **49.5 ns** |
| 切换/系统调用 比值 | **0.32x** |

**结论**：单次协程切换仅 **15.6 ns**，比一次 `getpid()` 系统调用（49.5 ns）还快 **3 倍**。这完美印证了汇编分析 —— **协程切换完全是用户态的寄存器读写（约 20 条 mov 指令），不陷入内核、不切换页表、不调度线程**。相比之下线程切换通常需 1000ns+。

---

## 7. libco epoll vs 朴素 Reactor

| 维度 | 朴素 Reactor | libco |
| --- | --- | --- |
| `data.ptr` 存什么 | fd | `stTimeoutItem_t*` 指针（能找到协程） |
| epoll 注册策略 | 注册一次，长期持有 | **临时注册，poll 完就 DEL** |
| 事件等待 | `epoll_wait(-1)` 无限等 | `epoll_wait(1)` 1ms（为驱动时间轮） |
| 超时处理 | 无 / 靠 epoll timeout | 独立时间轮，与就绪事件统一抽象 |
| 编程接口 | 显式事件回调（反直觉） | hook 系统调用，**同步写法** |
| 事件循环 | 手动写 while | `co_eventloop` 封装好 |
| 切换开销 | 无（无协程） | 15.6 ns/次 |

---

## 8. 完整闭环：`example_echosvr` 如何启动

```c
int main(...) {
    g_listen_fd = CreateTcpSocket(port, ip, true);
    listen(g_listen_fd, 1024);
    SetNonBlock(g_listen_fd);

    for (k = 0; k < proccnt; k++) {     // 多进程
        fork();
        // 子进程：
        co_eventloop(co_get_curr_thread_env()->pEpoll, ...);
    }
}
```

启动后，每个进程一个 `co_eventloop`，每个新连接创建一个协程，协程里写的是**完全同步的 read/write 代码**，底层自动走 hook → epoll → yield/resume。

---

## 9. 总结

libco 的设计哲学可以概括为一句话：**用 hook 把「异步的事件驱动」伪装成「同步的阻塞调用」**。

| 层次 | 解决什么问题 |
| --- | --- |
| `co_epoll.cpp` | 跨平台 + 结果缓存，让 epoll 可移植 |
| `co_routine.cpp` | 让 epoll 就绪事件直接唤醒对应协程（`data.ptr = item`，不是 fd） |
| `co_hook_sys_call.cpp` | 让用户完全不感知 epoll 存在，写同步代码即可 |

**代价与收益**：

- **代价**：每次 I/O 多一次 poll + epoll_ctl + yield/resume 往返，吞吐比裸 epoll 低 0.5%~10%。
- **收益**：单次协程切换 15.6 ns（比系统调用快 3 倍），编程模型从「回调地狱」回归「顺序代码」。

**实测数据回顾**：

- ✅ libco vs 裸 epoll 吞吐：差距 0.5% ~ 10%（hook 开销）
- ✅ 协程切换 15.6 ns vs `getpid()` 49.5 ns
- ✅ `coctx_swap.S` 汇编逐行剖析：寄存器布局、手工 call/ret 构造、参数传递机制
