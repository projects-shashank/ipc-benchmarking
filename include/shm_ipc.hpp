// include/shm_ipc.hpp
// Shared memory + POSIX semaphore IPC using memfd_create.
// memfd_create() gives us an anonymous shared memory region —
// no /dev/shm/ name, no cleanup race, container-friendly.
//
// Layout of shared region:
//   [ ShmControlBlock (semaphores) | payload[MAX_PAYLOAD] ]
//    ^ offset 0

#pragma once

#include "ipc_base.hpp"
#include <memory>
#include <semaphore.h>  // sem_t

static const size_t SHM_MAX_PAYLOAD = 65536; // 64KB — covers all benchmark sizes

// Control block lives at offset 0 of the shared region.
// Semaphores are initialized before fork() so both sides see them.
struct ShmControlBlock {
    sem_t sem_ready;            // sender posts after writing → receiver wakes
    sem_t sem_done;             // receiver posts after reading → sender wakes
    char  payload[SHM_MAX_PAYLOAD];
};

// Shared state created in factory before fork().
// Both ShmSender and ShmReceiver hold a shared_ptr to this.
struct ShmSharedState {
    int   memfd  = -1;          // fd from memfd_create()
    void* mapped = nullptr;     // mmap'd pointer (set before fork)
    size_t size  = 0;

    ShmSharedState();           // creates memfd, ftruncate, mmap, sem_init
    ~ShmSharedState();          // munmap, close
};

// ── ShmSender ─────────────────────────────────────────────────────────────────
class ShmSender : public IPCSender {
    std::shared_ptr<ShmSharedState> state_;

public:
    explicit ShmSender(std::shared_ptr<ShmSharedState> s)
        : state_(std::move(s)) {}

    bool setup() override;
    bool send(const void* buf, size_t len) override;
    void teardown() override;
};

// ── ShmReceiver ───────────────────────────────────────────────────────────────
class ShmReceiver : public IPCReceiver {
    std::shared_ptr<ShmSharedState> state_;

public:
    explicit ShmReceiver(std::shared_ptr<ShmSharedState> s)
        : state_(std::move(s)) {}

    bool setup() override;
    ssize_t recv(void* buf, size_t len) override;
    void teardown() override;
};