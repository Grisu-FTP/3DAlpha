// The hand-off between the effect decode worker and the main thread; see
// sample_stage.hpp.

#include "core/audio/sample_stage.hpp"

#include <utility>

namespace mc::audio {

SampleStage::SampleStage(usize capacity) : capacity_(capacity > 0 ? capacity : 1)
{
    waiting_.reserve(capacity_);
}

bool SampleStage::push(StagedSample&& staged)
{
    std::unique_lock<std::mutex> lock(mutex_);
    room_.wait(lock, [this] { return abandoned_ || waiting_.size() < capacity_; });
    if (abandoned_) {
        return false;
    }
    waiting_.push_back(std::move(staged));
    return true;
}

void SampleStage::finish()
{
    std::lock_guard<std::mutex> lock(mutex_);
    finished_ = true;
}

bool SampleStage::drain(std::vector<StagedSample>* out)
{
    bool moved = false;
    bool over = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (StagedSample& staged : waiting_) {
            out->push_back(std::move(staged));
        }
        moved = !waiting_.empty();
        waiting_.clear();
        over = finished_;
    }
    if (moved) {
        room_.notify_all();
    }
    return over;
}

void SampleStage::abandon()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        abandoned_ = true;
        waiting_.clear();
    }
    room_.notify_all();
}

bool SampleStage::abandoned() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return abandoned_;
}

}  // namespace mc::audio
