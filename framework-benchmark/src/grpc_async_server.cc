#include "benchmark.grpc.pb.h"

#include <grpcpp/grpcpp.h>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace {

class BusinessExecutor
{
public:
    BusinessExecutor(int thread_count, int delay_ms) : delay_ms_(delay_ms)
    {
        workers_.reserve(thread_count);
        for (int i = 0; i < thread_count; ++i)
        {
            workers_.emplace_back([this]() {
                while (true)
                {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(mutex_);
                        condition_.wait(lock, [this]() {
                            return stopping_ || !tasks_.empty();
                        });
                        if (stopping_ && tasks_.empty())
                        {
                            return;
                        }
                        task = std::move(tasks_.front());
                        tasks_.pop();
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms_));
                    task();
                }
            });
        }
    }

    ~BusinessExecutor()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        condition_.notify_all();
        for (std::thread &worker : workers_)
        {
            worker.join();
        }
    }

    void Submit(std::function<void()> task)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tasks_.push(std::move(task));
        }
        condition_.notify_one();
    }

private:
    int delay_ms_;
    bool stopping_ = false;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::queue<std::function<void()>> tasks_;
    std::vector<std::thread> workers_;
};

class LoginCall
{
public:
    LoginCall(rpcbench::UserService::AsyncService *service,
              grpc::ServerCompletionQueue *queue,
              BusinessExecutor *executor)
        : service_(service), queue_(queue), executor_(executor), responder_(&context_)
    {
        Request();
    }

    void Proceed(bool ok)
    {
        if (state_ == State::Requesting)
        {
            if (!ok)
            {
                delete this;
                return;
            }

            new LoginCall(service_, queue_, executor_);
            if (executor_)
            {
                executor_->Submit([this]() { Finish(); });
            }
            else
            {
                Finish();
            }
            return;
        }

        delete this;
    }

private:
    enum class State
    {
        Requesting,
        Finishing
    };

    void Request()
    {
        state_ = State::Requesting;
        service_->RequestLogin(
            &context_, &request_, &responder_, queue_, queue_, this);
    }

    void Finish()
    {
        response_.mutable_result()->set_errcode(0);
        response_.mutable_result()->set_errmsg("");
        response_.set_success(true);
        state_ = State::Finishing;
        responder_.Finish(response_, grpc::Status::OK, this);
    }

    rpcbench::UserService::AsyncService *service_;
    grpc::ServerCompletionQueue *queue_;
    BusinessExecutor *executor_;
    grpc::ServerContext context_;
    rpcbench::LoginRequest request_;
    rpcbench::LoginResponse response_;
    grpc::ServerAsyncResponseWriter<rpcbench::LoginResponse> responder_;
    State state_ = State::Requesting;
};

} // namespace

int main(int argc, char **argv)
{
    const std::string address = argc > 1 ? argv[1] : "127.0.0.1:9001";
    const int cq_threads = argc > 2 ? std::atoi(argv[2]) : 4;
    const int pending_calls = argc > 3 ? std::atoi(argv[3]) : 1000;
    const int delay_ms = argc > 4 ? std::atoi(argv[4]) : 0;
    const int business_threads = argc > 5 ? std::atoi(argv[5]) : 400;

    rpcbench::UserService::AsyncService service;
    grpc::ServerBuilder builder;
    builder.AddListeningPort(address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    std::unique_ptr<grpc::ServerCompletionQueue> queue = builder.AddCompletionQueue();
    std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
    if (!server)
    {
        std::cerr << "failed to start gRPC async server on " << address << std::endl;
        return EXIT_FAILURE;
    }

    std::unique_ptr<BusinessExecutor> executor;
    if (delay_ms > 0)
    {
        executor = std::make_unique<BusinessExecutor>(business_threads, delay_ms);
    }

    for (int i = 0; i < pending_calls; ++i)
    {
        new LoginCall(&service, queue.get(), executor.get());
    }

    std::cout << "gRPC async server listening on " << address
              << ", CQ threads=" << cq_threads
              << ", pending calls=" << pending_calls
              << ", business delay=" << delay_ms << " ms"
              << ", business threads=" << business_threads << std::endl;

    std::vector<std::thread> workers;
    workers.reserve(cq_threads);
    for (int i = 0; i < cq_threads; ++i)
    {
        workers.emplace_back([&queue]() {
            void *tag = nullptr;
            bool ok = false;
            while (queue->Next(&tag, &ok))
            {
                static_cast<LoginCall *>(tag)->Proceed(ok);
            }
        });
    }

    for (std::thread &worker : workers)
    {
        worker.join();
    }
    return EXIT_SUCCESS;
}
