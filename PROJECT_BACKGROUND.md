# 面向文件同步服务的协程化高并发 RPC 框架

本文为 KrpcCoroutine 提供一套适合学生项目的应用背景、简历写法和面试答辩口径。项目以一个
轻量级个人云盘/课程资料同步服务为应用场景，重点解决客户端重新连接、批量同步目录时，内部
元数据 RPC 同步等待导致请求线程膨胀的问题。

项目不声称运营过真实云盘，也不把本地 RPC 微基准说成真实文件上传吞吐。框架压测衡量的是
小消息控制面，例如登录认证、目录元数据查询、同步状态更新和任务确认；文件内容本身走独立
的数据通道，不通过当前 RPC 传输。

## 1. 项目定位

推荐名称：

> **面向文件同步服务的协程化高并发 RPC 框架**

更完整的名称：

> **面向校园资料云盘的元数据同步 RPC 框架**

技术型名称：

> **KrpcCoroutine：支持同步调用透明协程化的分布式 RPC 框架**

这是一个 RPC 通信框架项目，不是完整云盘产品。云盘只是解释业务问题的应用背景，当前仓库
真正实现的是 Protobuf RPC 协议、ZooKeeper 服务发现、TCP 长连接池、Provider 和协程化客户端。

## 2. 应用背景

一个简化的文件同步服务可以拆成以下组件：

- **Sync Gateway**：接收桌面客户端或移动端的登录、同步和状态请求；
- **User Service**：负责账户认证、用户信息查询；
- **Metadata Service**：保存目录树、文件版本、大小和哈希值；
- **Sync Coordinator**：比较客户端与服务端版本，生成同步任务；
- **Object Storage**：保存真正的文件内容；
- **ZooKeeper**：注册内部服务实例并提供服务发现。

客户端第一次登录或网络中断后恢复时，需要先拉取目录元数据，再根据文件哈希判断哪些内容需要
上传或下载。这里有一个重要的分层：

```text
文件元数据、版本、同步状态：小消息，适合内部 RPC
文件内容：大对象，使用 HTTP/对象存储/分块传输
```

因此当前项目的 RPC 不负责上传几百 MB 的文件，而是负责同步控制面。

学生做课程资料云盘或个人网盘原型时，不需要真的拥有大量用户。可以用本地压测程序模拟以下
突发场景：

- 多台虚拟客户端同时重新登录并拉取目录；
- 电脑从睡眠或断网状态恢复后，批量发送同步请求；
- 课程资料目录更新后，多客户端同时刷新文件版本；
- Metadata Service 扩容或迁移导致客户端重新选择服务节点。

传统内部 RPC 客户端使用同步 Stub。元数据查询在等待 Redis、数据库或对象存储索引返回时，
会一直占用请求线程。要保持 1000 个同时在途的同步会话，传统模型通常需要接近 1000 个线程，
带来线程栈、内核调度、上下文切换和连接池竞争开销。

项目保留直观的同步 Protobuf 调用方式，在底层加入有栈协程和系统调用 hook。RPC 等待网络
事件时，当前 Fiber 注册 epoll 并主动让出调度线程；fd 就绪后再恢复原 Fiber。这样上层代码
仍然可以按顺序写同步逻辑，等待中的同步 RPC 却不会一一占用操作系统线程。

## 3. 当前样例如何对应云盘场景

当前仓库沿用原 RPC 项目中的 `UserServiceRpc.Login`，它可以直接表示云盘用户登录认证。测试中
的 4ms 业务休眠可以解释为查询账户、目录版本或同步状态的轻量存储操作，不代表文件上传只需要
4ms，也不代表文件内容由当前 RPC 传输。

如果继续产品化，可以增加以下业务服务：

```protobuf
service MetadataServiceRpc {
    rpc GetManifest(ManifestRequest) returns (ManifestResponse);
    rpc UpdateSyncState(UpdateSyncStateRequest) returns (UpdateSyncStateResponse);
}

message ManifestRequest {
    uint64 user_id = 1;
    string client_revision = 2;
}

message ManifestResponse {
    string server_revision = 1;
    repeated FileEntry entries = 2;
}
```

返回的只是文件名、版本、大小、哈希和同步动作，文件内容再由数据通道传输。当前框架依赖
Protobuf 的 Service/Method 描述符动态派发，增加上述 Proto 不需要重写协程调度、连接池、
TCP 协议或服务发现。

## 4. 系统结构

```text
桌面端 / 移动端云盘客户端
    |
    | HTTP / WebSocket / 分块上传协议
    | 文件数据面不属于当前 RPC 仓库
    v
Sync Gateway
    |
    | 内部同步 Protobuf Stub
    v
KrpcCoroutine 客户端运行时
    |-- 1 个 IOManager
    |-- 4 个调度线程
    |-- 1000 个 RPC Fiber（压测配置）
    |-- TCP 长连接池
    |-- connect/send/recv hook
    |-- epoll + 定时器
    |
    | 自定义 TCP/Protobuf RPC
    v
User / Metadata Service Provider
    |-- Muduo Reactor
    |-- 4 个网络 I/O 线程
    |-- 可配置业务线程池
    |-- Login / Manifest / SyncState
    |
    v
Redis / MySQL / 对象存储索引

Object Storage
    `-- 文件内容上传、下载和分块合并

ZooKeeper
    |-- Provider 临时节点注册
    |-- Gateway 服务发现
    `-- watcher 更新本地节点缓存
```

需要区分外部和内部两条链路：客户端可以通过 HTTP 或 WebSocket 连接 Sync Gateway，大文件可以
直接走对象存储；Gateway 调用 User/Metadata Service 时才使用当前自定义 Krpc 协议。当前仓库
没有实现完整 Web 前端、对象存储、分块上传和 Gateway。

## 5. 一次目录同步控制面请求

1. 客户端登录或从断网状态恢复，向 Sync Gateway 请求同步。
2. Gateway 在 IOManager 中调度一个同步 Fiber，并通过同步 Stub 调用 Metadata Service。
3. `KrpcChannel` 从 ZooKeeper 本地缓存中选择一个 Provider。
4. 客户端从该节点连接桶借用 TCP 长连接，序列化 RPC Header 和 Protobuf 请求。
5. `connect/send/recv` 遇到 `EAGAIN` 时，hook 注册 epoll 事件并让当前 Fiber yield。
6. 调度线程继续执行其他同步 Fiber，不被当前请求的网络等待占住。
7. Provider 的 Muduo I/O 线程完成拆包和 Protobuf 解析，把调用投递到业务线程池。
8. Metadata Service 查询目录版本并返回需要上传、下载或删除的文件列表。
9. epoll 唤醒原 Fiber，客户端解析响应并归还连接，Stub 同步返回。
10. Gateway 再通过数据通道执行文件传输，或记录新的同步状态。

当前实际运行的示例是 `UserService::Login`，它验证的是同样的轻量小消息 RPC 路径。

## 6. 主要技术实现

### 6.1 有栈协程与 N:M 调度

基于 `ucontext_t` 保存和恢复 Fiber 上下文，通过 Scheduler 管理共享任务队列。一个 IOManager
使用 4 个调度线程承载 1000 个 RPC Fiber，Fiber 在用户态切换，不需要为每个在途同步请求
创建一个内核线程。

### 6.2 epoll 与系统调用 hook

hook `connect/send/recv/sleep` 等调用，通过 `dlsym(RTLD_NEXT, ...)` 获取原 libc 函数。socket
在内核层使用非阻塞模式；遇到 `EAGAIN` 时注册 READ/WRITE 事件并 yield，fd 就绪后恢复等待
Fiber。上层仍保持同步 socket 和同步 Stub 的调用语义。

### 6.3 定时器与 RPC 超时

Fiber 等待 I/O 时同步注册条件定时器。超时后取消 fd 事件并唤醒 Fiber，使调用以 `ETIMEDOUT`
返回。发送或接收失败的连接会被关闭，不再放回连接池。

### 6.4 TCP 长连接池

按 `IP:Port` 维护连接桶，使用 MPMC 队列保存空闲 fd，支持连接预热和请求完成后的连接复用。
连接池减少三次握手和 TCP 慢启动成本；协程减少等待响应时占用的线程，两者解决不同问题。

当前协议没有 `request_id`，一条 TCP 连接同一时间只能承载一个在途 RPC。压测中两个客户端
都预热 1000 条连接，是为了在现有协议限制下公平比较线程与协程，不是云盘生产部署要求。

### 6.5 自定义 TCP/Protobuf 协议

请求帧为：

```text
[total_len: 4 bytes, network order]
[header_len: 4 bytes, network order]
[RpcHeader protobuf]
[method request protobuf]
```

`RpcHeader` 包含 `service_name`、`method_name` 和 `args_size`。响应由 4 字节长度和 Protobuf
响应体组成。Provider 按长度字段从 Muduo Buffer 循环拆帧，客户端使用
`send_exact/recv_exact` 处理短写、不完整读取和 TCP 粘包/半包。

### 6.6 ZooKeeper 服务注册与发现

Provider 使用临时节点注册服务，进程断开后节点自动消失。客户端启动时加载节点到本地缓存，
后续 RPC 直接读取缓存，watcher 在节点变化时更新，避免每次同步请求都访问 ZooKeeper。

### 6.7 Provider 网络和业务线程分离

Provider 使用 4 个 Muduo 网络线程处理连接与字节流，再把 Protobuf 方法调用投递到业务线程池。
业务线程数可以配置为 100、200、400，用于压测时分析服务端瓶颈迁移。

## 7. 简历可直接使用的版本

### 项目介绍

> 以个人云盘/课程资料同步服务的元数据控制面为应用场景，针对客户端集中同步时同步 RPC 请求
> 线程膨胀的问题，设计并实现协程化 RPC 框架。基于 Muduo、Protobuf 和 ZooKeeper 完成内部
> RPC 通信与服务注册发现，通过 `ucontext_t + epoll + hook` 将同步 Stub 透明改造为可挂起 I/O，
> 使少量调度线程能够承载大量同时在途的元数据同步请求。

### 技术栈

> C++17、ucontext、epoll、有栈协程、系统调用 hook、Muduo、TCP 长连接池、Protobuf、
> ZooKeeper、MPMC 无锁队列、CMake

### 主要工作

1. 基于 `ucontext_t` 实现有栈 Fiber 和 N:M 调度器，结合 epoll 与定时器构建 IOManager；
   hook `connect/send/recv`，I/O 未就绪时挂起并恢复 Fiber，保持 Protobuf Stub 同步接口。
2. 设计长度字段加 Protobuf Header 的 TCP 二进制协议，Provider 基于 Muduo Buffer 循环拆帧，
   客户端通过 `send_exact/recv_exact` 处理短写、粘包和半包，并增加 RPC 超时和坏连接淘汰。
3. 基于 ZooKeeper 临时节点和 watcher 实现服务注册发现与本地缓存；按服务节点维护 TCP 长连接池
   和 MPMC 空闲连接队列，支持连接预热与复用。
4. 编写传统线程版和协程版 RPC 基准程序，在相同连接池及请求参数下采集成功 QPS、失败率、
   P99、CPU 时间和线程数；在自研程序模拟的 1000 逻辑并发下，以 4 个调度线程承载 1000 个
   Fiber，成功 QPS 从 **5.06 万提升至 5.85 万（+15.6%）**，P99 从 **43.29ms 降至
   34.59ms（-20.1%）**，客户端线程数从 **1004 降至 7（-99.3%）**。

### 一段式简历版本

> **面向文件同步服务的协程化高并发 RPC 框架**：针对客户端集中同步时同步 RPC 请求线程膨胀
> 的问题，基于 `ucontext_t + epoll + hook` 实现同步 Protobuf Stub 的透明协程化，以 4 个
> IOManager 调度线程承载 1000 个 RPC Fiber；实现 TCP/Protobuf 二进制协议、长连接池、超时
> 定时器及 ZooKeeper 服务注册发现。在单机 1000 逻辑并发、400 业务线程、4ms 模拟元数据查询、
> 100 万 RPC 的 Release 对照压测中，成功 QPS 5.06 万 -> 5.85 万（+15.6%），P99
> 43.29ms -> 34.59ms（-20.1%），客户端线程数 1004 -> 7。

## 8. 性能数字的真实口径

主数据测试条件：

- 单机 loopback、Release 构建；
- 压测程序创建 1000 个逻辑同步会话；
- 每个执行单元循环 1000 次，总请求数 100 万；
- 两个客户端均预热 1000 条 TCP 连接；
- Provider 使用 4 个网络线程、400 个业务线程；
- `Login` 使用 4ms 休眠模拟轻量元数据查询或同步状态更新；
- 传统版与协程版均采用三轮均值；
- IOManager 关闭时间不计入 RPC 工作区间。

| 指标 | 传统 1000 线程 | 4 线程/1000 协程 | 变化 |
|---|---:|---:|---:|
| 成功 QPS | 50,633.6 | 58,543.7 | +15.6% |
| P99 | 43.29ms | 34.59ms | -20.1% |
| 客户端进程线程数 | 1004 | 7 | -99.3% |
| 失败率 | 0.1317% | 0% | 协程版零失败 |
| 客户端平均 CPU 核数 | 1.52 | 2.07 | 协程版更高 |

计算方式：

```text
QPS 提升 = (58543.7 - 50633.6) / 50633.6 = 15.62%
P99 降低 = (43.29 - 34.59) / 43.29 = 20.10%
线程降低 = (1004 - 7) / 1004 = 99.30%
```

这些是 RPC 框架微基准，不是实际文件上传速度、下载速度或云盘用户数。简历应该写“自研压测
程序模拟的 1000 逻辑并发”，不能写“云盘每秒同步 5.85 万个文件”。

### 8.1 为什么测试 100、200、400 个业务线程

4ms 模拟业务下，服务端业务池的理想上限近似为：

```text
100 线程：100 / 0.004 = 25,000 QPS
200 线程：200 / 0.004 = 50,000 QPS
400 线程：400 / 0.004 = 100,000 QPS
```

100 和 200 业务线程时，两种客户端都接近 Provider 业务池上限，QPS 基本一致。提高到 400 是
为了在实验中移开服务端瓶颈，观察客户端调度模型差异，不是云盘实际部署 400 个业务线程。

### 8.2 无业务休眠的补充测试

关闭 4ms 休眠后，双方各完成 5 轮、每轮 10 万次 RPC：

```text
传统线程版平均 QPS：65,739.0
协程版平均 RPC QPS：71,321.6
提升约 8.5%
```

它更接近框架空载测试，可以作为补充，简历主数据仍建议使用带 P99 和失败率的 1000 并发矩阵。

## 9. 三分钟面试讲述

> 我把项目设定为个人云盘或课程资料同步服务的内部 RPC 框架。客户端重新登录或断网恢复后，
> Sync Gateway 需要调用 User/Metadata Service 查询目录版本和同步状态。文件内容本身通过
> 对象存储或分块数据通道传输，RPC 只处理登录、元数据和同步控制信息。
>
> 原有 Protobuf Stub 是同步阻塞的。假设很多虚拟客户端同时开始同步，传统模型需要大量请求线程
> 等待元数据服务返回。我不想把同步业务全部改成回调，所以把有栈协程库集成进 RPC 客户端。
> 通过 ucontext 保存 Fiber 上下文，hook connect、send 和 recv；系统调用遇到 EAGAIN 时把 fd
> 注册到 epoll，当前 Fiber yield，IOManager 转去执行其他同步请求，fd 就绪后再恢复。业务层
> 仍然调用同步 Stub，但网络等待不再占用调度线程。
>
> 通信层使用长度字段加 Protobuf Header 的 TCP 帧，Provider 基于 Muduo 拆包并把业务投递到
> 线程池；ZooKeeper 负责服务注册发现，连接池负责 TCP 连接复用。当前协议没有 request_id，
> 因此一条连接同时只能处理一个 RPC，这是后续准备改进的多路复用能力。
>
> 压测时我最初发现两版 QPS 基本一致，后来计算出服务端 100 个业务线程、每次 4ms 时理论上限
> 就是 2.5 万 QPS，瓶颈不在客户端。于是保持 1000 逻辑并发和 100 万请求不变，把业务线程
> 设置为 100、200、400。400 档中协程版成功 QPS 从 5.06 万提升到 5.85 万，P99 从 43.29ms
> 降到 34.59ms，客户端线程数从 1004 降到 7。不过协程版 CPU 从 1.52 核增加到 2.07 核，
> 所以我的结论不是协程一定节省 CPU，而是它减少了线程数量，并在客户端成为瓶颈时改善了成功
> 吞吐和尾延迟。

## 10. 高频追问

### 10.1 你真的运营过云盘吗？

没有。云盘是这套通用 RPC 框架的应用背景，当前交付物是内部 RPC 通信层和基准程序。我的工作
是实现并验证协程、网络协议、服务发现、连接池和对照压测，外部网盘 UI、对象存储和文件同步
协议属于可以继续扩展的业务模块。

### 10.2 为什么选择文件同步场景？

它是学生可以完成的实用原型，也能清楚地区分控制面和数据面：目录版本、文件哈希和同步状态是
小 RPC，文件内容用独立数据通道传输。这样既能解释高并发元数据等待，也不会把 RPC 说成大文件
传输协议。

### 10.3 仓库里为什么只有 Login，没有 MetadataService？

框架从通用 RPC 教学项目演进而来，保留 `UserService/Login` 作为最小可运行样例。它验证的是
固定大小轻量 RPC 的完整链路。框架通过 Protobuf Service/Method 描述符动态派发，增加
`MetadataService.GetManifest` 只需新增业务 Proto 和实现，不影响传输、连接池、协程和服务发现。

### 10.4 4ms 能同步一个大文件吗？

不能，也没有这么做。4ms 模拟的是账户、目录版本或同步状态查询。大文件通过对象存储或分块
数据通道传输，RPC 只返回元数据和同步动作。

### 10.5 5.85 万 QPS 是每秒同步 5.85 万个文件吗？

不是。这是单机 loopback 环境下固定小请求的 RPC 微基准成功 QPS，只用于比较线程客户端和协程
客户端。真实同步速度还受到文件大小、磁盘、网络带宽、对象存储和哈希计算限制。

### 10.6 一个 IOManager 还是四个 IOManager？

一个 IOManager，内部配置 4 个调度线程。`use_caller=true` 时调用者线程也是其中一个，另外
创建 3 个后台调度线程。1000 是 Fiber 数量，不是 IOManager 数量。

### 10.7 4 个线程为什么能运行 1000 个 Fiber？

只有就绪 Fiber 才占用调度线程。等待 RPC 响应的 Fiber 已注册 epoll 并 yield，调度线程可以执行
其他 Fiber。4 是本次实验配置，实际线程数应结合 CPU 核数和就绪任务比例调优。

### 10.8 业务只有 4ms，P99 为什么有 34ms？

P99 是客户端端到端延迟，包括连接池操作、网络收发、Provider 排队、Muduo 派发、等待业务线程、
Protobuf 编解码和 Fiber 再调度。1000 个请求同时在途时，排队时间会明显增加。

### 10.9 为什么服务端开 400 个业务线程？

这是为了移开服务端瓶颈的实验变量，不是实际部署建议。样例用 `sleep_for(4ms)` 模拟阻塞 I/O，
提高业务线程数能抬高 Provider 理论上限，从而暴露客户端调度模型差异。

### 10.10 为什么预热 1000 条连接？

当前协议没有 request_id，一条连接不能同时处理多个未完成请求，所以需要足够的独占连接维持
1000 逻辑并发。两个客户端预热相同数量是为了公平比较。生产化应增加 request_id 和
`request_id -> 等待 Fiber` 映射，用较少连接实现多路复用。

### 10.11 连接池和协程解决同一个问题吗？

不是。连接池减少 TCP 握手和慢启动，线程版同样能够使用；协程解决等待网络时占用线程的问题。
本次对照中两边使用相同连接池，比较的是客户端执行模型。

### 10.12 为什么协程版 CPU 更高？

hook、epoll、定时器、Fiber 切换、FdManager 和共享任务队列都需要 CPU。当前实现优化的是线程
数量、成功吞吐和 P99，不保证 CPU 秒下降。400 线程档中协程版平均使用 2.07 核，线程版 1.52 核。

### 10.13 为什么不用 C++20 coroutine？

目标是透明改造已有同步 Protobuf Stub。有栈协程可以在任意调用深度 yield，业务代码不用改成
`co_await`。C++20 coroutine 适合重新设计异步 API，但不会自动把已有阻塞调用变成非阻塞。

### 10.14 ZooKeeper 每次请求都查询吗？

不是。客户端启动时读取节点并维护本地缓存，后续 RPC 使用缓存；watcher 在节点变化时更新。
首次同步初始化在 IOManager 启动前完成，避免阻塞调度线程。

### 10.15 为什么不用 HTTP 调内部服务？

客户端到 Gateway 可以使用 HTTP/JSON，内部固定服务之间使用 Protobuf 能获得明确 Schema、更小
包体和生成式 Stub。ApacheBench 只能发送 HTTP，不能直接压当前自定义 Krpc 端口。

### 10.16 线程版失败会不会使 15.6% 的提升失真？

线程版三轮平均失败率为 0.1317%，远小于 15.6% 的成功 QPS 差距，不能只用失败解释吞吐差异。
但连接池尚未精确区分队列瞬时为空、容量耗尽和 connect 失败，所以简历应写“成功 QPS”，并在
面试中主动说明失败率。

### 10.17 这是专业压测工具测的吗？

这是为自定义 RPC 协议编写的闭环基准程序，不是 ApacheBench。每个线程或 Fiber 收到响应后继续
发下一个请求，并记录 QPS、P99、失败率、CPU 和线程数。它适合做同机对照，但不等于生产容量
评估。更完整的实验应使用独立发压机、固定 warm-up/measurement、open-loop 发压和 HDR
Histogram，并采集服务端 CPU、RSS、上下文切换和网络带宽。

## 11. 已实现能力与边界

当前已经实现：

- 同步 Protobuf Stub 调用；
- 自定义 TCP/Protobuf RPC 帧；
- Muduo Provider 和业务线程池；
- ZooKeeper 注册、服务发现和节点缓存；
- TCP 长连接池、连接预热、复用和坏连接淘汰；
- 有栈 Fiber、N:M Scheduler、IOManager、epoll 和定时器；
- `connect/send/recv/sleep` hook；
- 成功 QPS、失败率、CPU、进程线程数和 P99 统计。

当前尚未实现：

- 完整云盘前端和外部 HTTP/WebSocket Gateway；
- Metadata Service、对象存储和分块上传；
- 文件版本冲突处理和断点续传；
- request_id 和单连接多路复用；
- 连接池耗尽后的 Fiber 等待队列；
- 限流、熔断、链路追踪和 Prometheus 指标；
- 跨机器、独立压测机的生产级容量测试。

因此最准确的定位是：

> 这是一个以文件同步服务控制面为应用场景、通过虚拟并发完成性能验证的协程化 RPC 框架，
> 不是已经上线运营并完成大文件存储的完整云盘产品。

