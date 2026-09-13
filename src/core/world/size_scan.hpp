#pragma once

// **A world's size, measured off the frame.**
//
// `worldSize` walks the whole tree, and on a folder world that is a stat per
// chunk file across up to 4,096 leaf directories -- seconds on a card, for a
// world big enough to be worth asking about. The World Settings screen used to
// call it on the way in, which stopped the menu dead while it ran: a screen
// that simply freezes looks like a crash, and printing "measuring" first only
// made it a crash with a caption.
//
// So the walk moves to a thread of its own -- the I/O role, core 0 just below
// the main thread, which is where a thread that lives in FS round trips
// belongs -- and the screen draws "loading..." until an answer turns up. The
// main thread polls; nothing here calls back into a menu.
//
// **A result is kept until the next `start`.** `cancel` drops a walk still in
// progress and joins it, which the walk notices between directory entries, so
// it returns within one stat rather than one world. That is what makes it safe
// to call on the way out of the screen, and before a convert, copy or delete
// rewrites the very tree being walked.
//
// With no platform worker ops installed -- the host tests -- `std::thread` is
// used, which is the same shape. If a console refuses a thread the walk runs
// inline on the calling thread instead: slow, but never wrong.

#include "core/io/file_system.hpp"
#include "core/util/types.hpp"
#include "core/util/worker.hpp"
#include "core/world/world_list.hpp"

#include <atomic>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace mc::world {

class SizeScan {
public:
    enum class State {
        Idle,     // nothing has been asked for, or what was asked for was cancelled
        Running,  // a thread is walking the tree
        Done,     // `result` has the total
        Failed,   // the tree could not be walked; there is no number to show
    };

    SizeScan() = default;
    ~SizeScan();

    SizeScan(const SizeScan&) = delete;
    SizeScan& operator=(const SizeScan&) = delete;

    // Starts measuring `worldDir`. Any scan already running is cancelled and
    // joined first, so the object is always about one world. `fs` must outlive
    // the scan, which the menu's own file system does.
    void start(io::FileSystem& fs, std::string_view worldDir);

    // Stops a walk in progress and joins its thread. A finished measurement is
    // kept -- this drops work, not answers. Safe when nothing is running.
    void cancel();

    State state() const;
    bool running() const { return state() == State::Running; }

    // The finished total. False while a walk is running, and after one that
    // failed or was cancelled.
    bool result(WorldSize* out) const;

private:
    static void entry(void* self);
    static bool keepGoing(void* self);
    void run();
    void join();

    io::FileSystem* fs_ = nullptr;
    std::string path_;

    mutable std::mutex mutex_;
    WorldSize result_;
    State state_ = State::Idle;

    // Read by the walking thread between entries, written by whoever cancels.
    std::atomic<bool> stop_{false};

    void* platformThread_ = nullptr;
    std::thread thread_;
};

}  // namespace mc::world
