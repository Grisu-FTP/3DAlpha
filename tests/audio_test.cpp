#include "framework.hpp"

#include "core/audio/backend.hpp"
#include "core/audio/music_ticker.hpp"
#include "core/audio/resource_index.hpp"
#include "core/audio/sample.hpp"
#include "core/audio/sound_engine.hpp"
#include "core/audio/sound_pool.hpp"
#include "core/audio/vorbis_stream.hpp"
#include "core/io/posix_file_system.hpp"
#include "core/util/java_random.hpp"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using namespace mc;
using namespace mc::audio;

namespace {

// The three-step derivation in eb.a(String, File), which is where every
// mis-keyed sound comes from.
TEST(poolKeyStripsExtensionAndSlashes)
{
    CHECK_EQ(poolKey("random/click.ogg", true), std::string("random.click"));
    CHECK_EQ(poolKey("mob/cow.ogg", true), std::string("mob.cow"));
    CHECK_EQ(poolKey("step/grass1.ogg", true), std::string("step.grass"));
}

// The digit strip is what makes calm1/2/3 one key, and it is conditional on the
// pool. The streaming pool keeps its digits because a record is asked for by
// name -- get this backwards and `mellohi` becomes unaddressable.
TEST(poolKeyStripsDigitsOnlyForRandomPools)
{
    CHECK_EQ(poolKey("calm1.ogg", true), std::string("calm"));
    CHECK_EQ(poolKey("hal4.ogg", true), std::string("hal"));
    CHECK_EQ(poolKey("calm1.ogg", false), std::string("calm1"));
    CHECK_EQ(poolKey("13.ogg", false), std::string("13"));
}

// a1.1.2 indexes an empty string here and crashes. A card can hold a file
// called "1.ogg" and a player is not owed a hang for it.
TEST(poolKeySurvivesAnAllDigitName)
{
    CHECK_EQ(poolKey("1.ogg", true), std::string(""));
}

TEST(poolKeyKeepsANameWithNoExtension)
{
    CHECK_EQ(poolKey("music/calm", true), std::string("music.calm"));
}

TEST(soundPoolGroupsByKeyAndKeepsAFlatList)
{
    SoundPool pool(true);
    CHECK_EQ(pool.add("music/calm1.ogg", "/r/music/calm1.ogg"), std::string("music.calm"));
    pool.add("music/calm2.ogg", "/r/music/calm2.ogg");
    pool.add("music/calm3.ogg", "/r/music/calm3.ogg");
    pool.add("newmusic/hal1.ogg", "/r/newmusic/hal1.ogg");

    // Four entries, two keys.
    CHECK_EQ(pool.size(), usize(4));

    const SoundEntry* keyed = pool.randomEntry("newmusic.hal");
    CHECK(keyed != nullptr);
    CHECK_EQ(keyed->name, std::string("newmusic/hal1.ogg"));

    CHECK(pool.randomEntry("nothing.here") == nullptr);
}

// getRandomSound() ignores keys entirely -- it is uniform over every entry in
// the pool. The music ticker is its only caller and this is why `calm` and
// `hal` are not equally likely: `newmusic` simply has more files in it.
TEST(soundPoolRandomEntryIsUniformOverTheFlatList)
{
    SoundPool pool(true, 12345);
    pool.add("music/calm1.ogg", "a");
    pool.add("music/calm2.ogg", "b");
    pool.add("music/calm3.ogg", "c");

    int seen[3] = {0, 0, 0};
    for (int i = 0; i < 3000; ++i) {
        const SoundEntry* entry = pool.randomEntry();
        CHECK(entry != nullptr);
        seen[entry->path[0] - 'a']++;
    }
    for (int i = 0; i < 3; ++i) {
        CHECK(seen[i] > 800 && seen[i] < 1200);
    }
}

TEST(emptyPoolReturnsNothingRatherThanCrashing)
{
    SoundPool pool(true);
    CHECK(pool.randomEntry() == nullptr);
    CHECK(pool.randomEntry("anything") == nullptr);
}

// ---- the ticker ------------------------------------------------------

SoundPool threeTracks()
{
    SoundPool pool(true);
    pool.add("music/calm1.ogg", "calm1");
    pool.add("music/calm2.ogg", "calm2");
    pool.add("music/calm3.ogg", "calm3");
    return pool;
}

MusicState ready()
{
    MusicState state;
    state.available = true;
    state.musicVolume = 1.0f;
    state.musicPlaying = false;
    state.recordPlaying = false;
    return state;
}

// The counter is seeded in the constructor with nextInt(12000), so the first
// track lands in the first ten minutes. Drawn once per SoundManager, not once
// per world.
TEST(firstTrackLandsInTheFirstTenMinutes)
{
    for (i64 seed = 0; seed < 64; ++seed) {
        MusicTicker ticker(seed);
        CHECK(ticker.ticksRemaining() >= 0);
        CHECK(ticker.ticksRemaining() < 12000);
    }
}

// The exact schedule, against JavaRandom driven by hand. This is the whole
// feature: if these instants are wrong the game plays music at the wrong rate
// and nothing else will say so.
TEST(scheduleMatchesTheOriginalsArithmetic)
{
    const i64 seed = 0x5EED;
    SoundPool pool = threeTracks();

    // The ticker's generator draws nothing but the gaps -- the pool picks
    // tracks from its own, exactly as `eb.c` is separate from `of.h`. So the
    // oracle is two lines, and the schedule does not move when the player adds
    // a file to the card. The trailing +1 is the tick the counter spends at
    // zero, on which the track actually starts.
    JavaRandom oracle(seed);
    std::vector<i32> expected;
    i32 at = oracle.nextInt(12000);
    for (int i = 0; i < 6; ++i) {
        expected.push_back(at);
        at += oracle.nextInt(24000) + 24000 + 1;
    }

    MusicTicker ticker(seed);
    MusicState state = ready();
    std::vector<i32> actual;
    for (i32 tick = 0; tick < 400000 && actual.size() < expected.size(); ++tick) {
        if (ticker.tick(pool, state) != nullptr) {
            actual.push_back(tick);
        }
    }

    CHECK_EQ(actual.size(), expected.size());
    for (usize i = 0; i < actual.size(); ++i) {
        CHECK_EQ(actual[i], expected[i]);
    }
}

// The gap between two tracks is 20-40 minutes, so between 24000 and 48000
// ticks apart -- plus the one tick the counter spends at zero.
TEST(gapsBetweenTracksAreTwentyToFortyMinutes)
{
    SoundPool pool = threeTracks();
    MusicTicker ticker(99);
    MusicState state = ready();

    i32 last = -1;
    int gaps = 0;
    for (i32 tick = 0; tick < 500000; ++tick) {
        if (ticker.tick(pool, state) != nullptr) {
            if (last >= 0) {
                const i32 gap = tick - last;
                CHECK(gap >= 24000);
                CHECK(gap <= 48001);
                ++gaps;
            }
            last = tick;
        }
    }
    CHECK(gaps > 5);
}

// **The rule most likely to be broken by a refactor.** The two playing() checks
// come before the decrement in of.c(), so a playing track freezes the counter
// -- the silence between tracks is a full 20-40 minutes and the wall-clock
// period is the track's own length plus that. A ticker that decremented
// unconditionally would play music roughly twice as often as the real game.
TEST(counterDoesNotAdvanceWhileATrackIsPlaying)
{
    SoundPool pool = threeTracks();
    MusicTicker ticker(4242);
    MusicState state = ready();

    // Run to just before the first track.
    while (ticker.ticksRemaining() > 0) {
        CHECK(ticker.tick(pool, state) == nullptr);
    }
    CHECK(ticker.tick(pool, state) != nullptr);

    const i32 afterStart = ticker.ticksRemaining();
    CHECK(afterStart >= 24000);

    // Now a track is playing. Ten thousand ticks -- over eight minutes -- must
    // not move the counter by one.
    state.musicPlaying = true;
    for (int i = 0; i < 10000; ++i) {
        CHECK(ticker.tick(pool, state) == nullptr);
    }
    CHECK_EQ(ticker.ticksRemaining(), afterStart);

    // And it resumes the moment the track ends.
    state.musicPlaying = false;
    ticker.tick(pool, state);
    CHECK_EQ(ticker.ticksRemaining(), afterStart - 1);
}

// A record on a jukebox suppresses music exactly as a playing track does, and
// for the same reason: the check precedes the decrement.
TEST(aRecordFreezesTheCounterToo)
{
    SoundPool pool = threeTracks();
    MusicTicker ticker(31337);
    MusicState state = ready();
    state.recordPlaying = true;

    const i32 before = ticker.ticksRemaining();
    for (int i = 0; i < 5000; ++i) {
        CHECK(ticker.tick(pool, state) == nullptr);
    }
    CHECK_EQ(ticker.ticksRemaining(), before);
}

TEST(musicVolumeZeroSuppressesEverything)
{
    SoundPool pool = threeTracks();
    MusicTicker ticker(5);
    MusicState state = ready();
    state.musicVolume = 0.0f;

    const i32 before = ticker.ticksRemaining();
    for (int i = 0; i < 100000; ++i) {
        CHECK(ticker.tick(pool, state) == nullptr);
    }
    CHECK_EQ(ticker.ticksRemaining(), before);
}

TEST(anUnavailableBackendSuppressesEverything)
{
    SoundPool pool = threeTracks();
    MusicTicker ticker(6);
    MusicState state = ready();
    state.available = false;

    for (int i = 0; i < 100000; ++i) {
        CHECK(ticker.tick(pool, state) == nullptr);
    }
}

// No resources folder is the ordinary state of a fresh install. The counter
// runs down to zero and stays there, so the moment files appear a track starts
// rather than the player waiting out a fresh twenty minutes.
TEST(anEmptyPoolPlaysNothingAndLeavesTheCounterAtZero)
{
    SoundPool empty(true);
    MusicTicker ticker(8);
    MusicState state = ready();

    for (int i = 0; i < 20000; ++i) {
        CHECK(ticker.tick(empty, state) == nullptr);
    }
    CHECK_EQ(ticker.ticksRemaining(), 0);

    SoundPool pool = threeTracks();
    CHECK(ticker.tick(pool, state) != nullptr);
}

// ---- the resource walk ---------------------------------------------

struct TempDir {
    char path[64] = {};

    TempDir()
    {
        std::snprintf(path, sizeof(path), "/tmp/3dalpha_audio_XXXXXX");
        if (::mkdtemp(path) == nullptr) {
            path[0] = '\0';
        }
    }

    ~TempDir()
    {
        if (path[0] != '\0') {
            char command[128];
            std::snprintf(command, sizeof(command), "rm -rf '%s'", path);
            if (std::system(command) != 0) {
                std::fprintf(stderr, "warning: could not clean up %s\n", path);
            }
        }
    }

    std::string at(const char* name) const { return std::string(path) + "/" + name; }
};

// Writes a one-byte file, making its parents. The walk lists directories and
// never opens a file, so the content is irrelevant -- which is also what keeps
// any Mojang audio out of this repository. One byte rather than zero because a
// null pointer handed to the write path is a sanitizer report about the test
// rather than about the code.
void touch(io::FileSystem& fs, const std::string& path)
{
    const usize slash = path.rfind('/');
    if (slash != std::string::npos) {
        fs.makeDirectories(path.substr(0, slash).c_str());
    }
    const u8 byte = 0;
    fs.writeFileAtomic(path.c_str(), ConstByteSpan(&byte, 1));
}

// The five category names Minecraft.installResource recognises, and the
// requirement that nothing else is picked up -- a resources/ folder copied off
// a modern install also carries pack.mcmeta, icons/, pe/ and sound3/, and none
// of those are ours to interpret.
TEST(theWalkRoutesTheFiveCategoriesAndIgnoresTheRest)
{
    TempDir dir;
    CHECK(dir.path[0] != '\0');
    io::PosixFileSystem fs;

    touch(fs, dir.at("music/calm1.ogg"));
    touch(fs, dir.at("newmusic/hal1.ogg"));
    touch(fs, dir.at("sound/random/click.ogg"));
    touch(fs, dir.at("newsound/step/grass1.ogg"));
    touch(fs, dir.at("streaming/13.mus"));

    // None of these belong to any pool.
    touch(fs, dir.at("pack.mcmeta"));
    touch(fs, dir.at("icons/icon_16x16.png"));
    touch(fs, dir.at("pe/humble.png"));
    touch(fs, dir.at("sound3/ambient/weather/rain1.ogg"));

    ResourceIndex index;
    CHECK(indexResources(fs, dir.path, &index));

    CHECK_EQ(index.music.size(), usize(2));
    CHECK_EQ(index.sounds.size(), usize(2));
    CHECK_EQ(index.streaming.size(), usize(1));
    CHECK_EQ(index.total(), usize(5));
}

// **The category is consumed.** installResource splits the key at the first
// slash and registers the remainder, so the pool key is `random.bow` and not
// `sound.random.bow` -- which is the name the game actually asks for, and the
// difference between every effect lookup hitting and every one missing.
TEST(theWalkStripsTheCategoryBeforeKeying)
{
    TempDir dir;
    CHECK(dir.path[0] != '\0');
    io::PosixFileSystem fs;

    touch(fs, dir.at("sound/random/bow.ogg"));
    touch(fs, dir.at("newsound/step/grass1.ogg"));
    touch(fs, dir.at("music/calm1.ogg"));
    touch(fs, dir.at("streaming/mellohi.mus"));

    ResourceIndex index;
    CHECK(indexResources(fs, dir.path, &index));

    CHECK(index.sounds.randomEntry("random.bow") != nullptr);
    CHECK(index.sounds.randomEntry("step.grass") != nullptr);
    CHECK(index.music.randomEntry("calm") != nullptr);
    // The streaming pool keeps its digits and its exact name.
    CHECK(index.streaming.randomEntry("mellohi") != nullptr);

    // The bug this test exists for.
    CHECK(index.sounds.randomEntry("sound.random.bow") == nullptr);
    CHECK(index.music.randomEntry("music.calm") == nullptr);
}

// A fresh install has no resources folder. That is the ordinary state, not an
// error, and it must not be reported as one anywhere up the stack.
TEST(anAbsentResourcesFolderIsEmptyRatherThanBroken)
{
    io::PosixFileSystem fs;
    ResourceIndex index;
    CHECK(!indexResources(fs, "/tmp/3dalpha_audio_definitely_not_here", &index));
    CHECK(index.empty());
    CHECK(index.music.randomEntry() == nullptr);
}

// ---- the deferred open ---------------------------------------------

// Creating a source must touch nothing: it happens on the frame loop, and
// reading an SD card there is what CONTRIBUTING forbids on core 0. The failure
// for a file that is not there therefore arrives from prepare(), on the decode
// thread, and not from create().
TEST(creatingAStreamTouchesNoFileAndPrepareIsWhatFails)
{
    io::PosixFileSystem fs;
    std::unique_ptr<VorbisStream> stream =
        VorbisStream::create(fs, "/tmp/3dalpha_audio_no_such_track.ogg");

    if (!vorbisAvailable()) {
        // A build with no decoder refuses at create, which the engine treats
        // the same way -- silence.
        CHECK(stream == nullptr);
        return;
    }

    CHECK(stream != nullptr);
    CHECK(!stream->prepare());
}

// ---- `.mus`, Mojang's own container ---------------------------------

// **The key is the file's own name, hashed the way Java hashes a String.**
// `hk`'s constructor takes `url.getPath().substring(lastIndexOf("/") + 1)`, so
// the extension is part of it -- hashing "13" rather than "13.mus" is a
// different keystream and a file of noise.
TEST(theMusKeyIsTheFileNamesJavaHash)
{
    CHECK_EQ(musKey("13.mus"), i32(1451406847));
    CHECK_EQ(musKey("cat.mus"), i32(554186163));
    // The path in front of it is not part of the name.
    CHECK_EQ(musKey("sdmc:/3dalpha/resources/streaming/13.mus"), i32(1451406847));
    CHECK_EQ(musKey("streaming\\13.mus"), i32(1451406847));
}

// The first eight bytes of a real `streaming/13.mus`, which decode to an Ogg
// page header. Two things in `hk.read` can be got wrong and both survive the
// first byte: the state advances on the **decoded** byte, not the raw one, and
// that byte is **sign-extended** into the multiply. Byte 5 of this vector is
// 0xB3 raw and decodes past 0x7F, so the run is long enough to catch either.
TEST(musDecodesToOggWithTheStateCarriedOnThePlaintext)
{
    u8 bytes[] = {0xFA, 0x99, 0x38, 0x0A, 0x43, 0xB3, 0x72, 0xCE};
    i32 key = musKey("13.mus");
    musDecode(bytes, sizeof(bytes), &key);

    const u8 expected[] = {'O', 'g', 'g', 'S', 0x00, 0x02, 0x00, 0x00};
    for (usize i = 0; i < sizeof(bytes); ++i) {
        CHECK_EQ(int(bytes[i]), int(expected[i]));
    }

    // **The state carries across calls**, which is what lets the stream
    // decipher a file it only ever reads forwards. One block of eight and two
    // blocks of four must come out the same.
    u8 split[] = {0xFA, 0x99, 0x38, 0x0A, 0x43, 0xB3, 0x72, 0xCE};
    i32 first = musKey("13.mus");
    musDecode(split, 4, &first);
    musDecode(split + 4, 4, &first);
    for (usize i = 0; i < sizeof(split); ++i) {
        CHECK_EQ(int(split[i]), int(expected[i]));
    }
    CHECK_EQ(first, key);
}

// **The codec comes off the file name**, which is what lets a `streaming/`
// folder hold `13.mus` and `13.ogg` -- a beta-era one does, and the pool keys
// both as "13".
TEST(theCipherIsPickedOffTheExtension)
{
    CHECK(VorbisStream::isMus("streaming/13.mus"));
    CHECK(VorbisStream::isMus("13.MUS"));
    CHECK(!VorbisStream::isMus("streaming/13.ogg"));
    CHECK(!VorbisStream::isMus("mus"));
    CHECK(!VorbisStream::isMus(""));
}

TEST(creatingAMusStreamTouchesNoFileEither)
{
    io::PosixFileSystem fs;
    std::unique_ptr<VorbisStream> stream =
        VorbisStream::createMus(fs, "/tmp/3dalpha_audio_no_such_record.mus");
    if (!vorbisAvailable()) {
        CHECK(stream == nullptr);
        return;
    }
    CHECK(stream != nullptr);
    CHECK(!stream->prepare());
}

// ---- the jukebox's voice -------------------------------------------

// A backend that takes a stream and says it is playing one, which the
// recording backend below deliberately does not: `playRecord` is a start, a
// state and a stop, and none of the three can be seen through a `playMusic`
// that always refuses.
class StreamingBackend final : public Backend {
public:
    bool available() const override { return true; }
    bool playMusic(std::unique_ptr<PcmSource>, float gain) override
    {
        ++starts;
        playing = true;
        lastGain = gain;
        return true;
    }
    void stopMusic() override
    {
        if (playing) {
            ++stops;
        }
        playing = false;
    }
    bool musicPlaying() const override { return playing; }
    void setMusicGain(float gain) override { lastGain = gain; }
    SampleId addSample(const Sample&) override { return kNoSample; }
    void playSample(SampleId, float, float) override {}
    void update() override {}

    bool playing = false;
    int starts = 0;
    int stops = 0;
    float lastGain = 0.0f;
};

// **A disc does not outlive the world it was put on.** The engine belongs to
// the process here and a1.1.2's `SoundManager` belongs to the client, so
// nothing in the original has to stop a record on the way out of a world --
// quitting takes the whole mixer down. This port keeps one engine across every
// world and the title screen, and a record left playing followed the player
// back to the menu and went on playing over it. Reported from play.
//
// `stopRecord` is separate from `stopMusic` because the two share a voice: a
// caller that silenced a jukebox with `stopMusic` would cut a menu track.
TEST(aRecordIsStoppedOnTheWayOutOfAWorldAndTheMusicIsNot)
{
    TempDir dir;
    CHECK(dir.path[0] != '\0');
    io::PosixFileSystem fs;
    touch(fs, dir.at("streaming/13.mus"));

    StreamingBackend backend;
    SoundEngine engine(fs, backend, 1);
    CHECK(engine.loadResources(dir.path) > 0);

    engine.playRecord("13", 0.0, 0.0, 0.0);
    CHECK(engine.recordPlaying());
    CHECK_EQ(backend.starts, 1);

    // The music slider is not the disc's, and moving it to zero must not stop
    // one: a record is gated on `soundVolume`, which is `of.a`'s own first
    // line.
    engine.setMusicVolume(0.0f);
    CHECK(engine.recordPlaying());
    CHECK_EQ(backend.stops, 0);

    // Leaving the world is.
    engine.stopRecord();
    CHECK(!engine.recordPlaying());
    CHECK(!backend.musicPlaying());
    CHECK_EQ(backend.stops, 1);

    // And it is idempotent, because more than one path out of a world reaches
    // it and none of them knows whether another already did.
    engine.stopRecord();
    CHECK_EQ(backend.stops, 1);
}

// The other half: `stopRecord` on a voice carrying background music leaves it
// alone, so a menu track survives a world closing behind it.
TEST(stoppingARecordLeavesBackgroundMusicPlaying)
{
    io::PosixFileSystem fs;
    StreamingBackend backend;
    SoundEngine engine(fs, backend, 1);

    // Nothing here went through `playRecord`, so the voice is the music's.
    backend.playMusic(nullptr, 1.0f);
    CHECK(backend.musicPlaying());

    engine.stopRecord();
    CHECK(backend.musicPlaying());
    CHECK_EQ(backend.stops, 0);
}

// A track this card has no file for is silence and not a stuck voice -- the
// same degradation an absent resources folder gets.
TEST(aRecordThisCardDoesNotHaveIsSilence)
{
    TempDir dir;
    CHECK(dir.path[0] != '\0');
    io::PosixFileSystem fs;
    touch(fs, dir.at("streaming/13.mus"));

    StreamingBackend backend;
    SoundEngine engine(fs, backend, 1);
    engine.loadResources(dir.path);

    engine.playRecord("mellohi", 0.0, 0.0, 0.0);
    CHECK(!engine.recordPlaying());
    CHECK_EQ(backend.starts, 0);

    // And a null track is the eject: it stops whatever is on the voice and
    // starts nothing, which is `BlockJukeBox.ejectRecord`'s own call.
    engine.playRecord("13", 0.0, 0.0, 0.0);
    CHECK(engine.recordPlaying());
    engine.playRecord(nullptr, 0.0, 0.0, 0.0);
    CHECK(!engine.recordPlaying());
    CHECK_EQ(backend.stops, 1);
}

// ---- interface sounds ----------------------------------------------

// `of.a(String, float, float)`'s arithmetic, which is three lines and two of
// them are easy to get backwards. The 0.25f is the interface factor: it is real,
// it is audible, and neither music nor a positional sound has it.
TEST(interfaceSoundsCarryTheQuarterFactor)
{
    CHECK_EQ(interfaceGain(1.0f, 1.0f), 0.25f);
    CHECK_EQ(interfaceGain(0.5f, 1.0f), 0.125f);

    // The button-block click a1.1.2 plays for itself -- `random.click` at 0.3 --
    // which is what the cursor uses when it moves. See platform/ctr/menu.hpp.
    CHECK_EQ(interfaceGain(0.3f, 1.0f), 0.075f);
}

// `if (volume > 1.0F) volume = 1.0F;` comes *before* the multiply, so asking
// for more than full is asking for full and not for a quarter more than it.
// Positional sounds read a volume above 1 as a bigger fade distance instead;
// the interface path simply clips.
TEST(anInterfaceVolumeAboveOneIsClipped)
{
    CHECK_EQ(interfaceGain(2.0f, 1.0f), 0.25f);
    CHECK_EQ(interfaceGain(1000.0f, 1.0f), 0.25f);
}

TEST(theSoundVolumeScalesEveryEffect)
{
    CHECK_EQ(interfaceGain(1.0f, 0.5f), 0.125f);
    CHECK_EQ(interfaceGain(1.0f, 0.0f), 0.0f);
}

// A backend that takes samples and writes down what it was asked to play. It is
// the third implementation of the seam and exists for the same reason the
// second one does: to keep `audio::Backend` from becoming the shape of ndsp.
class RecordingBackend final : public Backend {
public:
    bool available() const override { return available_; }
    bool playMusic(std::unique_ptr<PcmSource>, float) override { return false; }
    void stopMusic() override {}
    bool musicPlaying() const override { return false; }
    void setMusicGain(float) override {}

    SampleId addSample(const Sample& sample) override
    {
        samples.push_back(sample.frames());
        return SampleId(samples.size() - 1);
    }

    void playSample(SampleId id, float gain, float pitch) override
    {
        plays.push_back(Play{id, gain, pitch});
    }

    void update() override {}

    struct Play {
        SampleId id;
        float gain;
        float pitch;
    };

    bool available_ = true;
    std::vector<usize> samples;
    std::vector<Play> plays;
};

// A sound the engine was never given is silence, not a failure and not a stall:
// nothing here may go and read it on the frame that asked. It is the same
// degradation as an absent resources folder, and this is the path the menus
// take on a console with no `sound/` on the card.
TEST(anEffectThatWasNeverPreloadedIsSilence)
{
    io::PosixFileSystem fs;
    RecordingBackend backend;
    SoundEngine engine(fs, backend, 1);

    engine.playSoundFX("random.click", 1.0f, 1.0f);
    CHECK_EQ(backend.plays.size(), usize(0));
    CHECK_EQ(engine.loadedSamples(), usize(0));
}

// `of.a`'s first line -- `if (!loaded || options.soundVolume == 0.0F) return;`
// -- tested from both halves. Neither is an error path and neither logs.
TEST(anUnavailableBackendPlaysNoEffects)
{
    io::PosixFileSystem fs;
    RecordingBackend backend;
    backend.available_ = false;
    SoundEngine engine(fs, backend, 1);

    engine.playSoundFX("random.click", 1.0f, 1.0f);
    CHECK_EQ(backend.plays.size(), usize(0));
}

TEST(aSoundVolumeOfZeroSuppressesEveryEffect)
{
    io::PosixFileSystem fs;
    RecordingBackend backend;
    SoundEngine engine(fs, backend, 1);
    engine.setSoundVolume(0.0f);

    engine.playSoundFX("random.click", 1.0f, 1.0f);
    CHECK_EQ(backend.plays.size(), usize(0));
}

// The preload walks the sound pool by key, and a file it cannot decode is
// skipped rather than reported. A card can hold anything; a `.txt` in `sound/`
// is not a reason to refuse to start.
TEST(preloadingSkipsWhatItCannotDecode)
{
    TempDir dir;
    CHECK(dir.path[0] != '\0');

    io::PosixFileSystem fs;
    touch(fs, dir.at("sound/random/click.ogg"));  // one byte, not Ogg
    touch(fs, dir.at("sound/random/bow.ogg"));

    RecordingBackend backend;
    SoundEngine engine(fs, backend, 1);
    CHECK_EQ(engine.loadResources(dir.path), usize(2));

    CHECK_EQ(engine.preloadSound("random.click"), usize(0));
    CHECK_EQ(backend.samples.size(), usize(0));

    engine.playSoundFX("random.click", 1.0f, 1.0f);
    CHECK_EQ(backend.plays.size(), usize(0));
}

// Too long is refused rather than truncated -- a sound that cuts off halfway is
// a bug that sounds like a decision. A one-byte file is not Ogg either, so this
// also pins that a broken file leaves `out` alone.
TEST(decodingASampleFailsWithoutTouchingTheOutput)
{
    TempDir dir;
    CHECK(dir.path[0] != '\0');

    io::PosixFileSystem fs;
    const std::string path = dir.at("sound/random/click.ogg");
    touch(fs, path);

    Sample sample;
    sample.channels = 2;
    CHECK(!decodeSample(fs, path, &sample));
    CHECK(sample.empty());
    CHECK_EQ(sample.channels, 2);

    CHECK(!decodeSample(fs, dir.at("no/such/file.ogg"), &sample));
}

}  // namespace
