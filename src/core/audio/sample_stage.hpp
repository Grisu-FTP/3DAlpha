#pragma once

// **Where a decoded effect waits between the worker that decoded it and the
// main thread that hands it to the backend.**
//
// The boot used to decode every effect `preloadEffects` names -- 110 samples
// and 7 MB of PCM against a real resources folder -- on the audio worker and
// then *join* it, so the first menu waited for all of it: Tremor on an ARM11
// plus an SD round trip per file, with a black top screen. Now only the click
// is decoded before the menu, and the rest is decoded while the menu is up.
//
// **The worker decodes, and only the main thread commits.** Committing is
// `Backend::addSample` -- a `linearAlloc`, which libctru does not promise is
// safe beside the renderer's own -- and a push onto `SoundEngine`'s list of
// playable samples, which the frame that clicks is reading. So a decoded
// sample is parked here, and `SoundEngine::pumpPreload` takes what is waiting
// once a frame and commits it where nothing else can be looking.
//
// **Bounded, and the worker is the one that waits.** At most `capacity`
// samples sit here decoded and uncommitted, so the heap holds a few effects'
// PCM at a time rather than all of them, and a screen that never pumps -- a
// progress screen, a disconnect message -- stalls the decode rather than
// growing it. The main thread never waits on the worker in here: `drain`
// takes the lock, moves what is there and leaves.

#include "core/audio/sample.hpp"
#include "core/util/types.hpp"

#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

namespace mc::audio {

struct StagedSample {
    std::string path;
    Sample sample;
};

class SampleStage {
public:
    explicit SampleStage(usize capacity);

    // Worker. Blocks while `capacity` samples are waiting. False once the
    // stage has been abandoned, and the sample is dropped: nobody is left to
    // commit it.
    bool push(StagedSample&& staged);

    // Worker, and last: nothing is pushed after it.
    void finish();

    // Main thread. Moves everything waiting onto the end of `out` and wakes a
    // worker blocked in `push`. True once the worker has finished and nothing
    // is left, which means nothing will ever arrive again.
    bool drain(std::vector<StagedSample>* out);

    // Main thread. Wakes a worker blocked in `push`, fails every later push
    // and throws away what was waiting.
    void abandon();

    // Either side. The worker asks between files so an abandon does not wait
    // for a decode it no longer wants.
    bool abandoned() const;

private:
    mutable std::mutex mutex_;
    std::condition_variable room_;
    std::vector<StagedSample> waiting_;
    usize capacity_;
    bool finished_ = false;
    bool abandoned_ = false;
};

}  // namespace mc::audio
