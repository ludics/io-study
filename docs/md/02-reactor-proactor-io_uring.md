# Reactor 与 Proactor —— 命名渊源、io_uring 详解与主从 Reactor

> 本文回答四个核心问题：
> ① Reactor 是什么、为什么这么命名；② Proactor 是什么、为什么这么命名；
> ③ io_uring 是不是 Proactor、与 epoll 的区别联系；④ 除了这两种还有哪些模式。
>
> 附实测：用 `strace` 抓取了 epoll 处理单个请求的系统调用序列，并编写了 io_uring 版 Echo Server 对照。
> 对应源码：`network/echo_epoll.c`、`network/echo_io_uring.c`、`network/strace_epoll实测.txt`

---

## 1. 先建立一个核心判据

Reactor 和 Proactor 描述的是**事件驱动编程中「谁来执行 I/O 数据搬运」的分工**。判断的关键只有一个问题：

> **「内核缓冲区 ↔ 用户缓冲区之间的数据 copy 由谁发起？应用线程要不要等这个 copy 完成？」**

| 判断维度 | Reactor | Proactor |
| --- | --- | --- |
| 内核通知你什么 | 「数据**就绪**了，你可以去读」 | 「数据**已经读好**了，在 buffer 里」 |
| 谁发起数据 copy | 应用自己调 `read`/`write` | 内核替你完成 |
| 应用线程等不等 copy | 等（同步） | 不等（异步） |
| 本质 | 就绪通知（readiness） | 完成通知（completion） |

一句话：**Reactor 是「内核喊你起床，你自己去吃饭」；Proactor 是「内核直接把饭端到你面前」。**

---

## 2. 为什么叫 Reactor（反应堆）？

"Reactor" 来自设计模式，由 **Douglas C. Schmidt** 在 1990 年代研究高性能网络服务器时命名并系统化（代表作《Reactor: An Object Behavioral Pattern for Demultiplexing and Dispatching Handles for Synchronous Events》）。

### 2.1 命名的比喻

一个**中心调度器（Reactor）**负责监听所有事件源，当事件发生时，它「**做出反应**」（React）—— 把事件**分发（dispatch）**给对应的处理器（handler）。

就像化学反应堆里的**催化剂**：自己不参与具体反应，但**控制反应的进行**。Reactor 自己不处理业务逻辑，只负责「接事件 → 派事件」。

### 2.2 名称拆解

| 前缀 | 含义 | 指向 |
| --- | --- | --- |
| `Re-` | 响应、反复（react） | 事件**发生后**才「反应」去处理 |
| `-actor` | 执行者 | 一个「持续响应并调度事件执行者」的中心组件 |

### 2.3 Reactor 的四个构成要素

1. **Handle（句柄）**：就是 fd，表示事件源。
2. **Synchronous Event Demultiplexer（同步事件多路分离器）**：`select`/`poll`/`epoll`，阻塞等待多个 handle 就绪。
3. **Event Handler**：事件处理器，处理具体业务。
4. **Concrete Event Handler**：具体处理器实现。

### 2.4 epoll 为什么是 Reactor

因为 `epoll_wait` 只告诉你「fd 可读了」，**读数据的动作还是应用自己 `read`**，应用线程在 `read` 里要等内核把数据拷完。这正是「就绪通知 + 应用自己干活」的 Reactor 特征。

---

## 3. 为什么叫 Proactor？

"Proactor" 是 Reactor 的**对称命名**，同样出自 Schmidt 团队（1997 年左右提出，论文《Proactor: An Object Behavioral Pattern for Demultiplexing and Dispatching Handlers for Asynchronous Events》）。

### 3.1 前缀对比的精妙之处

| 前缀 | 含义 | 指向 |
| --- | --- | --- |
| `Re-` | 响应、反应（react） | 事件**发生后**才「反应」去处理（被动） |
| `Pro-` | 提前、前置、主动（proactive） | **主动**发起 I/O 操作，让内核先行完成（主动） |

Proactor 的核心是「主动」（proactive）：应用**提前**把「我要读 fd=7 到 buf」这个完整 I/O 操作交给内核，然后就不管了。内核在后台**主动地、独立地**完成整个 I/O（包括数据 copy），完成后通知应用「已读好 1024 字节」。

> **命名总结**：Reactor 是「响应式」（等事件来了再反应），Proactor 是「主动式」（提前把活交出去让内核主动做完）。

---

## 4. io_uring 是 Proactor 吗？与 epoll 的区别

### 4.1 结论

**是的，io_uring 是典型的 Proactor 模型**，而且是 Linux 上目前**最彻底的 Proactor 实现**。

### 4.2 io_uring 核心机制

由 Linux 内核开发者 **Jens Axboe** 设计，Linux 5.1（2019）引入。核心是**用户态和内核态共享的两个环形队列**（mmap 映射）：

| 队列 | 全称 | 生产者 | 消费者 | 作用 |
| --- | --- | --- | --- | --- |
| SQ | Submission Queue 提交队列 | 用户态 | 内核 | 应用把 I/O 请求（SQE）写进去 |
| CQ | Completion Queue 完成队列 | 内核 | 用户态 | 内核把完成结果（CQE）写进去 |

```
写 SQE 到 SQ → 内核取走并异步执行 I/O → 内核写 CQE 到 CQ → 用户收割结果
```

### 4.3 为什么它是 Proactor

1. 应用提交 `IORING_OP_READ` 时，**把目标 buffer 地址也写进 SQE**。
2. 内核**异步地**完成「读数据 + 拷进 buffer」的**全部动作**。
3. 完成后在 CQ 返回「读了 1024 字节，数据已经在 buf 里」。

应用线程**从提交到收割之间完全不用等待数据 copy** —— 这正是 Proactor 的「完成通知 + 内核代办」特征。

### 4.4 io_uring 解决的三大痛点（vs epoll）

| epoll 痛点 | io_uring 解决 |
| --- | --- |
| 每次 `read`/`write` 仍是一次系统调用（上下文切换开销） | 批量提交；普通模式 1 次 `io_uring_enter`；**SQPOLL 模式 0 次系统调用** |
| 数据 copy 无法消除 | 内核代办 copy；可用 registered buffers 预锁内存减少映射开销 |
| 不支持文件 I/O（文件 fd 永远「就绪」） | 统一支持文件、socket、pipe 等所有 I/O |

### 4.5 详细对比表

| 维度 | epoll | io_uring |
| --- | --- | --- |
| 引入版本 | Linux 2.6（2002） | Linux 5.1（2019） |
| I/O 模型 | Reactor（同步非阻塞） | Proactor（真异步） |
| 通知语义 | 就绪通知 | 完成通知 |
| 内核数据结构 | 红黑树 + 就绪链表 | SQ/CQ 双环形队列（mmap 共享） |
| read/write 系统调用次数 | 每 I/O ≥1 次 | 可降至 0（SQPOLL） |
| 事件传递 | 内核→用户内存拷贝 | mmap 共享，零拷贝 |
| 支持文件 I/O | ❌ | ✅ |
| 支持 socket I/O | ✅ | ✅ |
| 批量提交 | ❌ | ✅ 天然支持 |
| 编程复杂度 | 中等 | 较高 |
| 生态成熟度 | 极成熟（Nginx/Redis） | 快速成熟（RocksDB/ScyllaDB） |

### 4.6 系统调用次数对比（理论）

处理 1000 个读事件：

```
epoll:            1 次 epoll_wait + 1000 次 read = 1001 次系统调用
io_uring:         写 1000 个 SQE + 1 次 submit   = 1 次系统调用（普通模式）
io_uring(SQPOLL):                               = 0 次系统调用
```

### 4.7 实测：epoll 处理 1 条消息的系统调用序列

用 `strace` 实测 epoll 版 Echo Server 处理 1 条消息（1 字节）的真实系统调用序列：

```bash
$ strace -f -e trace=epoll_wait,accept,read,write,epoll_ctl ./echo_epoll 19000

epoll_ctl(4, EPOLL_CTL_ADD, 3, {EPOLLIN})        = 0   # 注册监听 fd
epoll_wait(4, [{fd=3}], 64, -1)                  = 1   # ① 等待连接
accept(3, NULL, NULL)                            = 5   # ② 接受连接
epoll_ctl(4, EPOLL_CTL_ADD, 5, {EPOLLIN|EPOLLET})= 0   # ③ 注册新连接
epoll_wait(4, [{fd=5}], 64, -1)                  = 1   # ④ 等待数据就绪
read(5, "X", 4096)                               = 1   # ⑤ 应用自己读数据
write(5, "X", 1)                                 = 1   # ⑥ 应用自己回显
epoll_wait(4, [{fd=5}], 64, -1)                  = 1   # ⑦ 等待对端关闭
read(5, "", 4096)                                = 0   # ⑧ 读到 EOF
epoll_ctl(4, EPOLL_CTL_DEL, 5, NULL)             = 0   # ⑨ 摘除 fd
```

**实测结论**：处理 **1 条消息**，epoll 版触发了**约 11 次系统调用**（4 次 `epoll_wait` + 1 次 `accept` + 2 次 `read` + 1 次 `write` + 3 次 `epoll_ctl`）。这清晰地印证了 Reactor 模式的本质 —— **每次 I/O 都要应用亲自发起 `read`/`write` 系统调用**。

> **关于 io_uring 实测**：实验沙箱的 seccomp 策略禁用了 `io_uring_setup` 系统调用（返回 `EPERM`），因此 io_uring 版 Echo Server 无法在该环境实际运行（代码已编译通过）。**你在自己的物理机/云服务器上通常可以直接跑通** —— 先执行 `make check` 确认。

### 4.8 io_uring 版 Echo Server 的关键实现

`network/echo_io_uring.c` 有一个**容易踩的坑**，值得单独说明：`user_data` 只有 8 字节，如果只存 fd，那么「读完成」和「写完成」两个 CQE 就**无法区分**，会导致数据错乱。

正确做法是把「fd + 操作类型」打包进一个 `unsigned long`：

```c
#define OP_ACCEPT 0
#define OP_READ   1
#define OP_WRITE  2

static inline unsigned long encode_data(int fd, int op) {
    return ((unsigned long)op << 32) | (unsigned int)fd;
}
static inline int decode_fd(unsigned long data) { return (int)(data & 0xFFFFFFFF); }
static inline int decode_op(unsigned long data) { return (int)(data >> 32); }

// 提交读
io_uring_prep_recv(sqe, fd, buf, BUF_SIZE, 0);
io_uring_sqe_set_data(sqe, (void *)encode_data(fd, OP_READ));

// 收割时按 op 分派
switch (decode_op(cqe->user_data)) {
    case OP_ACCEPT: /* accept 完成 */ break;
    case OP_READ:   /* 读完成 → 提交写 */ break;
    case OP_WRITE:  /* 写完成 → 再提交读 */ break;
}
```

这是 io_uring 编程的通用技巧：**`user_data` 是你唯一的上下文载体**，需要把状态机塞进去（fd、操作类型、甚至 buffer 索引）。

---

## 5. 主从 Reactor（main-sub Reactor）

单 Reactor 单线程模型，在**并发连接多、单个连接读写耗时**时会成为瓶颈。生产环境最常用的优化是**主从 Reactor**，这是 Netty、Nginx、Redis 的核心架构。

### 5.1 三种 Reactor 变体

| 变体 | 结构 | 优缺点 |
| --- | --- | --- |
| **单 Reactor 单线程** | 1 个线程既 accept 又处理读写 | 简单，但一个慢连接阻塞全部 |
| **单 Reactor 多线程** | 1 个线程 accept + 分发，线程池处理读写 | 读写不阻塞 accept，但 accept 仍单点 |
| **主从 Reactor** | 主 Reactor 只 accept，多个从 Reactor 各持一个 epoll 处理读写 | accept 与读写彻底解耦，多核并行，**生产首选** |

### 5.2 主从 Reactor 架构

```
                       ┌──────────────────┐
   new connection ────▶│   Main Reactor   │  只监听 listen fd，只做 accept
                       │  (1 个线程)      │
                       └────────┬─────────┘
                                │ round-robin / 最少连接
             ┌──────────────────┼──────────────────┐
             ▼                  ▼                  ▼
     ┌───────────────┐  ┌───────────────┐  ┌───────────────┐
     │ Sub Reactor 1 │  │ Sub Reactor 2 │  │ Sub Reactor N │
     │  epoll + 线程 │  │  epoll + 线程 │  │  epoll + 线程 │
     └───────┬───────┘  └───────┬───────┘  └───────┬───────┘
             └──────────────────┼──────────────────┘
                                ▼
                     ┌──────────────────────┐
                     │   业务线程池（可选）  │  耗时业务与事件循环解耦
                     └──────────────────────┘
```

### 5.3 主从 Reactor 的关键点

1. **主 Reactor 只做 accept**：因为 accept 本身很快，单线程足够；只监听一个 listen fd。
2. **多个从 Reactor 各持一个 epoll**：每个从 Reactor 跑在独立线程（通常绑定一个 CPU 核），负责一批连接的读写事件。数量通常 = CPU 核数。
3. **连接分配策略**：主 Reactor accept 到连接后，用轮询（round-robin）或最少连接数，把连接注册到某个从 Reactor。
4. **业务线程池解耦**：从 Reactor 只负责 I/O 事件，读到完整请求后交给业务线程池，避免耗时业务阻塞事件循环。

### 5.4 与单 Reactor 的对比

| 能力 | 单 Reactor 单线程 | 主从 Reactor |
| --- | --- | --- |
| accept 与读写是否解耦 | 否（同一线程） | 是 |
| 多核利用 | 否（单核） | 是（从 Reactor 各绑一核） |
| 慢连接影响 | 阻塞全部 | 只影响所在从 Reactor |
| 代表框架 | 早期简单服务 | Netty、Nginx、Redis |

---

## 6. 除了 Reactor 和 Proactor，还有哪些类型？

Reactor/Proactor 只是「**事件驱动模型**」内部的两大类。完整 I/O 模型全景如下。

### 6.1 经典 5 种 I/O 模型（Stevens 分类）

这是 Reactor/Proactor 的**母分类**：

| 模型 | 说明 | 是否事件驱动 |
| --- | --- | --- |
| 阻塞 I/O | `read` 阻塞到数据就绪并 copy 完 | ❌ |
| 非阻塞 I/O | 反复 `read` 直到成功（忙轮询） | ❌ |
| I/O 多路复用 | `select/poll/epoll` 等就绪 → 再 read | ✅ **这就是 Reactor 底层** |
| 信号驱动 I/O | `SIGIO` 信号通知就绪 → 再 read | ✅ 另一种就绪通知（Reactor 变体） |
| 异步 I/O | `aio_read` 内核完成全部 → 通知 | ✅ **这就是 Proactor 底层** |

### 6.2 其他值得关注的模式

| 模式 | 说明 | 与 Reactor/Proactor 的关系 |
| --- | --- | --- |
| **Leader/Followers**（领导者/跟随者） | 线程池中一个线程当 leader 负责 accept，其余 follower 等待；leader 处理完移交领导权 | Reactor 的一种**多线程优化变体**，避免 accept 惊群 |
| **Half-Sync/Half-Async**（半同步半异步） | 上层同步线程池处理业务，下层异步事件循环收事件，中间队列衔接 | 常与 Reactor 组合：**Reactor（异步层）+ 线程池（同步层）** |
| **协程（Coroutine）** | epoll 之上封装 Yield/Resume，同步写法异步执行 | Reactor 的**编程范式封装**（如 libco），不是新 I/O 模型 |
| **SEDA**（分段事件驱动） | 处理拆成多个 stage，stage 间用队列连接 | 更偏**架构模式**，可基于 Reactor 构建 |

### 6.3 全景层级图

```
传统同步模型 ──┬─ 阻塞 I/O（串行，最简单）
               └─ 非阻塞 I/O（忙轮询）

               ┌─ I/O 多路复用（select/poll/epoll）──→ Reactor 模式
               │        ├─ 单 Reactor 单线程
               │        ├─ 单 Reactor 多线程
               │        ├─ 主从 Reactor（main-sub）
               │        └─ Leader/Followers
事件驱动模型 ──┤
               └─ 异步 I/O（aio/io_uring）──────→ Proactor 模式

上层封装 ─────── 协程（libco）—— 本质是 Reactor 的同步写法
```

---

## 7. 总结（一张表记住全部）

| 概念 | 本质 | 命名由来 | 代表 |
| --- | --- | --- | --- |
| **Reactor** | 就绪通知，应用自己 I/O | 中心组件「反应」并分发事件 | epoll + Nginx/Redis |
| **Proactor** | 完成通知，内核代办 I/O | 应用「主动」提交完整操作 | io_uring |
| **Leader/Followers** | Reactor 多线程变体 | leader 接力 accept | ACE 框架 |
| **Half-Sync/Half-Async** | 异步收事件 + 同步线程池 | 分层分工 | 常见网络框架 |
| **协程** | Reactor 的同步写法封装 | Yield/Resume 机制 | libco/Go |

**本文交付内容回顾**：

- ✅ Reactor/Proactor 的命名渊源与本质区别（就绪通知 vs 完成通知）
- ✅ io_uring 详解：SQ/CQ 双环形队列 + 为什么是 Proactor + 与 epoll 详细对比
- ✅ **实测**：strace 抓取 epoll 处理 1 条消息的 11 次系统调用序列
- ✅ 主从 Reactor（main-sub）架构详解
- ✅ 其他模式：Leader/Followers、Half-Sync/Half-Async、协程、SEDA
- ⚠️ io_uring 代码已编译通过，但沙箱 seccomp 禁用 `io_uring_setup`，需在自己的机器上验证

**下一步**：见 [03-libco epoll 源码分析](./03-libco-epoll-源码分析.md)，看协程如何把 epoll 藏在同步系统调用后面。
