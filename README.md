# KrpcCoroutine

KrpcCoroutine 将一个基于 Muduo、Protobuf、ZooKeeper 的 RPC 框架与基于
`ucontext_t`、epoll、定时器和系统调用 hook 的有栈协程库组合到同一个工程。

项目保留同步的 Protobuf Stub 调用方式，但当调用运行在 `IOManager` 调度的协程中时，
`connect/send/recv` 不再阻塞调度线程，而是在等待网络事件时让出当前协程。

## 构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

启动本机 ZooKeeper 后，分别运行：

```bash
./bin/server -i ./bin/test.conf
./bin/coroutine_client -i ./bin/test.conf
```

传统的 100 线程客户端可作为对照组：

```bash
./bin/client -i ./bin/test.conf
```

1000 万请求长测使用：

```bash
./bin/client -i ./bin/benchmark-100x.conf
./bin/coroutine_client -i ./bin/benchmark-100x.conf
```

两个配置默认都不预热连接，第一次调用动态建连，完成后归还连接池并持续复用。

1000 并发、每端 100 万请求的测试使用：

```bash
./bin/client -i ./bin/benchmark-1000-concurrency.conf
./bin/coroutine_client -i ./bin/benchmark-1000-concurrency.conf
```

服务端业务线程 100/200/400 的 1000 并发矩阵配置分别是：

```text
bin/benchmark-1000-concurrency.conf
bin/benchmark-1000-business-200.conf
bin/benchmark-1000-business-400.conf
```

完整设计、关键代码和调用流程见 [IMPLEMENTATION.md](IMPLEMENTATION.md)。
