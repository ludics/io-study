# 深入浅出 io_uring 编程

> 本文讲**怎么用 io_uring 写程序**：三个队列的结构、API 全貌、内核里怎么跑、完整示例、
> 以及一堆"文档没写但你一定会踩"的细节。所有数字都是本项目实测。
>
> 相关文档：
> - [04-epoll-编程指南.md](./04-epoll-编程指南.md)（Reactor 的做法）
> - [02-reactor-proactor-io_uring.md](./02-reactor-proactor-io_uring.md)（为什么叫 Proactor）
> - 本项目实现：`network/echo_io_uring.c`（基础版）、`network/echo_io_uring_adv.c`（全特性 + 兼容旧内核）、
>   `network/echo_io_uring_modern.c`（**只面向新内核的现代写法，推荐从这里学**）
>
> 环境：aarch64 / Ubuntu 22.04（内核 5.15 / liburing 2.1）与 Ubuntu 26.04（内核 7.0 / liburing 2.14）两台 VM。

---

## 1. 一句话定位

io_uring 是**完成通知 + 内核代做 I/O**：你只描述「把 fd 的数据读进这块内存」，
内核替你搬，搬完了通知你「已完成」。这就是 Proactor。

和 epoll 最本质的区别：

| | epoll（Reactor） | io_uring（Proactor） |
|---|---|---|
| 通知内容 | "这个 fd **可读**了" | "这个请求**已经完成**了" |
| 谁搬数据 | 应用调 `read()` | **内核** |
| 每条消息的系统调用 | read + write + 等待 ≈ 3 次 | 提交 + 收割，可低到 **0 次**（见 §7.2） |
| 接口形态 | fd 事件循环 | **提交队列 / 完成队列**（两个环形缓冲区） |

## 2. 三个内存结构（理解一切的基础）

```
        用户态                          内核态
   ┌──────────────────┐
   │  SQ 环 (提交队列) │◄─── mmap ───── 内核读
   │  head  tail      │
   ├──────────────────┤
   │  SQE 数组         │◄─── mmap ───── 内核读（每个条目 64 字节 = 一条请求描述）
   ├──────────────────┤
   │  CQ 环 (完成队列) │───► mmap ───── 内核写（每个条目 16 字节 = 一条完成结果）
   │  head  tail      │
   └──────────────────┘
        ↑ 应用读写                     ↑ 内核读写
```

三块内存都是 `mmap` 到用户态的共享内存。**应用"提交"请求只是往共享内存里写，
没有系统调用**；只有需要通知内核时（提交、唤醒、等待）才调 `io_uring_enter`。

实测一下内存布局（`strace -e trace=io_uring_setup`，本机 7.0）：

```
io_uring_setup(256, {sq_entries=256, cq_entries=512,
                     features=IORING_FEAT_SINGLE_MMAP|IORING_FEAT_NODROP|
                              IORING_FEAT_SUBMIT_STABLE|...|IORING_FEAT_FAST_POLL|...,
                     sq_off={head=0, tail=4, ring_mask=16, ring_entries=24, array=...},
                     cq_off={head=8, tail=12, ring_mask=20, ring_entries=28, overflow=44, cqes=64}})
```

- 你申请 256 个条目，内核给的 **CQ 是 512**（默认 2 倍，可用 `IORING_SETUP_CQSIZE` 改）。
- `sq_off` / `cq_off` 是**各字段在共享内存里的字节偏移** —— 这就是"零拷贝通信"的实现方式：
  双方约定好偏移，直接读写同一块内存。
- `features` 是内核回填的能力位图，启动时打出来能省很多瞎猜（本项目三个实现都打了）。

## 3. 编程模型：五步

```
 1. 建环      io_uring_queue_init[_params]()    ← 一次 mmap 三块内存
 2. 准备请求  io_uring_get_sqe() + io_uring_prep_*()   ← 只填内存，不进内核
 3. 提交      io_uring_submit()                 ← 需要时才进内核
 4. 收割      io_uring_wait_cqe[peek/peek_batch]() + cqe_seen/cq_advance
 5. 销毁      io_uring_queue_exit()
```

**第 2 步和第 4 步是理解 io_uring 的关键**：

- 第 2 步产出的是 **SQE（Submission Queue Entry）**，64 字节，`opcode/fd/addr/len/offset/flags/user_data` 等字段。
  它是**请求的完整描述**，内核照着它干活。
- 第 4 步拿到的是 **CQE（Completion Queue Entry）**，16 字节：`user_data` + `res` + `flags`。
  `res` 语义**与系统调用完全一致**（≥0 成功，<0 是负 errno）。

### 3.1 一个必须自己解决的问题：上下文编码

内核只会把 `user_data` **原样带回**，它不知道这是哪个 fd、哪种操作。所以：

```c
/* 把 (op, fd, len...) 塞进 64 位 user_data */
static unsigned long pack(int op, int fd, unsigned len) {
    return (unsigned long)op | ((unsigned long)(unsigned)fd << 8) | ((unsigned long)len << 32);
}
io_uring_sqe_set_data64(sqe, pack(OP_RECV, fd, 0));
...
unsigned long ud = cqe->user_data;
int op = ud & 0xFF;   /* 完成时自己解回来 */
```

**不区分操作类型就会逻辑错乱** —— read 完成和 write 完成要做的事完全相反。
更常见的做法是 `io_uring_sqe_set_data(sqe, my_req_ptr)` 直接塞指针（注意别塞栈上的临时变量）。

## 4. API 全貌

### 4.1 建环与销毁

```c
int io_uring_queue_init(unsigned entries, struct io_uring *ring, unsigned flags);
int io_uring_queue_init_params(unsigned entries, struct io_uring *ring,
                               struct io_uring_params *p);
void io_uring_queue_exit(struct io_uring *ring);
```

需要精细控制（SQPOLL、指定 SQ 线程 CPU、自定义 CQ 大小）时用 `_params` 版本：

```c
struct io_uring_params p;
memset(&p, 0, sizeof(p));
p.flags = IORING_SETUP_SQPOLL | IORING_SETUP_SQ_AFF;
p.sq_thread_cpu = 5;                    /* SQ 内核线程绑到核 5 */
io_uring_queue_init_params(1024, &ring, &p);
```

**`io_uring_queue_init` 失败的原因要对症下药**（本项目实现里都打印了这三条）：

1. 内核 < 5.1；
2. **容器默认 seccomp 策略拦截了 `io_uring_setup`**（最常见的坑，Docker/K8s 里几乎必踩）；
3. 没装 liburing。

### 4.2 SQE 的字段与 flags

```c
struct io_uring_sqe {
    __u8  opcode;        /* IORING_OP_* */
    __u8  flags;         /* IOSQE_* */
    __u16 ioprio;        /* 部分 opcode 用作"子标志"（如 RECV_MULTISHOT） */
    __s32 fd;            /* 目标 fd；若置了 IOSQE_FIXED_FILE，这里是"固定表下标" */
    __u64 off;           /* 偏移（文件 I/O 用） */
    __u64 addr;          /* 缓冲区地址（或 accept 的 sockaddr） */
    __u32 len;           /* 长度 */
    union { __u32 rw_flags; __u32 fsync_flags; __u16 poll_events; ... };
    __u64 user_data;     /* ★ 你的上下文，内核原样带回 */
    union { __u16 buf_index; __u16 buf_group; };  /* 提供缓冲区用 */
    ...
};
```

`flags` 里常用的位（值已从内核头文件确认）：

| flag | 值 | 作用 |
|---|---|---|
| `IOSQE_FIXED_FILE` | `1<<0` | `fd` 字段改为解释成固定文件表的下标 |
| `IOSQE_IO_DRAIN` | `1<<1` | 这个请求要等前面所有请求完成后才开始（排序屏障） |
| `IOSQE_IO_LINK` | `1<<2` | 与下一个 SQE 串成链：前一个失败则整链取消 |
| `IOSQE_IO_HARDLINK` | `1<<3` | 同上，但前一个失败也继续执行 |
| `IOSQE_ASYNC` | `1<<4` | 强制走异步路径（丢给 io-wq 线程池） |
| `IOSQE_BUFFER_SELECT` | `1<<5` | 让内核从提供缓冲区环里自选一块（见 §6.2） |
| `IOSQE_CQE_SKIP_SUCCESS` | `1<<6` | 成功时不产生 CQE（省收割开销） |

**⚠️ 顺序陷阱**：liburing 的 `io_uring_prep_*()` 内部会**先清零 `flags`/`ioprio`**，
所以自定义 flag 必须写在 prep **之后**：

```c
io_uring_prep_recv_multishot(sqe, idx, NULL, 0, 0);
sqe->flags |= IOSQE_BUFFER_SELECT;     /* ✅ 必须在 prep 之后 */
sqe->flags |= IOSQE_FIXED_FILE;        /* ✅ */
sqe->buf_group = 0;
```

### 4.3 准备请求：`io_uring_prep_*` 家族

不用手填 SQE —— 用 prep 函数（内部会 `memset(sqe, 0, sizeof(*sqe))` 再填字段）：

```c
io_uring_prep_accept(sqe, lfd, NULL, NULL, 0);
io_uring_prep_multishot_accept(sqe, lfd, NULL, NULL, 0);          /* ≥5.19 */
io_uring_prep_multishot_accept_direct(sqe, lfd, NULL, NULL, 0);   /* 直接进固定文件表 */
io_uring_prep_recv(sqe, fd, buf, len, 0);
io_uring_prep_recv_multishot(sqe, fd, NULL, 0, 0);                /* ≥5.19，一次提交持续收 */
io_uring_prep_send(sqe, fd, buf, len, MSG_NOSIGNAL);
io_uring_prep_send_zc(sqe, fd, buf, len, flags, zc_flags);        /* ≥6.0，零拷贝 */
io_uring_prep_read(sqe, fd, buf, len, offset);
io_uring_prep_write(sqe, fd, buf, len, offset);
io_uring_prep_timeout(sqe, &ts, 0, 0);
io_uring_prep_poll_add(sqe, fd, POLLIN);
io_uring_prep_cancel64(sqe, target_user_data, 0);
io_uring_sqe_set_data64(sqe, ud);        /* 或者 io_uring_sqe_set_data(sqe, ptr) */
/* ⚠️ io_uring_sqe_set_data64() 是 liburing 2.2 才有的。
   Ubuntu 22.04 自带的 2.1 没有它 —— 编译会报 implicit declaration 然后链接失败。
   老版本请用 io_uring_sqe_set_data(sqe, (void *)(uintptr_t)ud)（64 位平台上等价）。
   本仓库的 examples/io_uring_echo.c 就是这么做兼容的（Makefile 自动探测）。*/
```

**取 SQE 时环可能是满的**（SQ 环只有 entries 个槽位）：

```c
struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
if (!sqe) {                       /* SQ 满 → 先提交把内核消费掉，腾出槽位 */
    io_uring_submit(ring);
    sqe = io_uring_get_sqe(ring);
    if (!sqe) { /* 还是满，只能放弃或等 */ }
}
```

### 4.4 提交

```c
int io_uring_submit(struct io_uring *ring);                       /* 提交 SQ 里所有待提交 */
int io_uring_submit_and_wait(struct io_uring *ring, unsigned wait_nr);  /* 提交并等 N 个完成 */
int io_uring_enter(unsigned fd, unsigned to_submit, unsigned min_complete, unsigned flags, ...);
int io_uring_sq_ready(const struct io_uring *ring);   /* 还有几个 SQE 没提交 */
int io_uring_sq_space_left(const struct io_uring *ring);
```

- `io_uring_submit()` **在 SQ 环里没有待提交内容时不会进内核**（liburing 里直接 return 0）。
  所以"每轮循环都调一次 submit"是安全的，不会白花系统调用。
- **SQPOLL 模式下提交也不需要进内核**（除非要唤醒睡着的 SQ 线程）：

```c
/* 实测：全特性（SQPOLL + 忙轮询）下稳态 io_uring_enter 次数 = 0 */
```

### 4.5 收割

```c
int io_uring_wait_cqe(struct io_uring *ring, struct io_uring_cqe **cqe_ptr);      /* 等一个，会阻塞 */
int io_uring_peek_cqe(struct io_uring *ring, struct io_uring_cqe **cqe_ptr);      /* 只看一眼，不阻塞 */
unsigned io_uring_peek_batch_cqe(struct io_uring *ring, struct io_uring_cqe **cqes, unsigned count);
void io_uring_cqe_seen(struct io_uring *ring, struct io_uring_cqe *cqe);          /* 消费一个 */
void io_uring_cq_advance(struct io_uring *ring, unsigned nr);                     /* 一次消费 N 个 */
#define io_uring_for_each_cqe(ring, head, cqe) ...                                /* 遍历当前所有 */
```

**必须"消费"**：拿到 CQE 后不调 `cqe_seen`/`cq_advance`，CQ 环的 head 就不会前进，
环很快被填满，内核会进入 overflow 状态。

三种收割风格，各有适用场景：

```c
/* A. 最简单：等一个、处理一个（基础版 echo_io_uring.c 用的） */
io_uring_wait_cqe(&ring, &cqe);
handle(cqe);
io_uring_cqe_seen(&ring, cqe);

/* B. 批量（推荐）：一次把 CQ 里已有的全取走，只有 CQ 空时才进内核 —— 省掉大量 enter */
struct io_uring_cqe *cqes[64];
unsigned n = io_uring_peek_batch_cqe(&ring, cqes, 64);
if (n == 0) { io_uring_submit(&ring); continue; }
for (unsigned i = 0; i < n; i++) handle(cqes[i]);
io_uring_cq_advance(&ring, n);

/* C. 遍历式：配合 io_uring_wait_cqe 等一次，然后 for_each + cq_advance */
io_uring_wait_cqe(&ring, &cqe);
unsigned head, k = 0;
io_uring_for_each_cqe(&ring, head, cqe) { handle(cqe); k++; }
io_uring_cq_advance(&ring, k);
```

> **真实教训**：本项目的磁盘版 `disk/io_uring_disk.c` 一开始写成
> `for (i = 0; i < submitted; i++) io_uring_wait_cqe(&ring, &cqe);`
> —— 一批 32 个 I/O 最多进内核 32 次，把批量收割的红利整个吃光，
> 写 IOPS 只有 libaio 的 1/11。改成风格 B/C 后读 IOPS 从 213k 提到 **239k**，反超 libaio。

### 4.6 注册（register）：把"每次都要传的东西"变成一次性的

```c
int io_uring_register_files(struct io_uring *ring, const int *files, unsigned nr);
int io_uring_register_files_sparse(struct io_uring *ring, unsigned nr);          /* 只登记槽位 */
int io_uring_register_files_update(struct io_uring *ring, unsigned off, int *files, unsigned nr);
int io_uring_register_buf_ring(struct io_uring *ring, struct io_uring_buf_reg *reg, unsigned flags);
int io_uring_register_buffers(struct io_uring *ring, const struct iovec *iov, unsigned nr);
```

- **注册文件**：之后用 `IOSQE_FIXED_FILE` + 下标代替 fd，省掉每次操作的 `fget/fput`。
  `register_files_sparse` 先登记 N 个空槽，造连接时再用 `multishot_accept_direct` 自动分配。
- **注销单个槽位**：`io_uring_register_files_update(ring, idx, &neg_one, 1)`，`-1` 就是"清空这个槽"。
  **固定文件表里的文件不能直接 `close()`**。
- **注册缓冲区环**：见 §6.2。

### 4.7 完成事件的 flags

| CQE flag | 值 | 含义 |
|---|---|---|
| `IORING_CQE_F_BUFFER` | `1<<0` | 用了提供缓冲区；`flags >> IORING_CQE_BUFFER_SHIFT(16)` 就是 bid |
| `IORING_CQE_F_MORE` | `1<<1` | **这个请求还没结束，后面还有 CQE**（multishot 的标志） |
| `IORING_CQE_F_SOCK_NONEMPTY` | `1<<2` | 缓冲区里还有数据（BUNDLE 用） |
| `IORING_CQE_F_NOTIF` | `1<<3` | 零拷贝发送的**第二个** CQE：内存可以回收了 |
| `IORING_CQE_F_BUF_MORE` | `1<<4` | 增量模式缓冲区环还有部分空间 |

**`F_MORE` 用法**：multishot recv 收到数据时带 `F_MORE`，
说明"这次 recv 还在继续"，**不要**再补提交；只有**没有** `F_MORE` 时才需要重新挂 recv。
把这个判断写反，会挂出多条在飞的 recv，症状是数据被重复处理或乱序。

## 5. 完整示例：最朴素但正确的 echo server

完整可运行版本是 **[`examples/io_uring_echo.c`](../../examples/io_uring_echo.c)**（`make examples`）。
它**刻意不用 SQPOLL**，好让你先把"提交/收割"这两件事看清楚。骨架如下：

```c
enum { OP_ACCEPT = 1, OP_RECV, OP_SEND };

/* op(8) | fd(24) | off(16) | len(16) */
static unsigned long pack(int op, int fd, unsigned off, unsigned len) {
    return (unsigned long)(unsigned)op | ((unsigned long)(unsigned)fd << 8)
         | ((unsigned long)(off & 0xFFFF) << 32) | ((unsigned long)(len & 0xFFFF) << 48);
}

static void arm_recv(int fd) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    if (!sqe) return;
    io_uring_prep_recv(sqe, fd, buf_of(fd), BUF_SZ, 0);
    io_uring_sqe_set_data64(sqe, pack(OP_RECV, fd, 0, 0));
}

int main(void) {
    /* ... 建监听 socket，设非阻塞 ... */
    if (io_uring_queue_init(256, &ring, 0) < 0) return 2;

    arm_accept();
    io_uring_submit(&ring);          /* ★ 必须有在飞请求，否则循环无事可做 */

    for (;;) {
        struct io_uring_cqe *cqe;
        if (io_uring_wait_cqe(&ring, &cqe) < 0) break;

        int op = unpack_op(cqe->user_data), fd = unpack_fd(cqe->user_data);
        unsigned off = unpack_off(cqe->user_data), len = unpack_len(cqe->user_data);
        int res = cqe->res;
        io_uring_cqe_seen(&ring, cqe);

        switch (op) {
        case OP_ACCEPT:
            if (res >= 0) { set_nonblock(res); arm_recv(res); }
            arm_accept();                       /* ★ accept 是单次的，必须补一个 */
            break;
        case OP_RECV:
            if (res > 0)       arm_send(fd, 0, (unsigned)res);
            else if (res == 0) close(fd);
            else               close(fd);
            break;
        case OP_SEND:
            if (res < 0) { close(fd); break; }
            if ((unsigned)res < len)            /* ★ 部分写：接着发剩下的那截 */
                arm_send(fd, off + (unsigned)res, len - (unsigned)res);
            else
                arm_recv(fd);                   /* 回显完成，续挂接收 */
            break;
        }
        io_uring_submit(&ring);
    }
}
```

### 一次真实的消息往返长什么样

`strace -f -e trace=io_uring_enter,accept,recvfrom,sendto`（本机 7.0，处理 2 条消息）：

```
io_uring_setup(256, {...}) = 4

io_uring_enter(4, 1, 0, 0, NULL, 8) = 1                          ← 提交初始 accept（1 个 SQE）
io_uring_enter(4, 0, 1, IORING_ENTER_GETEVENTS, NULL, 8) = 0     ← 等完成（睡进去）
io_uring_enter(4, 2, 0, 0, NULL, 8) = 2                          ← 提交 2 个：给新连接 recv + 补 accept
io_uring_enter(4, 1, 0, 0, NULL, 8) = 1                          ← 提交 send
io_uring_enter(4, 1, 0, 0, NULL, 8) = 1                          ← 提交下一个 recv
io_uring_enter(4, 1, 0, 0, NULL, 8) = 1
io_uring_enter(4, 0, 1, IORING_ENTER_GETEVENTS, NULL, 8) = 0     ← 没活了，睡等
```

对照 epoll 的序列（见 04 篇），差别一目了然：

- epoll：`epoll_wait → read → write → epoll_wait → ...`，**每次读写都是独立的系统调用**。
- io_uring：只有 `io_uring_enter` 一种调用，且一次可以提交/收割多个。
  **注意 `accept(2)` / `recvfrom(2)` / `sendto(2)` 一次都没出现** —— 内核替我们做了。

## 6. 高级特性（现代写法）

每个特性都会给出"它解决什么问题 / 怎么用 / 什么代价"。

### 6.1 SQPOLL：提交不进内核

```c
p.flags = IORING_SETUP_SQPOLL | IORING_SETUP_SQ_AFF;
p.sq_thread_cpu = 5;                 /* 给 SQ 内核线程单独一个核 */
```

内核起一个线程持续轮询 SQ 环，应用发布 SQE 后**不需要 `io_uring_enter`**。

- **收益**：干掉"提交"那次系统调用。实测同一二进制 `SQPOLL=0→1`：52k → 117k QPS（**2.25x**）。
- **代价**：内核线程**忙轮询**，常驻吃满一个核。
- **⚠️ 部署铁律**：必须给它一个**独立的核**。把服务端 `taskset` 到单核时，
  SQ 线程和应用抢同一个核，实测 **QPS 从 63k 掉到 15.4k**，比不开 SQPOLL 还慢。
  典型用法：`ECHO_SQ_CPU=5 taskset -c 4 ./server`。
- **⚠️ 互斥关系**（实测内核返回 `EINVAL`）：`SQPOLL` **不能**与
  `IORING_SETUP_COOP_TASKRUN` / `IORING_SETUP_DEFER_TASKRUN` 同时用；
  但可以与 `IORING_SETUP_SINGLE_ISSUER` 共存。

### 6.2 提供缓冲区环：内存从 O(连接数) 降到 O(缓冲数)

普通做法要"每条连接常驻一块接收缓冲区"。提供缓冲区环反过来：应用先把一批缓冲区
挂到环上，提交 recv 时**让内核自己挑一块**，完成后在 CQE flags 里告诉你是哪一块。

```c
#define BGID 0
struct io_uring_buf_ring *br;
struct io_uring_buf_reg reg = {
    .ring_addr    = (unsigned long)br,
    .ring_entries = 256,          /* 必须是 2 的幂 */
    .bgid         = BGID,
};
io_uring_register_buf_ring(&ring, &reg, 0);

/* 挂上所有缓冲区：先写条目，最后一次性 advance 发布 tail（顺序不能反） */
io_uring_buf_ring_init(br);
for (int i = 0; i < 256; i++)
    io_uring_buf_ring_add(br, data + (size_t)i * BUF_SZ, BUF_SZ, i, 255 /*mask*/, i /*offset*/);
io_uring_buf_ring_advance(br, 256);

/* 提交 recv 时只写"从环里挑"，不写地址 */
io_uring_prep_recv_multishot(sqe, fd, NULL, 0, 0);
sqe->flags |= IOSQE_BUFFER_SELECT;
sqe->buf_group = BGID;

/* 完成时取 bid，用完归还 */
unsigned bid = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
io_uring_buf_ring_add(br, data + (size_t)bid * BUF_SZ, BUF_SZ, bid, 255, 0);
io_uring_buf_ring_advance(br, 1);
```

- **收益**：内存不再随连接数线性增长；消息长度不需要预先知道。
- **⚠️ 顺序陷阱**：`io_uring_buf_ring_add` 只写条目，必须 `advance` 才发布 tail。
  而且 **tail 与 `bufs[0]` 前 16 字节是重叠的**（内核省内存的技巧），
  所以必须"先写条目、后更新 tail"，反过来会让内核读到半个条目。
- **⚠️ 归还时机**：缓冲区被 send 引用期间**不能归还**。零拷贝发送要等 `F_NOTIF` 那个 CQE。
- 缓冲区用尽时内核返回 `-ENOBUFS`，且 multishot 会结束，需要补提交。

### 6.3 multishot：一次提交，持续收割

```c
io_uring_prep_recv_multishot(sqe, fd, NULL, 0, 0);            /* ★ 注意不是独立 opcode */
sqe->ioprio |= ...                                            /* 已被 prep 设置好 */
/* 判断：CQE 带 IORING_CQE_F_MORE → 还在继续，别补提交 */
if (!(cqe->flags & IORING_CQE_F_MORE)) arm_recv(fd);          /* 没有 F_MORE 才补 */
```

**⚠️ 最常见的误解**：`IORING_RECV_MULTISHOT` **不是 opcode**，而是
`IORING_OP_RECV` 上的一个 `ioprio` 标志位（值 `1<<1`）。网上很多说法是错的。
accept 同理：`IORING_ACCEPT_MULTISHOT` = `1<<0`。

收益：把"每条消息提交一次"变成"每条连接提交一次"，是**质变**。

### 6.4 固定文件表 + direct accept：连接建立也不用系统调用

```c
io_uring_register_files_sparse(&ring, MAX_CONNS);         /* 先登记 N 个空槽 */
...
io_uring_prep_multishot_accept_direct(sqe, lfd, NULL, NULL, 0);
/* 完成时 cqe->res 是"固定表下标"，不是 fd！ */
...
/* 之后都用 IOSQE_FIXED_FILE + 下标收发，省掉 fget/fput */
io_uring_prep_recv(sqe, idx, buf, len, 0);
sqe->flags |= IOSQE_FIXED_FILE;
/* 关闭：不能 close(idx)，要注销槽位 */
int neg = -1;
io_uring_register_files_update(&ring, idx, &neg, 1);
```

- **收益**：连接建立 0 系统调用；每次收发少一次 `fget/fput`。
- **代价**：拿到的是下标不是 fd，**不能对连接 `setsockopt`**（比如设 `TCP_NODELAY`）。
  真要设选项：用非 direct 的 accept 拿真 fd 设完，再 `register_files_update` 装进表
  （每条连接多一次系统调用）。
- **实测结论（重要）**：在本项目 256B ping-pong 这个量级上，
  `FIXED=1` vs `FIXED=0` **各跑 7 轮中位数都是 222k QPS —— 没有可测量收益**。
  理论收益要到大连接数 / 大 fd 表才显现。所以"用了固定表就一定更快"是错的，
  但你至少要会写。

### 6.5 SEND_ZC 零拷贝发送 —— 默认别开

```c
io_uring_prep_send_zc(sqe, fd, buf, len, MSG_NOSIGNAL, 0);
/* 会返回两个 CQE：
 *   第一个：res = 已发出字节数（数据已进协议栈）→ 内存还不能动
 *   第二个：带 IORING_CQE_F_NOTIF     → 内核不再引用这块内存，可以归还了
 */
```

- **⚠️ 必须等 `F_NOTIF`** 才能归还/复用缓冲区，否则会改写正在发出的数据。
- **实测：小消息下它是负收益**。同一个二进制只改 `ECHO_ZC`：
  **233k → 175k（-26%）**。因为每次发送多了一个 CQE 要处理，
  而 256B 消息省下的那次拷贝不值这个开销。另一份独立实现上也复现了同样现象。
- 结论：**「零拷贝」不是免费的午餐**。大消息 / 高带宽场景才值得开
  （还要注意 `IORING_NOTIF_USAGE_ZC_COPIED` 标志 —— 它表示"内核其实被迫拷了一份"，
  说明没真零拷贝成功）。

### 6.6 CQ 忙轮询：收割也不进内核

```c
unsigned n = io_uring_peek_batch_cqe(&ring, cqes, 64);   /* 只看用户态共享内存 */
if (n == 0) { io_uring_submit(&ring); continue; }        /* 顺手提交 */
for (unsigned i = 0; i < n; i++) handle(cqes[i]);
io_uring_cq_advance(&ring, n);
```

- **收益**：干掉"收割"那次系统调用。代价是烧一个核空转。
- **⚠️ 必须和 SQPOLL 配套**：只开忙轮询不开 SQPOLL → 提交仍要进内核，几乎没效果
  （实测 52.2k vs 51.6k）。两个都开才是质变：**63k → 189k**。
  原因很好记：**"省一半系统调用"没用，"把等待那一次省掉"才有用** ——
  因为进内核等待意味着进程要睡下去再被唤醒。

### 6.7 现代版的实测总账（内核 7.0，16 线程 × 256B）

| 配置 | 核数 | QPS | 每条消息的 io_uring_enter |
|---|---|---|---|
| epoll 单线程（基准） | 1 | 112k | —（走 epoll_wait） |
| io_uring 基础版 | 1 | 74.7k | 2.01 |
| 全特性（SQPOLL+缓冲环+multishot+ZC+忙轮询） | 2 | 175k | — |
| **全特性 + 关零拷贝（推荐）** | 2 | **233k** | **0.000** |
| 关掉忙轮询 | 2 | 56.9k | 0.14 |
| SQPOLL 和忙轮询都关 | 1 | 39.9k | 1.02 |

**233k / 112k = 2.08x**，且稳态系统调用为 0。这是本项目里 io_uring 唯一明确赢过 epoll 的路径。

## 7. 陷阱清单

| 陷阱 | 症状 | 正确做法 |
|---|---|---|
| 忘记 `io_uring_cqe_seen` / `cq_advance` | CQ 环填满、进入 overflow、不再收完成 | 每个拿到的 CQE 都要"消费" |
| 中断后不清理 CQE | 同上 | 处理好异常路径 |
| 在 prep 之前设自定义 flags | 被 prep 里的清零覆盖 → 行为"神秘消失" | flags 一律写在 prep 之后 |
| 把 `IORING_RECV_MULTISHOT` 当 opcode | 编译/运行都不对 | 它是 `ioprio` 标志位 |
| 误判 `F_MORE` | 挂出多条在飞的 recv，数据重复/乱序 | 有 `F_MORE` 就别补提交 |
| SQ 满了硬取 SQE | 拿到 NULL 直接解引用 → 崩溃 | 拿到 NULL 先 `submit` 再取 |
| 零拷贝发送不等 `F_NOTIF` | 回显内容被改写/花屏 | 严格等第二个 CQE |
| 固定表里 `close(idx)` | 关错 fd / 槽位泄漏 | `register_files_update(idx, {-1})` |
| 提供缓冲区环先 advance 再填条目 | 内核读到半个条目，偶发数据错乱 | 先填条目、最后一次性 advance |
| 每个在飞 I/O 共用一块缓冲区 | 数据互相覆盖（磁盘场景尤其明显） | 每块在飞请求独占缓冲区 |
| SQPOLL 只给一个核 | 比不开还慢 | 给 SQ 线程独立核（`SQ_AFF`） |
| 容器里直接跑 | `io_uring_setup` 返回 EPERM | `docker run --security-opt seccomp=unconfined` |
| 用老 UAPI 头文件硬编新特性 | 结构体重定义编译失败 | 条件编译，或只在支持的环境编译（见 `echo_io_uring_modern.c`） |

> 最后一条是本项目真实踩过的：手写 ABI 常量补丁在 Ubuntu 22.04 上能编，
> 拿到 Ubuntu 26.04（头文件已自带）就报 `redefinition of struct io_uring_buf_ring`。
> 判定宏要用 `IORING_OFF_PBUF_RING`（它和那些结构体同批引入），
> **不能**用 `IOU_PBUF_RING_MMAP`（那是 enum，`#ifdef` 看不见）。

## 8. 调试手段

```bash
# 1) 看提交/收割的系统调用序列（最能说明问题）
strace -f -e trace=io_uring_setup,io_uring_enter,io_uring_register,accept,recvfrom,sendto \
       ./bin/echo_io_uring 19001

# 2) 数每条消息几次系统调用（配合压测端的 QPS 一起算）
sudo perf stat -e syscalls:sys_enter_io_uring_enter -p <PID> -- sleep 6

# 3) 看它到底走了几条内核路径（io-wq 线程是"被 punt 到异步工作线程池"的证据）
ps -o comm= -L -p <PID> | sort | uniq -c          # 出现 iou-wrk-* 说明在走 io-wq
ls /proc/<PID>/task | wc -l

# 4) 找热点（虚拟机无 PMU 时用软件事件）
sudo perf record -e cpu-clock -F 3000 --call-graph fp -p <PID> -- sleep 6
sudo perf report --stdio -percent-limit 1
```

**`iou-wrk-*` 线程是个重要信号**：它意味着内核认为这个操作"不能在这里直接做"，
甩给了异步工作线程池，每次 I/O 多一次跨线程交接。
本项目的磁盘版就是靠这个发现 **O_DIRECT 写被 punt 到 io-wq**，所以写只有 libaio 的一半，
而读（不需要 punt）能和 libaio 持平。

## 9. 学这一套的顺序建议

1. 先用 §5 的朴素版本跑通 echo，用 strace 看清 `io_uring_enter` 的两种形态
   （提交 / `IORING_ENTER_GETEVENTS` 等待）。
2. 加忙轮询，用 `perf stat` 验证 enter 次数下降。
3. 加 SQPOLL，**注意给它独立核**，观察 2x 提升。
4. 加 multishot + 提供缓冲区环，把"每条消息一次提交"变成"每条连接一次提交"。
5. 最后才碰固定文件表和零拷贝 —— 并且**用数据判断它们值不值**（往往是"看着高级但不划算"）。

每一步都能在本项目里找到对应的环境变量开关做对照实验：
`ECHO_SQPOLL` / `ECHO_SPIN` / `ECHO_PBUF` / `ECHO_MULTISHOT` / `ECHO_FIXED` / `ECHO_ZC`。

---

## 附：如何复现本文的数字

```bash
make net
# 基础版（朴素用法）
./bin/echo_io_uring 19001 &
taskset -c 0-3 ./bin/bench_client 19001 16 256

# 现代版（默认就是实测最优：SQPOLL + 忙轮询 + 关零拷贝）
ECHO_SQ_CPU=5 taskset -c 4 ./bin/echo_io_uring_modern 19002 &

# 逐项开关做归因
ECHO_SQ_CPU=5 ECHO_SPIN=0 ./bin/echo_io_uring_modern 19003 &   # 关忙轮询
ECHO_SQ_CPU=5 ECHO_ZC=1   ./bin/echo_io_uring_modern 19004 &   # 开零拷贝
ECHO_SQPOLL=0 ECHO_SPIN=0 ./bin/echo_io_uring_modern 19005 &   # 退化成朴素模式

# 数系统调用
sudo perf stat -e syscalls:sys_enter_io_uring_enter -p <PID> -- sleep 6
```
