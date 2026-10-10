#获取服务端的源文件
set(SERVER_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/Kserver.cc)

#获取protobuf生成的.cc
set(PROTO_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/../user.pb.cc)

#创建服务端可执行文件
add_executable(server ${SERVER_SRCS} ${PROTO_SRCS})

#链接必要的库，尤其是example中生成的静态库
target_link_libraries(server krpc_core ${LIBS})

#设置编译选项
target_compile_options(server PRIVATE -Wall -Wextra)

# 设置 server 可执行文件输出目录
set_target_properties(server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/bin)
