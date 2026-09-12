# 深入浅出 libaio 编程

> 本文讲**怎么用 Linux 原生异步 I/O（libaio）写程序**，以及它那几个"必须知道否则一定踩"的限制。
> 所有数字都是本项目在真机上实测的。
>
> 相关文档：[05-io_uring-编程指南.md](./05-io_uring-编程指南.md)（它的继任者）、
> [04-epoll-编程指南.md](./04-epoll-编程指南.md)（Reactor）。
> 本项目实现：`disk/io_libaio.c`（带详细注释）、`disk/io_uring_disk.c`（对照）。
>
> 环境：aarch64 / Ubuntu 22.04（内核 5.15）与 Ubuntu 26.04（内核 7.0），测试盘为 ext4（`/dev/sda1`）。

---

## 1. 一句话定位

libaio 是 Linux 的**原生异步 I/O**：你提交一个「把磁盘某处读进这块内存」的请求，
内核在后台做完，你再去收结果。它是 Proactor 的**早期形态**。

**但它是一个"半成品"，先记住它的三大原罪**，再决定要不要用它：

1. **只对文件/块设备有效，完全不支持 socket** —— 网络 I/O 一点忙都帮不上。
2. **只有 `O_DIRECT` 才是真异步**；buffered I/O 在提交时可能直接阻塞住（等于白用）。
3. **每个请求只能是一个动作**，没有链式、没有超时、没有 multishot，
   而且对 `O_DIRECT` 的对齐要求很严。

所以：**新项目应该直接用 io_uring**。学 libaio 的价值在于 ——
它是理解「异步 I/O 到底靠什么提速」的最干净的教具，而且大量存量系统还在用它。

## 2. 编程模型：四个调用

```
   io_setup(depth, &ctx)          1. 建上下文（内核给你一个"事件环"）
        │
   io_prep_pwrite(&iocb, ...)      2. 描述请求：iocb 就是"请求描述符"
   io_submit(ctx, n, iocbs)        3. 批量提交 n 个请求 —— 不等结果，立刻返回
        │
   io_getevents(ctx, ...)          4. 收割完成事件（可以一次收一批）
        │
   io_destroy(ctx)                 5. 销毁
```

**和 epoll 对照着记**：epoll 是「告诉我哪些 fd 可读」，libaio 是「告诉我哪些请求做完了」。
后者才是 Proactor 语义。

## 3. API 详解

### 3.1 建/销毁上下文

```c
typedef unsigned long io_context_t;

int io_setup(unsigned nr_events, io_context_t *ctx);
int io_destroy(io_context_t ctx);
```

`nr_events` 是**这个上下文能同时容纳的在飞请求数**（内核据此分配事件环）。
本项目里直接取队列深度：

```c
io_context_t ctx = 0;
if (io_setup(depth, &ctx) < 0) { perror("io_setup"); return 1; }
```

**⚠️ 两个坑**：

- `ctx` 必须初始化为 0。它是个"句柄"（内核里的一个 id），传未初始化的垃圾值会拿到奇怪的错误。
- `nr_events` 设小了会怎样？`io_submit` 返回 `-EAGAIN`（"我没容量了"），
  不是崩溃 —— 但你会发现在飞数上不去，吞吐卡住。

实测确认调用形态（本机 7.0，`strace -e trace=io_setup`）：

```
io_setup(8, [0xe87a3b8e8000])           = 0      ← 容量 8
```

### 3.2 描述请求：`struct iocb`

```c
struct iocb {
    void     *data;        /* ★ 应用私有字段：完成时原样回来（相当于 io_uring 的 user_data） */
    unsigned  key;
    short     aio_lio_opcode;   /* IOCB_CMD_PREAD / PWRITE / FSYNC / FDSYNC / POLL ... */
    short     aio_reqprio;
    int       aio_fildes;
    void     *aio_buf;
    size_t    aio_nbytes;
    long long aio_offset;
    long      aio_reserved2;
    unsigned  aio_flags;
    unsigned  aio_resfd;   /* 事件通知用 eventfd（IOCB_FLAG_RESFD） */
};
```

用 `io_prep_*` 帮手填充，别手写字段：

```c
struct iocb cb;
io_prep_pwrite(&cb, fd, buf, 4096, offset);   /* 异步写 */
io_prep_pread (&cb, fd, buf, 4096, offset);   /* 异步读 */
io_prep_fsync(&cb, fd);                       /* 异步 fsync */
io_prep_poll (&cb, fd, POLLIN);               /* 异步 poll */
cb.data = (void *)(long)my_index;             /* ★ 自己的上下文 */
```

**⚠️ 对齐要求（O_DIRECT 下必须遵守）**：偏移、长度、**以及缓冲区地址**都必须是
逻辑块大小（通常 512 或 4096）的整数倍。三者任一不对齐 → `io_submit` 返回 `-EINVAL`。

```c
void *bufs;
posix_memalign(&bufs, 4096, block * depth);   /* 必须用对齐分配，malloc 不够 */
```

### 3.3 提交

```c
int io_submit(io_context_t ctx, long nr, struct iocb **iocbs);
```

- 参数是**指针数组**（`struct iocb **`），所以你需要额外准备一个 `struct iocb *` 数组。
  这是 libaio 比较烦人的地方：得同时维护 iocb 数组和指针数组。
- **返回值可能小于 `nr`**：内核只吃下了一部分（容量不足、或某些请求已被同步完成）。
  **必须处理部分提交** —— 剩下的要重新提交，否则你会一直等永远不会来的事件。

```c
int r = io_submit(ctx, n, cbs_p);
if (r < 0) { /* 全军覆没：-EINVAL(对齐/不支持)、-EAGAIN(容量满) */ }
else if (r < n) { /* ★ 只提交了 r 个，剩下 n-r 个要自己重试 */ }
```

- 提交是**真异步**：立刻返回，不等数据搬完。

实测形态（本机 7.0）：

```
io_submit(0xe87a3b8e8000, 8, [{aio_data=0,   aio_lio_opcode=IOCB_CMD_PWRITE,
                               aio_fildes=3, aio_buf="AAAAAAAA"..., aio_nbytes=4096,
                               aio_offset=0},
                              {aio_data=0x1, ... aio_offset=4096}, ... ]) = 8
```

一次塞 8 个，返回 8 —— **这就是"批量提交"**。

### 3.4 收割

```c
struct io_event {
    __u64 data;     /* 你在 iocb.data 里放的东西 */
    __u64 obj;      /* 完成的那个 iocb 的地址 */
    __s64 res;      /* ★ 结果：≥0 字节数，<0 负 errno */
    __s64 res2;     /* 次要结果（如 fsync 用） */
};

int io_getevents(io_context_t ctx, long min_nr, long nr,
                 struct io_event *events, struct timespec *timeout);
```

语义要点（**这里最容易出错**）：

| 参数 | 语义 |
|---|---|
| `min_nr` | **至少要收到这么多才返回**（否则阻塞等待） |
| `nr` | 一次最多收这么多（`events` 数组的容量） |
| `timeout` | `NULL` = 无限等待；`{0,0}` = 立刻返回（非阻塞轮询） |
| 返回值 | 实际收到几个；`<0` 是负 errno |

**⚠️ `min_nr == nr` 是个陷阱**：如果此时只有 3 个完成而 `min_nr=8`，这一句会**一直等着**，
把这 3 个已经完成的事件也压着不给你 —— 吞吐直接塌掉。

```c
/* ❌ 典型错误：假设总是能一次等到 depth 个 */
int got = io_getevents(ctx, depth, depth, evs, NULL);

/* ✅ 推荐：至少给我 1 个，最多 depth 个 */
int got = io_getevents(ctx, 1, depth, evs, NULL);

/* ✅ 或者：已经知道有 n 个在飞，就等这 n 个 */
int got = io_getevents(ctx, r, r, evs, NULL);   /* r 是刚才 io_submit 成功的个数 */
```

实测的批量收割证据（本机 7.0，一次调用收 8 个完成事件）：

```
io_pgetevents(0xe8a496c76000, 8, 8, [{data=0, res=4096, res2=0},
                                     {data=0x1, res=4096, res2=0},
                                     ... 共 8 条 ... ], NULL, {sigmask=NULL, sigsetsize=8}) = 8
```

**一次系统调用收走 8 个完成事件** —— 这是 libaio 唯一比 epoll 式轮询优雅的地方，
也是它当年存在的意义。

> ⚠️ **架构坑（和 `epoll_pwait` 同类）**：**aarch64 上 `io_getevents` 实际走的是
> `io_pgetevents`**（glibc/libaio 为了 time64 兼容做了重定向）。
> 所以 `strace -e trace=io_getevents` 会**一条都抓不到**（本项目实测计数为 0，一度以为没收到事件）。
> 过滤器写 `io_pgetevents,io_getevents` 才稳。

### 3.5 取消

```c
int io_cancel(io_context_t ctx, struct iocb *iocb, struct io_event *evt);
```

**别指望它**：只在请求"还没开始执行"时能取消成功（`-EINPROGRESS` 表示正在跑，取消不掉）。
生产代码不应该依赖 `io_cancel` 来做超时控制 —— 这也是 libaio 的硬伤之一
（io_uring 有 `IORING_OP_LINK_TIMEOUT` 之类的正规方案）。

## 4. 内核里发生了什么

### 4.1 一个上下文 = 一个"事件环"

`io_setup` 在内核里建一个 `kioctx`，其中有一个**环形缓冲区（aio ring）**
用来把完成事件从内核传给用户态 —— 这点和 io_uring 的 CQ 环思路一致，
区别在于 **libaio 的 ring 是内核分配的、用户态只能通过 `io_getevents` 去读**，
而 io_uring 是整个 SQ/CQ 都 mmap 给用户态直接读写。

这带来一个直接后果：
- io_uring 可以用"忙轮询用户态 CQ"实现**零系统调用收割**；
- libaio **收割必须调 `io_getevents`**（进内核），没有零调用的选项。

### 4.2 提交之后的路径

```
io_submit(ctx, n, iocbs)
  └─ 逐个 aio_read/aio_write
       ├─ 能立刻完成？（page cache 命中） → 直接完成，事件入 ring
       └─ 需要等磁盘 → 交给块层，块层在中断/软中断里完成 → 事件入 ring
```

**关键分叉：buffered vs O_DIRECT**

- **buffered**：如果数据在 page cache 里，`io_submit` **当场就做完并返回**（"假异步"）；
  如果不在，内核可能**在提交路径里同步等磁盘**，把整个 `io_submit` 卡住。
  结果就是：你在自己进程里被阻塞了，异步的意义没了。
- **`O_DIRECT`**：绕过 page cache，请求真正异步地飞出去。**这才是 libaio 的正确用法。**

本项目的实测把这一点暴露得非常直白（4KB，深度 32，同一块盘）：

| 模式 | 写 IOPS | 写带宽 | 说明 |
|---|---|---|---|
| O_DIRECT | 198,905 | 777 MB/s | 真实磁盘能力 |
| buffered | **1,984,205** | **7750 MB/s** | **page cache 假象**（快 10 倍是假的） |

所以：**用 libaio 而不加 `O_DIRECT`，你测的是内存，不是磁盘。**

### 4.3 为什么"深度"是唯一有效的杠杆

异步 I/O 的收益来自**并发深度**：让多个请求同时在飞，等磁盘并行处理。
本项目实测（4KB，`O_DIRECT`，真盘 ext4，总 8192 个 I/O）：

| 深度 | 写 IOPS | 读 IOPS |
|---|---|---|
| 1 | 7,811 | 8,729 |
| 32 | **198,905** | **245,671** |
| 提升 | **25.5x** | **28.1x** |

**同一份代码，只改深度，差 25 倍以上。**
原因很朴素：深度 1 时每个请求都要等一个完整的磁盘往返（约 128µs，正好是 7811 IOPS 的倒数）；
深度 32 时 32 个请求并行等，摊薄了延迟。

对照：**同步方式完全吃不到这个好处**（本项目实测同步方式从深度 1 到 32 几乎没有变化），
因为它一次只能有一个在飞请求。这就是「异步 I/O vs 同步 I/O」的全部差别所在 ——
**不是"异步更快"，而是"异步允许你并发"**。

### 4.4 一个反直觉的实测发现：io_uring 的写反而更慢

同一台机器、同一块盘、同样的 4KB / `O_DIRECT` / 深度 32：

| | 读 IOPS | 写 IOPS |
|---|---|---|
| libaio | ~208k | **213k** |
| io_uring | **239k**（反超） | **103k**（只有一半） |

为什么 io_uring 的写只有 libaio 的一半？用 `ps -o comm= -L -p <PID>` 一看就明白了：

```
$ ps -o comm= -L -p <io_uring 进程>
... iou-wrk-1234  iou-wrk-1235  iou-wrk-1236 ...   ← 17 个 io-wq 工作线程
```

**内核把 `O_DIRECT` 写"甩"给了 io-wq 异步工作线程池**，每次 I/O 多一次跨线程交接。
读走的是快路径（不需要 punt），所以读完全不落后。

**教训**：异步 I/O 的性能瓶颈常常不在 API 本身，而在**内核到底走了哪条实现路径**。
判断手段就是看进程里有没有 `iou-wrk-*` 线程。

> 补充：`O_DIRECT` 写的这份"punt 代价"是内核实现细节，会随版本变化。
> 所以在自己的内核上重测一遍是必要的，别照搬别人的数字。

## 5. 完整示例

完整可运行版本是 **[`examples/libaio_rw.c`](../../examples/libaio_rw.c)**（`make examples`）；
带详细注释的生产向版本是 `disk/io_libaio.c`。骨架如下：

```c
#define MAX_DEPTH 128

int main(void) {
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC | O_DIRECT, 0644);  /* ★ O_DIRECT */
    ftruncate(fd, block * 1024);

    io_context_t ctx = 0;
    if (io_setup(depth, &ctx) < 0) { perror("io_setup"); return 1; }

    struct iocb  *cbs   = calloc(depth, sizeof(struct iocb));
    struct iocb **cbs_p = calloc(depth, sizeof(struct iocb *));        /* io_submit 要指针数组 */
    void *bufs;
    posix_memalign(&bufs, 4096, block * depth);                        /* ★ 对齐分配 */

    long submitted = 0, done = 0;
    while (done < total) {
        int n = 0;
        while (n < depth && submitted < total) {                      /* 填满提交窗口 */
            struct iocb *cb = &cbs[n];
            off_t off = (off_t)((submitted * block) % file_sz);
            io_prep_pwrite(cb, fd, (char *)bufs + (size_t)n * block, block, off);
            cb->data = (void *)(long)submitted;                       /* 自己的上下文 */
            cbs_p[n] = cb;                                            /* ★ 指针数组 */
            n++; submitted++;
        }
        if (n == 0) break;

        int r = io_submit(ctx, n, cbs_p);
        if (r < 0) { fprintf(stderr, "io_submit: %s\n", strerror(-r)); break; }
        /* ★ r 可能 < n，真实代码要把剩下 n-r 个重新提交 */

        struct io_event evs[MAX_DEPTH];
        int got = io_getevents(ctx, r, r, evs, NULL);                 /* 一次收一批 */
        if (got < 0) { fprintf(stderr, "io_getevents: %s\n", strerror(-got)); break; }
        for (int i = 0; i < got; i++)
            if ((long)evs[i].res < 0)
                fprintf(stderr, "I/O 错误: %s\n", strerror(-(int)evs[i].res));
        done += got;
    }
    io_destroy(ctx);
    free(cbs); free(cbs_p); free(bufs);
    close(fd);
}
```

**四处 `★` 是 libaio 的核心要点**：`O_DIRECT`、指针数组、对齐分配、批量收割。

## 6. 陷阱清单

| 陷阱 | 症状 | 正确做法 |
|---|---|---|
| 不加 `O_DIRECT` | 提交路径被阻塞 / 测到的是 page cache | 用 `O_DIRECT`（并接受对齐要求） |
| 缓冲区没对齐 | `io_submit` 返回 `-EINVAL` | `posix_memalign(buf, 4096, ...)` |
| 偏移/长度没按块对齐 | 同上 | 512/4096 对齐 |
| 用同一个缓冲区喂多个在飞请求 | **数据互相覆盖** | 每块在飞 I/O 独占一块缓冲区 |
| `min_nr == nr` 且没那么多完成 | 死等，吞吐塌掉 | `io_getevents(ctx, 1, depth, ...)` |
| 忽略 `io_submit` 的部分提交 | 一直等永远不来的事件 | 检查返回值，重提剩下的 |
| `ctx` 未初始化 | 莫名其妙的错误 | `io_context_t ctx = 0;` |
| 想用它做网络 I/O | 完全用不了 | 网络用 epoll / io_uring |
| 想靠 `io_cancel` 做超时 | 取消失败（已在执行） | 换 io_uring 的 link+timeout |
| 在 tmpfs 上测 | 数字离谱（本项目在 `/tmp` 上测出 10000 MB/s） | 用真实磁盘路径 |
| 在 qcow2/虚拟盘上测绝对值 | 数字不可信（宿主机缓存吸收写入） | 只看相对关系，或上物理盘/直通盘 |

## 7. 和 io_uring 的关系

| 能力 | libaio | io_uring |
|---|---|---|
| 文件/块设备异步 | ✅ | ✅ |
| **网络（socket）异步** | ❌ | ✅ |
| 提交/收割的共享内存 | 只有完成 ring（且要 `io_getevents` 读） | SQ + CQ 全 mmap，可零系统调用 |
| 批量提交 | ✅（一次 `io_submit` n 个） | ✅（一次 enter n 个） |
| 有界批量收割 | ✅（`io_getevents` 一次 n 个） | ✅（`peek_batch_cqe`） |
| 一条请求包含多个动作 | ❌ | ✅（`IOSQE_IO_LINK` 链式） |
| 超时 | ❌ | ✅ |
| multishot | ❌ | ✅ |
| 提供缓冲区 | ❌ | ✅ |
| 零拷贝发送/接收 | ❌ | ✅ |

**结论：新项目用 io_uring；存量 libaio 代码知道怎么读、怎么调优（主要是 `O_DIRECT` + 深度）就够了。**
两者的调优思路是共通的 —— 都是「并发深度换吞吐」，都是「避免在提交路径上同步等待」。

## 8. 调试手段

```bash
# 1) 看提交/收割的形态和批量大小
strace -e trace=io_setup,io_submit,io_pgetevents,io_getevents,io_destroy ./bin/io_libaio

# 2) 数系统调用总量（对比"一次提交几个、一次收割几个"）
strace -c -e trace=io_submit,io_pgetevents ./bin/io_libaio /data/x 4096 8192 32 1

# 3) 看内核有没有把请求甩给工作线程（有 iou-wrk-* / kworker 参与就说明不是快路径）
ps -o comm= -L -p <PID> | sort | uniq -c

# 4) 确认测试文件所在文件系统（tmpfs 上的数字没有意义）
df -T /path/to/testfile
```

## 9. 什么时候还值得考虑 libaio

- **存量系统**：已经用它写好了，且跑在 `O_DIRECT` + 深度的正确配置上，没有换的理由。
- **内核太老**（< 5.1，没有 io_uring）：这是 libaio 唯一"不得不选"的场景。
- **学习**：它是理解「异步 I/O 靠什么提速」的最干净的模型 ——
  只有 4 个调用，没有 mmap、没有 SQ/CQ、没有 multishot，干扰最少。

除此之外，**优先 io_uring**：它能覆盖 libaio 的全部能力，还多了网络支持、
链式请求、超时、multishot、零系统调用收割。

---

## 附：如何复现本文的数字

```bash
make disk

# 复制到 VM 本地盘（不要用 /tmp —— 那可能是 tmpfs）
cp /tmp/x /home/ubuntu/   # 或者直接用 /home/ubuntu/xxx.dat 作为测试文件

# 深度对比（最关键的一组）
./bin/io_libaio /home/ubuntu/t.dat 4096 8192 1  1    # 深度 1
./bin/io_libaio /home/ubuntu/t.dat 4096 8192 32 1    # 深度 32  → 25x 以上
./bin/io_libaio /home/ubuntu/t.dat 4096 8192 32 0    # buffered → 假象

# 和 io_uring 对照（注意观察 iou-wrk-* 线程）
./bin/io_uring_disk /home/ubuntu/t.dat 4096 8192 32 1 &
ps -o comm= -L -p $! | sort | uniq -c

# 磁盘多维矩阵（深度/块大小/O_DIRECT）
python3 scripts/bench_matrix.py disk --mode quick
```
