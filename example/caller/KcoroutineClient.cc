#include "KrpcLogger.h"
#include "BenchmarkStats.h"
#include "Krpcapplication.h"
#include "Krpcchannel.h"
#include "KrpcConnectPool.h"
#include "Krpccontroller.h"
#include "ServiceDiscovery.h"
#include "ioscheduler.h"
#include "../user.pb.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <unordered_map>

namespace {

struct FailureStats
{
    long long count = 0;
    int64_t total_latency_ns = 0;
    int64_t max_latency_ns = 0;
};

int LoadPositiveConfig(const std::string &key, int default_value)
{
    const std::string value = KrpcApplication::GetConfig().Load(key);
    return value.empty() ? default_value : std::atoi(value.c_str());
}

void RunRpcFiber(int requests,
                 int timeout_ms,
                 std::atomic<int> &success_count,
                 std::atomic<int> &fail_count,
                 std::vector<int64_t> &latencies,
                 std::unordered_map<std::string, FailureStats> &failure_stats,
                 std::mutex &failure_mutex)
{
    Kuser::UserServiceRpc_Stub stub(
        new KrpcChannel(false, timeout_ms),
        google::protobuf::Service::STUB_OWNS_CHANNEL);

    Kuser::LoginRequest request;
    request.set_name("zhangsan");
    request.set_pwd("123456");

    Krpccontroller controller;
    for (int i = 0; i < requests; ++i)
    {
        Kuser::LoginResponse response;
        controller.Reset();
        const auto request_start = std::chrono::steady_clock::now();
        stub.Login(&controller, &request, &response, nullptr);
        const int64_t latency_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - request_start).count();

        if (!controller.Failed() && response.result().errcode() == 0 && response.success())
        {
            success_count.fetch_add(1, std::memory_order_relaxed);
            latencies.push_back(latency_ns);
        }
        else
        {
            fail_count.fetch_add(1, std::memory_order_relaxed);

            std::string reason;
            if (controller.Failed())
            {
                reason = controller.ErrorText();
            }
            else if (response.result().errcode() != 0)
            {
                reason = "RPC response errcode=" +
                    std::to_string(response.result().errcode());
            }
            else if (!response.success())
            {
                reason = "RPC response success=false";
            }
            else
            {
                reason = "Unknown RPC failure";
            }

            std::lock_guard<std::mutex> lock(failure_mutex);
            FailureStats &stats = failure_stats[reason];
            ++stats.count;
            stats.total_latency_ns += latency_ns;
            if (latency_ns > stats.max_latency_ns)
            {
                stats.max_latency_ns = latency_ns;
            }
        }
    }
}

} // namespace

int main(int argc, char **argv)
{
    KrpcApplication::Init(argc, argv);
    KrpcLogger logger(argv[0]);

    const int scheduler_threads = LoadPositiveConfig("coroutine_threads", 4);
    const int fiber_count = LoadPositiveConfig("coroutine_count", 100);
    const int requests_per_fiber = LoadPositiveConfig("requests_per_fiber", 1000);
    const int timeout_ms = LoadPositiveConfig("rpc_timeout_ms", 3000);
    const int warmup_connections = LoadPositiveConfig("warmup_connections", 0);

    // ZooKeeper 的首次同步查询在进入协程调度前完成，避免占住调度线程。
    ServiceDiscovery &discovery = ServiceDiscovery::GetInstance();
    discovery.Init();
    if (discovery.GetTargetNode("UserServiceRpc", "bootstrap").empty())
    {
        std::cerr << "No UserServiceRpc provider found in ZooKeeper" << std::endl;
        return EXIT_FAILURE;
    }

    if (warmup_connections > 0)
    {
        const std::string ip = KrpcApplication::GetConfig().Load("rpcserverip");
        const uint16_t port = static_cast<uint16_t>(std::atoi(
            KrpcApplication::GetConfig().Load("rpcserverport").c_str()));
        KrpcConnectPool::GetInstance().WarmUp(ip, port, warmup_connections, timeout_ms);
    }

    std::atomic<int> success_count{0};
    std::atomic<int> fail_count{0};
    std::atomic<int> completed_fibers{0};
    std::atomic<int64_t> rpc_elapsed_ns{0};
    std::atomic<int64_t> rpc_cpu_ns{0};
    std::unordered_map<std::string, FailureStats> failure_stats;
    std::mutex failure_mutex;
    std::vector<std::vector<int64_t>> latency_groups(fiber_count);
    for (auto &group : latency_groups)
    {
        group.reserve(requests_per_fiber);
    }
    const int64_t cpu_start_ns = ProcessCpuNowNs();
    const auto start = std::chrono::steady_clock::now();
    int process_threads = 0;

    {
        sylar::IOManager io_manager(scheduler_threads, true, "krpc_io");
        for (int i = 0; i < fiber_count; ++i)
        {
            io_manager.scheduleLock([&, i, requests_per_fiber, timeout_ms]() {
                RunRpcFiber(requests_per_fiber, timeout_ms, success_count, fail_count,
                            latency_groups[i], failure_stats, failure_mutex);
                if (completed_fibers.fetch_add(1) + 1 == fiber_count)
                {
                    const auto rpc_end = std::chrono::steady_clock::now();
                    rpc_elapsed_ns.store(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(rpc_end - start).count());
                    rpc_cpu_ns.store(ProcessCpuNowNs() - cpu_start_ns);
                }
            });
        }
        process_threads = ReadProcessThreadCount();
    }

    const auto end = std::chrono::steady_clock::now();
    const double total_seconds = std::chrono::duration<double>(end - start).count();
    const double rpc_seconds = rpc_elapsed_ns.load() / 1e9;
    const double shutdown_seconds = total_seconds - rpc_seconds;
    const double cpu_seconds = rpc_cpu_ns.load() / 1e9;
    const long long total = static_cast<long long>(fiber_count) * requests_per_fiber;
    const double p99_ms = PercentileMs(latency_groups, 0.99);

    std::cout << "scheduler threads: " << scheduler_threads << '\n'
              << "rpc fibers: " << fiber_count << '\n'
              << "total requests: " << total << '\n'
              << "success: " << success_count.load() << '\n'
              << "failed: " << fail_count.load() << '\n'
              << "failure rate: " << fail_count.load() * 100.0 / total << "%\n"
              << "rpc elapsed: " << rpc_seconds << " s\n"
              << "process cpu seconds: " << cpu_seconds << " s\n"
              << "average cpu cores: " << cpu_seconds / rpc_seconds << '\n'
              << "process threads: " << process_threads << '\n'
              << "p99 latency: " << p99_ms << " ms\n"
              << "shutdown elapsed: " << shutdown_seconds << " s\n"
              << "total elapsed: " << total_seconds << " s\n"
              << "rpc qps: " << total / rpc_seconds << '\n'
              << "success qps: " << success_count.load() / rpc_seconds << '\n';

    for (const auto &entry : failure_stats)
    {
        const FailureStats &stats = entry.second;
        const double average_ms = stats.total_latency_ns / 1e6 / stats.count;
        const double max_ms = stats.max_latency_ns / 1e6;
        std::cout << "failure reason [" << entry.first << "]"
                  << ", count: " << stats.count
                  << ", average latency: " << average_ms << " ms"
                  << ", max latency: " << max_ms << " ms\n";
    }
    std::cout << std::flush;

    return fail_count.load() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
