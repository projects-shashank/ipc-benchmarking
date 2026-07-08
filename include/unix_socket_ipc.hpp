#pragma once
#include "ipc_base.hpp"
#include <string>

class UnixSocketSender : public IPCSender {
    int sock_fd_ = -1;
    std::string path_;
public:
    explicit UnixSocketSender(const std::string& path = "/tmp/ipc_bench_0.sock")
        : path_(path) {}
    bool setup() override;
    bool send(const void* buf, size_t len) override;
    void teardown() override;
};

class UnixSocketReceiver : public IPCReceiver {
    int server_fd_ = -1;
    int client_fd_ = -1;
    std::string path_;
public:
    explicit UnixSocketReceiver(const std::string& path = "/tmp/ipc_bench_0.sock")
        : path_(path) {}
    bool setup() override;
    ssize_t recv(void* buf, size_t len) override;
    void teardown() override;
};