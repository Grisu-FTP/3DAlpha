#include "core/audio/sound_engine.hpp"

#include <cmath>

#include "core/audio/sample.hpp"
#include "core/audio/vorbis_stream.hpp"

namespace mc::audio {

namespace {

float clamp01(float value)
{
    return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
}

}  // namespace

float interfaceGain(float volume, float soundVolume)
{
    return clamp01(volume) * 0.25f * clamp01(soundVolume);
}

float positionalGain(float volume, float distance, float soundVolume)
{
    if (volume <= 0.0f) {
        return 0.0f;
    }
    // The range stretches with a volume above 1 and the gain does not.
    const float range = volume > 1.0f ? 16.0f * volume : 16.0f;
    const float fade = distance >= range ? 0.0f : 1.0f - distance / range;
    return clamp01(volume) * clamp01(soundVolume) * fade;
}

SoundEngine::SoundEngine(io::FileSystem& fs, Backend& backend, i64 seed)
    : fs_(fs), backend_(backend), ticker_(seed)
{
}

usize SoundEngine::loadResources()
{
    return loadResources(kResourcesDir);
}

usize SoundEngine::loadResources(std::string_view root)
{
    indexResources(fs_, root, &resources_);
    return resources_.total();
}

void SoundEngine::setMusicVolume(float volume)
{
    musicVolume_ = clamp01(volume);

    // **A disc is on the same voice and this slider is not its.** `of.a()`
    // moves `BgMusic` only; a record is gated on `soundVolume`, so turning the
    // music off must not stop one. See `playRecord`.
    if (recordPlaying_) {
        return;
    }

    // `of.a()`: zero stops the track outright rather than playing it silently,
    // so turning music off frees the decode thread and the wave buffers
    // instead of leaving them running for nothing.
    if (musicVolume_ == 0.0f) {
        backend_.stopMusic();
    } else {
        backend_.setMusicGain(musicVolume_);
    }
}

void SoundEngine::setSoundVolume(float volume)
{
    soundVolume_ = clamp01(volume);

    // A disc *is* gated on this one, and `of.a` reads the slider at the moment
    // it starts a source rather than afterwards -- so a player who drags it to
    // zero with a record on hears it fade to nothing rather than stop, and it
    // comes back when they drag it up again. The next tick would do this; doing
    // it here is what makes the slider feel connected to the sound.
    if (recordPlaying_) {
        backend_.setMusicGain(recordGain());
    }
}

SampleId SoundEngine::sampleFor(std::string_view path) const
{
    for (const LoadedSample& loaded : samples_) {
        if (loaded.path == path) {
            return loaded.id;
        }
    }
    return kNoSample;
}

usize SoundEngine::preloadSound(std::string_view key)
{
    usize loaded = 0;
    for (const SoundEntry& entry : resources_.sounds.entries()) {
        // The pool files entries under the derived key but does not hand it
        // back per entry, so it is re-derived here. It is the same three-step
        // `eb.a(String, File)` derivation and it is cheap; this runs once, over
        // a few hundred names, at boot.
        if (poolKey(entry.name, true) != key) {
            continue;
        }
        if (sampleFor(entry.path) != kNoSample) {
            continue;  // already resident from an earlier call
        }

        Sample sample;
        if (!decodeSample(fs_, entry.path, &sample)) {
            // A file the player put on the card that this build cannot decode.
            // The original discovers the same thing at play time and also says
            // nothing; the only difference is that we discover it at boot.
            continue;
        }

        const SampleId id = backend_.addSample(sample);
        if (id == kNoSample) {
            continue;  // a silent backend, or one that is full
        }
        samples_.push_back(LoadedSample{entry.path, id});
        ++loaded;
    }
    return loaded;
}

void SoundEngine::playSoundFX(std::string_view key, float volume, float pitch)
{
    // `if (!loaded || options.soundVolume == 0.0F) return;` -- of.a's first
    // line, in of.a's order. The volume test is against exactly zero, as the
    // original's is.
    if (!backend_.available() || soundVolume_ == 0.0f) {
        return;
    }

    // `eb.a(String)`: uniform among the entries sharing the key, drawn from the
    // pool's own Random. The draw happens before we know whether the file was
    // preloaded, which is deliberate -- the original draws here too, and moving
    // the test in front of it would make the sequence depend on what this port
    // happens to have resident.
    const SoundEntry* entry = resources_.sounds.randomEntry(key);
    if (entry == nullptr) {
        return;
    }

    const SampleId id = sampleFor(entry->path);
    if (id == kNoSample) {
        return;  // never preloaded: silence, and not an error. See the header.
    }

    backend_.playSample(id, interfaceGain(volume, soundVolume_), pitch);
}

void SoundEngine::setListener(double x, double y, double z)
{
    listenerX_ = x;
    listenerY_ = y;
    listenerZ_ = z;
}

void SoundEngine::playSoundAt(std::string_view key, double x, double y, double z,
                              float volume, float pitch)
{
    // of.b's first line, and of.a's -- the same test in the same order.
    if (!backend_.available() || soundVolume_ == 0.0f) {
        return;
    }

    // The draw comes before the volume test in the original and it stays there:
    // moving it would make which file plays depend on how far away the player
    // happened to be.
    const SoundEntry* entry = resources_.sounds.randomEntry(key);
    if (entry == nullptr || volume <= 0.0f) {
        return;
    }

    const SampleId id = sampleFor(entry->path);
    if (id == kNoSample) {
        return;  // never preloaded: silence, and not an error.
    }

    const double dx = x - listenerX_;
    const double dy = y - listenerY_;
    const double dz = z - listenerZ_;
    const float distance = float(std::sqrt(dx * dx + dy * dy + dz * dz));

    const float gain = positionalGain(volume, distance, soundVolume_);
    if (gain <= 0.0f) {
        return;  // out of range: a voice spent on silence is a voice lost
    }
    backend_.playSample(id, gain, pitch);
}

void SoundEngine::tick(int elapsedTicks)
{
    if (elapsedTicks <= 0 || !backend_.available()) {
        return;
    }

    // **A disc follows the listener.** paulscode re-attenuates a positional
    // source as the listener moves; here the streaming voice has one gain, so
    // it is recomputed on every tick the record is on. One square root a tick.
    if (recordPlaying_) {
        if (backend_.musicPlaying()) {
            backend_.setMusicGain(recordGain());
        } else {
            // The disc ran out. a1.1.2 leaves the jukebox's metadata alone --
            // the record is still in it and clicking the block still gives it
            // back -- so nothing but the voice is released here.
            recordPlaying_ = false;
        }
    }

    MusicState state;
    state.available = true;
    state.musicVolume = musicVolume_;
    state.musicPlaying = backend_.musicPlaying();

    // `playing("streaming")` -- the second thing `of.c()` asks, and the reason
    // the music counter does not advance while a disc is on. On this backend
    // the disc is *on* the music voice, so `musicPlaying` above already
    // suppresses it; this is set anyway because the ticker's rule is written
    // against the original's two flags and not against their overlap here.
    state.recordPlaying = recordPlaying_;

    for (int i = 0; i < elapsedTicks; ++i) {
        if (const SoundEntry* entry = ticker_.tick(resources_.music, state)) {
            startTrack(*entry);
            // The backend is now playing, and the original re-reads that on the
            // next tick rather than assuming it. Same here: the remaining ticks
            // of this frame see a track in progress and leave the counter alone.
            state.musicPlaying = backend_.musicPlaying();
        }
    }
}

void SoundEngine::stopMusic()
{
    backend_.stopMusic();
    recordPlaying_ = false;
}

void SoundEngine::stopRecord()
{
    if (!recordPlaying_) {
        return;
    }
    backend_.stopMusic();
    recordPlaying_ = false;
}

bool SoundEngine::recordPlaying() const
{
    // Ours *and* the backend's: the voice is shared with the music, so a disc
    // that has simply reached its end is not playing any more even though
    // nothing ejected it.
    return recordPlaying_ && backend_.musicPlaying();
}

float SoundEngine::recordGain() const
{
    // `setVolume("streaming", 0.5F * options.soundVolume)`, over the streaming
    // source's own range -- `16.0F * 4.0F`, four times a one-shot's.
    constexpr float kRecordVolume = 0.5f;
    constexpr float kRecordRange = 16.0f * 4.0f;
    const double dx = recordX_ - listenerX_;
    const double dy = recordY_ - listenerY_;
    const double dz = recordZ_ - listenerZ_;
    const float distance = float(std::sqrt(dx * dx + dy * dy + dz * dz));
    const float fade = distance >= kRecordRange ? 0.0f : 1.0f - distance / kRecordRange;
    return kRecordVolume * clamp01(soundVolume_) * fade;
}

void SoundEngine::playRecord(const char* track, double x, double y, double z)
{
    // `of.a`'s first line, and it is **soundVolume** rather than musicVolume:
    // a player who turned the music off still hears a jukebox.
    if (!backend_.available() || soundVolume_ == 0.0f) {
        return;
    }

    // `if (playing("streaming")) stop("streaming")` -- whatever was on the
    // voice goes first, and that happens even when this call is the null one.
    if (recordPlaying_) {
        backend_.stopMusic();
        recordPlaying_ = false;
    }
    if (track == nullptr) {
        return;  // the eject: stop and start nothing
    }

    // `streamingPool.getRandomSound(name)` -- by name, because the streaming
    // pool is the one pool that is not randomised: "13" files exactly
    // `streaming/13.mus`. A card with no such file is silence, as a card with
    // no resources folder at all is.
    const SoundEntry* entry = resources_.streaming.randomEntry(track);
    if (entry == nullptr) {
        return;
    }

    // `if (playing("BgMusic")) stop("BgMusic")`. On this backend the two are
    // the same voice, so starting the disc replaces the track by itself --
    // but the counter must not be left thinking a track is still on.
    // **The codec comes off the file, not off the call.** A `streaming/` folder
    // copied from a beta-era install holds `13.mus` *and* `13.ogg`, and the
    // pool keys both as "13"; deciphering the plain one would be noise.
    std::unique_ptr<VorbisStream> stream = VorbisStream::open(fs_, entry->path);
    if (!stream) {
        return;  // no decoder in this build
    }

    recordX_ = x;
    recordY_ = y;
    recordZ_ = z;
    if (!backend_.playMusic(std::move(stream), recordGain())) {
        return;
    }
    recordPlaying_ = true;
}

void SoundEngine::startTrack(const SoundEntry& entry)
{
    // Nothing is opened here. `create` records the path and returns; the card
    // read and the header parse happen in `prepare()`, on whatever thread the
    // backend decodes on -- which is the point, because this call sits in the
    // frame loop and reading an SD card does not belong there.
    //
    // So a file the player put on the card that turns out to be unreadable
    // fails later and silently. The counter has already been reset by then, so
    // it costs one quiet interval rather than a retry against a broken file on
    // every tick.
    std::unique_ptr<VorbisStream> stream = VorbisStream::create(fs_, entry.path);
    if (!stream) {
        return;  // no decoder in this build
    }
    backend_.playMusic(std::move(stream), musicVolume_);
}

}  // namespace mc::audio
