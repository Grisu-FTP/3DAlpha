#include "core/audio/music_ticker.hpp"

namespace mc::audio {

MusicTicker::MusicTicker(i64 seed) : rand_(seed), counter_(0)
{
    // `SoundManager`'s constructor: the initial countdown, a draw below 12000 -- 0..11999
    // ticks, so the
    // first track of a session arrives somewhere in the first ten minutes, mean
    // five. It is drawn in the constructor and not on the first tick, so the
    // draw happens once per SoundManager and not once per world.
    counter_ = rand_.nextInt(12000);
}

const SoundEntry* MusicTicker::tick(SoundPool& musicPool, const MusicState& state)
{
    if (!state.available || state.musicVolume == 0.0f) {
        return nullptr;
    }

    // Both of these precede the decrement in the original, which is what makes
    // the silence between tracks a full 20-40 minutes rather than a gap
    // measured from the moment the last track started. See the header.
    if (state.musicPlaying || state.recordPlaying) {
        return nullptr;
    }

    if (counter_ > 0) {
        --counter_;
        return nullptr;
    }

    // a method of another original class -- uniform over every installed music entry, `music/` and
    // `newmusic/` together, keys ignored. The pool draws from its own
    // generator, so which track this is cannot shift when the next one starts.
    const SoundEntry* entry = musicPool.randomEntry();
    if (entry == nullptr) {
        // No resources folder, or one with no music in it. The counter is left
        // at zero on purpose: the original does the same, so the moment a pool
        // becomes non-empty a track starts, and a player who copies files onto
        // the card does not wait out a fresh 20 minutes.
        return nullptr;
    }

    // the re-armed countdown, 24000 plus a draw below 24000 -- 24000..47999
    // ticks, 20 to 40
    // minutes. This generator draws nothing else, so the schedule is a function
    // of the seed alone and does not move when the player adds a track.
    counter_ = rand_.nextInt(24000) + 24000;
    return entry;
}

}  // namespace mc::audio
