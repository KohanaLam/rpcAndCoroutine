#include "benchmark.grpc.pb.h"

#include <grpcpp/grpcpp.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace {

class UserServiceImpl final : public rpcbench::UserService::Service
{
public:
    explicit UserServiceImpl(int delay_ms) : delay_ms_(delay_ms) {}

    grpc::Status Login(grpc::ServerContext *,
                       const rpcbench::LoginRequest *,
                       rpcbench::LoginResponse *response) override
    {
        if (delay_ms_ > 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms_));
        }
        response->mutable_result()->set_errcode(0);
        response->mutable_result()->set_errmsg("");
        response->set_success(true);
        return grpc::Status::OK;
    }

private:
    int delay_ms_;
};

} // namespace

int main(int argc, char **argv)
{
    const std::string address = argc > 1 ? argv[1] : "127.0.0.1:9000";
    const int pollers = argc > 2 ? std::atoi(argv[2]) : 4;
    const int max_pollers = argc > 3 ? std::atoi(argv[3]) : 400;
    const int delay_ms = argc > 4 ? std::atoi(argv[4]) : 0;

    UserServiceImpl service(delay_ms);
    grpc::ServerBuilder builder;
    builder.AddListeningPort(address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    builder.SetSyncServerOption(grpc::ServerBuilder::NUM_CQS, pollers);
    builder.SetSyncServerOption(grpc::ServerBuilder::MIN_POLLERS, pollers);
    builder.SetSyncServerOption(grpc::ServerBuilder::MAX_POLLERS, max_pollers);

    std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
    if (!server)
    {
        std::cerr << "failed to start gRPC sync server on " << address << std::endl;
        return EXIT_FAILURE;
    }

    std::cout << "gRPC sync server listening on " << address
              << ", pollers=" << pollers
              << ", max_pollers=" << max_pollers
              << ", business delay=" << delay_ms << " ms" << std::endl;
    server->Wait();
    return EXIT_SUCCESS;
}
