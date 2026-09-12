# 深入浅出 epoll 编程

> 本文讲**怎么用 epoll 写程序**：API 语义、内核里到底发生了什么、完整示例、以及一堆会咬人的细节。
> 原理性背景（Reactor 模式、select/poll/epoll 对比、epoll 三大数据结构）见
> [01-socket-epoll-深度解析.md](./01-socket-epoll-深度解析.md)；
> 与 io_uring 的对照见 [05-io_uring-编程指南.md](./05-io_uring-编程指南.md)。
>
> 本文所有数字都是在本项目里实测的（环境：aarch64 / 内核 5.15 与 7.0 两台 VM，
> 压测端为 `bin/bench_client`，客户端 16 线程、256B ping-pong）。复现命令见各节末尾。

---

## 1. 一句话定位

epoll 是 Linux 的**就绪通知**机制：它告诉你「哪些 fd 现在可以读/写了」，
**数据在哪、要不要搬、搬多少，都得你自己调 read/write 去处理**。这就是 Reactor 模型。

- 适合：连接数多、每连接流量小的场景（网关、代理、长连接推送、游戏服务器）。
- 不适合：单连接高带宽的场景 —— 那种场景瓶颈在拷贝和内存带宽，epoll 帮不上忙。

## 2. 编程模型：四步循环

epoll 的用法可以压缩成一张图：

```
   ┌─ 1. epoll_create1()  建一个 epoll 实例（一个内核对象，不是一个 fd 数组）
   │
   │  2. epoll_ctl(ADD)    把关心的 fd 注册进去，附带你要监听的事件位
   │
   └─ 3. epoll_wait()      阻塞等待，返回「就绪的 fd 列表」
          │
          └─ 4. 对每个就绪 fd 做非阻塞 read/write（必须能处理 EAGAIN）
                 │
                 └─ 处理完回到 3
```

**关键认知：epoll 只做「通知」。** 第 4 步里的 read/write 才是真正搬数据的系统调用，
也是 epoll 相对 io_uring 劣势的根源 —— 每条消息都要多进几次内核。

### 最小骨架

```c
int epfd = epoll_create1(0);

struct epoll_event ev = { .events = EPOLLIN, .data.fd = lfd };
epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &ev);

struct epoll_event evs[256];
for (;;) {
    int n = epoll_wait(epfd, evs, 256, -1);      // -1 = 永久阻塞
    for (int i = 0; i < n; i++) {
        int fd = evs[i].data.fd;
        /* 根据 fd 是谁、evs[i].events 里有哪些位，分派处理 */
    }
}
```

## 3. API 逐个过

### 3.1 `epoll_create1(int flags)`

创建一个 epoll 实例。`flags` 只认一个值：`EPOLL_CLOEXEC`（**强烈建议加上**，
否则 exec 出去的子进程会继承这个 fd）。返回的 fd 是这个实例的句柄，
**不用时 `close()` 它**（内核会顺带释放整个实例里注册的所有 epitem）。

```c
int epfd = epoll_create1(EPOLL_CLOEXEC);
```

> 老接口 `epoll_create(size)` 的 `size` 参数早已被忽略（内核 2.6.8 起），
> 新代码一律用 `epoll_create1`。

### 3.2 `epoll_ctl(int epfd, int op, int fd, struct epoll_event *ev)`

| op | 语义 | 注意 |
|---|---|---|
| `EPOLL_CTL_ADD` | 注册 | fd 已注册过会返回 `EEXIST` |
| `EPOLL_CTL_MOD` | 改关注的事件 | 必须已注册，否则 `ENOENT` |
| `EPOLL_CTL_DEL` | 注销 | 内核 ≥2.6.9 允许 `ev == NULL` |

`struct epoll_event` 的构造要点：

```c
struct epoll_event ev;
ev.events   = EPOLLIN | EPOLLET;   // 关注什么 + 触发方式
ev.data.fd  = cfd;                 // 或者 ev.data.ptr = my_conn;  <-- 更常用
epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &ev);
```

`data` 是个 union（`ptr` / `fd` / `u32` / `u64`），内核**原样保管、就绪时原样返回**。
真实项目几乎都用 `data.ptr` 指向自己的连接对象 —— 这样 epoll_wait 一返回
就直接拿到上下文，不用再用 fd 去查表：

```c
typedef struct conn { int fd; char buf[4096]; size_t len; void *user; } conn_t;
ev.data.ptr = conn;                 // 注册时
...
conn_t *c = evs[i].data.ptr;        // 就绪时，零查找成本
```

**支持的事件位（常用）**：

| 事件位 | 含义 | 备注 |
|---|---|---|
| `EPOLLIN` | 可读（含对端关闭 → read 返回 0） | |
| `EPOLLOUT` | 可写 | 别一直关注它，会让 epoll 一直唤醒你 |
| `EPOLLRDHUP` | 对端关闭了写方向 | ≥2.6.17，比等 read 返回 0 更早 |
| `EPOLLERR` | 出错 | **不需要注册也会上报** |
| `EPOLLHUP` | 挂断 | **不需要注册也会上报** |
| `EPOLLET` | 边沿触发 | 见 3.5 |
| `EPOLLONESHOT` | 只报一次 | 需 `MOD` 重新武装 |
| `EPOLLEXCLUSIVE` | 多个 epoll 实例共享一个 fd 时，只唤醒一个 | ≥4.5，解决 accept 惊群 |

### 3.3 `epoll_wait(int epfd, struct epoll_event *evs, int maxevents, int timeout)`

- 返回：就绪的 fd 个数（可能 0 = 超时），-1 = 出错。
- `maxevents` 必须 > 0；**返回值可能小于 maxevents**（内核一次最多给你这么多）。
- `timeout`：-1 永久阻塞，0 立即返回（纯轮询），>0 毫秒。
- 被信号打断返回 -1 且 `errno == EINTR`，**必须自己重试**（本项目所有实现都处理了这点）。
- 有多个线程同时 `epoll_wait` 同一个 epfd 时，**所有线程都会被唤醒**（除非用 `EPOLLEXCLUSIVE`）——
  这是「惊群」。所以多线程 Reactor 的常见做法不是共享 epfd，而是每个 worker 一个
  epoll + `SO_REUSEPORT` 分流（见本项目 `network/echo_epoll_mt.c`）。

### 3.4 水平触发（LT）与边沿触发（ET）

| | LT（默认） | ET（`EPOLLET`） |
|---|---|---|
| 通知时机 | **只要还可读就一直通知** | **只在"从不可读变成可读"这一刻通知一次** |
| 编程要求 | 每次通知读一次也行（下次还会通知） | **必须循环读到 `EAGAIN`**，否则剩下的数据永远收不到通知 |
| 编程难度 | 低 | 高，但只要遵守"读空"就等价于 LT 且更高效 |
| 丢事件风险 | 无 | 不读空就会丢 |

**ET 的代价与收益**，实测（本项目 `echo_epoll_mt.c`，可切 LT/ET）：

```bash
ECHO_LT=1 taskset -c 4 ./bin/echo_epoll_mt 9000 4    # LT
ECHO_LT=0 taskset -c 4 ./bin/echo_epoll_mt 9000 4    # ET
```

在本项目的 ping-pong 微基准里**两者差距很小**（每条连接只有一个在途请求，读一次就空了）。
ET 的真正价值在**批量场景**：一次通知后把缓冲区里的数据全部取走，
减少 epoll_wait 的调用次数。所以「ET 更快」不是普适结论，要看 workload。

**ET 下的硬性纪律（踩过就懂）**：

```c
/* ❌ 错：ET 下只读一次，剩下的数据再也不会通知你 */
int n = read(fd, buf, sizeof(buf));

/* ✅ 对：读到 EAGAIN 为止 */
for (;;) {
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n > 0)       { /* 处理 */ continue; }
    if (n == 0)      { /* 对端关闭 */ break; }
    if (errno == EAGAIN || errno == EWOULDBLOCK) break;   // 读干净了
    if (errno == EINTR) continue;
    /* 真错误 */ break;
}
```

### 3.5 `EPOLLONESHOT`

报一次事件后自动「解除武装」，之后需要 `EPOLL_CTL_MOD` 重新注册才会再报。
用途：把同一个连接的后续处理**移交给另一个线程**时，避免两个线程同时处理同一个 fd。

## 4. 内核里到底发生了什么

理解这一段，很多「为什么这么用」的规则就自然成立了。

### 4.1 一个 epoll 实例 = 三样东西

```c
struct eventpoll {
    struct rb_root  rbr;      // 红黑树：所有注册进来的 fd（epitem）
    struct list_head rdllist; // 就绪链表：当前"有事发生"的 epitem
    wait_queue_head_t wq;     // 等待队列：谁在 epoll_wait 上睡着
    ...
};
```

- **红黑树 `rbr`**：`epoll_ctl(ADD)` 插入一个 `epitem`（O(log n)），
  所以注册/注销的成本与连接数无关，也不会像 select/poll 那样每次调用都要重传 fd 集合。
- **就绪链表 `rdllist`**：`epoll_wait` 只需要看这个链表，**与总连接数无关** ——
  这就是「epoll 的 O(1) 就绪判定」的真实含义（不是"什么都 O(1)"）。
- **等待队列 `wq`**：`epoll_wait` 没有就绪项时睡在这里。

### 4.2 事件是怎么被"推"上来的：回调，而不是轮询

注册时内核会做一件关键的事：给目标 fd 的等待队列挂上一个**回调**（`ep_poll_callback`）。

```
epoll_ctl(ADD)
  └─ ep_insert()
       ├─ 建 epitem，插进 rbr
       └─ 调用目标 file 的 ->poll()，并传入一个特殊的等待队列项
            （ep_ptable_queue_proc：把 ep_poll_callback 挂到该 fd 的等待队列上）
```

之后**数据到达时**（网卡中断 → 协议栈 → `sock_def_readable`），
内核会唤醒该 socket 等待队列上的回调 → `ep_poll_callback`：

```
ep_poll_callback(epitem)
  ├─ 把这个 epitem 挂到 rdllist 上
  └─ 唤醒 eventpoll->wq 上的等待者（即正在 epoll_wait 的进程）
       └─ 进程被唤醒后从 rdllist 取走事件
```

**这一步是整个 epoll 的性能来源**：不需要遍历所有 fd 去问「你可读了吗」，
而是**数据到的那一瞬间由内核回调着把事件塞进就绪链表**。

### 4.3 LT 和 ET 在内核里的差别

`epoll_wait` 内部大致是：`ep_poll()` → 有就绪项则 `ep_send_events()`，
后者对 rdllist 上的每个 epitem 再调一次 `->poll()` **复核**（因为从"被唤醒"到你真正处理之间
状态可能已经变了），然后：

- **LT**：如果复核后仍然可读，就把这个 epitem **重新挂回 rdllist** →
  下次 `epoll_wait` 还会报它。这就是「一直通知」的实现方式。
- **ET**：不重新挂回。只有当**新的数据到达**、回调再次触发时才会重新入队 →
  只在"边沿"上报一次。

所以：
- LT 下「读一次就走」是安全的（会被重新入队再报一次），代价是**多一次 epoll_wait 往返**。
- ET 下「读一次就走」会**丢事件**：数据还在缓冲区里，但没有新的边沿，回调不会再触发。

### 4.4 `EPOLLHUP` / `EPOLLERR` 为什么总会上报

因为它们是"文件状态"而不是"关注的事件位"。内核在 `ep_send_events` 里会把这两个位
**无条件或进 revents**。所以：

```c
/* 只看 EPOLLIN 会漏掉"对端 RST"这类事件 */
if (!(evs[i].events & EPOLLIN)) continue;      // ❌ 可能漏掉 ERR/HUP

/* ✅ 更稳的写法：错误和挂断无论如何都要处理 */
if (evs[i].events & (EPOLLERR | EPOLLHUP)) { close_conn(fd); continue; }
if (evs[i].events & EPOLLIN) { ... }
```

本项目 `echo_epoll_mt.c` 里只处理了 `EPOLLIN`，这是**刻意保留的简化**：
真错误会在下一次 `read` 时以返回值形式暴露出来。生产代码建议按上面的写。

## 5. 一个完整的、教科书式正确的例子

下面是精简版；**完整可运行版本是 [`examples/epoll_echo.c`](../../examples/epoll_echo.c)**
（`make examples` 可编译，本文档写完后已在真机上跑过功能校验）。要点全在这里：

```c
#define MAX_CONN 1024
#define BUF_SZ   4096
static char g_buf[MAX_CONN][BUF_SZ];       // 下标即 fd：省掉 malloc，代价是常驻 4MB

static void set_nonblock(int fd) {
    int f = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, f | O_NONBLOCK);
}

/* 循环写完，处理"部分写"与 EAGAIN —— 生产代码必做，否则会发出残缺数据 */
static int write_all(int fd, const char *p, int len) {
    int off = 0, retry = 0;
    while (off < len) {
        ssize_t n = write(fd, p + off, len - off);
        if (n > 0) { off += (int)n; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (++retry > 100) return -1;
            struct pollfd pfd = { .fd = fd, .events = POLLOUT };
            poll(&pfd, 1, 100);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return -1;
    }
    return 0;
}

int main(void) {
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    set_nonblock(lfd);                       /* ★ 监听 fd 也要非阻塞 */
    bind(lfd, ...); listen(lfd, 512);

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = lfd };
    epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &ev);

    struct epoll_event evs[256];
    for (;;) {
        int n = epoll_wait(epfd, evs, 256, -1);
        if (n < 0) { if (errno == EINTR) continue; break; }

        for (int i = 0; i < n; i++) {
            int fd = evs[i].data.fd;

            if (fd == lfd) {
                for (;;) {                   /* ★ accept 也要循环取空 */
                    int cfd = accept(lfd, NULL, NULL);
                    if (cfd < 0) break;      /* EAGAIN 或错误 */
                    set_nonblock(cfd);       /* ★ 连接 fd 必须非阻塞 */
                    ev.events = EPOLLIN | EPOLLET;
                    ev.data.fd = cfd;
                    if (epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &ev) < 0) close(cfd);
                }
                continue;
            }

            int closed = 0;
            for (;;) {                       /* ★ ET：读到 EAGAIN */
                ssize_t r = read(fd, g_buf[fd], BUF_SZ);
                if (r > 0) { if (write_all(fd, g_buf[fd], (int)r) < 0) { closed = 1; break; } }
                else if (r == 0) { closed = 1; break; }
                else {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    if (errno == EINTR) continue;
                    closed = 1; break;
                }
            }
            if (closed) { epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL); close(fd); }
        }
    }
}
```

四处 `★` 是四个「不做就出 bug」的地方。

## 6. 陷阱清单（每条都在本项目里出现过或验证过）

| 陷阱 | 症状 | 正确做法 |
|---|---|---|
| ET 下不读空 | 连接卡住，数据"丢"了 | `while(read) 直到 EAGAIN` |
| 连接 fd 忘记设非阻塞 | **整个服务停摆**（单线程里 read 阻塞 = 所有连接饿死） | `set_nonblock(cfd)`；见 `disk/epoll_starve_demo.c` 实测 |
| 监听 fd 忘记设非阻塞 | 多进程/多线程抢 accept 时卡住 | 一起设上 |
| accept 只接一个 | 连接积压，迟迟不被接受 | 循环 `accept` 到 `EAGAIN` |
| 忽略「部分写」 | 客户端收到残缺数据（且 TCP 是字节流，它察觉不到） | `write_all` |
| 只判断 `EPOLLIN` | 漏掉 `EPOLLERR/EPOLLHUP`，fd 泄漏 | 先处理 ERR/HUP |
| 不处理 `EINTR` | 被信号打断后服务静默退出 | `continue` 重试 |
| fd 复用后 `data.ptr` 失效 | 野指针 / 处理错连接 | 关连接时同时清理自己的上下文 |
| 用同一个 epfd 给多线程 | 惊群、所有线程被唤醒 | 每 worker 一个 epfd + `SO_REUSEPORT` |
| 一直关注 `EPOLLOUT` | 空转（可写是常态，会被反复唤醒） | 只在真的写不出去时临时关注，写完摘掉 |
| 忘记 `EPOLL_CLOEXEC` | exec 后子进程继承无用的 epfd | `epoll_create1(EPOLL_CLOEXEC)` |

两个和本项目直接相关的实测：

- `disk/epoll_nonblock_demo.c`：阻塞 fd + ET 会让进程**睡死在最后一次 read 里**
  （内置 4 秒看门狗，阻塞模式 4.2s 后报【卡死】，非阻塞 0.36s 正常完成）。
- `disk/epoll_starve_demo.c`：阻塞 fd 会**饿死其他连接** ——
  第 1 个连接的 read 挂住 3027ms，第 2 个连接的数据明明已经到了，却只能等到 3384ms 才被处理。

## 7. 性能要点（带本项目实测）

### 7.1 每条消息的系统调用次数

用 `strace -c -f` 数固定 2000 条消息（本项目实测）：

| 服务端 | read | write | epoll_wait/pwait | 合计/条 |
|---|---|---|---|---|
| epoll 单线程 | 2002 | 2001 | 2002 | **3.0** |

**这是 epoll 的天花板所在**：每条消息至少要 read + write + 一次 epoll_wait 复核。

> ⚠️ **架构坑**：aarch64/riscv64 上**没有 `epoll_wait` 这个系统调用**，
> glibc 的 `epoll_wait()` 走的是 `epoll_pwait`。所以
> `strace -e trace=epoll_wait` 在 ARM 上**一条都抓不到**（我第一次就少统计了 2000 次）。
> 过滤器写 `epoll_pwait,epoll_wait` 最稳。

### 7.2 吞吐与扩展性

| 配置 | 核数 | QPS（256B ping-pong） |
|---|---|---|
| epoll 单线程 | 1 | **107k ~ 112k** |
| epoll 多 Reactor（4 worker，`SO_REUSEPORT`） | 4 | 22k →（64 连接时）**相对单线程 1.65x** |
| epoll 单线程（64 连接 Python 压测端） | 1 | 11~13k（压测端先饱和，不可用） |

**单线程 epoll 能打到 ~11 万 QPS（回环、256B）**，而且这还是有 GIL 的对照下用无 GIL 的
C++ 压测端测出来的。要再高就必须多核 —— 而多核的正确姿势是
**每 worker 独立 listen fd + `SO_REUSEPORT`**（内核按四元组哈希分流，免锁免惊群），
不是共享一个 epfd（那会惊群）。

### 7.3 延迟

同一批压测里，epoll 单线程的 p99 在 64 连接时是 **20000µs（20ms）** —— 明显高于
io_uring 的 3.7~5.8ms。原因不是 epoll 慢，而是**单核跑满后排队**：
epoll 单线程 64 连接只有 ~11k QPS，而 io_uring 能到 89k，排队长度差一个量级，
尾部延迟自然差一个量级。**读延迟数字前先看吞吐是否可比。**

## 8. 调试手段

```bash
# 1) 看系统调用序列（注意 ARM 上的 epoll_pwait）
strace -f -e trace=epoll_pwait,epoll_wait,epoll_ctl,accept,accept4,read,write \
       -o /tmp/ep.trace ./bin/echo_epoll 19000 &

# 2) 数系统调用总量（最有说服力的对比手段）
strace -c -f -e trace=epoll_pwait,read,write ./bin/echo_epoll 19000

# 3) 用 perf 找热点（虚拟机常常没有 PMU，用软件事件）
sudo perf record -e cpu-clock -F 3000 --call-graph fp -p <PID> -- sleep 6
sudo perf report --stdio --percent-limit 1
```

本项目实测的一份真实序列（`network/strace_epoll实测.txt`）：

```
epoll_ctl(4, EPOLL_CTL_ADD, 3, {events=EPOLLIN}) = 0
epoll_wait(4, [{events=EPOLLIN, data={u32=3}}], 64, -1) = 1     ← 监听 fd 可读
accept(3, NULL, NULL)  = 5
epoll_ctl(4, EPOLL_CTL_ADD, 5, {events=EPOLLIN|EPOLLET}) = 0
epoll_wait(4, [{events=EPOLLIN, data={u32=5}}], 64, -1) = 1     ← 连接可读
read(5, "X", 4096)     = 1                                      ← 同步搬数据
write(5, "X", 1)       = 1                                      ← 同步搬数据
epoll_wait(4, [{events=EPOLLIN, data={u32=5}}], 64, -1) = 1     ← 对端关闭也能读
read(5, "", 4096)      = 0
epoll_ctl(4, EPOLL_CTL_DEL, 5, NULL) = 0
```

一眼就能看出 Reactor 的特征：**每次读/写都伴随一次 epoll_wait 与一次 read/write**。

## 9. 什么时候不该用 epoll

- **要极致吞吐**：看 [05-io_uring-编程指南.md](./05-io_uring-编程指南.md) ——
  现代 io_uring 在同机同 workload 下能到 **2.08x epoll**。
- **单连接高带宽**：瓶颈在拷贝/内存带宽，epoll 与 io_uring 差别不大，
  更该考虑 `sendfile`/`splice` 这类零拷贝手段。
- **磁盘 I/O**：epoll **管不了普通文件** —— 文件永远"可读"（没有就绪概念），
  拿 epoll 去等文件就绪会立刻返回然后阻塞在 read 上。磁盘要用
  [06-libaio-编程指南.md](./06-libaio-编程指南.md) 或 io_uring。

---

## 附：如何复现本文的数字

```bash
make net
# 单线程 vs 多线程 Reactor（客户端绑核 0-3，服务端绑核 4）
taskset -c 4 ./bin/echo_epoll      19000 &
taskset -c 0-3 ./bin/bench_client 19000 16 256        # 看每秒的 QPS 行

taskset -c 4-7 ./bin/echo_epoll_mt 19001 4 &
taskset -c 0-3 ./bin/bench_client 19001 16 256

# ET vs LT
ECHO_LT=1 ./bin/echo_epoll_mt 19002 4    # LT
ECHO_LT=0 ./bin/echo_epoll_mt 19003 4    # ET

# 矩阵（扫连接数/消息大小/TCP_NODELAY）
python3 scripts/bench_matrix.py net --client cpp
```
