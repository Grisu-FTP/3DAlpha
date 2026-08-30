#pragma once

// a1.1.2's SoundPool: the name a resource file collapses to, and the uniform
// draw the music ticker makes from it.
//
// Transcribed from `eb.class` in minecraft-a1.1.2_01-client.jar. Two things
// happen here and they are easy to confuse, because the original stores both in
// the same object:
//
//   * Entries are **keyed** by a name derived from the resource path, and
//     `getRandomSoundFromSoundPool(key)` picks uniformly among the entries that
//     share a key. That is how `random.click` covers `random/click.ogg` and how
//     `step.grass` covers `grass1..grass6`.
//   * Entries are **also** kept in one flat list, and `getRandomSound()` -- no
//     argument -- picks uniformly from *that*, ignoring keys entirely. It has
//     exactly one caller: the background music ticker.
//
// The key derivation is three steps and every one of them is observable:
//
//     "random/click.ogg"  ->  "random/click"   strip from the first '.'
//                         ->  "random/click"   strip trailing digits, if random
//                         ->  "random.click"   '/' becomes '.'
//
// The digit strip is what makes `calm1`, `calm2` and `calm3` one key `calm`,
// and it is **conditional**: the streaming pool sets `isGetRandomSound = false`
// and keeps its digits, because a record is asked for by its exact name. Get
// that backwards and `mellohi` becomes unaddressable.
//
// **No track name is hardcoded, here or in the original.** The a1.1.2 jar
// contains no music and no list of it -- the client walked whatever the (now
// dead) resource server offered. The pool is therefore whatever files the
// player supplied, and a fixed table would be both wrong and unfaithful.
//
// The pool keeps **its own** `Random`, as `eb.c` does, rather than borrowing
// the SoundManager's. That is not a detail: the two streams are independent in
// the original, so which track is picked cannot perturb when the next one
// starts. Share one generator between them and adding a file to the card
// silently changes the music schedule, which is both wrong and impossible to
// debug from the outside.

#include "core/util/java_random.hpp"
#include "core/util/types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace mc::audio {

// One playable file. `name` is the original resource path with its extension
// intact -- the original keeps it because the codec is chosen from it -- and
// `path` is where it actually lives on this machine, standing in for the
// original's `URL`.
struct SoundEntry {
    std::string name;
    std::string path;
};

// `eb.a(String, File)`'s three-step derivation, exposed on its own so it can be
// tested without a file system. `stripDigits` is the pool's `isGetRandomSound`.
std::string poolKey(std::string_view resourceName, bool stripDigits);

class SoundPool {
public:
    // The original's `isGetRandomSound`, true for the sound and music pools and
    // false for the streaming one. It is a constructor argument rather than a
    // setter because the streaming pool's value is set before any entry is
    // added and changing it later would silently rekey the pool.
    //
    // a1.1.2 seeds `eb.c` from the wall clock. Seeding explicitly costs no
    // fidelity -- the draws are uniform either way -- and buys a test that can
    // name the track it expects.
    explicit SoundPool(bool randomised = true, i64 seed = 0)
        : randomised_(randomised), rand_(seed)
    {
    }

    // `eb.a(String, File)`. Returns the key it filed the entry under, which is
    // otherwise invisible and is exactly what a test wants to see.
    const std::string& add(std::string_view resourceName, std::string_view path);

    // `eb.a()` -- uniform over every entry in the pool, keys ignored. Null when
    // the pool is empty, which is the ordinary state of a game with no
    // resources folder and is not an error.
    const SoundEntry* randomEntry();

    // `eb.a(String)` -- uniform among the entries sharing `key`. Null when
    // nothing was filed under it.
    const SoundEntry* randomEntry(std::string_view key);

    // How many entries were filed under `key`. `randomEntry` answers a related
    // question but draws from the pool's Random to do it, and a screen that
    // wants to know whether a sound exists must not perturb the sequence that
    // decides which one plays -- so asking is separate from picking.
    usize countFor(std::string_view key) const;

    usize size() const { return entries_.size(); }
    bool empty() const { return entries_.empty(); }

    // For tests and for the options screen's "n sounds" line.
    const std::vector<SoundEntry>& entries() const { return entries_; }

private:
    struct Bucket {
        std::string key;
        std::vector<usize> indices;
    };

    bool randomised_;
    JavaRandom rand_;
    std::vector<SoundEntry> entries_;
    std::vector<Bucket> buckets_;
    std::string lastKey_;
};

}  // namespace mc::audio
