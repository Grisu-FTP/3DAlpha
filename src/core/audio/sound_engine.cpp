#include "core/audio/sound_engine.hpp"

#include "core/audio/vorbis_stream.hpp"

namespace mc::audio {

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
    musicVolume_ = volume < 0.0f ? 0.0f : (volume > 1.0f ? 1.0f : volume);

    // `of.a()`: zero stops the track outright rather than playing it silently,
    // so turning music off frees the decode thread and the wave buffers
    // instead of leaving them running for nothing.
    if (musicVolume_ == 0.0f) {
        backend_.stopMusic();
    } else {
        backend_.setMusicGain(musicVolume_);
    }
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
