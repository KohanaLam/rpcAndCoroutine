#include "Krpcapplication.h"
#include "BenchmarkStats.h"
#include "../user.pb.h"
#include "Krpccontroller.h"
#include "ServiceDiscovery.h"
#include <iostream>
#include <atomic>
#include <thread>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include "KrpcLogger.h"
#include "KrpcConnectPool.h"

namespace {

int LoadIntConfig(const std::string &key, int default_value)
{
    const std::string value = KrpcApplication::GetConfig().Load(key);
    return value.empty() ? default_value : std::atoi(value.c_str());
}

} // namespace

void send_request(std::atomic<int> &success_count,
                  std::atomic<int> &fail_count,
                  int requests_per_thread,
                  int timeout_ms,
                  std::unordered_map<std::string, int> &failure_reasons,
                  std::mutex &failure_mutex,
                  std::vector<int64_t> &latencies)
{
    // 注意这里增加了 STUB_OWNS_CHANNEL 自动管理 Channel 内存
    Kuser::UserServiceRpc_Stub stub(
        new KrpcChannel(false, timeout_ms),
        google::protobuf::Service::STUB_OWNS_CHANNEL);

    Kuser::LoginRequest request;
    request.set_name("zhangsan");
    request.set_pwd("123456");

    Kuser::LoginResponse response;
    Krpccontroller controller;

    for (int i = 0; i < requests_per_thread; ++i)
    {
        // 【关键修复 1】：每次请求前必须重置控制器，否则一次失败会导致后续全盘误判为失败！
        controller.Reset();

        const auto request_start = std::chrono::steady_clock::now();
        stub.Login(&controller, &request, &response, nullptr);
        const int64_t latency_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - request_start).count();

        if (controller.Failed())
        {
            // 失败时不要频繁打印 cout，这会极其严重地拖慢多线程性能，改成计数即可
            // std::cout << controller.ErrorText() << std::endl;
            fail_count++;
            std::lock_guard<std::mutex> lock(failure_mutex);
            failure_reasons[controller.ErrorText()]++;
        }
        else
        {
            if (int{} == response.result().errcode())
            {
                success_count++;
                latencies.push_back(latency_ns);
            }
            else
            {
                fail_count++;
            }
        }
    }
}

int main(int argc, char **argv)
{
    KrpcApplication::Init(argc, argv);
    FLAGS_logbufsecs = 5;
    KrpcLogger logger("MyRPC");
    const int thread_count = LoadIntConfig("thread_count", 100);
    const int requests_per_thread = LoadIntConfig("requests_per_thread", 1000);
    const int warmup_connections = LoadIntConfig("warmup_connections", 0);
    const int timeout_ms = LoadIntConfig("rpc_timeout_ms", 3000);

    ServiceDiscovery &discovery = ServiceDiscovery::GetInstance();
    discovery.Init();
    if (discovery.GetTargetNode("UserServiceRpc", "bootstrap").empty())
    {
        return EXIT_FAILURE;
    }

    if (warmup_connections > 0)
    {
        const std::string ip = KrpcApplication::GetConfig().Load("rpcserverip");
        const uint16_t port = std::atoi(
            KrpcApplication::GetConfig().Load("rpcserverport").c_str());
        KrpcConnectPool::GetInstance().WarmUp(ip, port, warmup_connections);
    }

    std::vector<std::thread> threads;
    std::atomic<int> success_count(0);
    std::atomic<int> fail_count(0);
    std::unordered_map<std::string, int> failure_reasons;
    std::mutex failure_mutex;
    std::vector<std::vector<int64_t>> latency_groups(thread_count);
    for (auto &group : latency_groups)
    {
        group.reserve(requests_per_thread);
    }

    const int64_t cpu_start_ns = ProcessCpuNowNs();
    const auto start_time = std::chrono::steady_clock::now();

    for (int i = 0; i < thread_count; i++)
    {
        threads.emplace_back([i, &success_count, &fail_count, requests_per_thread,
                              timeout_ms, &failure_reasons, &failure_mutex,
                              &latency_groups]() {
            send_request(success_count, fail_count, requests_per_thread, timeout_ms,
                         failure_reasons, failure_mutex, latency_groups[i]);
        });
    }

    const int process_threads = ReadProcessThreadCount();

    for (auto &t : threads)
    {
        t.join();
    }

    const auto end_time = std::chrono::steady_clock::now();
    const int64_t cpu_end_ns = ProcessCpuNowNs();
    std::chrono::duration<double> elapsed = end_time - start_time;
    const double cpu_seconds = (cpu_end_ns - cpu_start_ns) / 1e9;
    const double p99_ms = PercentileMs(latency_groups, 0.99);
    const long long total_requests = static_cast<long long>(thread_count) * requests_per_thread;

    LOG(INFO) << "Total requests: " << total_requests;
    LOG(INFO) << "Warmup connections: " << warmup_connections;
    LOG(INFO) << "Success count: " << success_count;
    LOG(INFO) << "Fail count: " << fail_count;
    LOG(INFO) << "Elapsed time: " << elapsed.count() << " seconds";
    LOG(INFO) << "QPS: " << total_requests / elapsed.count();
    LOG(INFO) << "Success QPS: " << success_count.load() / elapsed.count();
    LOG(INFO) << "Failure rate: " << fail_count.load() * 100.0 / total_requests << "%";
    LOG(INFO) << "Process CPU seconds: " << cpu_seconds;
    LOG(INFO) << "Average CPU cores: " << cpu_seconds / elapsed.count();
    LOG(INFO) << "Process threads: " << process_threads;
    LOG(INFO) << "P99 latency: " << p99_ms << " ms";
    for (const auto &entry : failure_reasons)
    {
        LOG(INFO) << "Failure reason [" << entry.first << "]: " << entry.second;
    }

    return 0;
}
