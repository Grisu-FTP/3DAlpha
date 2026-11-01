// Section meshing on a worker thread: slots filled on the caller's thread, meshed on
// the worker, collected and uploaded by the caller. See mesh_pool.hpp.

#include "core/mesh/mesh_pool.hpp"

namespace mc::mesh {

void MeshPool::start(int slots, bool threaded, int quadReserve)
{
    stop();
    slots_.clear();
    for (int i = 0; i < slots; ++i) {
        auto slot = std::make_unique<Slot>();
        slot->builder.setGreedy(greedy_);
        slot->builder.setCubeFormat(format_);
        slot->builder.reserveQuads(quadReserve);
        slots_.push_back(std::move(slot));
    }
    stopping_ = false;
    threaded_ = false;
    if (!threaded || slots <= 0) {
        return;
    }
    if (workerSpawn() != nullptr && workerJoin() != nullptr) {
        platformThread_ = workerSpawn()(&MeshPool::entry, this, WorkerRole::Mesh);
        threaded_ = platformThread_ != nullptr;
    } else {
        thread_ = std::thread([this] { loop(); });
        threaded_ = true;
    }
}

void MeshPool::stop()
{
    {
        std::lock_guard<std::mutex> guard(lock_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (platformThread_ != nullptr) {
        workerJoin()(platformThread_);
        platformThread_ = nullptr;
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    threaded_ = false;
}

void MeshPool::entry(void* self)
{
    static_cast<MeshPool*>(self)->loop();
}

bool MeshPool::submit(i32 chunkX, int sectionY, i32 chunkZ, u32 revision,
                      const ColumnNeighbourhood& neighbourhood)
{
    Slot* slot = nullptr;
    {
        std::lock_guard<std::mutex> guard(lock_);
        for (auto& candidate : slots_) {
            if (candidate->state == State::Free) {
                slot = candidate.get();
                // Claimed before the lock drops so no second submit takes it;
                // Running keeps the worker away until the fill is done.
                slot->state = State::Running;
                break;
            }
        }
    }
    if (slot == nullptr) {
        return false;
    }

    // The part that reads live columns, on the caller's thread and outside the
    // lock: the worker never sees a slot in this state.
    slot->scratch.fill(neighbourhood, sectionY);
    slot->builder.clear();

    {
        std::lock_guard<std::mutex> guard(lock_);
        slot->chunkX = chunkX;
        slot->chunkZ = chunkZ;
        slot->sectionY = sectionY;
        slot->revision = revision;
        slot->order = nextOrder_++;
        slot->state = State::Queued;
    }
    wake_.notify_one();
    return true;
}

bool MeshPool::runOne()
{
    Slot* slot = nullptr;
    {
        std::lock_guard<std::mutex> guard(lock_);
        for (auto& candidate : slots_) {
            if (candidate->state == State::Queued
                && (slot == nullptr || candidate->order < slot->order)) {
                slot = candidate.get();
            }
        }
        if (slot == nullptr) {
            return false;
        }
        slot->state = State::Running;
    }
    meshSection(slot->scratch, slot->builder);
    {
        std::lock_guard<std::mutex> guard(lock_);
        slot->state = State::Done;
    }
    return true;
}

void MeshPool::loop()
{
    for (;;) {
        {
            std::unique_lock<std::mutex> guard(lock_);
            wake_.wait(guard, [this] {
                if (stopping_) {
                    return true;
                }
                for (auto& slot : slots_) {
                    if (slot->state == State::Queued) {
                        return true;
                    }
                }
                return false;
            });
            if (stopping_) {
                return;
            }
        }
        runOne();
    }
}

void MeshPool::runPending()
{
    while (runOne()) {
    }
}

bool MeshPool::poll(Result* out)
{
    std::lock_guard<std::mutex> guard(lock_);
    Slot* best = nullptr;
    int bestIndex = -1;
    for (usize i = 0; i < slots_.size(); ++i) {
        Slot* candidate = slots_[i].get();
        if (candidate->state == State::Done && (best == nullptr || candidate->order < best->order)) {
            best = candidate;
            bestIndex = int(i);
        }
    }
    if (best == nullptr) {
        return false;
    }
    out->chunkX = best->chunkX;
    out->chunkZ = best->chunkZ;
    out->sectionY = best->sectionY;
    out->revision = best->revision;
    out->builder = &best->builder;
    out->slot = bestIndex;
    // Held by the caller until release(); not Free, not Done, so neither a
    // submit nor another poll can reach it.
    best->state = State::Running;
    return true;
}

void MeshPool::release(int slot)
{
    std::lock_guard<std::mutex> guard(lock_);
    slots_[usize(slot)]->state = State::Free;
}

int MeshPool::inFlight() const
{
    std::lock_guard<std::mutex> guard(lock_);
    int n = 0;
    for (const auto& slot : slots_) {
        n += slot->state != State::Free ? 1 : 0;
    }
    return n;
}

bool MeshPool::inFlight(i32 chunkX, int sectionY, i32 chunkZ) const
{
    std::lock_guard<std::mutex> guard(lock_);
    for (const auto& slot : slots_) {
        if (slot->state != State::Free && slot->chunkX == chunkX && slot->chunkZ == chunkZ
            && slot->sectionY == sectionY) {
            return true;
        }
    }
    return false;
}

void MeshPool::setGreedy(bool on)
{
    greedy_ = on;
    std::lock_guard<std::mutex> guard(lock_);
    for (auto& slot : slots_) {
        if (slot->state == State::Free) {
            slot->builder.setGreedy(on);
        }
    }
}

void MeshPool::setCubeFormat(CubeFormat format)
{
    format_ = format;
    std::lock_guard<std::mutex> guard(lock_);
    for (auto& slot : slots_) {
        if (slot->state == State::Free) {
            slot->builder.setCubeFormat(format);
        }
    }
}

}  // namespace mc::mesh
