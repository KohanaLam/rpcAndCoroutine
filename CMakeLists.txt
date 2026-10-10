cmake_minimum_required(VERSION 3.16)
project(KrpcCoroutine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Protobuf REQUIRED)

set(LIBS
    protobuf
    pthread
    zookeeper_mt
    muduo_net
    muduo_base
    glog
)

add_subdirectory(coroutine)
add_subdirectory(src)
add_subdirectory(example)

enable_testing()
add_subdirectory(tests)
