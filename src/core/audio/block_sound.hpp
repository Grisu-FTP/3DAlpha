#pragma once

// **Which noise a block makes, and how loud** -- the three call sites of
// `Block.stepSound` and the arithmetic each one applies.
//
// The table itself is generated (`block::stepSoundOf`, from blocks.json); what
// lives here is what the *callers* do with it, because that is where the three
// differ and where getting it wrong is audible:
//
//   | Event | Sound      | Volume          | Pitch       |
//   |-------|------------|-----------------|-------------|
//   | step  | `step`     | `volume * 0.15` | `pitch`     |
//   | break | `breakSound` | `(volume + 1) / 2` | `pitch * 0.8` |
//   | place | `step`      | `(volume + 1) / 2` | `pitch * 0.8` |
//
// Placing is the row worth staring at: `ItemBlock.onItemUse` plays the *step*
// getter at the *break* volume, so a block is placed with a footstep at a break
// block's loudness. Glass therefore breaks with `random.glass` and is placed
// with `step.stone`, which is a1.1.2 and not a slip here.
//
// A footstep's `* 0.15` is why none of these need the interface path's 0.25
// factor: these are positional sounds, attenuated by distance instead. See
// `audio::positionalGain`.

#include "core/block/block_def.hpp"
#include "core/util/types.hpp"

#include <string_view>

namespace mc::audio {

class SoundEngine;

// A sound ready to be played: the pool key, and the two numbers `playSoundAt`
// wants. `key` is empty for a block with no sound, which is air and any id this
// build does not know -- callers test `playable()` rather than the block.
struct SoundCue {
    std::string_view key;
    float volume = 0.0f;
    float pitch = 1.0f;

    bool playable() const { return !key.empty() && volume > 0.0f; }
};

// `Entity.moveEntity`'s footstep, `PlayerController.onPlayerDestroyBlock`'s
// break, and `ItemBlock.onItemUse`'s place.
SoundCue stepCue(block::BlockId id);
SoundCue breakCue(block::BlockId id);
SoundCue placeCue(block::BlockId id);

// Decode every footstep and break sound the block table can ask for, once.
//
// **This opens files and runs a decoder**, so it belongs at boot beside
// `loadResources`. It is bounded by the table rather than by the card: nine
// singletons name at most a handful of distinct keys, each covering however
// many numbered variants the player happens to have, and a key with no files
// costs nothing. Returns how many samples were taken.
usize preloadBlockSounds(SoundEngine& engine);

}  // namespace mc::audio
