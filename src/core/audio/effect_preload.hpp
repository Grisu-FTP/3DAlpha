#pragma once

// **The whole set of effect keys this port can make a noise about, in one
// place.**
//
// It exists because two callers have to agree on it and used to not. The
// console decodes the set on the audio worker at boot; `--audio-list` decodes
// it on the host so that the cost -- how many samples, how much linear memory
// -- can be *measured* against a real resources folder before the cap that has
// to hold it is chosen. When the list lived in `platform/ctr/main.cpp` the host
// harness could only ever check the menu click, so every other key's cost was a
// guess and `mob.*` was simply forgotten.
//
// **Preloading is not an optimisation.** A key that was never decoded plays
// nothing, deliberately and without an error -- there is no filesystem and no
// Vorbis in the per-frame path, so `playSoundFX` is a handle and two floats and
// can be nothing else. See core/audio/sound_engine.hpp. That makes this list
// the honest statement of what this port can be heard doing: anything missing
// from it is silent no matter how carefully its call site was transcribed.
//
// Two halves, and they are different in kind:
//
//   * **Derived.** Footsteps and break sounds come off the generated block
//     table (`preloadBlockSounds`) and the animals' three sounds apiece come
//     off `MobDef` (`entity::preloadMobSounds`). Those tables are the only
//     things that know which keys exist, so a hand-written copy beside them
//     would be a second version of the truth that nothing checks.
//   * **Listed.** A sound that a *behaviour* plays -- a door, a lever, a
//     pressure plate -- has no column anywhere, and neither does one that
//     `Entity` itself plays. Those are literals below. A key the player has no
//     files for costs nothing, so a list that is slightly too long is free and
//     one that is too short is a silence.

#include "core/util/types.hpp"

namespace mc::audio {

class SoundEngine;

// Decodes every effect key this build can name. Returns how many pool entries
// decoded -- which is the number `ctr::kMaxSamples` has to be able to hold, and
// is what `--audio-list` prints.
usize preloadEffects(SoundEngine& engine);

}  // namespace mc::audio
