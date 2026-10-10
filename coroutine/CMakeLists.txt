add_library(coroutine STATIC
    fd_manager.cpp
    fiber.cpp
    hook.cpp
    ioscheduler.cpp
    scheduler.cpp
    thread.cpp
    timer.cpp
)

target_include_directories(coroutine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(coroutine PUBLIC pthread dl)
target_compile_options(coroutine PRIVATE -Wall -Wextra)
