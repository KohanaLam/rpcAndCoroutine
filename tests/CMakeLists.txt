add_executable(coroutine_io_test coroutine_io_test.cc)
target_link_libraries(coroutine_io_test PRIVATE coroutine)
target_compile_options(coroutine_io_test PRIVATE -Wall -Wextra)

add_test(NAME coroutine_io_test COMMAND coroutine_io_test)
