// bench/harness.hpp
// Benchmark harness — measures latency and throughput for any IPC mechanism.
// Uses fork() to run sender and receiver in separate processes.
// Results reported as p50/p99/p999 latency and MB/s throughput.

#pragma once

#include <string>
#include <cstddef>      // size_t

enum class BenchMode {
    LATENCY,            // ping-pong round trip measurement
    THROUGHPUT          // blast N messages, measure MB/s
};

struct BenchConfig {
    std::string mechanism;
    BenchMode   mode       = BenchMode::LATENCY;
    size_t      msg_size   = 64;        // bytes per message
    int         count      = 10000;     // number of iterations
    int         warmup     = 200;       // warmup iterations (discarded)
    int         spin_budget = 1000;     // for rdma_shm only
};

struct BenchResult {
    // Latency (nanoseconds) — ping-pong mode
    double p50_ns  = 0;
    double p99_ns  = 0;
    double p999_ns = 0;

    // Throughput — blast mode
    double throughput_mbps = 0;

    // Shared
    size_t msg_size   = 0;
    int    count      = 0;
    std::string mechanism;
    std::string mode;
};

// Run the benchmark with the given config.
// Forks internally — returns result in the parent process.

BenchResult run_benchmark(const BenchConfig& cfg);

// Print result as a formatted table row.
void print_result(const BenchResult& r);

// Print table header.
void print_header(BenchMode mode);