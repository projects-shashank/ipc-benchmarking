#include "ipc_base.hpp"
#include "pipe_ipc.hpp"
#include "fifo_ipc.hpp"
#include "mqueue_ipc.hpp"
#include "shm_ipc.hpp"
#include "rdma_shm_ipc.hpp"
#include "unix_socket_ipc.hpp"
#include <string>

IPCPair make_ipc_pair(const std::string& mechanism, int instance) {
    IPCPair pair;
    std::string id = std::to_string(instance);

    if (mechanism == "pipe") {
        auto state = std::make_shared<PipeSharedState>();
        pair.sender   = std::make_unique<PipeSender>(state);
        pair.receiver = std::make_unique<PipeReceiver>(state);
    }
    else if (mechanism == "fifo") {
        std::string path = "/tmp/ipc_bench_" + id + ".fifo";
        pair.sender   = std::make_unique<FifoSender>(path);
        pair.receiver = std::make_unique<FifoReceiver>(path);
    }
    else if (mechanism == "mq") {
        std::string name = "/ipc_bench_mq_" + id;
        pair.sender   = std::make_unique<MQSender>(name);
        pair.receiver = std::make_unique<MQReceiver>(name);
    }
    else if (mechanism == "shm") {
        auto state = std::make_shared<ShmSharedState>();
        pair.sender   = std::make_unique<ShmSender>(state);
        pair.receiver = std::make_unique<ShmReceiver>(state);
    }
    else if (mechanism == "rdma_shm") {
        auto state = std::make_shared<RdmaShmSharedState>();
        pair.sender   = std::make_unique<RdmaShmSender>(state);
        pair.receiver = std::make_unique<RdmaShmReceiver>(state);
    }
    else if (mechanism == "unix") {
        std::string path = "/tmp/ipc_bench_" + id + ".sock";
        pair.sender   = std::make_unique<UnixSocketSender>(path);
        pair.receiver = std::make_unique<UnixSocketReceiver>(path);
    }

    return pair;
}