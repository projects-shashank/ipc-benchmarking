// include/rdma_shm_ipc.hpp
// RDMA-style zero-syscall shared memory IPC.
//
// Fast path: sender stores atomically, receiver spins — zero syscalls.
// Slow path: spin budget exceeded → receiver sleeps via futex_wait,
//            sender checks sleeping flag → calls futex_wake.
//
// Control block layout (64 bytes, one cache line):
//   seq_send  : atomic<uint64_t> — sender increments after writing
//   seq_recv  : atomic<uint64_t> — receiver increments after reading
//   sleeping  : atomic<uint32_t> — receiver sets before futex_wait
//   _pad      : fills to 64 bytes — prevents false sharing with payload
//
// Payload starts on second cache line — no false sharing.

#pragma once

#include "ipc_base.hpp"
#include <memory>
#include <atomic>
#include <cstdint>
#include <cstddef>      // offsetof

static const int RDMA_DEFAULT_SPIN_BUDGET = 1000;

// ── Stats ─────────────────────────────────────────────────────────────────────
struct RdmaStats {
    uint64_t slow_path_hits  = 0;   // times futex_wait was called
    uint64_t total_spins     = 0;   // total spin iterations across all recvs
    uint64_t bytes_received  = 0;   // total payload bytes received
    uint64_t messages_sent   = 0;   // total messages sent

    void print() const;
};

// ── Control block ─────────────────────────────────────────────────────────────
static const size_t RDMA_MAX_PAYLOAD = 65536; // 64KB

struct alignas(64) RdmaControlBlock {
    std::atomic<uint64_t> seq_send  {0};    // sender increments after write
    std::atomic<uint64_t> seq_recv  {0};    // receiver increments after read
    std::atomic<uint32_t> sleeping  {0};    // receiver sets before futex_wait
    char _pad[64 - sizeof(uint64_t)        // pad control to exactly 64 bytes
                 - sizeof(uint64_t)        // so payload starts on new cache line
                 - sizeof(uint32_t)];

    // Payload on its own cache line — no false sharing with seq numbers.
    // Receiver spin loop watches seq_send — must stay in its own cache line.
    alignas(64) char payload[RDMA_MAX_PAYLOAD];
};

// Verify control block is exactly 64 bytes before payload at compile time.
static_assert(offsetof(RdmaControlBlock, payload) == 64,
              "RdmaControlBlock control section must be exactly 64 bytes");

// ── Shared state ──────────────────────────────────────────────────────────────
struct RdmaShmSharedState {
    int    memfd  = -1;
    void*  mapped = nullptr;
    size_t size   = 0;

    RdmaShmSharedState();
    ~RdmaShmSharedState();
};

// ── RdmaShmSender ─────────────────────────────────────────────────────────────
class RdmaShmSender : public IPCSender {
    std::shared_ptr<RdmaShmSharedState> state_;
    RdmaStats stats_;

public:
    explicit RdmaShmSender(std::shared_ptr<RdmaShmSharedState> s)
        : state_(std::move(s)) {}

    bool setup() override;
    bool send(const void* buf, size_t len) override;
    void teardown() override;
};

// ── RdmaShmReceiver ───────────────────────────────────────────────────────────
class RdmaShmReceiver : public IPCReceiver {
    std::shared_ptr<RdmaShmSharedState> state_;
    int       spin_budget_  = RDMA_DEFAULT_SPIN_BUDGET;
    RdmaStats stats_;
    bool      print_stats_  = true;     // suppress on reverse channel

public:
    explicit RdmaShmReceiver(std::shared_ptr<RdmaShmSharedState> s)
        : state_(std::move(s)) {}

    void set_spin_budget(int budget) { spin_budget_ = budget; }
    void set_print_stats(bool print) { print_stats_ = print; }

    bool    setup() override;
    ssize_t recv(void* buf, size_t len) override;
    void    teardown() override;
};