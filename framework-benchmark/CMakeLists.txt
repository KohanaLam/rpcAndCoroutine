cmake_minimum_required(VERSION 3.16)
project(KrpcGrpcBenchmark LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Protobuf REQUIRED)
find_package(gRPC CONFIG REQUIRED)

set(PROTO_FILE ${CMAKE_CURRENT_SOURCE_DIR}/proto/benchmark.proto)
set(GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${GENERATED_DIR})

set(PROTO_SRC ${GENERATED_DIR}/benchmark.pb.cc)
set(PROTO_HDR ${GENERATED_DIR}/benchmark.pb.h)
set(GRPC_SRC ${GENERATED_DIR}/benchmark.grpc.pb.cc)
set(GRPC_HDR ${GENERATED_DIR}/benchmark.grpc.pb.h)

add_custom_command(
    OUTPUT ${PROTO_SRC} ${PROTO_HDR} ${GRPC_SRC} ${GRPC_HDR}
    COMMAND protobuf::protoc
    ARGS
        --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/proto
        --cpp_out=${GENERATED_DIR}
        --grpc_out=${GENERATED_DIR}
        --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
        ${PROTO_FILE}
    DEPENDS ${PROTO_FILE}
    VERBATIM)

add_library(grpc_benchmark_proto STATIC ${PROTO_SRC} ${GRPC_SRC})
target_include_directories(grpc_benchmark_proto PUBLIC ${GENERATED_DIR})
target_link_libraries(grpc_benchmark_proto PUBLIC protobuf::libprotobuf gRPC::grpc++)

foreach(target grpc_sync_server grpc_async_server grpc_sync_client grpc_async_client)
    add_executable(${target} src/${target}.cc)
    target_link_libraries(${target} PRIVATE grpc_benchmark_proto pthread)
    target_compile_options(${target} PRIVATE -Wall -Wextra)
endforeach()

get_filename_component(KRPC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
add_executable(
    krpc_delay_server
    src/krpc_delay_server.cc
    ${KRPC_ROOT}/example/user.pb.cc)
target_include_directories(
    krpc_delay_server PRIVATE
    ${KRPC_ROOT}/src/include
    ${KRPC_ROOT}/example)
target_link_libraries(
    krpc_delay_server PRIVATE
    ${KRPC_ROOT}/build-release/src/libkrpc_core.a
    protobuf::libprotobuf
    pthread
    zookeeper_mt
    muduo_net
    muduo_base
    glog)
target_compile_options(krpc_delay_server PRIVATE -Wall -Wextra)

set_target_properties(
    grpc_sync_server grpc_async_server grpc_sync_client grpc_async_client krpc_delay_server
    PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/bin)
