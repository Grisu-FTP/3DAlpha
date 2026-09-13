#pragma once

// **What the world's sound sink was handed**, in the order it was handed it.
//
// `TickWorld::playSoundAt` is the one seam every sound the engine emits from
// core goes through -- a door, a pressure plate, a cow, an arrow striking a
// wall, a body hitting water -- so a test that wants to know what something
// sounded like wires one of these to the world instead of building an audio
// backend. Nothing here decodes anything and nothing here needs a device.
//
// The mirror of tests/drop_catcher.hpp, and for the same reason: core plays
// through a function pointer so that `core/tick/` and `core/entity/` never own
// a sound engine. See core/tick/tick_world.hpp.

#include "core/tick/tick_world.hpp"
#include "core/util/types.hpp"

#include <string>
#include <vector>

namespace mc::test {

struct Sound {
    std::string key;
    double x, y, z;
    float volume, pitch;
};

struct SoundCatcher {
    std::vector<Sound> sounds;

    static void sink(void* ctx, const char* key, double x, double y, double z, float volume,
                     float pitch)
    {
        static_cast<SoundCatcher*>(ctx)->sounds.push_back(
            Sound{std::string(key), x, y, z, volume, pitch});
    }

    // Held by pointer, so the catcher has to outlive the world -- which every
    // fixture here arranges by declaring them in that order.
    void watch(tick::TickWorld& world) { world.setSoundSink(&SoundCatcher::sink, this); }

    void clear() { sounds.clear(); }

    int countOf(const char* key) const
    {
        int n = 0;
        for (const Sound& s : sounds) {
            n += int(s.key == key);
        }
        return n;
    }

    // The first sound under this key, or null. Tests want the numbers on it,
    // not just that it happened.
    const Sound* first(const char* key) const
    {
        for (const Sound& s : sounds) {
            if (s.key == key) {
                return &s;
            }
        }
        return nullptr;
    }
};

}  // namespace mc::test
