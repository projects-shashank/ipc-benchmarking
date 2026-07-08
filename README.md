# ipc-bench

A C++17 benchmark library that implements six Linux IPC mechanisms from
scratch and measures them under a unified harness. Built to understand
performance tradeoffs at the OS and hardware level — not just "shared memory
is faster" but why, by how much, and where the bottleneck actually is.

---

## Motivation

Every IPC tutorial says "shared memory is the fastest." None of them show
you numbers. This project measures all six mechanisms under identical
conditions — same harness, same message sizes, same percentile reporting —
so the tradeoffs are quantitative, not qualitative.

The results reveal something most tutorials miss: SHM with semaphores has
the same latency as a pipe, despite doing zero data copies. The bottleneck
is not the copy — it is the kernel wakeup. Eliminating that is what the
RDMA-style mechanism does.

---

## Mechanisms

| # | Mechanism | Key property |
|---|---|---|
| 1 | pipe | Unnamed kernel pipe, fork-only, 64KB ring buffer |
| 2 | fifo | Named pipe, filesystem rendezvous, unrelated processes |
| 3 | mq | POSIX message queue, message-preserving, priority ordering |
| 4 | shm | Shared memory + POSIX semaphore, zero data copy |
| 5 | rdma_shm | RDMA-style SHM, atomic sequence numbers, zero syscalls on fast path |
| 6 | unix | Unix domain socket, bidirectional, production standard |

---

## Results

### Latency (p50 one-way, 64B message, 10,000 round trips)

| Mechanism | p50 (ns) | p99 (ns) | p999 (ns) | bottleneck |
|---|---|---|---|---|
| pipe | 2279 | 5799 | 76698 | 2x kernel copy + 2x syscall |
| fifo | 2251 | 4116 | 10248 | 2x kernel copy + 2x syscall |
| mq | 2215 | 4562 | 58474 | 2x kernel copy + 2x syscall |
| shm | 2158 | 5363 | 8146 | 2x futex syscall (sem_post + sem_wait) |
| rdma_shm | 133 | 161 | 189 | cache line invalidation (MESI protocol) |
| unix | 2944 | 4184 | 8611 | 2x kernel copy + 2x syscall |

rdma_shm is 16x faster than pipe at p50. The p999 gap is even larger —
189ns vs 76698ns for pipe — because rdma_shm never enters the kernel on
the fast path, so it is immune to scheduler jitter.

### Throughput (50,000 messages)

| Mechanism | 64B (MB/s) | 1KB (MB/s) | 4KB (MB/s) |
|---|---|---|---|
| pipe | 178 | 2906 | 4784 |
| fifo | 143 | 1999 | 4386 |
| mq | 118 | 1323 | 3898 |
| shm | 13 | 219 | 846 |
| rdma_shm | 372 | 3066 | 5194 |
| unix | 96 | 1363 | 3753 |

SHM throughput is low because sem_post + sem_wait are called on every
message regardless of size — the syscall cost dominates at small payloads.
rdma_shm avoids this and leads at all sizes.

---

## Key insight

pipe, FIFO, MQ, and Unix socket all have similar latency (~2200ns) despite
very different designs. The common bottleneck is two kernel mode switches
per message — one to enter the kernel on send, one on receive. Each mode
switch costs roughly 1-2 microseconds regardless of payload size.

SHM with semaphores eliminates the data copy but keeps the two mode
switches (sem_post and sem_wait are both futex syscalls). Same latency
as pipe.

RDMA-style SHM eliminates both the copy and the syscalls. The sender
does one atomic fetch_add (a single LOCK XADD instruction, ~10ns). The
receiver spins on an atomic load (~1-4ns per iteration from L1 cache).
No kernel involvement. The bottleneck shifts to hardware: the CPU cache
coherency protocol (MESI) costs ~40-80ns for a cross-core cache line
invalidation. That is the floor — no software optimization can go lower
on a single machine without hardware changes.

---

## Connection to production systems

This project implements the same principles used in:

**Kafka zero-copy design**
Producers write to the OS page cache. Consumers read from the same
physical pages — same physical RAM, no kernel copy on the data path,
exactly like our shm mechanism. For network consumers, Kafka uses
sendfile() to move data from page cache to NIC without copying through
user space.

**Linux io_uring (kernel 5.1+)**
A shared memory ring buffer between user space and the kernel with
atomic head/tail pointers — the same pattern as rdma_shm. In SQPOLL
mode a kernel thread polls the submission ring so user space submits
I/O with zero syscalls.

**LMAX Disruptor**
The direct inspiration for rdma_shm. A financial exchange processing
6 million orders/second using a lock-free ring buffer with sequence
numbers and memory_order_release/acquire — no locks, no condition
variables, no kernel involvement on the fast path.

**DPDK**
Intel's user-space network driver framework passes packets between
processing stages via lock-free ring buffers using the same atomic
sequence number pattern. Used for 100Gbps+ packet forwarding.

**gRPC and production daemons**
Unix domain sockets are the local transport for gRPC, Docker, Redis,
PostgreSQL, and most production daemons. Our unix mechanism is exactly
what they use — socket(), bind(), listen(), accept(), send(), recv().

---

## Build

Requirements: Linux x86-64, GCC or Clang with C++17, CMake 3.16+

    sudo apt install cmake g++ -y
    git clone <repo>
    cd ipc-bench
    mkdir build && cd build
    cmake ..
    make -j$(nproc)

---

## Usage

Single mechanism, latency mode:

    ./build/ipc_bench --mechanism pipe --mode latency --msg-size 64 --count 10000

Single mechanism, throughput mode:

    ./build/ipc_bench --mechanism rdma_shm --mode throughput --msg-size 4096 --count 50000

All mechanisms, all sizes:

    ./build/ipc_bench --all --mode latency --count 10000
    ./build/ipc_bench --all --mode throughput --count 50000

Tune spin budget for rdma_shm (higher = more CPU burn, lower latency):

    ./build/ipc_bench --mechanism rdma_shm --mode latency --spin-budget 5000

Generate CSV and charts:

    bash results/run_all.sh
    pip3 install matplotlib pandas --break-system-packages
    python3 results/plot.py

---

## Project structure

    ipc-bench/
    +-- include/
    |   +-- ipc_base.hpp          abstract IPCSender / IPCReceiver + factory
    |   +-- pipe_ipc.hpp
    |   +-- fifo_ipc.hpp
    |   +-- mqueue_ipc.hpp
    |   +-- shm_ipc.hpp
    |   +-- rdma_shm_ipc.hpp
    |   +-- unix_socket_ipc.hpp
    +-- src/
    |   +-- ipc_factory.cpp       single point of knowledge about all mechanisms
    |   +-- pipe_ipc.cpp
    |   +-- fifo_ipc.cpp
    |   +-- mqueue_ipc.cpp
    |   +-- shm_ipc.cpp
    |   +-- rdma_shm_ipc.cpp
    |   +-- unix_socket_ipc.cpp
    +-- bench/
    |   +-- harness.hpp / harness.cpp
    |   +-- main.cpp              CLI entry point
    +-- results/
    |   +-- run_all.sh
    |   +-- plot.py
    |   +-- data.csv
    +-- DESIGN.md
    +-- README.md
    +-- CMakeLists.txt

---

## Design highlights

**Strategy pattern via abstract interface**
The harness holds IPCSender* and IPCReceiver* base class pointers. It
never includes a mechanism header or knows which mechanism is running.
Any performance difference in results is due to the mechanism, not the
measurement code.

**memfd_create instead of shm_open**
shm_open creates a named entry in /dev/shm that persists after process
exit and can collide between concurrent runs. memfd_create creates an
anonymous RAM-backed fd — no filesystem entry, no name collision, works
inside containers where /dev/shm may be restricted.

**Spin-then-sleep (tunable)**
Pure spin burns a CPU core. Pure sleep (semaphore) pays 1-2us syscall
overhead on every message. Spin-then-sleep spins for spin_budget
iterations, then calls futex_wait if no message arrives. Zero syscalls
when messages are back-to-back (fast path), one wakeup syscall when idle
(slow path). The crossover is tunable via --spin-budget.

**Cache line padding**
The rdma_shm control block (seq_send, seq_recv, sleeping) is padded to
exactly 64 bytes so the payload starts on a separate cache line. Without
padding, every payload write invalidates the cache line the receiver spin
loop is watching, adding ~40ns per iteration. With padding the sequence
numbers stay in L1 cache.

**acquire/release memory ordering**
The sender uses memory_order_release on seq_send fetch_add. The receiver
uses memory_order_acquire on seq_send load. This synchronization pair
guarantees the payload write is visible to the receiver before the
sequence number increment — correct on both x86 (strong ordering) and
ARM (weak ordering). Using relaxed ordering here would be a silent
correctness bug on ARM.

**Backpressure**
Before writing a new message, rdma_shm sender checks that seq_recv has
caught up to seq_send. If the receiver has not consumed the previous
message, the sender spins. This prevents silent data corruption if the
sender is faster than the receiver.

---

## What I would add next

**MPMC lock-free ring buffer**
Extend rdma_shm to multiple producers and consumers using the LMAX
Disruptor pattern — producers claim slots with fetch_add on a tail
counter, mark slots ready with a sequence number, consumers read from
the head. This enables N:M producer/consumer benchmarks and directly
models how Kafka partitions work internally.

**io_uring async pipe transport**
Replace blocking read/write on pipes with io_uring submission and
completion rings. Eliminates the per-message syscall for pipe-based IPC
using the kernel's own zero-syscall interface. Interesting comparison:
does io_uring pipe approach rdma_shm latency?

**Unix socket with SCM_RIGHTS fd passing**
Pass a memfd file descriptor over a Unix socket to transfer ownership of
a shared memory region between unrelated processes at runtime — the
pattern used by Wayland compositors and Android Binder. No pre-shared
state required at fork time.

**Huge pages**
MAP_HUGETLB on the shared memory region reduces TLB miss rate in the
spin loop. On a workload with large payloads the TLB miss (~100-200ns)
can dominate. Measurable at high message rates.

**Hardware mailbox simulation**
Model inter-processor communication using a volatile struct as an MMIO
register file and eventfd as an interrupt line. Directly analogous to
ARM Cortex-A to DSP messaging on Qualcomm SoCs — relevant to firmware
and driver development interviews.

---

## License

MIT
