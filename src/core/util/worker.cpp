#include "core/util/worker.hpp"

namespace mc {
namespace {

WorkerSpawn gSpawn = nullptr;
WorkerJoin gJoin = nullptr;

}  // namespace

void setWorkerThreadOps(WorkerSpawn spawn, WorkerJoin join)
{
    // Both or neither: a spawn with no join would leak a thread handle on every
    // world close, and a join with no spawn would be handed a std::thread's
    // nothing.
    if (spawn == nullptr || join == nullptr) {
        gSpawn = nullptr;
        gJoin = nullptr;
        return;
    }
    gSpawn = spawn;
    gJoin = join;
}

WorkerSpawn workerSpawn()
{
    return gSpawn;
}

WorkerJoin workerJoin()
{
    return gJoin;
}

}  // namespace mc
