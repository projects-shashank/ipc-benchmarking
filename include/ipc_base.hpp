// include/ipc_base.hpp
// Abstract base classes for all IPC mechanisms.
// The benchmark harness only ever sees these interfaces —
// it never knows which concrete mechanism is running underneath.

#pragma once

#include <cstddef>      // size_t
#include <memory>       // std::unique_ptr
#include <string>       // std::string
#include <utility>      // std::pair

// ── IPCSender ─────────────────────────────────────────────────────────────────
class IPCSender {
public:
    virtual ~IPCSender() = default;

    // setup() performs all OS-level initialization — pipe(), shm_open(), etc.
    // Called once before any send() calls.
    // Returns true on success, false on failure (check errno).
    // Reason for not doing this in constructor: OS calls can fail,
    // and reporting errors from constructors is awkward.
    virtual bool setup() = 0;

    // send() transmits exactly len bytes from buf to the receiver.
    // Blocks until the data is accepted by the mechanism.
    // Returns true on success, false on error.
    virtual bool send(const void* buf, size_t len) = 0;

    // teardown() releases all OS resources — closes fds, unlinks names,
    // unmaps shared memory. Called once after all send() calls are done.
    virtual void teardown() = 0;
};

// ── IPCReceiver ───────────────────────────────────────────────────────────────
class IPCReceiver {
public:
    virtual ~IPCReceiver() = default;

    // setup() mirrors IPCSender::setup() — initializes the receive side.
    // For mechanisms that require both sides to set up independently
    // (e.g. Unix socket: server binds, client connects), each side
    // does its own setup in the right order.
    virtual bool setup() = 0;

    // recv() reads exactly len bytes into buf from the sender.
    // Blocks until data is available.
    // Returns number of bytes received, -1 on error.
    // Why ssize_t: allows returning -1 for error, unlike size_t.
    virtual ssize_t recv(void* buf, size_t len) = 0;

    // teardown() releases receive-side OS resources.
    virtual void teardown() = 0;
};

// ── IPCPair ───────────────────────────────────────────────────────────────────
// A matched sender + receiver for one mechanism.
// Both share any state they need to coordinate
// (e.g. shared memory fd, pipe file descriptors).
struct IPCPair {
    std::unique_ptr<IPCSender>   sender;
    std::unique_ptr<IPCReceiver> receiver;
};

// ── Factory ───────────────────────────────────────────────────────────────────
// Returns the right concrete IPCPair for the given mechanism name.
// Valid names: "pipe", "fifo", "mq", "shm", "rdma_shm", "unix"
// Returns an empty IPCPair (nullptr sender/receiver) for unknown names.
IPCPair make_ipc_pair(const std::string& mechanism, int instance = 0);