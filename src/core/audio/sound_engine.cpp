#include "core/audio/sound_engine.hpp"

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

void SoundEngine::tick(int elapsedTicks)
{
    if (elapsedTicks <= 0 || !backend_.available()) {
        return;
    }

    MusicState state;
    state.available = true;
    state.musicVolume = musicVolume_;
    state.musicPlaying = backend_.musicPlaying();

    // Records are a stage-4 concern -- there is no jukebox and `.mus` is
    // Mojang's own container, which nothing here decodes. The flag is wired
    // through rather than omitted so the rule it guards is already tested.
    state.recordPlaying = false;

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
