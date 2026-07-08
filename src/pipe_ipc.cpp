// src/pipe_ipc.cpp
// Implementation of pipe-based IPC.
//
// Design notes:
// - pipe() is called in PipeSender::setup() which runs BEFORE fork().
//   This is the only way to share the fd pair — pipe() must be called
//   in the parent so both sides inherit it after fork().
// - PipeReceiver::setup() just closes the write end — it doesn't call pipe()
//   again. The fd pair already exists from PipeSender::setup().
// - send() uses a write loop to handle partial writes (POSIX allows write()
//   to return less than requested if interrupted by a signal).
// - recv() uses a read loop for the same reason.

#include "../include/pipe_ipc.hpp"

#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <iostream>

bool PipeSender::setup() {
    // pipe() already called in PipeSharedState constructor.
    // Just close the read end — sender only writes.
    if (state_->fd[0] != -1) {
        close(state_->fd[0]);
        state_->fd[0] = -1;
    }
    return state_->fd[1] != -1;
}

bool PipeSender::send(const void* buf, size_t len) {
    const char* ptr = static_cast<const char*>(buf);
    size_t remaining = len;

    while (remaining > 0) {
        ssize_t n = write(state_->fd[1], ptr, remaining);
        if (n == -1) {
            if (errno == EINTR) continue;
            std::cerr << "[PipeSender] write() failed: " << strerror(errno) << "\n";
            return false;
        }
        ptr       += n;
        remaining -= static_cast<size_t>(n);
    }
    return true;
}

void PipeSender::teardown() {
    if (state_->fd[1] != -1) {
        close(state_->fd[1]);
        state_->fd[1] = -1;
    }
}

bool PipeReceiver::setup() {
    // Close the write end — receiver only reads.
    if (state_->fd[1] != -1) {
        close(state_->fd[1]);
        state_->fd[1] = -1;
    }
    return state_->fd[0] != -1;
}

ssize_t PipeReceiver::recv(void* buf, size_t len) {
    char* ptr = static_cast<char*>(buf);
    size_t remaining = len;

    while (remaining > 0) {
        ssize_t n = read(state_->fd[0], ptr, remaining);
        if (n == -1) {
            if (errno == EINTR) continue;
            std::cerr << "[PipeReceiver] read() failed: " << strerror(errno) << "\n";
            return -1;
        }
        if (n == 0) {
            return static_cast<ssize_t>(len - remaining);
        }
        ptr       += n;
        remaining -= static_cast<size_t>(n);
    }
    return static_cast<ssize_t>(len);
}

void PipeReceiver::teardown() {
    if (state_->fd[0] != -1) {
        close(state_->fd[0]);
        state_->fd[0] = -1;
    }
}