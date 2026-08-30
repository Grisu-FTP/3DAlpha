#include "core/audio/sound_pool.hpp"

namespace mc::audio {

namespace {

bool isAsciiDigit(char c)
{
    // Deliberately not `std::isdigit`, which is locale-dependent and whose
    // argument must be representable as unsigned char. A resource name comes
    // off a card and can hold anything.
    return c >= '0' && c <= '9';
}

}  // namespace

std::string poolKey(std::string_view resourceName, bool stripDigits)
{
    std::string key(resourceName);

    // `name.substring(0, name.indexOf("."))`. The original would throw on a
    // name with no dot; there are none in practice, and truncating to nothing
    // would be worse than keeping the name, so an absent dot leaves it whole.
    const usize dot = key.find('.');
    if (dot != std::string::npos) {
        key.resize(dot);
    }

    // `while (Character.isDigit(name.charAt(name.length() - 1)))`. The original
    // indexes an empty string here if a name is all digits, so "1.ogg" is a
    // crash in a1.1.2; we stop instead, because a card can hold that file and a
    // player is not owed a hang for it.
    if (stripDigits) {
        while (!key.empty() && isAsciiDigit(key.back())) {
            key.pop_back();
        }
    }

    // `name.replaceAll("/", ".")`.
    for (char& c : key) {
        if (c == '/') {
            c = '.';
        }
    }

    return key;
}

const std::string& SoundPool::add(std::string_view resourceName, std::string_view path)
{
    const std::string key = poolKey(resourceName, randomised_);

    entries_.push_back(SoundEntry{std::string(resourceName), std::string(path)});
    const usize index = entries_.size() - 1;

    // Linear, and that is the right shape here: a full resources folder is a
    // few hundred entries built once at boot, and a hash map would cost more in
    // allocation than the scan ever costs in time.
    for (Bucket& bucket : buckets_) {
        if (bucket.key == key) {
            bucket.indices.push_back(index);
            lastKey_ = key;
            return lastKey_;
        }
    }

    buckets_.push_back(Bucket{key, {index}});
    lastKey_ = key;
    return lastKey_;
}

const SoundEntry* SoundPool::randomEntry()
{
    if (entries_.empty()) {
        return nullptr;
    }
    return &entries_[usize(rand_.nextInt(i32(entries_.size())))];
}

const SoundEntry* SoundPool::randomEntry(std::string_view key)
{
    for (const Bucket& bucket : buckets_) {
        if (bucket.key == key) {
            const usize pick = bucket.indices[usize(rand_.nextInt(i32(bucket.indices.size())))];
            return &entries_[pick];
        }
    }
    return nullptr;
}

}  // namespace mc::audio
