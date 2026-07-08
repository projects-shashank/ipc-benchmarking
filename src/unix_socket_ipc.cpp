// src/unix_socket_ipc.cpp
// Unix domain socket IPC implementation.
//
// Design notes:
// - Receiver creates and binds the socket file, calls listen(), accept().
// - Sender retries connect() until server is ready — same pattern as FIFO.
// - Two fds on receiver side: server_fd_ (listening) and client_fd_
//   (returned by accept() for the actual connection). We keep server_fd_
//   open until teardown so accept() doesn't block on reconnects.
// - unlink() in teardown() removes the socket file — same as FIFO cleanup.
// - Write and read loops handle partial I/O exactly like pipe/FIFO.

#include "../include/unix_socket_ipc.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <iostream>

bool UnixSocketSender::setup() {
    sock_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_fd_ == -1) {
        std::cerr << "[UnixSocketSender] socket() failed: " << strerror(errno) << "\n";
        return false;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path_.c_str(), sizeof(addr.sun_path) - 1);

    int retries = 50;
    while (retries-- > 0) {
        if (connect(sock_fd_,
                    reinterpret_cast<struct sockaddr*>(&addr),
                    sizeof(addr)) == 0) return true;
        if (errno != ENOENT && errno != ECONNREFUSED) {
            std::cerr << "[UnixSocketSender] connect() failed: " << strerror(errno) << "\n";
            close(sock_fd_); sock_fd_ = -1;
            return false;
        }
        usleep(10000);
    }
    std::cerr << "[UnixSocketSender] timed out\n";
    close(sock_fd_); sock_fd_ = -1;
    return false;
}

bool UnixSocketSender::send(const void* buf, size_t len) {
    const char* ptr  = static_cast<const char*>(buf);
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t n = ::send(sock_fd_, ptr, remaining, MSG_NOSIGNAL);
        if (n == -1) {
            if (errno == EINTR) continue;
            std::cerr << "[UnixSocketSender] send() failed: " << strerror(errno) << "\n";
            return false;
        }
        ptr       += n;
        remaining -= static_cast<size_t>(n);
    }
    return true;
}

void UnixSocketSender::teardown() {
    if (sock_fd_ != -1) { close(sock_fd_); sock_fd_ = -1; }
}

bool UnixSocketReceiver::setup() {
    unlink(path_.c_str());
    server_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd_ == -1) {
        std::cerr << "[UnixSocketReceiver] socket() failed: " << strerror(errno) << "\n";
        return false;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path_.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(server_fd_,
             reinterpret_cast<struct sockaddr*>(&addr),
             sizeof(addr)) == -1) {
        std::cerr << "[UnixSocketReceiver] bind() failed: " << strerror(errno) << "\n";
        close(server_fd_); server_fd_ = -1;
        return false;
    }
    if (listen(server_fd_, 1) == -1) {
        std::cerr << "[UnixSocketReceiver] listen() failed: " << strerror(errno) << "\n";
        close(server_fd_); server_fd_ = -1;
        return false;
    }
    client_fd_ = accept(server_fd_, nullptr, nullptr);
    if (client_fd_ == -1) {
        std::cerr << "[UnixSocketReceiver] accept() failed: " << strerror(errno) << "\n";
        close(server_fd_); server_fd_ = -1;
        return false;
    }
    return true;
}

ssize_t UnixSocketReceiver::recv(void* buf, size_t len) {
    char*  ptr       = static_cast<char*>(buf);
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t n = ::recv(client_fd_, ptr, remaining, 0);
        if (n == -1) {
            if (errno == EINTR) continue;
            std::cerr << "[UnixSocketReceiver] recv() failed: " << strerror(errno) << "\n";
            return -1;
        }
        if (n == 0) return static_cast<ssize_t>(len - remaining);
        ptr       += n;
        remaining -= static_cast<size_t>(n);
    }
    return static_cast<ssize_t>(len);
}

void UnixSocketReceiver::teardown() {
    if (client_fd_ != -1) { close(client_fd_); client_fd_ = -1; }
    if (server_fd_ != -1) { close(server_fd_); server_fd_ = -1; }
    unlink(path_.c_str());
}