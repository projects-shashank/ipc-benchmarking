// bench/main.cpp
// CLI entry point for the benchmark harness.
//
// Usage:
//   ./ipc_bench --mechanism <name> --mode <latency|throughput>
//               --msg-size <bytes> --count <n> --spin-budget <n>
//
// Examples:
//   ./ipc_bench --mechanism pipe --mode latency --msg-size 64
//   ./ipc_bench --mechanism rdma_shm --mode latency --spin-budget 5000
//   ./ipc_bench --all --mode latency    (runs all 6 mechanisms)

#include <iostream>
#include <string>
#include <cstring>
#include <vector>
#include "harness.hpp"

static void print_usage(const char* argv0) {
    std::cerr << "Usage: " << argv0
              << " [--mechanism <pipe|fifo|mq|shm|rdma_shm|unix>|--all]\n"
              << "       [--mode <latency|throughput>]\n"
              << "       [--msg-size <bytes>] [--count <n>]\n"
              << "       [--spin-budget <n>]\n";
}

int main(int argc, char* argv[]) {
    BenchConfig cfg;
    bool run_all = false;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--mechanism") == 0 && i+1 < argc) {
            cfg.mechanism = argv[++i];
        } else if (std::strcmp(argv[i], "--all") == 0) {
            run_all = true;
        } else if (std::strcmp(argv[i], "--mode") == 0 && i+1 < argc) {
            std::string m = argv[++i];
            cfg.mode = (m == "throughput") ? BenchMode::THROUGHPUT
                                           : BenchMode::LATENCY;
        } else if (std::strcmp(argv[i], "--msg-size") == 0 && i+1 < argc) {
            cfg.msg_size = static_cast<size_t>(std::stoul(argv[++i]));
        } else if (std::strcmp(argv[i], "--count") == 0 && i+1 < argc) {
            cfg.count = std::stoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--spin-budget") == 0 && i+1 < argc) {
            cfg.spin_budget = std::stoi(argv[++i]);
        }
    }

    if (!run_all && cfg.mechanism.empty()) {
        print_usage(argv[0]);
        return 1;
    }

    // Mechanisms to run.
    std::vector<std::string> mechanisms;
    if (run_all) {
        mechanisms = {"pipe", "fifo", "mq", "shm", "rdma_shm", "unix"};
    } else {
        mechanisms = {cfg.mechanism};
    }

    // Message sizes for --all mode.
    std::vector<size_t> sizes;
    if (run_all) {
        sizes = {64, 1024, 4096};  // 64B, 1KB, 4KB
        // Note: 64KB excluded for mq (system limit 8KB).
        // Individual mechanism runs use whatever --msg-size was passed.
    } else {
        sizes = {cfg.msg_size};
    }

    print_header(cfg.mode);

    for (const auto& mech : mechanisms) {
        for (size_t sz : sizes) {
            // Skip large messages for mq — system limit is 8KB.
            if (mech == "mq" && sz > 8192) {
                continue;
            }

            BenchConfig c   = cfg;
            c.mechanism     = mech;
            c.msg_size      = sz;

            BenchResult r = run_benchmark(c);
            print_result(r);
        }
    }

    return 0;
}