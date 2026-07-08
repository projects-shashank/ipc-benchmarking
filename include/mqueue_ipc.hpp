#pragma once
#include "ipc_base.hpp"
#include <mqueue.h>
#include <string>

class MQSender : public IPCSender {
    mqd_t mq_ = (mqd_t)-1;
    std::string name_;
public:
    explicit MQSender(const std::string& name = "/ipc_bench_mq_0")
        : name_(name) {}
    bool setup() override;
    bool send(const void* buf, size_t len) override;
    void teardown() override;
};

class MQReceiver : public IPCReceiver {
    mqd_t mq_ = (mqd_t)-1;
    std::string name_;
public:
    explicit MQReceiver(const std::string& name = "/ipc_bench_mq_0")
        : name_(name) {}
    bool setup() override;
    ssize_t recv(void* buf, size_t len) override;
    void teardown() override;
};