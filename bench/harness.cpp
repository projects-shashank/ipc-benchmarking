// bench/harness.cpp
// Benchmark harness implementation.

#include "harness.hpp"
#include "ipc_base.hpp"
#include "rdma_shm_ipc.hpp"

#include <iostream>
#include <iomanip>      // std::setw, std::fixed, std::setprecision
#include <vector>
#include <algorithm>    // std::sort
#include <numeric>      // std::accumulate
#include <cstring>      // memset
#include <cmath>        // std::round

#include <unistd.h>     // fork(), pipe(), read(), write()
#include <sys/wait.h>   // waitpid()
#include <time.h>       // clock_gettime, CLOCK_MONOTONIC


// ── Timing helpers ─────────────────────────────────────────────────────────────

static inline uint64_t now_ns() {
    struct timespec ts;
    // CLOCK_MONOTONIC: never jumps backward, unaffected by NTP adjustments.
    // Resolution is typically 1ns on modern Linux kernels.
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL
         + static_cast<uint64_t>(ts.tv_nsec);
}

// ── Percentile calculation ─────────────────────────────────────────────────────

static double percentile(std::vector<uint64_t>& samples, double pct) {
    if (samples.empty()) return 0.0;
    std::sort(samples.begin(), samples.end());
    size_t idx = static_cast<size_t>(pct / 100.0 * samples.size());
    if (idx >= samples.size()) idx = samples.size() - 1;
    return static_cast<double>(samples[idx]);
}

// ── Shared result pipe ─────────────────────────────────────────────────────────
// We need to get BenchResult from child back to parent.
// Use a simple pipe — child writes the struct, parent reads it.

struct ResultPipe {
    int fd[2] = {-1, -1};

    ResultPipe() {
        if (pipe(fd) == -1) {
            std::cerr << "[ResultPipe] pipe() failed: "
                      << strerror(errno) << "\n";
        }
    }

    ~ResultPipe() {
        if (fd[0] != -1) close(fd[0]);
        if (fd[1] != -1) close(fd[1]);
    }

    void send_result(const BenchResult& r) {
        close(fd[0]); fd[0] = -1;
        ssize_t n = write(fd[1], &r, sizeof(r));
        if (n != static_cast<ssize_t>(sizeof(r))) {
            std::cerr << "[ResultPipe] write() incomplete\n";
        }
        close(fd[1]); fd[1] = -1;
    }

    BenchResult recv_result() {
        close(fd[1]); fd[1] = -1;
        BenchResult r{};
        ssize_t n = read(fd[0], &r, sizeof(r));
        if (n != static_cast<ssize_t>(sizeof(r))) {
            std::cerr << "[ResultPipe] read() incomplete\n";
        }
        close(fd[0]); fd[0] = -1;
        return r;
    }
};

// ── Latency benchmark (ping-pong) ──────────────────────────────────────────────

// Sender side: send message, wait for echo, record round-trip time.
static BenchResult run_latency_sender(IPCSender* tx, IPCReceiver* rx,
                                      const BenchConfig& cfg) {
    std::vector<uint8_t> msg(cfg.msg_size, 0xAB); // fill with pattern
    std::vector<uint8_t> echo(cfg.msg_size);
    std::vector<uint64_t> samples;
    samples.reserve(cfg.count);

    // Warmup — not measured. Lets the OS and CPU settle:
    // page faults resolved, branch predictors warmed, caches populated.
    for (int i = 0; i < cfg.warmup; ++i) {
        tx->send(msg.data(), cfg.msg_size);
        rx->recv(echo.data(), cfg.msg_size);
    }

    // Measured iterations.
    for (int i = 0; i < cfg.count; ++i) {
        uint64_t t0 = now_ns();
        tx->send(msg.data(), cfg.msg_size);
        rx->recv(echo.data(), cfg.msg_size);
        uint64_t t1 = now_ns();

        // Round trip time / 2 = one-way latency.
        samples.push_back((t1 - t0) / 2);
    }

    BenchResult r;
    r.mechanism = cfg.mechanism;
    r.mode      = "latency";
    r.msg_size  = cfg.msg_size;
    r.count     = cfg.count;
    r.p50_ns    = percentile(samples, 50.0);
    r.p99_ns    = percentile(samples, 99.0);
    r.p999_ns   = percentile(samples, 99.9);
    return r;
}

// Receiver side: receive message, echo it back.
static void run_latency_receiver(IPCReceiver* rx, IPCSender* tx,
                                 const BenchConfig& cfg) {
    std::vector<uint8_t> buf(cfg.msg_size);
    int total = cfg.warmup + cfg.count;

    for (int i = 0; i < total; ++i) {
        rx->recv(buf.data(), cfg.msg_size);
        tx->send(buf.data(), cfg.msg_size); // echo back
    }
}

// ── Throughput benchmark ───────────────────────────────────────────────────────

// Sender side: blast messages as fast as possible.
static BenchResult run_throughput_sender(IPCSender* tx,
                                         const BenchConfig& cfg) {
    std::vector<uint8_t> msg(cfg.msg_size, 0xAB);

    // Warmup.
    for (int i = 0; i < cfg.warmup; ++i) {
        tx->send(msg.data(), cfg.msg_size);
    }

    uint64_t t0 = now_ns();
    for (int i = 0; i < cfg.count; ++i) {
        tx->send(msg.data(), cfg.msg_size);
    }
    uint64_t t1 = now_ns();

    double elapsed_s = static_cast<double>(t1 - t0) / 1e9;
    double total_mb  = static_cast<double>(cfg.msg_size)
                     * static_cast<double>(cfg.count) / (1024.0 * 1024.0);

    BenchResult r;
    r.mechanism        = cfg.mechanism;
    r.mode             = "throughput";
    r.msg_size         = cfg.msg_size;
    r.count            = cfg.count;
    r.throughput_mbps  = total_mb / elapsed_s;
    return r;
}

// Receiver side: consume all messages.
static void run_throughput_receiver(IPCReceiver* rx, const BenchConfig& cfg) {
    std::vector<uint8_t> buf(cfg.msg_size);
    int total = cfg.warmup + cfg.count;

    for (int i = 0; i < total; ++i) {
        rx->recv(buf.data(), cfg.msg_size);
    }
}

// ── Main entry point ───────────────────────────────────────────────────────────


BenchResult run_benchmark(const BenchConfig& cfg) {
    // Create the IPC pair BEFORE fork() — shared state initialized here.
    IPCPair fwd = make_ipc_pair(cfg.mechanism, 0); // forward channel
    IPCPair rev = make_ipc_pair(cfg.mechanism, 1); // reverse channel (echo)

    if (cfg.mechanism == "rdma_shm") {
        auto* r = dynamic_cast<RdmaShmReceiver*>(rev.receiver.get());
        if (r) r->set_print_stats(false);
    }

    // Result pipe: child sends BenchResult back to parent.
    ResultPipe result_pipe;

    pid_t pid = fork();
    if (pid == -1) {
        std::cerr << "[harness] fork() failed\n";
        return {};
    }

    if (pid == 0) {
        // ── CHILD: receiver side ─────────────────────────────────────────
        fwd.receiver->setup();

        if (cfg.mode == BenchMode::LATENCY) {
            rev.sender->setup();
            run_latency_receiver(fwd.receiver.get(),
                                 rev.sender.get(), cfg);
            rev.sender->teardown();
        } else {
            run_throughput_receiver(fwd.receiver.get(), cfg);
        }

        fwd.receiver->teardown();
        result_pipe.send_result({}); // signal completion
        _exit(0);
    }

    // ── PARENT: sender side ───────────────────────────────────────────────
    fwd.sender->setup();

    BenchResult result;
    if (cfg.mode == BenchMode::LATENCY) {
        rev.receiver->setup();
        result = run_latency_sender(fwd.sender.get(),
                                    rev.receiver.get(), cfg);
        rev.receiver->teardown();
    } else {
        result = run_throughput_sender(fwd.sender.get(), cfg);
    }

    fwd.sender->teardown();

    // Wait for child.
    result_pipe.recv_result();
    int status;
    waitpid(pid, &status, 0);

    return result;
}

// ── Output formatting ──────────────────────────────────────────────────────────

void print_header(BenchMode mode) {
    if (mode == BenchMode::LATENCY) {
        std::cout << std::left
                  << std::setw(12) << "mechanism"
                  << std::setw(10) << "msg_size"
                  << std::setw(12) << "p50(ns)"
                  << std::setw(12) << "p99(ns)"
                  << std::setw(12) << "p999(ns)"
                  << "\n";
        std::cout << std::string(58, '-') << "\n";
    } else {
        std::cout << std::left
                  << std::setw(12) << "mechanism"
                  << std::setw(10) << "msg_size"
                  << std::setw(16) << "throughput(MB/s)"
                  << "\n";
        std::cout << std::string(38, '-') << "\n";
    }
}

void print_result(const BenchResult& r) {
    if (r.mode == "latency") {
        std::cout << std::left
                  << std::setw(12) << r.mechanism
                  << std::setw(10) << r.msg_size
                  << std::setw(12) << std::fixed
                  << std::setprecision(1) << r.p50_ns
                  << std::setw(12) << r.p99_ns
                  << std::setw(12) << r.p999_ns
                  << "\n";
    } else {
        std::cout << std::left
                  << std::setw(12) << r.mechanism
                  << std::setw(10) << r.msg_size
                  << std::setw(16) << std::fixed
                  << std::setprecision(1) << r.throughput_mbps
                  << "\n";
    }
}