// strace_test.cpp
// Minimal program that sends 10 messages through a mechanism.
// Used ONLY for strace syscall counting — not for benchmarking.
// No ping-pong, no timing, just raw send/receive.
//
// Compile: g++ -O2 -std=c++17 -I../include strace_test.cpp
//          ../src/pipe_ipc.cpp ../src/rdma_shm_ipc.cpp
//          ../src/ipc_factory.cpp ../src/fifo_ipc.cpp
//          ../src/mqueue_ipc.cpp ../src/shm_ipc.cpp
//          ../src/unix_socket_ipc.cpp -lrt -lpthread -o strace_test

#include <iostream>
#include <cstring>
#include <unistd.h>
#include <sys/wait.h>
#include "ipc_base.hpp"

int main(int argc, char* argv[]) {
    std::string mechanism = (argc > 1) ? argv[1] : "pipe";
    int count = 10;

    IPCPair pair = make_ipc_pair(mechanism, 0);
    if (!pair.sender || !pair.receiver) {
        std::cerr << "unknown mechanism\n";
        return 1;
    }

    const char* msg    = "hello strace";
    size_t      msglen = strlen(msg);
    char        buf[256] = {};

    pid_t pid = fork();
    if (pid == 0) {
        // child — receiver
        pair.receiver->setup();
        for (int i = 0; i < count; i++) {
            pair.receiver->recv(buf, msglen);
        }
        pair.receiver->teardown();
        _exit(0);
    }

    // parent — sender
    usleep(20000); // 20ms — let receiver set up
    pair.sender->setup();
    for (int i = 0; i < count; i++) {
        pair.sender->send(msg, msglen);
    }
    pair.sender->teardown();

    waitpid(pid, nullptr, 0);
    return 0;
}
