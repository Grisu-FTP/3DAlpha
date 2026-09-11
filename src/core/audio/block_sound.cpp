// The three callers' arithmetic. See block_sound.hpp.

#include "core/audio/block_sound.hpp"

#include "core/audio/sound_engine.hpp"
#include "core/block/registry.hpp"

namespace mc::audio {
namespace {

// `(stepSound.getVolume() + 1.0F) / 2.0F` and `stepSound.getPitch() * 0.8F`,
// which a break and a place share exactly.
SoundCue loudCue(std::string_view key, const block::StepSound& sound)
{
    return SoundCue{key, (sound.volume + 1.0f) / 2.0f, sound.pitch * 0.8f};
}

}  // namespace

SoundCue stepCue(block::BlockId id)
{
    const block::StepSound& sound = block::stepSoundOf(id);
    if (sound.silent()) {
        return SoundCue{};
    }
    return SoundCue{sound.step, sound.volume * 0.15f, sound.pitch};
}

SoundCue breakCue(block::BlockId id)
{
    const block::StepSound& sound = block::stepSoundOf(id);
    if (sound.silent()) {
        return SoundCue{};
    }
    return loudCue(sound.breakSound, sound);
}

SoundCue placeCue(block::BlockId id)
{
    const block::StepSound& sound = block::stepSoundOf(id);
    if (sound.silent()) {
        return SoundCue{};
    }
    return loudCue(sound.step, sound);
}

usize preloadBlockSounds(SoundEngine& engine)
{
    // A linear scan over at most a dozen keys, skipping the ones already seen.
    // A set would be machinery for nine rows.
    std::string_view seen[2 * mcver::kStepSoundCount];
    int count = 0;
    usize loaded = 0;

    for (int row = 0; row < mcver::kStepSoundCount; ++row) {
        const block::StepSound& sound = mcver::kStepSounds[row];
        if (sound.silent()) {
            continue;
        }
        for (std::string_view key : {std::string_view(sound.step),
                                     std::string_view(sound.breakSound)}) {
            bool known = false;
            for (int i = 0; i < count; ++i) {
                known = known || seen[i] == key;
            }
            if (known) {
                continue;
            }
            seen[count++] = key;
            loaded += engine.preloadSound(key);
        }
    }
    return loaded;
}

}  // namespace mc::audio
