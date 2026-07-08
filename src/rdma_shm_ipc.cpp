// src/rdma_shm_ipc.cpp
// RDMA-style zero-syscall shared memory implementation.

#include "../include/rdma_shm_ipc.hpp"

#include <sys/mman.h>       // mmap(), munmap()
#include <sys/syscall.h>    // SYS_memfd_create, SYS_futex
#include <linux/memfd.h>    // MFD_CLOEXEC
#include <linux/futex.h>    // FUTEX_WAIT, FUTEX_WAKE
#include <unistd.h>         // ftruncate(), close()
#include <cerrno>           // errno
#include <cstring>          // strerror, memcpy
#include <iostream>         // std::cerr, std::cout

// ── Helpers ───────────────────────────────────────────────────────────────────

static int memfd_create_compat(const char* name, unsigned int flags) {
    return static_cast<int>(syscall(SYS_memfd_create, name, flags));
}

static void futex_wait(std::atomic<uint32_t>* addr, uint32_t expected) {
    syscall(SYS_futex, addr, FUTEX_WAIT, expected, nullptr, nullptr, 0);
}

static void futex_wake(std::atomic<uint32_t>* addr, int n) {
    syscall(SYS_futex, addr, FUTEX_WAKE, n, nullptr, nullptr, 0);
}

// cpu_relax: emits x86 PAUSE instruction inside spin loops.
// Tells the CPU "I'm spinning" — reduces power, avoids memory order
// violation false positives on the out-of-order execution pipeline,
// and yields pipeline resources to the sibling hyperthread.
static inline void cpu_relax() {
    asm volatile("pause" ::: "memory");
}

// ── RdmaStats ─────────────────────────────────────────────────────────────────

void RdmaStats::print() const {
    std::cout << "[RdmaStats] messages_sent  : " << messages_sent  << "\n"
              << "[RdmaStats] bytes_received : " << bytes_received << "\n"
              << "[RdmaStats] slow_path_hits : " << slow_path_hits << "\n"
              << "[RdmaStats] total_spins    : " << total_spins    << "\n";
    if (messages_sent > 0) {
        std::cout << "[RdmaStats] avg_spins/msg  : "
                  << total_spins / messages_sent << "\n";
    }
}

// ── RdmaShmSharedState ────────────────────────────────────────────────────────

RdmaShmSharedState::RdmaShmSharedState() {
    size = sizeof(RdmaControlBlock);

    memfd = memfd_create_compat("ipc_bench_rdma", MFD_CLOEXEC);
    if (memfd == -1) {
        std::cerr << "[RdmaShmSharedState] memfd_create() failed: "
                  << strerror(errno) << "\n";
        return;
    }

    if (ftruncate(memfd, static_cast<off_t>(size)) == -1) {
        std::cerr << "[RdmaShmSharedState] ftruncate() failed: "
                  << strerror(errno) << "\n";
        close(memfd);
        memfd = -1;
        return;
    }

    mapped = mmap(nullptr, size,
                  PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0);
    if (mapped == MAP_FAILED) {
        std::cerr << "[RdmaShmSharedState] mmap() failed: "
                  << strerror(errno) << "\n";
        close(memfd);
        memfd  = -1;
        mapped = nullptr;
        return;
    }

    // Zero-initialize the control block before fork().
    // Atomics are initialized in the struct definition,
    // but we zero the whole block to be safe.
    RdmaControlBlock* blk = static_cast<RdmaControlBlock*>(mapped);
    blk->seq_send.store(0, std::memory_order_relaxed);
    blk->seq_recv.store(0, std::memory_order_relaxed);
    blk->sleeping.store(0, std::memory_order_relaxed);
}

RdmaShmSharedState::~RdmaShmSharedState() {
    if (mapped && mapped != MAP_FAILED) {
        munmap(mapped, size);
        mapped = nullptr;
    }
    if (memfd != -1) {
        close(memfd);
        memfd = -1;
    }
}

// ── RdmaShmSender ─────────────────────────────────────────────────────────────

bool RdmaShmSender::setup() {
    return state_->mapped != nullptr &&
           state_->mapped != MAP_FAILED;
}

bool RdmaShmSender::send(const void* buf, size_t len) {
    if (len > RDMA_MAX_PAYLOAD) {
        std::cerr << "[RdmaShmSender] message too large: " << len << "\n";
        return false;
    }

    RdmaControlBlock* blk = static_cast<RdmaControlBlock*>(state_->mapped);

    // ── Backpressure ──────────────────────────────────────────────────────
    // Wait until receiver has consumed the previous message.
    // seq_send - seq_recv must be 0 before we write the next message.
    // Without this, a fast sender overwrites a message the receiver
    // hasn't read yet — silent data corruption.
    uint64_t sent = blk->seq_send.load(std::memory_order_relaxed);
    while (blk->seq_recv.load(std::memory_order_acquire) < sent) {
        cpu_relax(); // spin waiting for receiver to catch up
    }

    // ── Write payload ─────────────────────────────────────────────────────
    // Direct store into shared RAM — no syscall, no kernel buffer.
    memcpy(blk->payload, buf, len);

    // ── Release store on seq_send ─────────────────────────────────────────
    // memory_order_release: guarantees the payload memcpy above is
    // visible to any thread/process that loads seq_send with acquire.
    // This is the "gate" — payload is fully written before seq increments.
    blk->seq_send.fetch_add(1, std::memory_order_release);
    stats_.messages_sent++;

    // ── Wake receiver if sleeping ─────────────────────────────────────────
    // Check sleeping flag AFTER incrementing seq_send.
    // If receiver went to slow path, wake it — one syscall only when needed.
    if (blk->sleeping.load(std::memory_order_acquire) == 1) {
        futex_wake(&blk->sleeping, 1);
    }

    return true;
}

void RdmaShmSender::teardown() {
    // stats printed by receiver since it has the full picture
}

// ── RdmaShmReceiver ───────────────────────────────────────────────────────────

bool RdmaShmReceiver::setup() {
    return state_->mapped != nullptr &&
           state_->mapped != MAP_FAILED;
}

ssize_t RdmaShmReceiver::recv(void* buf, size_t len) {
    if (len > RDMA_MAX_PAYLOAD) {
        std::cerr << "[RdmaShmReceiver] buffer too large\n";
        return -1;
    }

    RdmaControlBlock* blk = static_cast<RdmaControlBlock*>(state_->mapped);

    uint64_t expected = blk->seq_recv.load(std::memory_order_relaxed) + 1;
    int spin_count = 0;

    while (true) {
        // Acquire load: if seq_send >= expected, payload is fully written
        // and safe to read (release on sender side guarantees this).
        uint64_t current = blk->seq_send.load(std::memory_order_acquire);

        if (current >= expected) {
            // ── Fast path: message ready, zero syscalls ────────────────
            stats_.total_spins += static_cast<uint64_t>(spin_count);
            break;
        }

        spin_count++;

        if (spin_count < spin_budget_) {
            // Still within budget — keep spinning with PAUSE hint.
            cpu_relax();
        } else {
            // ── Slow path: spin budget exhausted ──────────────────────
            // Step 1: announce we're going to sleep.
            blk->sleeping.store(1, std::memory_order_release);

            // Step 2: recheck AFTER setting sleeping flag.
            // Sender may have posted between our last spin check and
            // us setting sleeping=1. Without this recheck we'd sleep
            // forever if sender already passed the sleeping check.
            current = blk->seq_send.load(std::memory_order_acquire);
            if (current >= expected) {
                blk->sleeping.store(0, std::memory_order_relaxed);
                stats_.total_spins += static_cast<uint64_t>(spin_count);
                break;
            }

            // Step 3: sleep until sender calls futex_wake.
            stats_.slow_path_hits++;
            futex_wait(&blk->sleeping, 1);

            // Woke up — clear flag and reset spin budget.
            blk->sleeping.store(0, std::memory_order_relaxed);
            spin_count = 0;
        }
    }

    // ── Read payload ──────────────────────────────────────────────────────
    memcpy(buf, blk->payload, len);
    stats_.bytes_received += len;
    stats_.messages_sent++;   // track here — sender stats are in another process

    // Signal sender we've consumed this message.
    blk->seq_recv.fetch_add(1, std::memory_order_release);

    return static_cast<ssize_t>(len);
}

void RdmaShmReceiver::teardown() {
    if (print_stats_) {
        stats_.print();
    }
}