// src/mqueue_ipc.cpp
// POSIX message queue implementation.
//
// Design notes:
// - Receiver creates the queue with O_CREAT in setup().
// - Sender opens the existing queue with O_WRONLY in setup().
// - No blocking-open problem: mq_open() returns immediately on both sides.
// - Sender retries open() briefly in case receiver hasn't created it yet.
// - mq_msgsize set to 64KB to handle all benchmark message sizes.
// - mq_unlink() called in receiver teardown() — receiver created it.
// - Priority fixed at 0 — we don't need priority ordering for benchmarking.

#include "../include/mqueue_ipc.hpp"

#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <fstream>
#include <algorithm>

static size_t get_max_msgsize() {
    std::ifstream f("/proc/sys/fs/mqueue/msgsize_max");
    size_t val = 8192;
    if (f.is_open()) f >> val;
    return val;
}

static const size_t MQ_MSGSIZE = get_max_msgsize();
static const int    MQ_MAXMSG  = 10;

bool MQSender::setup() {
    int retries = 50;
    while (retries-- > 0) {
        mq_ = mq_open(name_.c_str(), O_WRONLY);
        if (mq_ != (mqd_t)-1) return true;
        if (errno != ENOENT) {
            std::cerr << "[MQSender] mq_open() failed: " << strerror(errno) << "\n";
            return false;
        }
        usleep(10000);
    }
    std::cerr << "[MQSender] timed out waiting for queue: " << name_ << "\n";
    return false;
}

bool MQSender::send(const void* buf, size_t len) {
    if (len > MQ_MSGSIZE) {
        std::cerr << "[MQSender] message too large\n";
        return false;
    }
    if (mq_send(mq_, static_cast<const char*>(buf), len, 0) == -1) {
        std::cerr << "[MQSender] mq_send() failed: " << strerror(errno) << "\n";
        return false;
    }
    return true;
}

void MQSender::teardown() {
    if (mq_ != (mqd_t)-1) { mq_close(mq_); mq_ = (mqd_t)-1; }
}

bool MQReceiver::setup() {
    struct mq_attr attr{};
    attr.mq_maxmsg  = MQ_MAXMSG;
    attr.mq_msgsize = static_cast<long>(MQ_MSGSIZE);

    mq_ = mq_open(name_.c_str(), O_CREAT | O_RDONLY, 0666, &attr);
    if (mq_ == (mqd_t)-1) {
        std::cerr << "[MQReceiver] mq_open() failed: " << strerror(errno) << "\n";
        return false;
    }
    return true;
}

ssize_t MQReceiver::recv(void* buf, size_t len) {
    ssize_t n = mq_receive(mq_, static_cast<char*>(buf),
                           std::max(len, MQ_MSGSIZE), nullptr);
    if (n == -1) {
        std::cerr << "[MQReceiver] mq_receive() failed: " << strerror(errno) << "\n";
        return -1;
    }
    return n;
}

void MQReceiver::teardown() {
    if (mq_ != (mqd_t)-1) { mq_close(mq_); mq_ = (mqd_t)-1; }
    mq_unlink(name_.c_str());
}