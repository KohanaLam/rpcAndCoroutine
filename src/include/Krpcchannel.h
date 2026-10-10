#ifndef _Krpcchannel_h_
#define _Krpcchannel_h_

#include <google/protobuf/service.h>
#include <cstdint>
#include <unistd.h>
#include "ServiceDiscovery.h"

class KrpcChannel : public google::protobuf::RpcChannel
{
public:
    explicit KrpcChannel(bool connectNow = false, uint64_t timeout_ms = 3000)
        : timeout_ms_(timeout_ms)
    {
        (void)connectNow;
    }
    
    // 【关键修改点】：析构函数也直接实现
    virtual ~KrpcChannel() override {}

    void CallMethod(const ::google::protobuf::MethodDescriptor *method,
                    ::google::protobuf::RpcController *controller,
                    const ::google::protobuf::Message *request,
                    ::google::protobuf::Message *response,
                    ::google::protobuf::Closure *done) override;

private:
    ssize_t send_exact(int fd, const char *buf, size_t size);
    ssize_t recv_exact(int fd, char *buf, size_t size);
    uint64_t timeout_ms_;
};

#endif
