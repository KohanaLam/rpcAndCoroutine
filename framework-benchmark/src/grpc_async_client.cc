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

namespace {

class AsyncBenchmark;

class CallChain
{
public:
    CallChain(rpcbench::UserService::Stub *stub,
              grpc::CompletionQueue *queue,
              AsyncBenchmark *benchmark,
              int requests)
        : stub_(stub), queue_(queue), benchmark_(benchmark), remaining_(requests)
    {
        request_.set_name("zhangsan");
        request_.set_pwd("123456");
    }

    void Start();
    void Complete(bool ok);

private:
    rpcbench::UserService::Stub *stub_;
    grpc::CompletionQueue *queue_;
    AsyncBenchmark *benchmark_;
    int remaining_;
    rpcbench::LoginRequest request_;
    rpcbench::LoginResponse response_;
    grpc::Status status_;
    std::unique_ptr<grpc::ClientContext> context_;
    std::unique_ptr<grpc::ClientAsyncResponseReader<rpcbench::LoginResponse>> reader_;
};

class AsyncBenchmark
{
public:
    AsyncBenchmark(const std::shared_ptr<grpc::Channel> &channel,
                   int concurrency,
                   int requests_per_chain,
                   int cq_threads)
        : stub_(rpcbench::UserService::NewStub(channel)),
          concurrency_(concurrency),
          requests_per_chain_(requests_per_chain),
          cq_threads_(cq_threads)
    {
        chains_.reserve(concurrency_);
        for (int i = 0; i < concurrency_; ++i)
        {
            chains_.emplace_back(
                std::make_unique<CallChain>(stub_.get(), &queue_, this, requests_per_chain_));
        }
    }

    void Record(bool success)
    {
        if (success)
        {
            success_.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
            failed_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void ChainFinished()
    {
        if (completed_chains_.fetch_add(1, std::memory_order_acq_rel) + 1 == concurrency_)
        {
            queue_.Shutdown();
        }
    }

    int Run()
    {
        const auto start = std::chrono::steady_clock::now();
        std::vector<std::thread> workers;
        workers.reserve(cq_threads_);
        for (int i = 0; i < cq_threads_; ++i)
        {
            workers.emplace_back([this]() {
                void *tag = nullptr;
                bool ok = false;
                while (queue_.Next(&tag, &ok))
                {
                    static_cast<CallChain *>(tag)->Complete(ok);
                }
            });
        }

        for (const std::unique_ptr<CallChain> &chain : chains_)
        {
            chain->Start();
        }

        for (std::thread &worker : workers)
        {
            worker.join();
        }

        const double seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start).count();
        const long long total = static_cast<long long>(concurrency_) * requests_per_chain_;
        std::cout << "mode: grpc_async\n"
                  << "CQ threads: " << cq_threads_ << '\n'
                  << "logical concurrency: " << concurrency_ << '\n'
                  << "total requests: " << total << '\n'
                  << "success: " << success_.load() << '\n'
                  << "failed: " << failed_.load() << '\n'
                  << "elapsed: " << seconds << " s\n"
                  << "success qps: " << success_.load() / seconds << std::endl;
        return failed_.load() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }

private:
    std::unique_ptr<rpcbench::UserService::Stub> stub_;
    grpc::CompletionQueue queue_;
    int concurrency_;
    int requests_per_chain_;
    int cq_threads_;
    std::vector<std::unique_ptr<CallChain>> chains_;
    std::atomic<long long> success_{0};
    std::atomic<long long> failed_{0};
    std::atomic<int> completed_chains_{0};
};

void CallChain::Start()
{
    context_ = std::make_unique<grpc::ClientContext>();
    context_->set_deadline(
        std::chrono::system_clock::now() + std::chrono::seconds(3));
    response_.Clear();
    status_ = grpc::Status();
    reader_ = stub_->PrepareAsyncLogin(context_.get(), request_, queue_);
    reader_->StartCall();
    reader_->Finish(&response_, &status_, this);
}

void CallChain::Complete(bool ok)
{
    benchmark_->Record(
        ok && status_.ok() && response_.result().errcode() == 0 && response_.success());
    --remaining_;
    if (remaining_ > 0)
    {
        Start();
    }
    else
    {
        benchmark_->ChainFinished();
    }
}

} // namespace

int main(int argc, char **argv)
{
    const std::string address = argc > 1 ? argv[1] : "127.0.0.1:9001";
    const int concurrency = argc > 2 ? std::atoi(argv[2]) : 1000;
    const int requests_per_chain = argc > 3 ? std::atoi(argv[3]) : 1000;
    const int cq_threads = argc > 4 ? std::atoi(argv[4]) : 4;

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

    AsyncBenchmark benchmark(channel, concurrency, requests_per_chain, cq_threads);
    return benchmark.Run();
}
