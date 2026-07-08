// src/shm_ipc.cpp
// Shared memory + semaphore IPC implementation.
//
// Design notes:
// - ShmSharedState constructor runs at factory time, BEFORE fork().
//   memfd_create(), ftruncate(), mmap(), sem_init() all happen here.
//   After fork() both parent and child have the mapping in their
//   address space — same physical pages, different virtual addresses.
// - ShmSender::setup() and ShmReceiver::setup() are intentionally
//   minimal — the real setup is in ShmSharedState constructor.
// - send(): memcpy payload → sem_post(sem_ready) → sem_wait(sem_done)
//   recv(): sem_wait(sem_ready) → memcpy payload → sem_post(sem_done)
// - The only syscalls in the data path are sem_post and sem_wait
//   (both are futex wrappers). No kernel copy of payload data.

#include "../include/shm_ipc.hpp"

#include <sys/mman.h>       // mmap(), munmap(), MAP_SHARED, MAP_FAILED
#include <sys/syscall.h>    // SYS_memfd_create
#include <linux/memfd.h>    // MFD_CLOEXEC
#include <unistd.h>         // ftruncate(), close()
#include <cerrno>           // errno
#include <cstring>          // strerror, memcpy
#include <iostream>         // std::cerr

// memfd_create() is available via syscall on Linux 3.17+.
// glibc added a wrapper in 2.27 — use syscall directly for compatibility.
static int memfd_create_compat(const char* name, unsigned int flags) {
    return static_cast<int>(syscall(SYS_memfd_create, name, flags));
}

// ── ShmSharedState ────────────────────────────────────────────────────────────

ShmSharedState::ShmSharedState() {
    size = sizeof(ShmControlBlock);

    // memfd_create() creates an anonymous RAM-backed file.
    // Arguments:
    //   "ipc_bench_shm" — name for debugging only (shows in /proc/PID/fd/)
    //                     not a filesystem path, no cleanup needed
    //   MFD_CLOEXEC     — close fd on exec() — good hygiene
    memfd = memfd_create_compat("ipc_bench_shm", MFD_CLOEXEC);
    if (memfd == -1) {
        std::cerr << "[ShmSharedState] memfd_create() failed: "
                  << strerror(errno) << "\n";
        return;
    }

    // ftruncate() sets the size of the memory object.
    // A fresh memfd has size 0 — must size it before mmap().
    if (ftruncate(memfd, static_cast<off_t>(size)) == -1) {
        std::cerr << "[ShmSharedState] ftruncate() failed: "
                  << strerror(errno) << "\n";
        close(memfd);
        memfd = -1;
        return;
    }

    // mmap() maps the shared region into this process's address space.
    // After fork(), the child inherits this mapping — same physical pages.
    // MAP_SHARED: writes are visible to all processes mapping this fd.
    mapped = mmap(nullptr, size,
                  PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0);
    if (mapped == MAP_FAILED) {
        std::cerr << "[ShmSharedState] mmap() failed: "
                  << strerror(errno) << "\n";
        close(memfd);
        memfd  = -1;
        mapped = nullptr;
        return;
    }

    // Initialize semaphores IN the shared region before fork().
    // pshared=1 means the semaphore is shared between PROCESSES
    // (not just threads). Both parent and child will use these.
    ShmControlBlock* blk = static_cast<ShmControlBlock*>(mapped);

    // sem_ready starts at 0 — receiver blocks until sender posts.
    if (sem_init(&blk->sem_ready, 1, 0) == -1) {
        std::cerr << "[ShmSharedState] sem_init(sem_ready) failed: "
                  << strerror(errno) << "\n";
        return;
    }

    // sem_done starts at 0 — sender waits after sending until
    // receiver signals it has consumed the message.
    if (sem_init(&blk->sem_done, 1, 0) == -1) {
        std::cerr << "[ShmSharedState] sem_init(sem_done) failed: "
                  << strerror(errno) << "\n";
        return;
    }
}

ShmSharedState::~ShmSharedState() {
    if (mapped && mapped != MAP_FAILED) {
        // Destroy semaphores while memory is still mapped.
        ShmControlBlock* blk = static_cast<ShmControlBlock*>(mapped);
        sem_destroy(&blk->sem_ready);
        sem_destroy(&blk->sem_done);
        munmap(mapped, size);
        mapped = nullptr;
    }
    if (memfd != -1) {
        close(memfd);
        memfd = -1;
    }
}

// ── ShmSender ─────────────────────────────────────────────────────────────────

bool ShmSender::setup() {
    // All setup done in ShmSharedState constructor.
    // Just verify the mapping is valid.
    return state_->mapped != nullptr &&
           state_->mapped != MAP_FAILED;
}

bool ShmSender::send(const void* buf, size_t len) {
    if (len > SHM_MAX_PAYLOAD) {
        std::cerr << "[ShmSender] message too large: " << len << "\n";
        return false;
    }

    ShmControlBlock* blk = static_cast<ShmControlBlock*>(state_->mapped);

    // Write payload directly into shared RAM — no syscall, no kernel copy.
    // This is just a memcpy into physical pages both processes can see.
    memcpy(blk->payload, buf, len);

    // sem_post() increments sem_ready from 0 to 1, waking the receiver.
    // This IS a syscall (futex under the hood) — but the data copy above
    // was free. Compare to pipe where BOTH the copy AND the wakeup
    // go through the kernel.
    if (sem_post(&blk->sem_ready) == -1) {
        std::cerr << "[ShmSender] sem_post(sem_ready) failed: "
                  << strerror(errno) << "\n";
        return false;
    }

    // Wait for receiver to finish reading before we overwrite the payload.
    // This gives us flow control — sender can't lap the receiver.
    if (sem_wait(&blk->sem_done) == -1) {
        std::cerr << "[ShmSender] sem_wait(sem_done) failed: "
                  << strerror(errno) << "\n";
        return false;
    }

    return true;
}

void ShmSender::teardown() {
    // Shared state cleanup handled by ShmSharedState destructor.
}

// ── ShmReceiver ───────────────────────────────────────────────────────────────

bool ShmReceiver::setup() {
    return state_->mapped != nullptr &&
           state_->mapped != MAP_FAILED;
}

ssize_t ShmReceiver::recv(void* buf, size_t len) {
    if (len > SHM_MAX_PAYLOAD) {
        std::cerr << "[ShmReceiver] buffer too large: " << len << "\n";
        return -1;
    }

    ShmControlBlock* blk = static_cast<ShmControlBlock*>(state_->mapped);

    // Block until sender posts sem_ready.
    // sem_wait() decrements sem_ready — if 0, sleeps until sem_post().
    if (sem_wait(&blk->sem_ready) == -1) {
        std::cerr << "[ShmReceiver] sem_wait(sem_ready) failed: "
                  << strerror(errno) << "\n";
        return -1;
    }

    // Payload is now safe to read — sender fully wrote it before sem_post.
    memcpy(buf, blk->payload, len);

    // Signal sender that we've consumed the message —
    // sender can now safely write the next one.
    if (sem_post(&blk->sem_done) == -1) {
        std::cerr << "[ShmReceiver] sem_post(sem_done) failed: "
                  << strerror(errno) << "\n";
        return -1;
    }

    return static_cast<ssize_t>(len);
}

void ShmReceiver::teardown() {
    // Shared state cleanup handled by ShmSharedState destructor.
}