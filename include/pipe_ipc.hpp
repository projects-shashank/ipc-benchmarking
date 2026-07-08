#pragma once
#include "ipc_base.hpp"
#include <memory>
#include <unistd.h>
#include <cstring>
#include <iostream>

struct PipeSharedState {
    int fd[2] = {-1, -1};

    PipeSharedState() {
        // pipe() MUST be called here — at factory time, before fork().
        // By the time setup() runs, fork() has already happened and
        // each process has its own copy of this struct.
        if (pipe(fd) == -1) {
            std::cerr << "[PipeSharedState] pipe() failed: "
                      << strerror(errno) << "\n";
        }
    }
};

class PipeSender : public IPCSender {
    std::shared_ptr<PipeSharedState> state_;
public:
    explicit PipeSender(std::shared_ptr<PipeSharedState> s) : state_(std::move(s)) {}
    bool setup() override;
    bool send(const void* buf, size_t len) override;
    void teardown() override;
};

class PipeReceiver : public IPCReceiver {
    std::shared_ptr<PipeSharedState> state_;
public:
    explicit PipeReceiver(std::shared_ptr<PipeSharedState> s) : state_(std::move(s)) {}
    bool setup() override;
    ssize_t recv(void* buf, size_t len) override;
    void teardown() override;
};