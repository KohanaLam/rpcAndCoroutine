#include "benchmark.grpc.pb.h"

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char **argv)
{
    const std::string address = argc > 1 ? argv[1] : "127.0.0.1:9000";
    const int thread_count = argc > 2 ? std::atoi(argv[2]) : 1000;
    const int requests_per_thread = argc > 3 ? std::atoi(argv[3]) : 1000;

    grpc::ChannelArguments channel_arguments;
    channel_arguments.SetInt("grpc.enable_http_proxy", 0);
    std::shared_ptr<grpc::Channel> channel = grpc::CreateCustomChannel(
        address, grpc::InsecureChannelCredentials(), channel_arguments);
    if (!channel->WaitForConnected(
            std::chrono::system_clock::now() + std::chrono::seconds(10)))
    {
        std::cerr << "failed to connect to " << address << std::endl;
        return EXIT_FAILURE;
    }

    std::atomic<long long> success{0};
    std::atomic<long long> failed{0};
    std::vector<std::thread> threads;
    threads.reserve(thread_count);

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < thread_count; ++i)
    {
        threads.emplace_back([channel, requests_per_thread, &success, &failed]() {
            std::unique_ptr<rpcbench::UserService::Stub> stub =
                rpcbench::UserService::NewStub(channel);
            rpcbench::LoginRequest request;
            request.set_name("zhangsan");
            request.set_pwd("123456");

            for (int j = 0; j < requests_per_thread; ++j)
            {
                grpc::ClientContext context;
                context.set_deadline(
                    std::chrono::system_clock::now() + std::chrono::seconds(3));
                rpcbench::LoginResponse response;
                const grpc::Status status = stub->Login(&context, request, &response);
                if (status.ok() && response.result().errcode() == 0 && response.success())
                {
                    success.fetch_add(1, std::memory_order_relaxed);
                }
                else
                {
                    failed.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    for (std::thread &thread : threads)
    {
        thread.join();
    }
    const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    const long long total = static_cast<long long>(thread_count) * requests_per_thread;

    std::cout << "mode: grpc_sync\n"
              << "logical concurrency: " << thread_count << '\n'
              << "total requests: " << total << '\n'
              << "success: " << success.load() << '\n'
              << "failed: " << failed.load() << '\n'
              << "elapsed: " << seconds << " s\n"
              << "success qps: " << success.load() / seconds << std::endl;
    return failed.load() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
