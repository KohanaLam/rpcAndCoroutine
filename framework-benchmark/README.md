# Krpc 与 gRPC QPS 对照测试

该目录是独立基准工程，不修改原 Krpc/协程源码。

正式三轮测试的原始 QPS、平均值、比较结论和边界说明见 [RESULTS.md](RESULTS.md)。

## 固定口径

- Release 构建、单机 loopback。
- 支持无业务休眠和固定 4 ms 业务耗时两组测试。
- 1000 逻辑并发，每个执行单元顺序调用 1000 次，总计 100 万 RPC。
- QPS = 成功响应数量 / 客户端墙钟时间。
- Krpc 使用现有 Provider 和现有线程/协程客户端。
- gRPC 同步组使用同步客户端与同步服务端。
- gRPC 异步组使用 4 个 CompletionQueue 客户端线程与 4 个异步服务端 CQ 线程。

## 比较组

```text
Krpc thread client   -> Krpc Provider
Krpc coroutine client -> Krpc Provider

gRPC sync client     -> gRPC sync server :9000
gRPC async client    -> gRPC async server :9001
```

Krpc 线上响应帧没有用于并发多路复用的 request ID，1000 个在途调用需要接近 1000 条
池连接；gRPC 使用原生 HTTP/2 Channel 多路复用。连接模型差异属于完整框架能力的一部分，
不强制改成相同连接数。

## 构建

```bash
cmake -S framework-benchmark -B framework-benchmark/build -DCMAKE_BUILD_TYPE=Release
cmake --build framework-benchmark/build -j4
```

## 手工运行

```bash
# gRPC sync
./framework-benchmark/build/bin/grpc_sync_server 127.0.0.1:9000 4 100 0
./framework-benchmark/build/bin/grpc_sync_client 127.0.0.1:9000 1000 1000

# gRPC async
./framework-benchmark/build/bin/grpc_async_server 127.0.0.1:9001 4 1000
./framework-benchmark/build/bin/grpc_async_client 127.0.0.1:9001 1000 1000 4

# fixed 4 ms business delay
./framework-benchmark/build/bin/krpc_delay_server -i ./bin/benchmark-1000-business-400.conf
./framework-benchmark/build/bin/grpc_sync_server 127.0.0.1:9000 4 100 4
./framework-benchmark/build/bin/grpc_async_server 127.0.0.1:9001 4 1000 4 400
```

原 Krpc 使用 `bin/benchmark-1000-business-400.conf`，其中服务端业务线程为 400、两个客户端
预热 1000 条连接。`krpc_delay_server` 固定等待 4 ms，且不修改原 `Kserver.cc`。
