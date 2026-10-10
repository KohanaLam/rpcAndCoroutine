#include "Krpcapplication.h"
#include "Krpcprovider.h"
#include "user.pb.h"

#include <chrono>
#include <string>
#include <thread>

class UserService final : public Kuser::UserServiceRpc
{
public:
    void Login(::google::protobuf::RpcController *,
               const ::Kuser::LoginRequest *,
               ::Kuser::LoginResponse *response,
               ::google::protobuf::Closure *done) override
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
        response->mutable_result()->set_errcode(0);
        response->mutable_result()->set_errmsg("");
        response->set_success(true);
        done->Run();
    }
};

int main(int argc, char **argv)
{
    KrpcApplication::Init(argc, argv);
    KrpcProvider provider;
    provider.NotifyService(new UserService());
    provider.Run();
    return 0;
}
