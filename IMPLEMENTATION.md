# RPC 客户端协程化实现说明

## 1. 目标

传统同步 RPC 客户端会在 `connect/send/recv` 上阻塞调用线程。为了同时发起大量请求，
通常只能创建大量线程；线程的栈空间和内核调度成本随并发数一起增长。

本项目保留下面这种同步调用方式：

```cpp
stub.Login(&controller, &request, &response, nullptr);
```

但是调用运行在有栈协程中。网络暂时不可读或不可写时，hook 层把 fd 注册到 epoll，
保存当前协程并执行 `yield()`。同一个调度线程随即运行其他 RPC 协程；fd 就绪后，
IOManager 再把原协程放回任务队列，从 `recv/send` 调用处继续执行。

因此这里的“异步 I/O”指底层执行方式，不要求业务代码改写为回调或状态机。

## 2. 工程结构

```text
rpcAndCoroutine/
├── coroutine/                 ucontext 协程、调度器、epoll、定时器、hook
├── src/                       Krpc 框架核心
├── example/callee/            Muduo RPC 服务端
├── example/caller/Kclient.cc  传统 100 线程对照客户端
├── example/caller/KcoroutineClient.cc
│                              4 个调度线程、100 个 RPC 协程
├── tests/                     协程 I/O 和超时测试
└── bin/test.conf              RPC、ZooKeeper 和协程参数
```

顶层 `CMakeLists.txt` 统一使用 C++17。`coroutine/CMakeLists.txt` 将最终的 `6hook`
源码编译成 `libcoroutine.a`，并链接 `pthread` 和 `dl`。`hook.cpp` 中定义的同名
系统调用符号会进入 `coroutine_client`，原始 libc 函数通过 `dlsym(RTLD_NEXT, ...)`
获得。

## 3. 一个 IOManager 如何调度 RPC

协程客户端的入口位于 `example/caller/KcoroutineClient.cc`：

```cpp
sylar::IOManager io_manager(scheduler_threads, true, "krpc_io");
for (int i = 0; i < fiber_count; ++i) {
    io_manager.scheduleLock([&]() {
        RunRpcFiber(requests_per_fiber, timeout_ms,
                    success_count, fail_count);
    });
}
```

默认配置是一个 IOManager、4 个调度线程、100 个 RPC 协程。`use_caller=true`
表示 IOManager 创建 3 个后台工作线程，并把调用者线程作为第 4 个调度线程。

每个协程内部顺序执行 1000 次普通的 `stub.Login()`。这保证最多约 100 个 RPC
同时处于网络等待状态，同时避免一次性调度 10 万个协程并耗尽连接池。

## 4. 同步 recv 为什么不会阻塞线程

核心实现在 `coroutine/hook.cpp` 的 `do_io()`：

```cpp
ssize_t n = recv_f(fd, buffer, size, flags);
if (n == -1 && errno == EAGAIN) {
    IOManager *iom = IOManager::GetThis();
    iom->addEvent(fd, IOManager::READ);
    Fiber::GetThis()->yield();
    goto retry;
}
```

完整流程如下：

```text
RPC 协程调用 recv
        |
        v
底层非阻塞 recv 返回 EAGAIN
        |
        v
IOManager::addEvent(fd, READ)，保存当前 Fiber
        |
        v
Fiber::yield()，调度线程继续运行其他 RPC
        |
        v
epoll_wait 得到 EPOLLIN
        |
        v
IOManager 将 Fiber 放回共享任务队列
        |
        v
Fiber 从 recv 内部恢复并重试读取
```

`connect` 等待 WRITE 事件，`send` 等待 WRITE 事件，处理方式相同。协程从共享
任务队列中动态抢占，保留原协程库的 N:M 调度方式。

## 5. fd 自动接管

hook 开启后，`do_io()` 使用：

```cpp
FdMgr::GetInstance()->get(fd, true);
```

即使连接是连接池预热阶段创建的，第一次在 RPC 协程中使用时也会被 FdManager
接管。FdCtx 使用原始 `fcntl` 把 socket 设置成系统非阻塞，但仍记录用户设置的
阻塞语义。这样上层看到的是同步 socket，底层执行的是非阻塞 epoll I/O。

FdManager 单例改为函数内静态对象，避免每次 `send/recv` 都争用单例初始化互斥锁。

## 6. KrpcChannel 的改动

RPC 调用入口仍是 `src/Krpcchannel.cc` 中的 `KrpcChannel::CallMethod()`。

发送请求从单次 `send()` 改成了 `send_exact()`：

```cpp
while (total_sent < size) {
    ssize_t n = send(fd, buf + total_sent, size - total_sent, MSG_NOSIGNAL);
    if (n == -1 && errno == EINTR) continue;
    if (n <= 0) return -1;
    total_sent += n;
}
```

hook 将 socket 设为非阻塞后，短写是正常情况，所以必须循环到整个 RPC 帧发送完毕。
响应继续由 `recv_exact()` 循环读取长度头和 Protobuf body。循环中的每次 `send/recv`
都可能让出协程，但 `CallMethod()` 本身仍保持直线式同步代码。

## 7. 连接和调用超时

`KrpcChannel` 默认超时为 3000ms。连接池在 `connect` 前设置 `SO_SNDTIMEO` 和
`SO_RCVTIMEO`，hook 层将其记录到 FdCtx：

```cpp
setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
```

等待 I/O 时，hook 同时向 TimerManager 注册条件定时器。定时器到期会取消 fd 事件、
重新调度等待协程，并让系统调用以 `errno == ETIMEDOUT` 返回。连接、发送或接收失败后，
KrpcChannel 把连接标记为坏连接，连接池负责关闭而不是复用它。

定时器比较器使用“到期时间 + Timer 地址”排序，多个协程具有相同到期时间时不会被
`std::set` 当作重复定时器丢弃；`getNextTimer()` 对 `m_tickled` 的修改也改为独占锁。

## 8. ZooKeeper 为什么提前初始化

ZooKeeper C 客户端的首次连接和 `zoo_wget_children` 是同步操作，不能依赖当前这套
socket hook 完整接管。因此 `KcoroutineClient.cc` 在创建 IOManager 前完成
ServiceDiscovery 初始化和首次节点缓存加载：

```cpp
ServiceDiscovery &discovery = ServiceDiscovery::GetInstance();
discovery.Init();
discovery.GetTargetNode("UserServiceRpc", "bootstrap");
```

`ServiceDiscovery::Init()` 内部使用 `std::call_once`，之后每次 RPC 调用只读取本地服务
缓存和一致性哈希环，不会重复连接 ZooKeeper。节点变化仍由 watcher 更新缓存。

## 9. 连接复用约束

当前 RPC 协议没有 request_id，因此不能在一个 TCP 连接上同时发送多个未完成请求。
连接池采用独占借用：一个协程借到 fd 后，直到收到完整响应才归还。不同协程可以使用
不同连接并发请求，同一连接上的请求和响应始终一一对应。

若未来要实现单连接多路复用，需要在请求头和响应头中加入 request_id，并增加
`request_id -> 等待协程/response` 映射；这属于下一层协议能力，不是本次 hook 改造。

## 10. 测试与运行

构建和基础测试：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

`tests/coroutine_io_test.cc` 使用一个 IOManager 调度线程。第一个协程同步阻塞在
`recv`，第二个协程负责发送数据；若 hook 没有执行 `yield`，测试会永久阻塞。
测试还验证了 `SO_RCVTIMEO` 能通过定时器把等待协程唤醒并返回 `ETIMEDOUT`。

端到端运行要求本机 ZooKeeper 监听 `127.0.0.1:2181`：

```bash
./bin/server -i ./bin/test.conf
./bin/coroutine_client -i ./bin/test.conf
```

传统线程对照：

```bash
./bin/client -i ./bin/test.conf
```

## 11. 本机验证结果

2026-07-22 使用 Release 客户端，在当前虚拟机、同一个 Provider、服务端业务模拟休眠
5ms 的条件下完成了两组历史对照测试。

10 万请求短测：

| 客户端 | 请求模型 | 预热连接 | 成功/失败 | RPC 耗时 | RPC QPS |
|---|---:|---:|---:|---:|---:|
| 传统线程版 | 100 线程 x 1000 | 0 | 100000 / 0 | 5.385s | 18568.4 |
| 协程版 | 4 调度线程、100 协程 x 1000 | 0 | 100000 / 0 | 5.319s | 18802.2 |

协程客户端还测得 `shutdown elapsed = 2.996s`。旧计时代码把 IOManager 析构等待
算入 RPC 耗时，得到 `8.315s` 和约 `12000 QPS`，这个数字不能代表 RPC 吞吐。
现在 `KcoroutineClient.cc` 会分别输出 RPC、关闭和总耗时。

请求数放大 100 倍后的长测：

| 客户端 | 总请求 | 预热连接 | 成功/失败 | 总耗时 | 端到端 QPS |
|---|---:|---:|---:|---:|---:|
| 传统线程版 | 10000000 | 0 | 10000000 / 0 | 529.711s | 18878.2 |
| 协程版 | 10000000 | 0 | 10000000 / 0 | 533.823s | 18732.8 |

协程长测是在拆分计时前完成的，因此表中使用包含固定关闭等待的端到端口径；即使如此，
它与线程版也只相差约 0.77%。结合短测测得的约 3 秒固定关闭耗时，可以确认两种客户端
的稳态 RPC 吞吐都在 1.88 万 QPS 左右。

进程线程数还包含 ZooKeeper 和一致性哈希后台线程。传统版共观察到 104 个线程；协程版
共 7 个线程，其中真正用于 RPC 调度的是 4 个。测试说明同步 RPC 等待不会占住调度线程，
并且在本次负载下，4 个调度线程可以达到与 100 个阻塞请求线程接近的稳态吞吐。

随后注释 `Kserver.cc` 中的 5ms 业务休眠，在每轮 10 万请求、关闭连接预热的相同条件下，
交替完成 5 轮测试：

| 轮次 | 传统线程版 QPS | 协程版 RPC QPS |
|---:|---:|---:|
| 1 | 65166.2 | 70489.9 |
| 2 | 65498.5 | 68429.7 |
| 3 | 63024.8 | 71116.6 |
| 4 | 66824.1 | 73059.1 |
| 5 | 68181.6 | 73512.6 |
| 平均 | 65739.0 | 71321.6 |

线程版平均 RPC 耗时为 1.522s，协程版为 1.403s；协程版平均 QPS 高约 8.5%。五轮中
两边累计各完成 50 万请求，均为零失败。服务端不再有固定 5ms 业务瓶颈后，测试主要覆盖
协议编解码、连接池、Muduo 派发、hook、epoll 和协程调度，协程版少量调度线程的优势
开始体现。

协程版每轮仍有约 3 到 5 秒的 IOManager 固定关闭等待，但该时间发生在所有 RPC 完成
之后，已由独立的 `shutdown elapsed` 输出记录，不计入 `rpc qps`。

最后将服务端业务耗时设为 4ms，同时把客户端并发提高到 1000。传统版使用 1000 个
请求线程，协程版使用 4 个调度线程承载 1000 个协程；每个执行单元调用 1000 次，
所以每个客户端每轮完成 100 万次 RPC。三组零失败样本如下：

| 轮次 | 传统线程版 QPS | 协程版 RPC QPS |
|---:|---:|---:|
| 1 | 24059.5 | 24204.2 |
| 2 | 24242.1 | 24235.8 |
| 3 | 24218.0 | 24199.9 |
| 平均 | 24173.2 | 24213.3 |

两者平均 QPS 只相差约 0.17%，可以视为相同。服务端有 100 个业务线程，每次处理 4ms，
理论业务上限约为 25000 QPS；两种客户端都达到了理论上限的约 97%，此时瓶颈已经在
服务端，继续增加客户端并发无法明显提高吞吐。

协程版三轮均为 100 万成功、0 失败。线程版在另外两次冷启动样本中分别出现 5272 和
407 次 `Borrow connection from pool failed`，未纳入零失败均值。这说明 1000 个线程
同时动态建连会偶发触发连接借用路径失败；`Kclient.cc` 已增加按错误文本聚合的统计输出，
便于后续区分高并发压测中的有效吞吐与失败调用。

## 12. 1000 并发下的服务端扩容矩阵

为观察客户端吞吐拐点，保持服务端业务耗时 4ms、客户端逻辑并发 1000、每端总请求
100 万不变，只把服务端业务线程依次设置为 100、200、400。两个客户端都在计时前完成
ZooKeeper 首次查询和 1000 条连接预热。

指标口径：

- 成功 QPS：成功响应数量除以 RPC 工作区间墙钟时间。
- 失败率：失败调用数除以总调用数。
- CPU 秒和平均核数：仅统计客户端 RPC 工作区间，平均核数等于 CPU 秒除以墙钟秒。
- 进程线程数：从 `/proc/self/status` 的 `Threads` 实测。
- P99：100 万个请求中成功 RPC 的端到端延迟分位值。
- 协程客户端的 IOManager 关闭等待单独统计，不进入 RPC QPS 和 CPU 区间。

测试结果：

| 服务端业务线程 | 服务端总线程 | 客户端 | 成功 QPS | 失败率 | 客户端 CPU 秒 | 平均核数 | 客户端线程 | P99 |
|---:|---:|---|---:|---:|---:|---:|---:|---:|
| 100 | 107 | 传统1000线程 | 24283.6 | 0% | 38.10 | 0.93 | 1004 | 43.09ms |
| 100 | 107 | 4线程/1000协程 | 24253.2 | 0% | 52.95 | 1.28 | 7 | 44.49ms |
| 200 | 207 | 传统1000线程 | 48548.2 | 0% | 33.74 | 1.64 | 1004 | 21.32ms |
| 200 | 207 | 4线程/1000协程 | 48523.7 | 0% | 40.50 | 1.97 | 7 | 22.82ms |
| 400 | 407 | 传统1000线程（三轮均值） | 50633.6 | 0.1317% | 30.04 | 1.52 | 1004 | 43.29ms |
| 400 | 407 | 4线程/1000协程（三轮均值） | 58543.7 | 0% | 35.42 | 2.07 | 7 | 34.59ms |

100 和 200 业务线程时，业务池的理论上限分别是 2.5 万和 5 万 QPS，两种客户端都达到
约 97%，吞吐和 P99 基本相同。业务线程增加到 400 后，理论上限提高到 10 万，但系统
只达到约 5 到 6 万，说明瓶颈已经转移到线程池调度、Muduo I/O、协议/网络和客户端路径。

400 线程档中，协程版成功 QPS 比线程版三轮均值高约 15.6%，P99 低约 20.1%，并保持
零失败；客户端线程数从 1004 降为 7。代价是协程版平均使用 2.07 核，高于线程版的
1.52 核，说明当前 hook、epoll、定时器和 Fiber 调度以更多用户态/系统态 CPU 换取了
更少线程、更高成功吞吐和更好的尾延迟，不能表述为“协程更省 CPU”。

400 线程档传统客户端的三轮失败率分别为 0%、0.0255%、0.3697%，错误均为
`Borrow connection from pool failed`。这些样本已经预热 1000 条连接，服务端日志仍出现
超过预热数量的额外动态连接，因此失败不再能简单归因于冷启动 connect 风暴。更可能的
原因是高竞争下无锁空闲队列的 `try_pop` 暂时返回空，连接池继续创建连接并触及 1024
上限；要完全定性还需要让 `BorrowConnection` 区分队列瞬时空、容量耗尽和 connect errno。

复现配置：

```bash
# 100 business threads
./bin/server -i ./bin/benchmark-1000-concurrency.conf

# 200 / 400 business threads
./bin/server -i ./bin/benchmark-1000-business-200.conf
./bin/server -i ./bin/benchmark-1000-business-400.conf

# 使用与 server 相同的配置分别运行
./bin/client -i <config>
./bin/coroutine_client -i <config>
```

## 13. 当前边界

- 服务端仍使用 Muduo Reactor 和业务线程池；本次只改造同步阻塞痛点所在的客户端。
- RPC 对业务层保持同步接口，尚未增加 Future、Promise 或 C++20 coroutine API。
- 一个 TCP 连接同一时间只有一个在途请求，没有实现单连接多路复用。
- ZooKeeper 首次同步查询在协程调度器启动前完成，避免阻塞调度线程。

这几个边界是有意保留的：它们让本次改造集中证明 hook、epoll 和有栈协程如何透明地
改变同步 RPC 的等待方式，而不同时重写服务端网络栈和 RPC 协议。
