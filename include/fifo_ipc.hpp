#pragma once
#include "ipc_base.hpp"
#include <string>

class FifoSender : public IPCSender {
    int fd_ = -1;
    std::string path_;
public:
    explicit FifoSender(const std::string& path = "/tmp/ipc_bench_0.fifo")
        : path_(path) {}
    bool setup() override;
    bool send(const void* buf, size_t len) override;
    void teardown() override;
};

class FifoReceiver : public IPCReceiver {
    int fd_ = -1;
    std::string path_;
public:
    explicit FifoReceiver(const std::string& path = "/tmp/ipc_bench_0.fifo")
        : path_(path) {}
    bool setup() override;
    ssize_t recv(void* buf, size_t len) override;
    void teardown() override;
};