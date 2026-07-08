#include "../include/fifo_ipc.hpp"

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <cerrno>
#include <cstring>
#include <iostream>

bool FifoSender::setup() {
    int retries = 50;
    while (retries-- > 0) {
        fd_ = open(path_.c_str(), O_WRONLY);
        if (fd_ != -1) return true;
        if (errno != ENOENT) {
            std::cerr << "[FifoSender] open() failed: " << strerror(errno) << "\n";
            return false;
        }
        usleep(10000);
    }
    std::cerr << "[FifoSender] timed out waiting for FIFO: " << path_ << "\n";
    return false;
}

bool FifoSender::send(const void* buf, size_t len) {
    const char* ptr  = static_cast<const char*>(buf);
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t n = write(fd_, ptr, remaining);
        if (n == -1) {
            if (errno == EINTR) continue;
            std::cerr << "[FifoSender] write() failed: " << strerror(errno) << "\n";
            return false;
        }
        ptr       += n;
        remaining -= static_cast<size_t>(n);
    }
    return true;
}

void FifoSender::teardown() {
    if (fd_ != -1) { close(fd_); fd_ = -1; }
}

bool FifoReceiver::setup() {
    if (mkfifo(path_.c_str(), 0666) == -1) {
        if (errno != EEXIST) {
            std::cerr << "[FifoReceiver] mkfifo() failed: " << strerror(errno) << "\n";
            return false;
        }
    }
    fd_ = open(path_.c_str(), O_RDONLY);
    if (fd_ == -1) {
        std::cerr << "[FifoReceiver] open() failed: " << strerror(errno) << "\n";
        return false;
    }
    return true;
}

ssize_t FifoReceiver::recv(void* buf, size_t len) {
    char*  ptr       = static_cast<char*>(buf);
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t n = read(fd_, ptr, remaining);
        if (n == -1) {
            if (errno == EINTR) continue;
            std::cerr << "[FifoReceiver] read() failed: " << strerror(errno) << "\n";
            return -1;
        }
        if (n == 0) return static_cast<ssize_t>(len - remaining);
        ptr       += n;
        remaining -= static_cast<size_t>(n);
    }
    return static_cast<ssize_t>(len);
}

void FifoReceiver::teardown() {
    if (fd_ != -1) { close(fd_); fd_ = -1; }
    unlink(path_.c_str());
}