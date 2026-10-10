# KrpcCoroutine 简历项目表述

## 基于原 RPC 项目介绍的融合版本

### 基于 C++ 开发的协程化分布式 RPC 网络通信框架｜项目时间按实际填写

**技术栈：** C++17、Muduo、Protobuf、ZooKeeper、TCP、Linux、CMake、多线程、ucontext_t、
epoll、有栈协程、系统调用 Hook、MPMC 无锁队列

**项目描述：** 基于 C++ 开发轻量级分布式 RPC 框架，服务端以 Muduo Reactor 作为事件驱动网络
层，使用 Protobuf 完成消息序列化和服务接口定义，通过 ZooKeeper 实现服务注册、发现与节点动态
更新；在原有同步 RPC 客户端基础上融合有栈协程运行时，通过 IOManager、epoll 和系统调用 Hook
将 `connect/send/recv` 网络等待转换为 Fiber 挂起，在保留同步 Stub 编程方式的同时，使用少量
调度线程承载大量并发 RPC 调用。

**核心功能与实现：**

- 设计框架入口与配置模块，完成命令行解析、配置加载和运行环境初始化；使用 Protobuf 定义消息、
  服务与方法接口，实现请求和响应的序列化/反序列化。
- 封装 RPC 核心模块 `Channel`、`Controller` 和 `Provider`，完成服务节点选择、请求组包、网络传输、
  Provider 拆包、Protobuf 动态方法派发、响应回传以及错误状态管理。
- 设计 `[total_len][header_len][RpcHeader][args]` 自定义 TCP 二进制协议，Provider 基于 Muduo
  Buffer 循环解析完整帧，处理 TCP 粘包/半包；客户端使用 `send_exact/recv_exact` 处理非阻塞
  Socket 的短写和不完整读取。
- 集成 ZooKeeper 实现服务注册与发现，Provider 使用临时节点发布实例，客户端通过 watcher 更新
  本地节点缓存，并结合一致性哈希完成服务实例选择，减少每次调用对注册中心的同步访问。
- 按服务节点维护 TCP 长连接池，使用 MPMC 队列保存空闲 fd，支持连接预热、请求完成后的连接复用
  和坏连接淘汰，减少频繁 TCP 三次握手带来的开销。
- 基于 `ucontext_t` 实现有栈 Fiber 和 N:M Scheduler，使用 IOManager 统一管理协程任务、epoll
  READ/WRITE 事件和超时定时器；Hook `sleep/connect/send/recv` 等调用，使 RPC Fiber 在 I/O
  未就绪时主动让出调度线程，fd 就绪或超时后恢复执行。
- 保持 `stub.Login(...)` 等同步 Protobuf Stub 接口不变，将完整 `KrpcChannel::CallMethod()` 调用
  运行在 Fiber 中，实现“同步接口、异步执行”，避免高并发网络等待与操作系统线程一一绑定。

**项目难点：**

- **同步调用透明协程化：** 在不重写 Protobuf Stub 和上层业务代码的情况下，通过符号 Hook 与
  fd 上下文管理接管阻塞系统调用；处理预热阶段创建的 fd 自动纳管、非阻塞短写、I/O 超时唤醒和
  坏连接回收，保证 Fiber 能从原 `send/recv` 调用位置继续执行。
- **并发模型与线程边界：** 客户端采用 N:M 协程调度，Provider 仍使用 Muduo 网络线程与独立业务
  线程池；将 ZooKeeper 首次同步初始化放在 IOManager 启动前，避免阻塞调度线程，并将 RPC 工作
  时间与 IOManager 关闭等待分开统计。
- **高并发连接管理：** 当前协议没有 request ID，一条连接同时只能承载一个在途 RPC，因此连接池
  需要保证连接独占借用和响应完成后归还；通过连接预热和 MPMC 空闲队列降低高并发动态建连与
  共享锁竞争。
- **性能瓶颈定位：** 分别调整 Provider 业务线程为 100、200、400，验证 4ms 模拟业务下服务端
  理论上限约为 2.5 万、5 万、10 万 QPS，区分服务端业务池瓶颈与客户端线程/协程调度瓶颈，避免
  仅凭单组 QPS 得出协程必然更快的结论。

**压力测试：** 自行编写传统线程版与协程版 Krpc 基准程序，在同一台开发机、Release 构建、相同
Provider、相同 TCP 长连接池条件下进行对照。保持 1000 逻辑并发、每个执行单元循环 1000 次、
总计 100 万 RPC、4ms 模拟业务、400 个服务端业务线程和 1000 条预热连接时，传统版使用 1000 个
请求线程，成功 QPS 为 **50,633.6**、P99 为 **43.29ms**；协程版使用 4 个 IOManager 调度线程
承载 1000 个 Fiber，成功 QPS 达到 **58,543.7（+15.6%）**，P99 降至 **34.59ms（-20.1%）**，
客户端进程线程数由 **1004 降至 7（-99.3%）**，协程版失败率为 **0%**。

### 更适合一页简历的压缩版

**基于 C++ 开发的协程化分布式 RPC 网络通信框架｜项目时间按实际填写**

**技术栈：** C++17、Muduo、Protobuf、ZooKeeper、TCP、Linux、CMake、ucontext_t、epoll、
有栈协程、系统调用 Hook、MPMC 无锁队列

**项目描述：** 基于 Muduo Reactor、Protobuf 和 ZooKeeper 实现轻量级分布式 RPC 框架，在原有
同步客户端基础上融合有栈协程运行时，通过 IOManager、epoll 和 Hook 将网络等待转换为 Fiber
挂起，在保持同步 Stub 接口的同时，以少量调度线程承载大量并发 RPC。

**核心功能与实现：**

- 封装 `Channel/Controller/Provider`，设计 `[total_len][header_len][RpcHeader][args]`
  TCP/Protobuf 协议，基于 Muduo Buffer 处理粘包/半包并完成动态方法派发；集成 ZooKeeper 临时
  节点、watcher、本地缓存和一致性哈希，实现服务注册发现与实例选择。
- 基于 `ucontext_t` 实现有栈 Fiber、N:M Scheduler 和 IOManager，结合 epoll、定时器与
  `connect/send/recv` Hook，在 I/O 等待时挂起并恢复 RPC Fiber；完善短写、定长读取、超时和
  fd 自动接管，保留 Protobuf Stub 同步调用方式。
- 按服务节点维护 MPMC TCP 长连接池，支持连接预热、独占借用、响应后复用和坏连接淘汰；Provider
  使用 4 个 Muduo 网络线程和可配置业务线程池，实现网络 I/O 与业务执行解耦。

**压力测试：** 在自研基准程序模拟的 1000 逻辑并发、100 万 RPC、4ms 模拟业务和相同长连接池
条件下，协程客户端以 4 个调度线程承载 1000 个 Fiber，相比传统 1000 线程客户端，成功 QPS
**5.06 万 -> 5.85 万（+15.6%）**，P99 **43.29ms -> 34.59ms（-20.1%）**，客户端进程
线程数 **1004 -> 7（-99.3%）**。

## 可直接放入简历的版本

### 支持同步调用透明协程化的高性能分布式 RPC 框架｜项目时间按实际填写

**项目介绍：** 针对传统同步 RPC 客户端在高并发 I/O 等待场景下需要创建大量工作线程的问题，
将基于 Muduo、Protobuf、ZooKeeper 的分布式 RPC 框架与有栈协程运行时结合。框架在保留
Protobuf Stub 同步调用方式的同时，通过系统调用 Hook、非阻塞 Socket 和 epoll 将网络等待转换为
Fiber 挂起，使少量调度线程可以承载大量同时在途的 RPC 请求；服务端采用 Muduo Reactor 与独立
业务线程池处理请求。

**技术栈：** C++17、ucontext_t、epoll、有栈协程、系统调用 Hook、Muduo、TCP 长连接池、
MPMC 无锁队列、Protobuf、ZooKeeper、CMake

**主要工作：**

1. **协程运行时与 I/O 调度：** 基于 `ucontext_t` 实现有栈 Fiber 的上下文保存与恢复，设计 N:M
   Scheduler 和 IOManager；结合 epoll 与定时器管理 READ/WRITE 事件及超时任务，Hook
   `sleep/connect/send/recv` 等系统调用，在 I/O 未就绪时挂起当前 Fiber，并在 fd 就绪后恢复执行。
2. **同步 RPC 客户端透明协程化：** 保持 `stub.Login(...)` 等 Protobuf 同步接口不变，将完整 RPC
   调用运行在 IOManager Fiber 中；完善 fd 自动接管、非阻塞短写循环、定长响应读取和 RPC 超时
   处理，使同步调用在底层以 epoll 驱动，避免网络等待阻塞调度线程。
3. **RPC 通信协议与并发服务端：** 设计基于长度字段和 Protobuf Header 的 TCP 二进制协议，帧格式
   为 `[total_len][header_len][RpcHeader][args]`；Provider 基于 Muduo Buffer 循环拆帧，处理 TCP
   粘包/半包，使用 4 个网络 I/O 线程和可配置业务线程池完成协议解析、方法派发及响应发送。
4. **服务治理与连接复用：** 基于 ZooKeeper 临时节点完成服务注册，结合 watcher 更新本地节点
   缓存，并通过一致性哈希选择服务实例；按服务节点维护 TCP 长连接池，使用 MPMC 队列保存空闲
   fd，支持连接预热、请求后复用和坏连接淘汰。
5. **性能测试与瓶颈分析：** 编写传统线程版和协程版 RPC 基准程序，在相同 Provider、连接池和
   请求参数下统计成功 QPS、失败率、P99、CPU 时间及进程线程数。在 Release 构建、1000 逻辑
   并发、100 万 RPC、4ms 模拟业务、400 个服务端业务线程和 1000 条预热连接条件下，协程版以
   4 个调度线程承载 1000 个 Fiber，成功 QPS 由 **50,633.6 提升至 58,543.7（+15.6%）**，P99
   由 **43.29ms 降至 34.59ms（-20.1%）**，客户端进程线程数由 **1004 降至 7（-99.3%）**。

## 一页简历的精简版本

### 支持同步调用透明协程化的高性能分布式 RPC 框架｜项目时间按实际填写

- 针对同步 RPC 客户端高并发下请求线程膨胀的问题，基于 `ucontext_t + epoll + Hook` 实现有栈
  Fiber、N:M 调度器和 IOManager，在不修改 Protobuf Stub 同步接口的前提下，将
  `connect/send/recv` 网络等待转换为 Fiber 挂起与恢复。
- 设计 `[total_len][header_len][RpcHeader][args]` TCP/Protobuf 二进制协议，基于 Muduo Reactor
  与业务线程池完成请求拆包、动态方法派发和响应发送；集成 ZooKeeper 临时节点、watcher、本地
  节点缓存、一致性哈希及 MPMC TCP 长连接池。
- 在单机 Release 环境、自研基准程序模拟 1000 逻辑并发和 100 万次 RPC 的对照测试中，协程版以
  4 个调度线程承载 1000 个 Fiber，成功 QPS **5.06 万 -> 5.85 万（+15.6%）**，P99
  **43.29ms -> 34.59ms（-20.1%）**，客户端进程线程数 **1004 -> 7（-99.3%）**。

## 30 秒面试介绍

> 这个项目是把我学习的 RPC 框架和有栈协程库结合起来，重点改造传统同步 RPC 客户端。原来的
> Protobuf Stub 会阻塞在 connect、send、recv 上，高并发时需要大量请求线程。我保留同步 Stub
> 接口，通过 ucontext 实现 Fiber，使用 IOManager、epoll 和系统调用 Hook，在 I/O 未就绪时挂起
> Fiber，fd 就绪后再恢复。Provider 仍然采用 Muduo Reactor 加业务线程池。1000 逻辑并发的对照
> 测试中，协程客户端使用 4 个调度线程替代 1000 个请求线程，成功 QPS 提升约 15.6%，P99 降低
> 约 20.1%，客户端进程线程数从 1004 降到 7。

## 指标口径

- 测试环境：同一台开发机、loopback、Release 构建。
- 请求模型：传统版 1000 个线程，协程版 4 个调度线程承载 1000 个 Fiber。
- 请求数量：每个执行单元循环 1000 次，共 100 万 RPC。
- Provider：4 个 Muduo 网络线程、400 个业务线程，业务方法模拟阻塞 4ms。
- 连接条件：两个客户端均预热 1000 条 TCP 长连接。
- QPS：成功响应数除以 RPC 工作区间墙钟时间。
- P99：成功 RPC 从 Stub 调用开始到完整响应解析结束的 99 分位延迟。
- 协程客户端的 IOManager 关闭等待不计入 RPC 工作区间。

| 指标 | 传统线程客户端 | 协程客户端 | 变化 |
|---|---:|---:|---:|
| 成功 QPS | 50,633.6 | 58,543.7 | +15.6% |
| P99 | 43.29ms | 34.59ms | -20.1% |
| 客户端进程线程数 | 1004 | 7 | -99.3% |
| 失败率 | 0.1317% | 0% | 协程版零失败 |
| 客户端平均 CPU 核数 | 1.52 | 2.07 | 协程版更高 |

## 面试时必须说明的边界

1. **不是全链路协程化。** 协程运行时用于客户端 RPC 调用和协程 I/O 测试；Provider 仍使用
   Muduo Reactor 和业务线程池。
2. **不是 HTTP 网关。** 当前端口使用自定义 TCP/Protobuf 协议，没有实现 HTTP/JSON 入口，
   因而当前数据不是 wrk 测得，而是自研 Krpc 基准客户端测得。
3. **没有单连接多路复用。** 当前协议没有 request ID，一条连接同时只有一个在途 RPC；1000
   并发测试因此预热了 1000 条连接。
4. **协程版没有降低 CPU。** 当前实现用更多 Hook、epoll、定时器和 Fiber 调度开销换取更少线程、
   更高成功吞吐和更低 P99；测试中平均 CPU 核数由 1.52 增加至 2.07。
5. **400 个业务线程是实验控制变量。** 100/200 业务线程时 Provider 的 4ms 业务池已成为瓶颈，
   两个客户端分别都接近 2.5 万/5 万 QPS；设置 400 是为了移开服务端瓶颈并观察客户端差异，
   不是生产部署建议。

## 不应照搬原简历的表述

- 不写“RPC 网关”，因为当前没有 HTTP/JSON 或其他外部协议入口。
- 不写“AI 服务统一入口”，因为当前没有 AI 服务编排和多模型业务代码。
- 不写“使用 wrk 压测自定义 RPC”，因为标准 wrk 只能发送 HTTP。
- 不写“系统吞吐从 861 QPS 提升到 2.5 万 QPS”，该数据不属于本项目。
- 不写“平均延迟从 294ms 降到 3.88ms”，该数据不属于本项目。
- 不写“彻底解决所有同步阻塞”，ZooKeeper 首次初始化和连接预热目前仍在 IOManager 启动前同步
  完成，连接池耗尽时也尚未实现 Fiber 等待队列。
- 不写“协程显著降低 CPU”，实测结果与该结论相反。
