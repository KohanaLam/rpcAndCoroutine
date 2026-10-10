#获取protobuf生成的.cc
set(PROTO_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/../user.pb.cc)

# 传统多线程客户端，用作对照组
add_executable(client Kclient.cc ${PROTO_SRCS})

# 少量调度线程运行大量同步 RPC 协程
add_executable(coroutine_client KcoroutineClient.cc ${PROTO_SRCS})

#链接必要的库，尤其是example中生成的静态库
target_link_libraries(client krpc_core ${LIBS})
target_link_libraries(coroutine_client krpc_core coroutine ${LIBS})

# 设置编译选项
target_compile_options(client PRIVATE -Wall -Wextra)
target_compile_options(coroutine_client PRIVATE -Wall -Wextra)

# 设置 client 可执行文件输出目录
set_target_properties(client coroutine_client PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/bin)
