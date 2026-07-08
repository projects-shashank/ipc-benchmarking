# IPC Benchmark Library — Design Document

## Problem

Choosing an IPC mechanism involves real tradeoffs between latency, throughput,
CPU cost, and complexity. Most documentation describes these tradeoffs
qualitatively. This project measures them quantitatively under a unified
harness so the numbers are directly comparable.

## Architecture

### Abstract interface (Strategy pattern)

All six mechanisms implement `IPCSender` and `IPCReceiver` base classes with
identical `setup() / send() / recv() / teardown()` methods. The benchmark
harness holds base-class pointers and never knows which mechanism it's
measuring. This guarantees every mechanism runs identical measurement code —
any performance difference in the results is due to the mechanism, not the
measurement.

### Factory function

`make_ipc_pair(mechanism, instance)` is the single point of knowledge about
all six mechanisms. Adding a seventh mechanism requires touching only the
factory and a new header/implementation pair — nothing else changes.

### Shared state before fork()

Mechanisms that require both sides to share OS handles (pipe fd pair, shared
memory mapping, semaphores) initialize that state in a shared struct
constructor that runs at factory time, before `fork()`. Both parent and child
inherit the initialized state. This avoids the race condition where one side
tries to use a resource the other side hasn't created yet.

### Instance IDs for path uniqueness

The latency benchmark creates two IPC pairs per measurement (forward channel
sender→receiver, reverse channel receiver→sender for echo). Each pair gets a
unique instance ID (0 and 1) appended to its filesystem path or queue name,
preventing collision between the two channels.

## Design decisions

### Why memfd_create instead of shm_open

`shm_open()` creates a named entry in `/dev/shm/` that persists after process
exit and can collide between concurrent benchmark runs. `memfd_create()` creates
an anonymous RAM-backed file descriptor — no filesystem entry, no cleanup race,
works correctly inside containers where `/dev/shm/` may be restricted.

### Why spin-then-sleep instead of pure spin or pure sleep

Pure spin (busy-poll): lowest latency, burns 100% of one CPU core even when
idle. Unacceptable for infrequent messages.

Pure sleep (semaphore/futex): always pays 1-2µs syscall overhead even when the
message is already waiting. Unnecessarily slow for high-frequency messaging.

Spin-then-sleep: spin for `spin_budget` iterations (~few µs), then sleep via
futex if no message arrives. Zero syscalls when messages are frequent (fast
path), one wakeup syscall when messages are infrequent (slow path). Best of
both worlds with a tunable crossover point.

### Why acquire/release on the sequence number

The sender writes payload bytes before incrementing `seq_send`. Without memory
ordering, the compiler or CPU could reorder the store to `seq_send` before the
payload write — the receiver would see a new sequence number but read stale
payload bytes. `memory_order_release` on the sender's `fetch_add` and
`memory_order_acquire` on the receiver's `load` form a synchronization pair:
the payload write is guaranteed visible to the receiver before the sequence
number increment is.

### Why cache line padding on the RDMA control block

`seq_send`, `seq_recv`, and `sleeping` are 20 bytes total. Without padding they
would share a cache line with the payload. Every payload write from the sender
would invalidate that cache line, forcing the receiver's spin loop (which reads
`seq_send` on every iteration) to reload from L2/L3 instead of L1. The 44-byte
pad pushes the payload to its own cache line — the spin loop stays in L1 cache,
reducing per-iteration cost from ~10ns to ~1-4ns.

### Why CLOCK_MONOTONIC

`gettimeofday()` uses wall-clock time which can jump backward if NTP adjusts
the system clock mid-benchmark. `CLOCK_MONOTONIC` is guaranteed to never go
backward — it measures elapsed time since a fixed arbitrary point, unaffected
by NTP or `settimeofday()`.

### Why p50/p99/p999 instead of mean

Mean latency hides tail spikes caused by OS scheduler preemption, TLB flushes,
or cache eviction. p99 represents what 1 in 100 messages experiences — the
number that matters for SLA-bound systems. p999 reveals worst-case behavior.
A mechanism with low mean but high p999 is unusable for real-time workloads.

## Results summary

| Mechanism | p50 latency (64B) | bottleneck |
|---|---|---|
| pipe | ~2200 ns | 2× kernel copy + 2× syscall |
| fifo | ~2200 ns | 2× kernel copy + 2× syscall |
| mq | ~2200 ns | 2× kernel copy + 2× syscall |
| shm+sem | ~2200 ns | 2× futex syscall |
| rdma_shm | ~133 ns | cache line invalidation (MESI) |
| unix | ~2900 ns | 2× kernel copy + 2× syscall |

Key insight: pipe, FIFO, MQ, and Unix socket are all bottlenecked by the same
thing — two kernel mode switches per message. SHM+semaphore eliminates the data
copy but keeps the kernel wakeup cost. RDMA-style SHM eliminates both,
pushing the bottleneck to hardware: the CPU cache coherency protocol.

## What I would add next

- **MPMC lock-free ring buffer**: multi-producer multi-consumer channel using
  the LMAX Disruptor pattern — fetch_add to claim slots, sequence numbers for
  ordering. Enables N:M producer/consumer benchmarks.

- **io_uring async pipe**: replace blocking read/write on pipes with io_uring
  submission/completion rings. Eliminates the syscall per message for pipe-based
  IPC using the kernel's own zero-syscall interface.

- **Unix socket with SCM_RIGHTS fd passing**: pass a memfd file descriptor over
  a Unix socket to transfer ownership of a shared memory region between
  unrelated processes — the production pattern used by Wayland compositors.

- **Huge pages for shared memory**: MAP_HUGETLB on the shared region reduces
  TLB misses in the spin loop. Measurable at high message rates on large
  payloads.

- **Hardware mailbox simulation**: model inter-processor communication using
  volatile MMIO-style registers and eventfd as an interrupt — directly analogous
  to ARM Cortex-A to DSP communication on Qualcomm SoCs.
