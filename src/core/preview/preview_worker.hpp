#pragma once

// **The one background thread behind the main menu's bottom-screen previews.**
//
// A skin is a PNG on the card, a pack's scene needs its terrain.png out of a
// zip, and a world's diorama is 576 chunk inflates over a batched handful of
// card reads. None of that may happen in a frame (CONTRIBUTING.md: no
// filesystem or decompression on the render thread), and all of it has to be
// ready *before* the cursor arrives or scrolling stops feeling free. So the
// menu says what it wants, nearest row first, and this works through it and
// hands results back to be picked up at the top of a frame.
//
// **Wanting replaces, it does not accumulate.** Every cursor move posts the
// whole wanted list for its screen again, and anything no longer on it is
// dropped from the queue. A job already running is asked -- through
// `stillWanted`, which a long job checks between chunks -- rather than
// interrupted, so a world that is scrolled past stops reading at the next chunk
// and keeps what it has.
//
// **Results are data, not callbacks.** The main thread polls; nothing here ever
// calls into the menu or touches the GPU, and a result for a row that has since
// scrolled away is simply one the menu ignores.
//
// **`quiesce` is the one wait**, and it exists because a world the diorama is
// reading may be the world the player is about to open, delete, copy or
// convert. It drops every job about that world and waits for a running one to
// notice -- at most one chunk read -- on a button press that is about to block
// on the card for far longer anyway.

#include "core/util/types.hpp"
#include "core/util/worker.hpp"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace mc::preview {

// One screen's worth of jobs is one kind. The menu names them; this only uses
// the number to keep one screen's queue from replacing another's.
inline constexpr int kPreviewKinds = 4;

struct PreviewJob {
    u8 kind = 0;
    // The row the job is for, handed back on its results.
    i32 index = 0;
    // What the job is about, and what `quiesce` and `stillWanted` match on: a
    // world's path, a skin's key, a pack's path.
    std::string key;
    // Where to read from, when that is not the key itself.
    std::string path;
};

struct PreviewResult {
    u8 kind = 0;
    i32 index = 0;
    std::string key;
    bool ok = false;
    // Small numbers a kind wants alongside its bytes: a tile index, a vertex
    // count, the diorama's six per-facing counts. What they mean is the kind's
    // business.
    i32 value[10] = {};
    std::vector<u8> bytes;
};

class PreviewWorker {
public:
    // Runs one job on the worker's thread. It may post any number of results,
    // including none, and should return early when `stillWanted` says so.
    using Handler = void (*)(void* context, PreviewWorker& worker, const PreviewJob& job);

    PreviewWorker() = default;
    ~PreviewWorker();

    PreviewWorker(const PreviewWorker&) = delete;
    PreviewWorker& operator=(const PreviewWorker&) = delete;

    // Starts the thread: the platform's, with `role`, when the platform
    // installed worker ops, and `std::thread` otherwise. False when the
    // platform refused one, in which case `runOne` still works.
    bool start(Handler handler, void* context, WorkerRole role);

    // Drops everything queued and joins. Safe to call when not started.
    void stop();

    bool running() const;

    // Replaces what `kind` wants with `jobs`, in the order given. A job that
    // is already running and is on the new list is not queued a second time.
    void setWanted(u8 kind, std::vector<PreviewJob> jobs);

    // The oldest finished result, if there is one. Main thread.
    bool poll(PreviewResult* out);

    // Forgets every job about `key` and returns once none is running.
    void quiesce(std::string_view key);

    // Worker side: whether the latest wanted list for this job's kind still
    // names its key.
    bool stillWanted(const PreviewJob& job) const;

    // Worker side: hands a result back.
    void post(PreviewResult&& result);

    // Runs the next queued job on the calling thread, for the host tests and
    // for a console that would not give the menu a thread. False when there
    // was nothing to run.
    bool runOne();

private:
    static void entry(void* self);
    void workerMain();
    bool takeLocked(PreviewJob* out);
    void runJob(const PreviewJob& job);

    Handler handler_ = nullptr;
    void* context_ = nullptr;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable idle_;

    std::deque<PreviewJob> pending_[kPreviewKinds];
    std::vector<std::string> wanted_[kPreviewKinds];
    std::deque<PreviewResult> results_;

    // The job the thread is running, and whether there is one.
    PreviewJob current_;
    bool busy_ = false;
    bool stop_ = false;
    bool running_ = false;

    void* platformThread_ = nullptr;
    std::thread thread_;
};

}  // namespace mc::preview
