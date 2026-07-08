cmake_minimum_required(VERSION 3.16)
project(ipc_bench CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(-Wall -Wextra -O2)

include_directories(include bench)

set(IPC_SOURCES
    src/ipc_factory.cpp
    src/pipe_ipc.cpp
    src/fifo_ipc.cpp
    src/mqueue_ipc.cpp
    src/shm_ipc.cpp
    src/rdma_shm_ipc.cpp
    src/unix_socket_ipc.cpp
)

add_executable(ipc_bench
    ${IPC_SOURCES}
    bench/harness.cpp
    bench/main.cpp
)

target_link_libraries(ipc_bench
    rt
    pthread
)