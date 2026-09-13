#include "core/preview/preview_worker.hpp"

#include <algorithm>
#include <utility>

namespace mc::preview {

PreviewWorker::~PreviewWorker()
{
    stop();
}

bool PreviewWorker::start(Handler handler, void* context, WorkerRole role)
{
    stop();
    {
        std::lock_guard<std::mutex> guard(mutex_);
        handler_ = handler;
        context_ = context;
        stop_ = false;
        running_ = true;
    }

    if (workerSpawn() != nullptr && workerJoin() != nullptr) {
        platformThread_ = workerSpawn()(&PreviewWorker::entry, this, role);
        if (platformThread_ == nullptr) {
            std::lock_guard<std::mutex> guard(mutex_);
            running_ = false;
            return false;
        }
        return true;
    }
    thread_ = std::thread([this] { workerMain(); });
    return true;
}

void PreviewWorker::stop()
{
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (!running_) {
            return;
        }
        stop_ = true;
        for (int kind = 0; kind < kPreviewKinds; ++kind) {
            pending_[kind].clear();
            wanted_[kind].clear();
        }
    }
    wake_.notify_all();
    if (platformThread_ != nullptr) {
        workerJoin()(platformThread_);
        platformThread_ = nullptr;
    } else if (thread_.joinable()) {
        thread_.join();
    }
    std::lock_guard<std::mutex> guard(mutex_);
    running_ = false;
    // Stopped is not dead: the queues are empty and `runOne` works again, so
    // a menu that gave its thread back can still be handed work.
    stop_ = false;
    results_.clear();
}

bool PreviewWorker::running() const
{
    std::lock_guard<std::mutex> guard(mutex_);
    return running_;
}

void PreviewWorker::setWanted(u8 kind, std::vector<PreviewJob> jobs)
{
    if (kind >= kPreviewKinds) {
        return;
    }
    {
        std::lock_guard<std::mutex> guard(mutex_);
        std::vector<std::string>& wanted = wanted_[kind];
        wanted.clear();
        wanted.reserve(jobs.size());
        std::deque<PreviewJob>& pending = pending_[kind];
        pending.clear();
        for (PreviewJob& job : jobs) {
            job.kind = kind;
            wanted.push_back(job.key);
            if (busy_ && current_.kind == kind && current_.key == job.key) {
                continue;
            }
            pending.push_back(std::move(job));
        }
    }
    wake_.notify_all();
}

bool PreviewWorker::poll(PreviewResult* out)
{
    std::lock_guard<std::mutex> guard(mutex_);
    if (results_.empty()) {
        return false;
    }
    *out = std::move(results_.front());
    results_.pop_front();
    return true;
}

void PreviewWorker::quiesce(std::string_view key)
{
    std::unique_lock<std::mutex> guard(mutex_);
    for (int kind = 0; kind < kPreviewKinds; ++kind) {
        std::deque<PreviewJob>& pending = pending_[kind];
        pending.erase(std::remove_if(pending.begin(), pending.end(),
                                     [&](const PreviewJob& job) { return job.key == key; }),
                      pending.end());
        std::vector<std::string>& wanted = wanted_[kind];
        wanted.erase(std::remove(wanted.begin(), wanted.end(), key), wanted.end());
    }
    idle_.wait(guard, [&] { return !busy_ || current_.key != key; });
}

bool PreviewWorker::stillWanted(const PreviewJob& job) const
{
    std::lock_guard<std::mutex> guard(mutex_);
    if (stop_ || job.kind >= kPreviewKinds) {
        return false;
    }
    const std::vector<std::string>& wanted = wanted_[job.kind];
    return std::find(wanted.begin(), wanted.end(), job.key) != wanted.end();
}

void PreviewWorker::post(PreviewResult&& result)
{
    std::lock_guard<std::mutex> guard(mutex_);
    if (stop_) {
        return;
    }
    results_.push_back(std::move(result));
}

bool PreviewWorker::runOne()
{
    PreviewJob job;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (busy_ || !takeLocked(&job)) {
            return false;
        }
    }
    runJob(job);
    return true;
}

void PreviewWorker::entry(void* self)
{
    static_cast<PreviewWorker*>(self)->workerMain();
}

void PreviewWorker::workerMain()
{
    for (;;) {
        PreviewJob job;
        {
            std::unique_lock<std::mutex> guard(mutex_);
            wake_.wait(guard, [&] {
                if (stop_) {
                    return true;
                }
                for (int kind = 0; kind < kPreviewKinds; ++kind) {
                    if (!pending_[kind].empty()) {
                        return true;
                    }
                }
                return false;
            });
            if (stop_) {
                return;
            }
            if (!takeLocked(&job)) {
                continue;
            }
        }
        runJob(job);
    }
}

bool PreviewWorker::takeLocked(PreviewJob* out)
{
    // Kinds in order: only one screen is up at a time, so this is a tie-break
    // that never matters in practice rather than a priority scheme.
    for (int kind = 0; kind < kPreviewKinds; ++kind) {
        if (!pending_[kind].empty()) {
            *out = std::move(pending_[kind].front());
            pending_[kind].pop_front();
            current_ = *out;
            busy_ = true;
            return true;
        }
    }
    return false;
}

void PreviewWorker::runJob(const PreviewJob& job)
{
    Handler handler = nullptr;
    void* context = nullptr;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        handler = handler_;
        context = context_;
    }
    if (handler != nullptr) {
        handler(context, *this, job);
    }
    {
        std::lock_guard<std::mutex> guard(mutex_);
        busy_ = false;
        current_ = PreviewJob{};
    }
    idle_.notify_all();
}

}  // namespace mc::preview
