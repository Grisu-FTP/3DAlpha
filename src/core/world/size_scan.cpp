#include "core/world/size_scan.hpp"

#include <utility>

namespace mc::world {

SizeScan::~SizeScan()
{
    cancel();
}

void SizeScan::start(io::FileSystem& fs, std::string_view worldDir)
{
    cancel();

    fs_ = &fs;
    path_.assign(worldDir);
    {
        std::lock_guard<std::mutex> guard(mutex_);
        result_ = WorldSize();
        state_ = path_.empty() ? State::Failed : State::Running;
    }
    if (path_.empty()) {
        return;
    }
    stop_.store(false);

    if (workerSpawn() != nullptr && workerJoin() != nullptr) {
        platformThread_ = workerSpawn()(&SizeScan::entry, this, WorkerRole::Io);
        if (platformThread_ != nullptr) {
            return;
        }
        // The console would not give this one a thread. Measuring inline is
        // what the screen did before there was a thread to ask for: it blocks,
        // which is bad, against showing no size at all, which is worse.
        run();
        return;
    }
    thread_ = std::thread([this] { run(); });
}

void SizeScan::cancel()
{
    stop_.store(true);
    join();
    std::lock_guard<std::mutex> guard(mutex_);
    if (state_ == State::Running) {
        // Only reachable when a thread was refused and `run` never got to
        // finish -- a joined walk has already said how it ended.
        state_ = State::Idle;
    }
}

SizeScan::State SizeScan::state() const
{
    std::lock_guard<std::mutex> guard(mutex_);
    return state_;
}

bool SizeScan::result(WorldSize* out) const
{
    std::lock_guard<std::mutex> guard(mutex_);
    if (state_ != State::Done) {
        return false;
    }
    *out = result_;
    return true;
}

void SizeScan::entry(void* self)
{
    static_cast<SizeScan*>(self)->run();
}

bool SizeScan::keepGoing(void* self)
{
    return !static_cast<SizeScan*>(self)->stop_.load();
}

void SizeScan::run()
{
    // Measured into a local and published at the end, so the main thread never
    // reads a total half of a tree has been added to.
    WorldSize measured;
    const bool ok = worldSize(*fs_, path_, &measured, this, &SizeScan::keepGoing);

    std::lock_guard<std::mutex> guard(mutex_);
    if (stop_.load()) {
        // Cancelled rather than failed. The screen that asked has gone, and the
        // partial number it would have shown is not one.
        state_ = State::Idle;
        return;
    }
    result_ = measured;
    state_ = ok ? State::Done : State::Failed;
}

void SizeScan::join()
{
    if (platformThread_ != nullptr) {
        workerJoin()(platformThread_);
        platformThread_ = nullptr;
        return;
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

}  // namespace mc::world
