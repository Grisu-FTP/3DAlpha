#include "platform/ctr/menu.hpp"

#include "platform/ctr/overlay.hpp"
#include "platform/ctr/renderer.hpp"

#include "core/settings/world_settings.hpp"
#include "core/texture/background.hpp"
#include "core/texture/jar_import.hpp"
#include "core/util/java_random.hpp"
#include "core/util/seed_text.hpp"
#include "core/world/any_storage.hpp"
#include "core/world/format/converter.hpp"
#include "core/world/world_format.hpp"

#include "version_config.hpp"
#include "version_slots.hpp"

#include <3ds.h>

#include <cstdio>
#include <cstring>
#include <ctime>

namespace mc::ctr {

namespace {

constexpr float kScreenWidth = 400.0f;
constexpr float kScreenHeight = 240.0f;

// citro2d's vertex budget, in quads. See the note in initCommon.
constexpr int kMenuObjects = 1024;
constexpr int kOverlayObjects = 2 * kMenuObjects;

// The palette. Ours, not Mojang's -- see the note at the top of menu.hpp.
constexpr u32 kInk = C2D_Color32(0xF0, 0xF0, 0xF0, 0xFF);
constexpr u32 kInkDim = C2D_Color32(0xA0, 0xA0, 0xA0, 0xFF);
constexpr u32 kInkWarn = C2D_Color32(0xFF, 0xA8, 0x60, 0xFF);
constexpr u32 kShadow = C2D_Color32(0x14, 0x14, 0x14, 0xC0);
constexpr u32 kOutline = C2D_Color32(0x14, 0x14, 0x14, 0xFF);
constexpr u32 kFill = C2D_Color32(0x6A, 0x6A, 0x6A, 0xFF);
constexpr u32 kFillSelected = C2D_Color32(0x7A, 0x85, 0xA3, 0xFF);
constexpr u32 kFillDisabled = C2D_Color32(0x4A, 0x4A, 0x4A, 0xFF);
constexpr u32 kBevelLight = C2D_Color32(0xFF, 0xFF, 0xFF, 0x30);
constexpr u32 kBevelDark = C2D_Color32(0x00, 0x00, 0x00, 0x40);

// Row and button geometry, in one place so the two list screens line up.
constexpr float kButtonWidth = 220.0f;
constexpr float kButtonHeight = 26.0f;
constexpr float kRowHeight = 28.0f;
constexpr float kRowGap = 4.0f;
constexpr float kRowsTop = 36.0f;
constexpr int kVisibleRows = 6;

// Anything above this and a2 is `- empty -` on a1.1.2's own screen; here it is
// only the point at which the list scrolls.
int rowCount(usize worlds)
{
    return int(worlds) + 1;  // + "Create New World"
}

// A per-tile shade for the backdrop. Deterministic, so the menu does not
// shimmer between frames.
u32 tileHash(int x, int y)
{
    u32 h = u32(x) * 73856093u ^ u32(y) * 19349663u;
    h ^= h >> 13;
    h *= 0x9E3779B1u;
    return h ^ (h >> 16);
}

u8 clampByte(int value)
{
    return u8(value < 0 ? 0 : (value > 255 ? 255 : value));
}

void formatWhen(i64 lastPlayed, char* out, usize size)
{
    if (lastPlayed <= 0) {
        std::snprintf(out, size, "never played");
        return;
    }
    const std::time_t seconds = std::time_t(lastPlayed / 1000);
    std::tm parts{};
    if (::gmtime_r(&seconds, &parts) == nullptr) {
        std::snprintf(out, size, "-");
        return;
    }
    // UTC, and deliberately unlabelled: the 3DS has a time zone in its system
    // config and newlib on this toolchain does not read it, so a local time
    // here would be a lie on every console east or west of Greenwich.
    std::snprintf(out, size, "%04d-%02d-%02d %02d:%02d", parts.tm_year + 1900,
                  parts.tm_mon + 1, parts.tm_mday, parts.tm_hour, parts.tm_min);
}

// swkbd's filter hook for the world name. The names already on the card are
// reached through a file-static because the applet API has one user pointer and
// libctru's own callback signature takes it -- see swkbdSetFilterCallback --
// and the alternative is letting the player type a name that then silently
// fails to create.
const std::vector<world::WorldEntry>* gExistingWorlds = nullptr;

SwkbdCallbackResult validateWorldName(void* user, const char** message, const char* text,
                                      size_t length)
{
    (void)user;

    char buffer[80];
    if (length >= sizeof(buffer)) {
        *message = "that name is too long";
        return SWKBD_CALLBACK_CONTINUE;
    }
    std::memcpy(buffer, text, length);
    buffer[length] = '\0';

    std::string name;
    if (!world::sanitizeWorldName(buffer, &name)) {
        *message = "that name has nothing a card can store";
        return SWKBD_CALLBACK_CONTINUE;
    }
    if (gExistingWorlds != nullptr) {
        for (const world::WorldEntry& entry : *gExistingWorlds) {
            if (entry.name == name) {
                // Storage::create refuses to write over a level.dat, so this
                // would fail anyway -- it just fails here, where the player can
                // fix it, instead of after the keyboard has closed.
                *message = "there is already a world with that name";
                return SWKBD_CALLBACK_CONTINUE;
            }
        }
    }
    return SWKBD_CALLBACK_OK;
}

// The console's clock, folded into something an LCG can be seeded with. The
// system tick is 268 MHz, so two worlds made a second apart are nowhere near
// each other in the sequence.
i64 clockSeed()
{
    return i64(svcGetSystemTick()) ^ (i64(osGetTime()) << 20);
}

// **The autosave ladder, and why it is a ladder.**
//
// The interval is ours rather than the original's -- a1.1.2 has no timed
// autosave at all; see WorldStreamer::setAutosaveSeconds -- so there is no
// number to be faithful to, only one to be sensible about. A d-pad row that
// stepped by one second would take four hundred presses to cross the useful
// range, so the row walks these instead. 0 is Off, which means the world is
// still written when the pause menu opens and when it is left.
constexpr int kAutosaveSteps[] = {0, 15, 30, 45, 60, 120, 300};
constexpr int kAutosaveStepCount = int(sizeof(kAutosaveSteps) / sizeof(kAutosaveSteps[0]));

// The nearest rung at or below a value, so a number a player typed into 3ds.ini
// by hand lands somewhere sensible rather than off the end of the row.
int autosaveIndex(int seconds)
{
    int best = 0;
    for (int i = 0; i < kAutosaveStepCount; ++i) {
        if (kAutosaveSteps[i] <= seconds) {
            best = i;
        }
    }
    return best;
}

int clampAutosave(int seconds)
{
    return kAutosaveSteps[autosaveIndex(seconds)];
}

void autosaveLabel(int seconds, char* out, usize size)
{
    if (seconds <= 0) {
        std::snprintf(out, size, "Autosave: Off");
    } else if (seconds < 60) {
        std::snprintf(out, size, "Autosave: %ds", seconds);
    } else if (seconds % 60 == 0) {
        std::snprintf(out, size, "Autosave: %dm", seconds / 60);
    } else {
        std::snprintf(out, size, "Autosave: %dm %ds", seconds / 60, seconds % 60);
    }
}

// The order the gamemode row steps through. Spectator first because it is the
// default and the only implemented one; the other two are drawn disabled --
// see settings::gamemodeImplemented.
constexpr settings::Gamemode kGamemodeOrder[] = {
    settings::Gamemode::Spectator,
    settings::Gamemode::Survival,
    settings::Gamemode::Creative,
};
constexpr int kGamemodeCount = int(sizeof(kGamemodeOrder) / sizeof(kGamemodeOrder[0]));

int gamemodeIndex(settings::Gamemode mode)
{
    for (int i = 0; i < kGamemodeCount; ++i) {
        if (kGamemodeOrder[i] == mode) {
            return i;
        }
    }
    return 0;
}

// Bytes as something a player can read at a glance. Deliberately coarse: this
// is a size on a screen, not an accounting figure, and "1.4 MB" answers the
// question "will this fit" better than eight digits do.
void formatBytes(u64 bytes, char* out, usize size)
{
    if (bytes < 1024ull) {
        std::snprintf(out, size, "%llu B", (unsigned long long)bytes);
    } else if (bytes < 1024ull * 1024ull) {
        std::snprintf(out, size, "%.1f KB", double(bytes) / 1024.0);
    } else {
        std::snprintf(out, size, "%.1f MB", double(bytes) / (1024.0 * 1024.0));
    }
}

// The rows of the World Settings screen. In game there is a world open and
// streaming behind this, so copying, deleting and converting it are not on
// offer -- the three of them all mean rewriting files something else holds.
enum WorldSettingsRow {
    kRowGamemode = 0,
    kRowFormat,
    kRowSize,
    kRowCopy,
    kRowDelete,
    kRowBack,
    kRowCount,
};
constexpr int kWorldSettingsRowsInGame = 2;  // gamemode, back

int worldSettingsRowFor(int cursor, bool inGame)
{
    // In game the cursor only has two positions, and the second of them is
    // Back -- which is the last row, not the second one.
    if (!inGame) {
        return cursor;
    }
    return cursor == 0 ? kRowGamemode : kRowBack;
}

}  // namespace

i64 nowMillis()
{
    return i64(osGetTime()) - 2208988800000LL;
}

bool Menu::initCommon(bool isNew3DS, int objects)
{
    isNew3DS_ = isNew3DS;
    maxDistance_ = isNew3DS ? kPlayMaxDistanceNew3DS : kPlayMaxDistanceOld3DS;

    // **Only on the first visit.** init() runs around every trip to the menu so
    // the render target is not sitting in VRAM during a game, but the player's
    // choices are not re-read from the card each lap -- that would undo an
    // unsaved change and cost a pack decode per world exit.
    if (!settingsLoaded_) {
        settingsLoaded_ = true;
        loadSettings();
    }

    if (renderDistance_ == 0) {
        // The two configurations the VBO pool was measured against, which is
        // what makes them the defaults rather than the maxima above.
        renderDistance_ = isNew3DS ? 10 : 6;
    }
    if (renderDistance_ > maxDistance_) {
        // A card carried between an Old and a New 3DS: the saved value is the
        // other console's, and clamping beats offering a distance this model
        // was never measured at.
        renderDistance_ = maxDistance_;
    }
    if (renderDistance_ < 2) {
        renderDistance_ = 2;
    }

    // -1 is "no file said", which is first boot or a file an older build wrote.
    // 0 is a real answer -- the player turned the timer off -- so it cannot be
    // the sentinel, which is why this one is not the render distance's 0.
    if (autosaveSeconds_ < 0) {
        autosaveSeconds_ = settings::kDefaultAutosaveSeconds;
    }
    autosaveSeconds_ = clampAutosave(autosaveSeconds_);

    if (chunkCacheMB_ <= 0) {
        // What is spare after the heap split in platform/ctr/heap.cpp: 40 MB of
        // newlib heap on a New 3DS against ~15 MB of block data at the longest
        // distance it offers, and ~21 MB on an Old one. A column is 18,013
        // bytes on a real world, so 8 MB holds ~465 of them.
        chunkCacheMB_ = isNew3DS ? 8 : 2;
    }

    // **The object budget is arithmetic rather than taste.** citro2d sizes its
    // vertex buffer from this and silently drops geometry once it is full, so
    // it has to cover the busiest frame: the backdrop, six rows at five quads
    // each, and a few hundred glyphs -- twice, because every label draws its
    // shadow as a second pass. The backdrop used to be 240 quads of its own and
    // is one since it became a tiled texture, which is most of the headroom the
    // glyphs now spend. 1024 objects is 128 KB of linear memory, which is why
    // this is not simply C2D_DEFAULT_MAX_OBJECTS -- the menu gives it back
    // before the renderer asks for its pool.
    //
    // **The pause menu asks for twice that, and the reason is the second eye.**
    // The buffer is per *frame*, not per scene: citro2d resets it at
    // C3D_FrameEnd, so an overlay drawn once per eye spends it twice, and the
    // busiest screen it can reach in game -- the pack list, six rows of names
    // and counts -- lands near 400 objects an eye. 1024 would very nearly do
    // and "very nearly" here means a row of text silently missing from one eye
    // on the console and on nothing else. Falling back to 1024 rather than
    // refusing, because a menu that cannot be opened is worse than one that is
    // thin on room: at slider zero there is only one eye and the question does
    // not arise.
    if (!C2D_Init(objects) && !C2D_Init(1024)) {
        return false;
    }
    C2D_Prepare();

    // Both failures unwind through shutdown() rather than by hand, so the
    // shader-park rule below is obeyed on the way out of a half-built menu too.
    // Every step of it is null-safe and C2D_Fini is a no-op when citro2d is not
    // up, so it is safe to call at any point after C2D_Init succeeded.
    textBuf_ = C2D_TextBufNew(1024);
    if (textBuf_ == nullptr) {
        shutdown();
        return false;
    }

    // What the system font's line box actually is on this console, asked once.
    // Every y in the drawing code was written against it; fontTop() converts.
    C2D_Text probe;
    C2D_TextParse(&probe, textBuf_, "Ag");
    float probeWidth = 0.0f;
    float probeHeight = 0.0f;
    C2D_TextGetDimensions(&probe, 1.0f, 1.0f, &probeWidth, &probeHeight);
    if (probeHeight > 0.0f) {
        // A zero would put every bitmap label half a line high. The default it
        // keeps is the 3DS system font's own line box.
        systemLineHeight_ = probeHeight;
    }
    C2D_TextBufClear(textBuf_);

    // The atlas and the pack's art, neither of which needs a listing. **The
    // world list and the pack list are not read here**, and that is the whole
    // of why a pause menu now opens in a frame rather than in a second or two:
    // listWorlds opens and gunzips a level.dat per world on the card, and
    // listPacks reads every pack zip in its entirety to count what is in it.
    // Neither is on screen when the menu opens, and the screens that show them
    // ask for them on the way in.
    ensureAtlas();
    consoleDirty_ = true;
    return true;
}

bool Menu::init(bool isNew3DS)
{
    // One eye: the main menu turns stereo off on its way in.
    if (!initCommon(isNew3DS, kMenuObjects)) {
        return false;
    }

    target_ = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    if (target_ == nullptr) {
        shutdown();
        return false;
    }

    // The left eye only. Nothing here has any depth to it, and the right eye's
    // framebuffer would otherwise hold whatever the last game frame left in it.
    gfxSet3D(false);
    return true;
}

bool Menu::initOverlay(bool isNew3DS)
{
    // No target and no gfxSet3D: the renderer still owns the top screen and is
    // about to draw the world this menu sits on top of. That is also why
    // nothing here has to be given back afterwards -- there is no screen to
    // reclaim and no eye to put back in citro3d's output table.
    return initCommon(isNew3DS, kOverlayObjects);
}

void Menu::shutdown()
{
    // **Before C2D_Fini, which frees citro2d's shader program.** citro3d holds
    // the last program bound and dereferences it on the next bind, so without
    // this the game's first frame reads through freed memory and data-aborts --
    // which is precisely what `crashlogs/004-loading-a-world-from-the-menu` is.
    // See parkShaderProgram in renderer.hpp.
    parkShaderProgram();

    // Before C2D_Fini as well, though for a plainer reason than the shader
    // park: these are two textures' worth of linear memory and the renderer is
    // about to ask for its pool.
    font_.shutdown();
    background_.shutdown();
    artUploaded_ = false;

    if (target_ != nullptr) {
        C3D_RenderTargetDelete(target_);
        target_ = nullptr;
    }
    if (textBuf_ != nullptr) {
        C2D_TextBufDelete(textBuf_);
        textBuf_ = nullptr;
    }
    C2D_Fini();
}

void Menu::refreshWorlds()
{
    // Before the list, not after: a console switched off part way through a
    // conversion leaves either a staging directory worth nothing or a world
    // half unpacked, and both are settled here rather than shown to the player.
    // Cheap when there is nothing to do, which is every boot but one.
    world::format::recoverConversions(fs_, kSavesDir);

    world::listWorlds(fs_, kSavesDir, &worlds_);

    const int rows = rowCount(worlds_.size());
    if (worldCursor_ >= rows) {
        worldCursor_ = rows - 1;
    }
    if (worldCursor_ < 0) {
        worldCursor_ = 0;
    }
    if (worldScroll_ > worldCursor_) {
        worldScroll_ = worldCursor_;
    }
}


void Menu::refreshPacks()
{
    texture::listPacks(fs_, texture::kPacksDir, &packs_);

    // Row 0 is "+ Extract from a jar...", pinned above the list the way
    // "+ Create New World" is on the world screen.
    const int rows = int(packs_.size()) + 1;
    if (packCursor_ >= rows) {
        packCursor_ = rows - 1;
    }
    if (packCursor_ < 0) {
        packCursor_ = 0;
    }
    if (packScroll_ > packCursor_) {
        packScroll_ = packCursor_;
    }

    // **A pack that is no longer on the card falls back to Dev Art rather than
    // to nothing.** A player who deleted a zip from a PC would otherwise come
    // back to a saved setting naming a file that is gone.
    if (!packName_.empty()) {
        bool present = false;
        for (const texture::PackEntry& pack : packs_) {
            if (pack.name == packName_) {
                present = true;
                break;
            }
        }
        if (!present) {
            packName_.clear();
            atlas_.rgba.clear();
        }
    }

    // The atlas may have been thrown away just above, by a pack that is no
    // longer on the card.
    ensureAtlas();
}

void Menu::ensureAtlas()
{
    if (!atlas_.empty()) {
        loadPackArt(/*force=*/false);
        return;
    }

    // Whatever the saved name is, there is no image yet on the first pass
    // through here. Building it means the pack screen can show what is live and
    // `run` always has something to hand `runGame`.
    const std::string path =
        packName_.empty() ? std::string() : texture::packPath(texture::kPacksDir, packName_);
    const texture::PackError error = texture::buildAtlas(fs_, path, &atlas_);
    if (error != texture::PackError::Ok) {
        // The saved pack is gone, or is on the card and will not decode -- a
        // truncated download, or a zip somebody edited. Falling back silently
        // would leave the player looking at Dev Art with no idea why, so the
        // reason goes on the console and 3ds.ini is left alone: the pack may be
        // fixable, and forgetting the choice for them is not ours to do.
        message_ = texture::packErrorText(error);
        packName_.clear();
        texture::buildAtlas(fs_, std::string(), &atlas_);
    }

    loadPackArt(/*force=*/false);
}

void Menu::refreshJars()
{
    // The packs folder and the folder above it. A player who dropped a download
    // onto the card has not necessarily put it in the right place, and a bare
    // "no jar found" for a file sitting one directory up is a bad answer.
    const std::string dirs[2] = {std::string(texture::kPacksDir), std::string(kRootDir)};
    texture::listJars(fs_, dirs, 2, &jars_);

    if (jarCursor_ >= int(jars_.size())) {
        jarCursor_ = int(jars_.size()) - 1;
    }
    if (jarCursor_ < 0) {
        jarCursor_ = 0;
    }
    if (jarScroll_ > jarCursor_) {
        jarScroll_ = jarCursor_;
    }
}

bool Menu::selectPack(int index)
{
    if (index < 0 || usize(index) >= packs_.size()) {
        return false;
    }
    const texture::PackEntry& pack = packs_[usize(index)];

    // Into a scratch image, not over the live one. A pack that fails halfway
    // through decoding must leave the player looking at the world they had,
    // not at an atlas that is half of one pack and half of another.
    texture::AtlasImage loaded;
    const texture::PackError error = texture::buildAtlas(fs_, pack.path, &loaded);
    if (error != texture::PackError::Ok) {
        message_ = texture::packErrorText(error);
        consoleDirty_ = true;
        return false;
    }

    atlas_ = std::move(loaded);
    packName_ = pack.builtIn ? std::string() : pack.name;
    ++packRevision_;
    message_ = nullptr;
    consoleDirty_ = true;

    // Forced, because the pack's *name* is not what changed here: extracting a
    // jar a second time rewrites a pack in place, and the menu would otherwise
    // keep drawing with the font the old copy had.
    loadPackArt(/*force=*/true);

    saveSettings();
    return true;
}

void Menu::loadPackArt(bool force)
{
    const std::string path =
        packName_.empty() ? std::string() : texture::packPath(texture::kPacksDir, packName_);

    bool decoded = false;
    if (force || !artLoaded_ || artPackName_ != packName_) {
        // Neither failure is worth a message. A pack with no default.png is not
        // broken -- the pack screen already says how many of the 58 files it
        // carries -- and the drawing falls back on its own.
        texture::buildFont(fs_, path, &fontImage_);
        texture::buildBackground(fs_, path, atlas_, &backgroundTile_);
        artPackName_ = packName_;
        artLoaded_ = true;
        decoded = true;
    }

    // Only when there is something new to upload or nothing on the GPU yet.
    // refreshPacks() also runs when the player opens the pack list, and
    // deleting and recreating two textures under a menu that is mid-frame is
    // not something a keypress should be doing.
    if (decoded || !artUploaded_) {
        uploadPackArt();
    }
}

void Menu::uploadPackArt()
{
    // Torn down first: this runs again on every visit to the menu and after
    // every pack change, and C3D_TexInit over a live texture would leak it.
    font_.shutdown();
    background_.shutdown();

    if (!fontImage_.empty()) {
        font_.init(fontImage_);
    }
    if (backgroundTile_.size() == texture::kBackgroundBytes) {
        background_.init(backgroundTile_.data());
    }
    artUploaded_ = true;
}

int Menu::fontScale(float scale) const
{
    // Four steps, and the thresholds are where the system font's line box
    // passes the cell heights: 0.65 x 30 is 19 pixels against two cells' 16,
    // 0.9 x 30 is 27 against three cells' 24, 1.2 x 30 is 36 against four
    // cells' 32. Body text lands on one cell, which is the size a1.1.2 draws
    // every one of its own menus at -- the top screen is 400x240 and the
    // original's GUI space at scale 2 is 427x240, so a cell here is a cell
    // there.
    if (scale >= 1.2f) {
        return 4;
    }
    if (scale >= 0.9f) {
        return 3;
    }
    if (scale >= 0.65f) {
        return 2;
    }
    return 1;
}

float Menu::fontTop(float y, float scale, int pixels) const
{
    // The system font's line box, centred on the smaller bitmap line. Rounded,
    // because a glyph drawn on a half pixel is a glyph with a soft edge.
    const float box = systemLineHeight_ * scale;
    const float line = float(texture::kFontCellPixels * pixels);
    const float top = y + (box - line) * 0.5f;
    return float(int(top + 0.5f));
}

void Menu::extractJar(int index)
{
    if (index < 0 || usize(index) >= jars_.size()) {
        return;
    }
    const texture::JarEntry jar = jars_[usize(index)];

    // One frame saying what is happening before the card is read. The import is
    // a second or two of blocking work on a console, and a screen that simply
    // stops looks like a crash.
    std::printf("\x1b[2J\x1b[1;1H");
    std::printf("Extracting textures from\n  \x1b[33m%s\x1b[0m\n\n", jar.name.c_str());
    std::printf("This reads the jar once and copies\n");
    std::printf("its PNGs across without decoding\n");
    std::printf("them. Nothing is written back to\n");
    std::printf("the jar.\n");
    drawFrame();

    const texture::ImportResult result = texture::importJar(fs_, jar.path, texture::kPacksDir);
    consoleDirty_ = true;

    if (!result.ok()) {
        message_ = texture::packErrorText(result.error);
        return;
    }

    importedJar_ = jar.path;
    importedPack_ = result.outPath;
    importedCount_ = result.copied;
    message_ = nullptr;

    // The new pack becomes the live one straight away. Importing a pack and
    // then having to find it in a list is a step with no decision in it.
    refreshPacks();
    for (int i = 0; i < int(packs_.size()); ++i) {
        if (!packs_[usize(i)].builtIn && packs_[usize(i)].path == result.outPath) {
            selectPack(i);
            packCursor_ = i + 1;  // + the pinned extract row
            break;
        }
    }

    // Only now, with a pack that has been re-read off the card and decoded, is
    // it honest to ask about deleting the jar it came from.
    setScreen(Screen::ConfirmDeleteJar);
}

void Menu::loadSettings()
{
    settings::GameSettings saved;
    if (!settings::loadSettings(fs_, settings::kSettingsPath, &saved)) {
        return;  // first boot; the defaults stand
    }
    renderDistance_ = saved.renderDistance;
    packName_ = saved.texturePack;
    autosaveSeconds_ = saved.autosaveSeconds;
    chunkCacheMB_ = saved.chunkCacheMB;
}

void Menu::saveSettings()
{
    settings::GameSettings current;
    current.renderDistance = renderDistance_;
    current.texturePack = packName_;
    current.autosaveSeconds = autosaveSeconds_;
    current.chunkCacheMB = chunkCacheMB_;

    if (!fs_.makeDirectories(kRootDir)) {
        return;
    }
    // Failing to save is not worth interrupting the player over: the choice
    // still applies to this session, and the card being full or locked will
    // announce itself the moment they try to make a world.
    settings::saveSettings(fs_, settings::kSettingsPath, current);
}

void Menu::setScreen(Screen screen)
{
    screen_ = screen;
    consoleDirty_ = true;
}

void Menu::printConsoleHelp()
{
    if (!consoleDirty_ && printedScreen_ == screen_) {
        return;
    }
    printedScreen_ = screen_;
    consoleDirty_ = false;

    std::printf("\x1b[2J\x1b[1;1H");
    std::printf("\x1b[32m3DAlpha %s\x1b[0m\n\n", mcver::kDisplay);

    switch (screen_) {
    case Screen::Title:
        std::printf("Up/Down  choose\n");
        std::printf("A        select\n");
        std::printf("START    exit to the home menu\n\n");
        std::printf("Multiplayer arrives at M5.\n");
        break;
    case Screen::Pause:
        std::printf("Up/Down  choose\n");
        std::printf("A        select\n");
        std::printf("B/START  back to the world\n\n");
        std::printf("The world is stopped: nothing is\n");
        std::printf("streamed or generated and the sun\n");
        std::printf("does not move while this is up.\n\n");
        std::printf("Exit World saves first, the way\n");
        std::printf("closing it any other way does.\n");
        break;
    case Screen::Worlds:
        std::printf("Up/Down  choose a world\n");
        std::printf("A        play it\n");
        std::printf("X        its settings, size,\n");
        std::printf("         copy and delete\n");
        std::printf("B        back\n\n");
        std::printf("Worlds live on the card at:\n");
        std::printf("  \x1b[33m%s/\x1b[0m\n\n", kSavesDir);
        if (worlds_.empty()) {
            std::printf("There are none yet. Make one, or\n");
            std::printf("copy an Alpha save into that folder\n");
            std::printf("-- a level.dat and the base36 chunk\n");
            std::printf("folders beside it.\n");
        }
        break;
    case Screen::WorldSettings:
        std::printf("Left/Right  change the value\n");
        std::printf("Up/Down     choose a row\n");
        std::printf("A           run the row\n");
        std::printf("B           back\n\n");
        std::printf("These belong to one world, not to\n");
        std::printf("the console. They are kept in\n");
        std::printf("  \x1b[33m<world>/%s\x1b[0m\n", settings::kWorldSettingsName);
        std::printf("which a real Minecraft client never\n");
        std::printf("reads, so nothing here changes what\n");
        std::printf("a PC copy of the world means.\n\n");
        if (inGame_) {
            // Said rather than left to be discovered: the rows are simply not
            // there, and a player who used them from the home screen would
            // otherwise think they had gone missing.
            std::printf("Copy, Delete and Format need the\n");
            std::printf("world closed. Exit the world first.\n");
        } else {
            std::printf("\x1b[33mSurvival and Creative are greyed\n");
            std::printf("out\x1b[0m: there is no player body yet,\n");
            std::printf("so Spectator is the only mode that\n");
            std::printf("would tell the truth.\n");
        }
        break;
    case Screen::ConfirmConvert:
        std::printf("Converting rewrites every file in\n");
        std::printf("the world into the other shape.\n\n");
        std::printf("\x1b[33mNothing is lost either way.\x1b[0m Files\n");
        std::printf("this format does not understand are\n");
        std::printf("carried across untouched, and every\n");
        std::printf("chunk is read back and checked\n");
        std::printf("before the old copy is removed.\n\n");
        std::printf("A  convert it\n");
        std::printf("B  leave it alone\n");
        break;
    case Screen::Options:
        std::printf("Left/Right  change the value\n");
        std::printf("Up/Down     choose a row\n");
        std::printf("A           open Texture Pack\n");
        std::printf("B           back\n\n");
        std::printf("Render distance is what a player is\n");
        std::printf("offered; the debug page (SELECT+Y in\n");
        std::printf("game) goes further for measuring.\n");
        if (inGame_) {
            std::printf("\nBoth rows apply to the world you\n");
            std::printf("are standing in, as soon as you\n");
            std::printf("go back to it.\n");
        }
        break;
    case Screen::TexturePacks:
        std::printf("Up/Down  choose\n");
        std::printf("A        use it, or extract a jar\n");
        std::printf("B        back\n\n");
        std::printf("Packs live on the card at:\n");
        std::printf("  \x1b[33m%s/\x1b[0m\n\n", texture::kPacksDir);
        std::printf("A pack is a zip in the pre-1.5 jar\n");
        std::printf("layout -- terrain.png at the root --\n");
        std::printf("or that same tree in a folder.\n\n");
        // Said plainly rather than implied. Everything else a pack carries is
        // kept and counted, and nothing samples it yet.
        std::printf("\x1b[33mOnly terrain.png is drawn so far.\x1b[0m\n");
        std::printf("A pack's gui, font and mob textures\n");
        std::printf("are kept but nothing reads them yet.\n");
        break;
    case Screen::PickJar:
        std::printf("Up/Down  choose a jar\n");
        std::printf("A        extract it\n");
        std::printf("B        back\n\n");
        if (jars_.empty()) {
            std::printf("There are no .jar files in\n");
            std::printf("  \x1b[33m%s/\x1b[0m\n", texture::kPacksDir);
            std::printf("or\n");
            std::printf("  \x1b[33m%s/\x1b[0m\n\n", kRootDir);
            std::printf("Copy your own Minecraft jar to one\n");
            std::printf("of them. Nothing is downloaded and\n");
            std::printf("nothing is sent anywhere.\n");
        } else {
            std::printf("The PNGs are copied out of the jar\n");
            std::printf("into a pack zip beside it. The jar\n");
            std::printf("itself is only read.\n");
        }
        break;
    case Screen::ConfirmDeleteJar:
        std::printf("The pack has been written and read\n");
        std::printf("back, and it works. The jar is not\n");
        std::printf("needed any more.\n\n");
        std::printf("\x1b[31mDeleting it cannot be undone.\x1b[0m\n\n");
        std::printf("A  delete the jar\n");
        std::printf("B  keep it\n");
        break;
    case Screen::ConfirmDelete:
        std::printf("\x1b[31mDeleting a world cannot be undone.\x1b[0m\n\n");
        std::printf("A  delete it\n");
        std::printf("B  keep it\n");
        break;
    }

    if (message_ != nullptr) {
        std::printf("\n\x1b[31m%s\x1b[0m\n", message_);
    }
}

MenuChoice Menu::run()
{
    MenuChoice choice;

    // The card, read here rather than in init(): this is the entry point with
    // a world list on it, and the pause menu -- which has none -- should not
    // pay for one. recoverConversions rides along with it, which is right: a
    // conversion interrupted by a flat battery is settled the next time the
    // saves folder is looked at, and that is here.
    refreshWorlds();

    while (aptMainLoop()) {
        hidScanInput();
        const u32 down = hidKeysDown();

        bool done = false;
        switch (screen_) {
        case Screen::Title:
            done = handleTitle(down, &choice);
            break;
        case Screen::Worlds:
            done = handleWorlds(down, &choice);
            break;
        case Screen::Options:
            handleOptions(down);
            break;
        case Screen::ConfirmDelete:
            handleConfirmDelete(down);
            break;
        case Screen::TexturePacks:
            handleTexturePacks(down);
            break;
        case Screen::PickJar:
            handlePickJar(down);
            break;
        case Screen::ConfirmDeleteJar:
            handleConfirmDeleteJar(down);
            break;
        case Screen::WorldSettings:
            handleWorldSettings(down);
            break;
        case Screen::ConfirmConvert:
            handleConfirmConvert(down);
            break;
        case Screen::Pause:
            // Unreachable: it lives under runPause, which puts the screen back
            // on its way out. Named rather than defaulted so the compiler keeps
            // saying so if a screen is ever added and forgotten here.
            setScreen(Screen::Title);
            break;
        }
        if (done) {
            choice.renderDistance = renderDistance_;
            choice.autosaveSeconds = autosaveSeconds_;
            choice.chunkCacheMB = chunkCacheMB_;
            choice.atlas = atlas_;
            return choice;
        }

        present();
    }

    // aptMainLoop said no: the system is taking the application away, and the
    // only honest answer is to stop rather than to open a world.
    choice.action = MenuChoice::Action::Quit;
    choice.renderDistance = renderDistance_;
    choice.autosaveSeconds = autosaveSeconds_;
    choice.chunkCacheMB = chunkCacheMB_;
    choice.atlas = atlas_;
    return choice;
}

PauseChoice Menu::runPause(const char* worldName, const char* worldPath, int renderDistance,
                           const PauseBackdrop& backdrop)
{
    // Held for the length of the loop. With one, every frame is the caller's
    // and carries the world. Without one -- which means a caller that opened
    // this with init() rather than initOverlay() -- the menu draws its own
    // frames onto its own target, dirt backdrop and all, exactly as it did
    // before it could be transparent.
    backdrop_ = backdrop;

    // **The live distance, not the saved one.** The debug settings page can put
    // a world at distance 20, well past what this screen will offer; clamping
    // to maxDistance_ here would mean that merely opening the pause menu undid
    // it. What the Options row refuses is a step *up* past the maximum, so a
    // value that arrives above it can be read and lowered and nothing else.
    renderDistance_ = renderDistance;

    inGame_ = true;
    pauseWorldName_ = worldName != nullptr ? worldName : "";
    // The screen is the same one the world list opens, so it is pointed at a
    // world the same way. Copied rather than borrowed: unlike the name, which
    // is only drawn, this is what saveWorldSettings writes to.
    openWorldSettings(pauseWorldName_, worldPath != nullptr ? worldPath : "");
    pauseCursor_ = 0;
    resumeScreen_ = screen_;
    setScreen(Screen::Pause);

    const u32 revisionAtEntry = packRevision_;

    // Exit, not Resume, if the loop never runs: the only way past aptMainLoop
    // below is the system taking the application away, and the caller's answer
    // to that has to be to close the world rather than to carry on playing a
    // frame at a time into a shutdown.
    PauseChoice choice;
    choice.action = PauseChoice::Action::ExitWorld;

    while (aptMainLoop()) {
        hidScanInput();
        const u32 down = hidKeysDown();

        bool done = false;
        switch (screen_) {
        case Screen::Pause:
            done = handlePause(down, &choice);
            break;
        case Screen::WorldSettings:
            handleWorldSettings(down);
            break;
        case Screen::Options:
            handleOptions(down);
            break;
        case Screen::TexturePacks:
            handleTexturePacks(down);
            break;
        case Screen::PickJar:
            handlePickJar(down);
            break;
        case Screen::ConfirmDeleteJar:
            handleConfirmDeleteJar(down);
            break;
        default:
            // Title, Worlds and ConfirmDelete are not reachable from here --
            // nothing in the pause subtree navigates to them -- and landing on
            // one would mean offering to delete the world being played.
            setScreen(Screen::Pause);
            break;
        }
        if (done) {
            break;
        }

        present();
    }

    inGame_ = false;
    backdrop_ = PauseBackdrop{};
    pauseWorldName_ = "";
    // What the caller applies to the world it still has open. The file was
    // written the moment the row changed; this is the copy in memory.
    choice.gamemode = worldSettings_.gamemode;
    // Back to where the main menu was standing when this world was opened, so
    // Exit World returns to the world list rather than to the pause menu.
    setScreen(resumeScreen_);

    choice.renderDistance = renderDistance_;
    choice.autosaveSeconds = autosaveSeconds_;
    choice.atlasChanged = packRevision_ != revisionAtEntry;
    return choice;
}

void Menu::present()
{
    printConsoleHelp();
    drawFrame();
}

namespace {

// The circle pad reports through the same button mask as the d-pad, so both
// work everywhere without a second code path.
constexpr u32 kUp = KEY_DUP | KEY_CPAD_UP;
constexpr u32 kDown = KEY_DDOWN | KEY_CPAD_DOWN;
constexpr u32 kLeft = KEY_DLEFT | KEY_CPAD_LEFT;
constexpr u32 kRight = KEY_DRIGHT | KEY_CPAD_RIGHT;

int step(u32 down, int cursor, int count)
{
    if (down & kUp) {
        cursor = cursor == 0 ? count - 1 : cursor - 1;
    }
    if (down & kDown) {
        cursor = cursor + 1 >= count ? 0 : cursor + 1;
    }
    return cursor;
}

}  // namespace

bool Menu::handleTitle(u32 down, MenuChoice* choice)
{
    constexpr int kRows = 4;  // singleplayer, multiplayer, options, quit
    titleCursor_ = step(down, titleCursor_, kRows);

    if (down & KEY_START) {
        choice->action = MenuChoice::Action::Quit;
        return true;
    }
    if ((down & KEY_A) == 0) {
        return false;
    }

    switch (titleCursor_) {
    case 0:
        message_ = nullptr;
        refreshWorlds();
        setScreen(Screen::Worlds);
        break;
    case 1:
        // Multiplayer is drawn and does nothing on purpose: the protocol work
        // is M5, and a button that is missing reads as an oversight while one
        // that is greyed out reads as a plan.
        break;
    case 2:
        setScreen(Screen::Options);
        break;
    default:
        choice->action = MenuChoice::Action::Quit;
        return true;
    }
    return false;
}

// a1.1.2's own version of this screen is `ie.class` -- title "Game menu",
// three buttons, laid out top to bottom as **Back to game**, **Save and quit to
// title**, **Options...**. The title is kept; the order and two of the labels
// are not, and both deviations are deliberate.
//
// The order here is Resume, World Settings, Options, Exit World, which puts the
// destructive row at the far end of the list from the cursor's resting place. On
// a console the cursor is moved with a d-pad rather than pointed at, so "one row
// down from where it starts" is a place a thumb lands by accident; on the
// original's order that row is the one that closes the world.
//
// World Settings is a fourth row the original does not have, and it sits above
// Options rather than below it because the two are read as a pair and the
// narrower one -- this world -- comes before the wider one -- this console.
//
// "Exit World" rather than "Save and quit to title" because the saving is not
// optional and never has been: `WorldStreamer::close` writes level.dat on the
// way out of a world however the player left it, so a label offering it as
// though it were a choice would be describing a decision nobody is being given.
// The console line under the screen says it happens.
bool Menu::handlePause(u32 down, PauseChoice* choice)
{
    constexpr int kRows = 4;  // resume, world settings, options, exit world
    pauseCursor_ = step(down, pauseCursor_, kRows);

    // START opened this and START closes it again, which is the gesture a
    // player already has in their hand. B is the same answer, for the same
    // reason it is on every other screen here.
    if ((down & (KEY_START | KEY_B)) != 0) {
        choice->action = PauseChoice::Action::Resume;
        return true;
    }
    if ((down & KEY_A) == 0) {
        return false;
    }

    switch (pauseCursor_) {
    case 0:
        choice->action = PauseChoice::Action::Resume;
        return true;
    case 1:
        message_ = nullptr;
        worldSettingsCursor_ = 0;
        // Already pointed at the open world by runPause; nothing to re-read,
        // and nothing here may walk the tree of a world that is streaming.
        setScreen(Screen::WorldSettings);
        return false;
    case 2:
        message_ = nullptr;
        setScreen(Screen::Options);
        return false;
    default:
        choice->action = PauseChoice::Action::ExitWorld;
        return true;
    }
}

bool Menu::handleWorlds(u32 down, MenuChoice* choice)
{
    const int rows = rowCount(worlds_.size());
    worldCursor_ = step(down, worldCursor_, rows);

    // Keep the cursor on screen, scrolling by the smallest amount that does it.
    if (worldCursor_ < worldScroll_) {
        worldScroll_ = worldCursor_;
    }
    if (worldCursor_ >= worldScroll_ + kVisibleRows) {
        worldScroll_ = worldCursor_ - kVisibleRows + 1;
    }

    if (down & KEY_B) {
        message_ = nullptr;
        setScreen(Screen::Title);
        return false;
    }

    // X is the world's own screen rather than a delete confirmation. Delete is
    // still one press further in, behind the same confirmation it always had;
    // what changed is that X now also reaches size, format and copy, which had
    // nowhere to live when it went straight to a yes/no.
    if ((down & KEY_X) != 0 && worldCursor_ > 0
        && usize(worldCursor_ - 1) < worlds_.size()) {
        const world::WorldEntry& entry = worlds_[usize(worldCursor_ - 1)];
        message_ = nullptr;
        worldSettingsCursor_ = 0;
        openWorldSettings(entry.name, entry.path);
        setScreen(Screen::WorldSettings);
        return false;
    }

    if ((down & KEY_A) == 0) {
        return false;
    }

    if (worldCursor_ == 0) {
        std::string name;
        if (!askWorldName(&name)) {
            return false;
        }
        i64 seed = 0;
        if (!askSeed(&seed)) {
            return false;
        }
        return createWorld(name, seed, choice);
    }

    const world::WorldEntry& entry = worlds_[usize(worldCursor_ - 1)];
    choice->action = MenuChoice::Action::Play;
    choice->worldPath = entry.path;
    choice->worldName = entry.name;

    // Read here rather than left to the caller: a per-world setting has to
    // start from the world it is about, and this is the one place a world is
    // chosen. Reading it fresh is also what stops the last world's gamemode
    // leaking into the next one.
    settings::WorldSettings worldSettings;
    settings::loadWorldSettings(fs_, entry.path, &worldSettings);
    choice->gamemode = worldSettings.gamemode;
    return true;
}

void Menu::handleOptions(u32 down)
{
    constexpr int kRows = 4;  // render distance, autosave, texture pack, back
    optionsCursor_ = step(down, optionsCursor_, kRows);

    if (optionsCursor_ == 0) {
        const int before = renderDistance_;
        if ((down & kLeft) != 0 && renderDistance_ > 2) {
            --renderDistance_;
        }
        if ((down & kRight) != 0 && renderDistance_ < maxDistance_) {
            ++renderDistance_;
        }
        if (renderDistance_ != before) {
            saveSettings();
        }
    }

    if (optionsCursor_ == 1) {
        const int before = autosaveSeconds_;
        int index = autosaveIndex(autosaveSeconds_);
        if ((down & kLeft) != 0 && index > 0) {
            --index;
        }
        if ((down & kRight) != 0 && index < kAutosaveStepCount - 1) {
            ++index;
        }
        autosaveSeconds_ = kAutosaveSteps[index];
        if (autosaveSeconds_ != before) {
            saveSettings();
        }
    }

    if ((down & KEY_A) != 0 && optionsCursor_ == 2) {
        message_ = nullptr;
        refreshPacks();
        setScreen(Screen::TexturePacks);
        return;
    }

    if ((down & KEY_B) != 0 || ((down & KEY_A) != 0 && optionsCursor_ == 3)) {
        setScreen(inGame_ ? Screen::Pause : Screen::Title);
    }
}

// Everything the screen needs off the card, taken once on the way in.
//
// **Nothing here happens in a draw.** Reading a settings file is one open, but
// measuring a folder world is a stat per chunk file across up to 4,096 leaf
// directories, and doing that per frame would turn a menu into a card
// benchmark.
void Menu::openWorldSettings(const std::string& name, const std::string& path)
{
    selectedWorldName_ = name;
    selectedWorldPath_ = path;
    selectedSize_ = world::WorldSize();
    selectedSizeKnown_ = false;

    worldSettings_ = settings::WorldSettings();
    selectedFormat_ = world::WorldFormat::Unknown;
    if (!selectedWorldPath_.empty()) {
        // False means there is no file, which is the ordinary state of every
        // world that predates this feature. The defaults stand and nothing is
        // written until a row changes.
        settings::loadWorldSettings(fs_, selectedWorldPath_, &worldSettings_);
        selectedFormat_ = world::detectFormat(fs_, selectedWorldPath_);
    }
    // The row starts on the world's own mode, so browsing on one world never
    // shows up on the next.
    gamemodeCursor_ = gamemodeIndex(worldSettings_.gamemode);

    // In game the size row is not drawn, and the world is open and being
    // written to, so a total taken now would be stale before it was read.
    if (!inGame_) {
        measureSelectedWorld();
    }
}

void Menu::measureSelectedWorld()
{
    if (selectedWorldPath_.empty()) {
        return;
    }

    // One frame saying what is happening, for the same reason extractJar draws
    // one: a screen that simply stops for a second looks like a crash.
    std::printf("\x1b[2J\x1b[1;1H");
    std::printf("Measuring\n  \x1b[33m%s\x1b[0m\n\n", selectedWorldName_.c_str());
    std::printf("Adding up what it occupies on the\n");
    std::printf("card. A folder world is one file per\n");
    std::printf("chunk, so this takes a moment.\n");
    drawFrame();

    selectedSizeKnown_ = world::worldSize(fs_, selectedWorldPath_, &selectedSize_);
    consoleDirty_ = true;
}

void Menu::saveWorldSettings()
{
    if (selectedWorldPath_.empty()) {
        return;
    }
    // Failing to write is worth saying, unlike 3ds.ini: this is a setting about
    // one world, and a player who set it and came back to find it reset would
    // have no way to tell that from the row not working.
    if (!settings::saveWorldSettings(fs_, selectedWorldPath_, worldSettings_)) {
        message_ = "could not write the world's settings";
        consoleDirty_ = true;
    }
}

// The world's own settings, as against the console's.
void Menu::handleWorldSettings(u32 down)
{
    const int rows = inGame_ ? kWorldSettingsRowsInGame : int(kRowCount);
    worldSettingsCursor_ = step(down, worldSettingsCursor_, rows);
    const int row = worldSettingsRowFor(worldSettingsCursor_, inGame_);

    if (row == kRowGamemode) {
        const int before = gamemodeCursor_;
        if ((down & kLeft) != 0 && gamemodeCursor_ > 0) {
            --gamemodeCursor_;
        }
        if ((down & kRight) != 0 && gamemodeCursor_ < kGamemodeCount - 1) {
            ++gamemodeCursor_;
        }
        // **The row moves onto a disabled mode; the world does not.** It is the
        // value that is refused, not the movement -- a player can put the row
        // on Survival, see it greyed out and read why, which is the whole
        // reason the two unimplemented modes are listed rather than hidden. A
        // dead arrow key would say nothing at all.
        if (gamemodeCursor_ != before
            && settings::gamemodeImplemented(kGamemodeOrder[gamemodeCursor_])) {
            worldSettings_.gamemode = kGamemodeOrder[gamemodeCursor_];
            saveWorldSettings();
        }
    }

    if ((down & KEY_A) != 0 || (down & (kLeft | kRight)) != 0) {
        switch (row) {
        case kRowFormat:
            // Left, Right and A all mean the same thing here, because there
            // are exactly two formats: whichever one this world is not.
            beginConvert();
            return;
        case kRowCopy:
            if ((down & KEY_A) != 0) {
                copySelectedWorld();
                return;
            }
            break;
        case kRowDelete:
            if ((down & KEY_A) != 0) {
                setScreen(Screen::ConfirmDelete);
                return;
            }
            break;
        default:
            break;
        }
    }

    if ((down & KEY_B) != 0 || ((down & KEY_A) != 0 && row == kRowBack)) {
        setScreen(inGame_ ? Screen::Pause : Screen::Worlds);
    }
}

bool Menu::beginConvert()
{
    if (inGame_ || selectedWorldPath_.empty()) {
        return false;
    }
    convertTarget_ = selectedFormat_ == world::WorldFormat::Packed ? world::WorldFormat::Folder
                                                                  : world::WorldFormat::Packed;

    std::printf("\x1b[2J\x1b[1;1H");
    std::printf("Looking at\n  \x1b[33m%s\x1b[0m\n\n", selectedWorldName_.c_str());
    std::printf("Working out what converting it would\n");
    std::printf("cost, before anything is written.\n");
    drawFrame();

    convertEstimate_ = world::format::ConvertEstimate();
    const world::format::ConvertResult result =
        world::format::estimateConversion(fs_, selectedWorldPath_, convertTarget_,
                                          &convertEstimate_);
    consoleDirty_ = true;
    if (result != world::format::ConvertResult::Ok) {
        message_ = world::format::describeConvertResult(result);
        return false;
    }
    message_ = nullptr;
    setScreen(Screen::ConfirmConvert);
    return true;
}

void Menu::runConvert()
{
    // The progress callback the converter drives. It draws a frame and reads
    // the buttons, which is the whole reason a synchronous conversion is
    // bearable: the console has nothing else to do, so the frame loop lives
    // inside the operation rather than around it.
    struct Pump {
        Menu* menu;

        static bool observe(void* context, const world::format::ConvertProgress& progress)
        {
            Pump* pump = static_cast<Pump*>(context);

            if (!aptMainLoop()) {
                // The system is taking the application away. Cancelling is the
                // safe answer and the only one: the staging directory goes and
                // the world is untouched.
                return false;
            }
            hidScanInput();
            if ((hidKeysDown() & KEY_B) != 0) {
                return false;
            }

            std::printf("\x1b[2J\x1b[1;1H");
            std::printf("Converting\n  \x1b[33m%s\x1b[0m\n\n", pump->menu->selectedWorldName_.c_str());
            std::printf("%s\n", progress.stage);
            if (progress.filesTotal > 0) {
                std::printf("  %lu / %lu\n", (unsigned long)progress.filesDone,
                            (unsigned long)progress.filesTotal);
            }
            std::printf("\nB  stop -- the world is not changed\n");
            std::printf("   until this finishes.\n");
            pump->menu->drawFrame();
            return true;
        }
    };

    Pump pump{this};
    world::format::ConvertOptions options;
    options.context = &pump;
    options.observe = &Pump::observe;

    const world::format::ConvertResult result =
        world::format::convertWorld(fs_, selectedWorldPath_, convertTarget_, options);
    consoleDirty_ = true;

    if (result != world::format::ConvertResult::Ok) {
        message_ = world::format::describeConvertResult(result);
    } else {
        message_ = nullptr;
    }

    // Either way the world on the card may have changed shape and size, so both
    // the list and this screen are re-read rather than patched.
    refreshWorlds();
    openWorldSettings(selectedWorldName_, selectedWorldPath_);
    setScreen(Screen::WorldSettings);
}

void Menu::handleConfirmConvert(u32 down)
{
    if ((down & KEY_B) != 0) {
        setScreen(Screen::WorldSettings);
        return;
    }
    if ((down & KEY_A) != 0) {
        runConvert();
    }
}

void Menu::copySelectedWorld()
{
    if (inGame_ || selectedWorldPath_.empty()) {
        return;
    }

    std::string name;
    if (!askCopyName(selectedWorldName_, &name)) {
        return;
    }

    const std::string target = world::worldPath(kSavesDir, name);

    std::printf("\x1b[2J\x1b[1;1H");
    std::printf("Copying\n  \x1b[33m%s\x1b[0m\nto\n  \x1b[33m%s\x1b[0m\n\n",
                selectedWorldName_.c_str(), name.c_str());
    std::printf("File for file, in whichever format\n");
    std::printf("the original is in. A copy is a\n");
    std::printf("backup, not a conversion.\n");
    drawFrame();

    const bool ok = world::copyWorld(fs_, selectedWorldPath_, target);
    consoleDirty_ = true;
    message_ = ok ? nullptr : "could not copy that world -- is the card full?";
    refreshWorlds();
}

void Menu::handleConfirmDelete(u32 down)
{
    if (down & KEY_B) {
        setScreen(Screen::WorldSettings);
        return;
    }
    if ((down & KEY_A) == 0) {
        return;
    }

    // The world this screen is about is the one the settings screen was opened
    // on, held by path rather than by list index: the list is re-read on every
    // refresh, and an index would be pointing at a different row.
    if (!selectedWorldPath_.empty()) {
        message_ = world::deleteWorld(fs_, selectedWorldPath_) ? nullptr
                                                               : "could not delete that world";
    }
    refreshWorlds();
    // Back to the list rather than to the settings screen: the world it was
    // about is gone.
    selectedWorldPath_.clear();
    selectedWorldName_.clear();
    setScreen(Screen::Worlds);
}


void Menu::handleTexturePacks(u32 down)
{
    // Row 0 is "+ Extract from a jar...", then one row per pack with Dev Art
    // first among them.
    const int rows = int(packs_.size()) + 1;
    packCursor_ = step(down, packCursor_, rows);

    if (packCursor_ < packScroll_) {
        packScroll_ = packCursor_;
    }
    if (packCursor_ >= packScroll_ + kVisibleRows) {
        packScroll_ = packCursor_ - kVisibleRows + 1;
    }

    if (down & KEY_B) {
        message_ = nullptr;
        setScreen(Screen::Options);
        return;
    }
    if ((down & KEY_A) == 0) {
        return;
    }

    if (packCursor_ == 0) {
        message_ = nullptr;
        refreshJars();
        setScreen(Screen::PickJar);
        return;
    }

    // A failed load leaves the previous pack live and puts the reason on the
    // console; there is nothing else to do here, and staying on this screen is
    // what lets the player read it and pick another.
    selectPack(packCursor_ - 1);
}

void Menu::handlePickJar(u32 down)
{
    if (!jars_.empty()) {
        jarCursor_ = step(down, jarCursor_, int(jars_.size()));
        if (jarCursor_ < jarScroll_) {
            jarScroll_ = jarCursor_;
        }
        if (jarCursor_ >= jarScroll_ + kVisibleRows) {
            jarScroll_ = jarCursor_ - kVisibleRows + 1;
        }
    }

    if (down & KEY_B) {
        message_ = nullptr;
        setScreen(Screen::TexturePacks);
        return;
    }
    if ((down & KEY_A) == 0 || jars_.empty()) {
        return;
    }
    extractJar(jarCursor_);
}

void Menu::handleConfirmDeleteJar(u32 down)
{
    if (down & KEY_B) {
        // Keeping the jar is the ordinary answer and the one B falls to, which
        // is why this screen has no cursor: the destructive choice needs the
        // button that is never pressed by accident on the way out of a menu.
        importedJar_.clear();
        setScreen(Screen::TexturePacks);
        return;
    }
    if ((down & KEY_A) == 0) {
        return;
    }

    if (!importedJar_.empty()) {
        // The one file this project deletes that the player did not make here.
        // It is reached only from a verified import -- importJar re-opened the
        // pack it wrote and decoded its terrain.png before this screen existed.
        message_ = fs_.removeFile(importedJar_.c_str()) ? nullptr : "could not delete the jar";
        importedJar_.clear();
    }
    refreshJars();
    setScreen(Screen::TexturePacks);
}

bool Menu::askWorldName(std::string* out)
{
    constexpr int kMaxText = 64;

    char initial[kMaxText];
    const std::string suggestion = world::defaultWorldName(worlds_);
    std::snprintf(initial, sizeof(initial), "%s", suggestion.c_str());

    SwkbdState swkbd;
    swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, kMaxText - 1);
    swkbdSetInitialText(&swkbd, initial);
    swkbdSetHintText(&swkbd, "World name");
    swkbdSetFeatures(&swkbd, SWKBD_DARKEN_TOP_SCREEN);
    swkbdSetValidation(&swkbd, SWKBD_NOTEMPTY_NOTBLANK, SWKBD_FILTER_CALLBACK, 0);
    gExistingWorlds = &worlds_;
    swkbdSetFilterCallback(&swkbd, validateWorldName, nullptr);

    char text[kMaxText];
    const SwkbdButton pressed = swkbdInputText(&swkbd, text, sizeof(text));
    gExistingWorlds = nullptr;

    // The applet owned both screens; libctru's console caches a framebuffer
    // address at consoleInit and never looks it up again, so it has to be told
    // again. Same reason, and same safety argument, as
    // Overlay::teleportViaKeyboard.
    consoleInit(GFX_BOTTOM, nullptr);
    consoleDirty_ = true;
    C2D_Prepare();

    if (pressed != SWKBD_BUTTON_CONFIRM) {
        return false;
    }
    // Sanitised again rather than smuggled out of the filter: the player can
    // still edit after the last time it ran.
    return world::sanitizeWorldName(text, out);
}

bool Menu::askCopyName(std::string_view sourceName, std::string* out)
{
    constexpr int kMaxText = 64;

    // The first free "World<n>" rather than the source's name with something
    // appended: the validator refuses a name already on the card, so offering
    // the source's own name would open the keyboard on a value it will not
    // accept.
    char initial[kMaxText];
    const std::string suggestion = world::defaultWorldName(worlds_);
    std::snprintf(initial, sizeof(initial), "%s", suggestion.c_str());

    char hint[64];
    std::snprintf(hint, sizeof(hint), "Copy of %.*s", int(sourceName.size()),
                  sourceName.data());

    SwkbdState swkbd;
    swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, kMaxText - 1);
    swkbdSetInitialText(&swkbd, initial);
    swkbdSetHintText(&swkbd, hint);
    swkbdSetFeatures(&swkbd, SWKBD_DARKEN_TOP_SCREEN);
    swkbdSetValidation(&swkbd, SWKBD_NOTEMPTY_NOTBLANK, SWKBD_FILTER_CALLBACK, 0);
    // The same validation the create keyboard uses, so a copy cannot land on a
    // name a new world could not have.
    gExistingWorlds = &worlds_;
    swkbdSetFilterCallback(&swkbd, validateWorldName, nullptr);

    char text[kMaxText];
    const SwkbdButton pressed = swkbdInputText(&swkbd, text, sizeof(text));
    gExistingWorlds = nullptr;

    consoleInit(GFX_BOTTOM, nullptr);
    consoleDirty_ = true;
    C2D_Prepare();

    if (pressed != SWKBD_BUTTON_CONFIRM) {
        return false;
    }
    return world::sanitizeWorldName(text, out);
}

bool Menu::askSeed(i64* out)
{
    constexpr int kMaxText = 64;

    SwkbdState swkbd;
    swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, kMaxText - 1);
    swkbdSetHintText(&swkbd, "Seed -- leave blank for a random one");
    swkbdSetFeatures(&swkbd, SWKBD_DARKEN_TOP_SCREEN);
    // No validation: **blank is a valid answer here** and means "roll one",
    // which is what a1.1.2 does every time because it never asks at all.
    swkbdSetValidation(&swkbd, SWKBD_ANYTHING, 0, 0);

    char text[kMaxText];
    const SwkbdButton pressed = swkbdInputText(&swkbd, text, sizeof(text));

    consoleInit(GFX_BOTTOM, nullptr);
    consoleDirty_ = true;
    C2D_Prepare();

    if (pressed != SWKBD_BUTTON_CONFIRM) {
        return false;
    }

    if (!seedFromText(text, out)) {
        // a1.1.2's own answer: `new World(File, String)` seeds itself with
        // `new Random().nextLong()`. Java's no-argument Random draws from a
        // process-wide uniquifier mixed with nanoTime, which is not something a
        // console can reproduce and not something worth reproducing -- what
        // matters is that it is a nextLong off a clock-seeded LCG.
        JavaRandom random(clockSeed());
        *out = random.nextLong();
    }
    return true;
}

bool Menu::createWorld(const std::string& name, i64 seed, MenuChoice* choice)
{
    if (!fs_.makeDirectories(kSavesDir)) {
        message_ = "could not make the saves folder on the card";
        consoleDirty_ = true;
        return false;
    }

    const std::string path = world::worldPath(kSavesDir, name);
    const i64 now = nowMillis();

    // **New worlds are packed.** It is the format that suits the hardware --
    // one file per region instead of one per chunk, on a card whose clusters
    // are 16 KB and whose file operations are IPC round trips -- and a world
    // made here has no PC client waiting for it. A player who wants one goes
    // to World Settings and converts, which is lossless in both directions.
    world::AnyStorage storage(fs_);
    if (storage.create(path, seed, now, world::WorldFormat::Packed) != world::OpenResult::Ok) {
        message_ = "could not create the world -- is the card full or locked?";
        consoleDirty_ = true;
        return false;
    }

    // **SnowCovered is rolled here because a1.1.2 rolls it here.** In `cn`'s
    // constructor, the branch taken when there is no level.dat reads
    // `this.snowCovered = this.rand.nextInt(4) == 0`, and `rand` is the World's
    // **unseeded** `new Random()` -- so it is a one-in-four coin flip at
    // creation, not a function of the seed, and it is then persisted and never
    // rolled again. The generator reads it: it puts ice at sea level - 1 across
    // every ocean. Storage::create cannot do this itself because core has no
    // clock, and the harnesses want it off and deterministic.
    JavaRandom random(clockSeed());
    storage.level().snowCovered = random.nextInt(4) == 0;
    const bool saved = storage.saveLevel();
    storage.close(now);
    if (!saved) {
        message_ = "could not write level.dat";
        consoleDirty_ = true;
        return false;
    }

    // The one place a world comes into existence, so the one place its
    // settings file starts out. Written rather than left absent so a player who
    // opens the folder on a PC finds it and can see what it holds; a world that
    // predates this, or one copied in from a PC, still reads as defaults.
    settings::WorldSettings worldSettings;
    settings::saveWorldSettings(fs_, path, worldSettings);

    message_ = nullptr;
    choice->action = MenuChoice::Action::Play;
    choice->worldPath = path;
    choice->worldName = name;
    choice->created = true;
    choice->gamemode = worldSettings.gamemode;
    return true;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void Menu::drawFrame()
{
    if (backdrop_.drawFrame != nullptr) {
        // The caller's frame, with the world already in it. It calls back once
        // per eye; everything this function would otherwise do -- begin, clear,
        // pick a target, end -- belongs to whoever owns the world.
        backdrop_.drawFrame(backdrop_.context, this, &Menu::drawOverlayEntry);
        return;
    }

    if (target_ == nullptr) {
        // initOverlay() and then no backdrop to draw on, which is a caller that
        // paired the two wrong rather than anything a player can reach. There
        // is no target and the screen belongs to somebody else, so the only
        // safe thing to draw is nothing.
        return;
    }

    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    prepare2D();
    C2D_TargetClear(target_, C2D_Color32(0x18, 0x14, 0x10, 0xFF));
    C2D_SceneBegin(target_);
    drawScreen();
    C3D_FrameEnd(0);
}

void Menu::prepare2D()
{
    // citro2d's own state: its shader, its attribute and buffer layout, its
    // combiner stages, and the dirty flags that make it re-send the rest.
    C2D_Prepare();

    // **The two pieces of state citro2d never sets and always assumes.** It
    // calls neither C3D_AlphaBlend nor C3D_AlphaTest anywhere -- checked in
    // libcitro2d.a, not assumed -- and simply inherits what C3D_Init left:
    // src-alpha over one-minus-src-alpha, and no alpha test. That holds right
    // up until something else has drawn, and the something else here is a world
    // renderer that turns blending off for its opaque pass and the alpha test
    // on for its cutouts. Inheriting those would make the scrim solid -- which
    // is the whole of the transparency this menu is drawing for -- and would
    // punch every glyph's antialiased edge out of a pack's font.
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                   GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
    C3D_AlphaTest(false, GPU_ALWAYS, 0x00);
}

void Menu::drawOverlayEntry(void* context, C3D_RenderTarget* target)
{
    static_cast<Menu*>(context)->drawOverlay(target);
}

void Menu::drawOverlay(C3D_RenderTarget* target)
{
    // citro2d's whole state, re-established over the renderer's. It is the
    // supported way round: prepare2D puts back everything citro2d needs, and
    // Renderer::applyWorldState does the same for the world at the top of
    // every eye.
    prepare2D();

    // **Off, not merely reordered.** citro2d draws with GEQUAL against depths
    // of its own between 0 and 0.5; the buffer under it now holds the world's,
    // written by a pass whose test is GREATER and whose near geometry sits high
    // in the range. A menu that respected that would be a menu with terrain
    // poking through it. Order within the 2D pass is submission order, which is
    // the order these functions already draw in, and no depth is written back.
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);

    C2D_SceneBegin(target);
    drawScreen();

    // The batch has to be handed over before the caller ends the frame or draws
    // the next eye over it; citro2d otherwise holds it until its next flush.
    C2D_Flush();
}

void Menu::drawScreen()
{
    // Once per eye rather than once per frame, which is right either way: every
    // label is re-parsed as it is drawn, and the buffer is only a scratch pad
    // between the parse and the draw.
    C2D_TextBufClear(textBuf_);

    drawBackground();
    switch (screen_) {
    case Screen::Title:
        drawTitle();
        break;
    case Screen::Pause:
        drawPause();
        break;
    case Screen::Worlds:
        drawWorlds();
        break;
    case Screen::WorldSettings:
        drawWorldSettings();
        break;
    case Screen::ConfirmConvert:
        drawConfirmConvert();
        break;
    case Screen::Options:
        drawOptions();
        break;
    case Screen::ConfirmDelete:
        drawConfirmDelete();
        break;
    case Screen::TexturePacks:
        drawTexturePacks();
        break;
    case Screen::PickJar:
        drawPickJar();
        break;
    case Screen::ConfirmDeleteJar:
        drawConfirmDeleteJar();
        break;
    }
}

void Menu::drawBackground()
{
    // **Over a world there is no backdrop at all, only the scrim.** That is
    // what the original does: `GuiScreen.drawScreen` draws the dirt only when
    // `mc.theWorld` is null, and fills the screen with a gradient over the
    // world when it is not. The world under this one is the caller's, drawn
    // into this same frame a moment ago -- see PauseBackdrop.
    if (backdrop_.drawFrame != nullptr) {
        drawScrim();
        return;
    }

    // a1.1.2's own menu backdrop: the dirt tile, tiled at 32 pixels and
    // multiplied by 0x404040. Both constants are read out of `GuiScreen`
    // (`bh.class`) rather than remembered, and the darkening is already in the
    // texels -- see core/texture/background.hpp.
    if (background_.ready()) {
        background_.draw(kScreenWidth, kScreenHeight, 0.0f);
    } else {
        // Nothing to tile with, which means no atlas either -- a menu that
        // failed to build one at all. Flat quads with a stable per-tile shade,
        // which is what this screen drew before any pack was read.
        constexpr float kTile = 20.0f;
        constexpr int kCols = int(kScreenWidth / kTile);
        constexpr int kRows = int(kScreenHeight / kTile);

        for (int y = 0; y < kRows; ++y) {
            for (int x = 0; x < kCols; ++x) {
                const int shade = int(tileHash(x, y) & 15u) - 7;
                const u32 colour = C2D_Color32(clampByte(70 + shade), clampByte(52 + shade),
                                               clampByte(37 + shade), 0xFF);
                C2D_DrawRectSolid(float(x) * kTile, float(y) * kTile, 0.0f, kTile, kTile,
                                  colour);
            }
        }
    }

    if (inGame_) {
        // A world behind the menu that the menu is *not* drawing over: this is
        // the pause menu without a backdrop, which is the fallback path. The
        // dirt is up, and the scrim is what says the world is still there
        // rather than gone.
        drawScrim();
    }
}

void Menu::drawScrim()
{
    // The original's own: `GuiScreen.drawScreen` fills the screen with a
    // gradient from 0xC0101010 to 0xD0101010 whenever a world is open behind
    // it, read out of `bh.class` rather than remembered. Two alphas eight apart
    // is barely a gradient, which is the point -- it is what the original
    // draws.
    //
    // 0.05 rather than the backdrop's 0.0. Over a world the depth test is off
    // and only submission order matters, but the two paths draw the same thing
    // and the numbers here already state their own order (outline 0.1, fill
    // 0.2, bevel 0.3, text 0.4).
    const u32 top = C2D_Color32(0x10, 0x10, 0x10, 0xC0);
    const u32 bottom = C2D_Color32(0x10, 0x10, 0x10, 0xD0);
    C2D_DrawRectangle(0.0f, 0.0f, 0.05f, kScreenWidth, kScreenHeight, top, top, bottom, bottom);
}

void Menu::drawTitle()
{
    drawLabelCentered("3DAlpha", kScreenWidth * 0.5f, 36.0f, 1.4f, kInk, true);
    drawLabelCentered(mcver::kDisplay, kScreenWidth * 0.5f, 72.0f, 0.5f, kInkDim, true);

    const float x = (kScreenWidth - kButtonWidth) * 0.5f;
    const char* labels[] = {"Singleplayer", "Multiplayer", "Options", "Quit"};
    for (int i = 0; i < 4; ++i) {
        const Rect rect{x, 100.0f + float(i) * (kButtonHeight + 6.0f), kButtonWidth,
                        kButtonHeight};
        drawButton(rect, labels[i], titleCursor_ == i, i != 1);
    }
}

void Menu::drawPause()
{
    // "Game menu" is the original's own title for this screen, from ie.class.
    drawLabelCentered("Game menu", kScreenWidth * 0.5f, 30.0f, 1.0f, kInk, true);
    // A card can hold hundreds of worlds and their names come off it unchecked,
    // so this is clipped like every other name here rather than centred and
    // allowed to run off both edges.
    drawLabelClipped(pauseWorldName_, 40.0f, 62.0f, 0.5f, kInkDim, kScreenWidth - 80.0f);

    // Four rows rather than three, so they start higher and are spaced tighter
    // than they were: 240 pixels does not stretch, and the line about saving
    // still has to sit under the last of them.
    const float x = (kScreenWidth - kButtonWidth) * 0.5f;
    const char* labels[] = {"Resume", "World Settings", "Options", "Exit World"};
    for (int i = 0; i < 4; ++i) {
        const Rect rect{x, 80.0f + float(i) * (kButtonHeight + 4.0f), kButtonWidth,
                        kButtonHeight};
        drawButton(rect, labels[i], pauseCursor_ == i, true);
    }

    drawLabelCentered("Exiting saves the world.", kScreenWidth * 0.5f, 206.0f, 0.45f, kInkDim,
                      true);
}

void Menu::drawWorldSettings()
{
    drawLabelCentered("World Settings", kScreenWidth * 0.5f, 8.0f, 0.7f, kInk, true);
    // Clipped rather than centred, for the same reason every other name here
    // is: it came off a card and can be anything a PC let somebody type.
    drawLabelClipped(selectedWorldName_.c_str(), 40.0f, 28.0f, 0.5f, kInkDim,
                     kScreenWidth - 80.0f);

    const float x = (kScreenWidth - kButtonWidth) * 0.5f;
    const int rows = inGame_ ? kWorldSettingsRowsInGame : int(kRowCount);

    // **Six rows and two lines of note in 240 pixels**, which is what sets
    // these numbers rather than taste. The buttons are shorter than
    // kButtonHeight and the pitch is five pixels wider than they are, so
    // drawButton's two-pixel outline does not overlap the row below: 42 + 6 x
    // 28 puts the last outline's bottom edge at 207, and the note fits under
    // it.
    const float top = 42.0f;
    const float pitch = 28.0f;
    const float height = 23.0f;

    // A row's value sits to the right of its name on the same button, so six
    // rows fit in 240 pixels without a second column of labels.
    constexpr float kValueX = 96.0f;

    for (int i = 0; i < rows; ++i) {
        const int row = worldSettingsRowFor(i, inGame_);
        const Rect rect{x, top + float(i) * pitch, kButtonWidth, height};
        const bool selected = worldSettingsCursor_ == i;

        if (row == kRowBack) {
            drawButton(rect, "Back", selected, true);
            continue;
        }
        if (row == kRowCopy || row == kRowDelete) {
            drawButton(rect, row == kRowCopy ? "Copy..." : "Delete...", selected, true);
            continue;
        }

        drawButton(rect, "", selected, true);
        const char* name = row == kRowGamemode ? "Gamemode:"
                                               : (row == kRowFormat ? "Format:" : "Size:");
        drawLabel(name, rect.x + 8.0f, rect.y + 6.0f, 0.5f, kInkDim, C2D_AlignLeft, true);

        char value[64];
        u32 colour = kInk;
        if (row == kRowGamemode) {
            const settings::Gamemode mode = kGamemodeOrder[gamemodeCursor_];
            std::snprintf(value, sizeof(value), "%s", settings::gamemodeLabel(mode));
            // The value, not the button, is what says a mode is unavailable:
            // the row itself still works, and greying the whole button would
            // read as "this row is broken".
            colour = settings::gamemodeImplemented(mode) ? kInk : kInkDim;
        } else if (row == kRowFormat) {
            std::snprintf(value, sizeof(value), "%s",
                          world::formatName(selectedFormat_));
        } else if (!selectedSizeKnown_) {
            std::snprintf(value, sizeof(value), "--");
            colour = kInkDim;
        } else {
            // On-disk first, because that is what the card actually gives up
            // and the number a player checking free space needs. The chunk
            // count comes with it so the two formats can be compared.
            char onDisk[24];
            formatBytes(selectedSize_.onDiskBytes, onDisk, sizeof(onDisk));
            std::snprintf(value, sizeof(value), "%s  (%lu files)", onDisk,
                          (unsigned long)selectedSize_.fileCount);
        }
        drawLabelClipped(value, rect.x + kValueX, rect.y + 5.0f, 0.5f, colour,
                         rect.w - kValueX - 8.0f);
    }

    // A line under the rows, saying whichever thing this screen most needs to
    // say. Not on the console alone: a row that can be moved onto and refuses
    // to change is otherwise indistinguishable from one that is broken.
    const int row = worldSettingsRowFor(worldSettingsCursor_, inGame_);
    const float noteY = top + float(rows) * pitch + 2.0f;
    if (row == kRowGamemode && !settings::gamemodeImplemented(kGamemodeOrder[gamemodeCursor_])) {
        drawLabelCentered("Not implemented yet -- there is no player", kScreenWidth * 0.5f,
                          noteY, 0.42f, kInkWarn, true);
        drawLabelCentered("body to collide with. Spectator is real.", kScreenWidth * 0.5f,
                          noteY + 13.0f, 0.42f, kInkDim, true);
    } else if (row == kRowFormat && !inGame_) {
        drawLabelCentered(selectedFormat_ == world::WorldFormat::Packed
                              ? "Packed: fast on this console, but only"
                              : "Folder: what a PC Minecraft client opens.",
                          kScreenWidth * 0.5f, noteY, 0.42f, kInkDim, true);
        drawLabelCentered(selectedFormat_ == world::WorldFormat::Packed
                              ? "3DAlpha opens it. A/Left/Right converts."
                              : "A/Left/Right packs it for this console.",
                          kScreenWidth * 0.5f, noteY + 13.0f, 0.42f, kInkDim, true);
    } else if (row == kRowSize && selectedSizeKnown_) {
        char content[24];
        char onDisk[24];
        formatBytes(selectedSize_.contentBytes, content, sizeof(content));
        formatBytes(selectedSize_.onDiskBytes, onDisk, sizeof(onDisk));
        char line[96];
        // Both numbers, because the gap between them *is* the argument for
        // packing: a 2,917-byte chunk in a 16 KB cluster costs 16 KB.
        std::snprintf(line, sizeof(line), "%s of data, %s of card", content, onDisk);
        drawLabelCentered(line, kScreenWidth * 0.5f, noteY, 0.42f, kInkDim, true);
    } else if (inGame_) {
        drawLabelCentered("Copy, Delete and Format need the world", kScreenWidth * 0.5f,
                          noteY, 0.42f, kInkDim, true);
        drawLabelCentered("closed. They are on the world list's X.",
                          kScreenWidth * 0.5f, noteY + 13.0f, 0.42f, kInkDim, true);
    }
}

void Menu::drawConfirmConvert()
{
    const bool toPacked = convertTarget_ == world::WorldFormat::Packed;

    drawLabelCentered(toPacked ? "Pack this world?" : "Unpack this world?",
                      kScreenWidth * 0.5f, 30.0f, 0.8f, kInk, true);
    drawLabelClipped(selectedWorldName_.c_str(), 40.0f, 58.0f, 0.55f, kInkWarn,
                     kScreenWidth - 80.0f);

    char now[24];
    char after[24];
    char free[24];
    formatBytes(convertEstimate_.sourceOnDisk, now, sizeof(now));
    formatBytes(convertEstimate_.targetOnDisk, after, sizeof(after));
    formatBytes(convertEstimate_.freeBytes, free, sizeof(free));

    char line[96];
    std::snprintf(line, sizeof(line), "%s now, about %s after", now, after);
    drawLabelCentered(line, kScreenWidth * 0.5f, 88.0f, 0.5f, kInk, true);

    // **Both copies exist at once**, so the space that matters is the target's
    // size on top of what is already there, not the difference between them.
    if (convertEstimate_.freeBytes > 0) {
        std::snprintf(line, sizeof(line), "%s free -- both copies exist at once", free);
    } else {
        std::snprintf(line, sizeof(line), "free space unknown on this card");
    }
    drawLabelCentered(line, kScreenWidth * 0.5f, 108.0f, 0.42f, kInkDim, true);

    std::snprintf(line, sizeof(line), "%lu chunks, %lu files",
                  (unsigned long)convertEstimate_.chunks, (unsigned long)convertEstimate_.files);
    drawLabelCentered(line, kScreenWidth * 0.5f, 126.0f, 0.42f, kInkDim, true);

    drawLabelCentered(toPacked ? "A packed world is not openable on a PC."
                               : "The result is a plain Alpha save again.",
                      kScreenWidth * 0.5f, 148.0f, 0.42f, toPacked ? kInkWarn : kInkDim,
                      true);

    const float half = kButtonWidth * 0.5f;
    drawButton(Rect{kScreenWidth * 0.5f - half - 8.0f, 176.0f, half, kButtonHeight},
               "A Convert", true, true);
    drawButton(Rect{kScreenWidth * 0.5f + 8.0f, 176.0f, half, kButtonHeight}, "B Cancel",
               false, true);
}

void Menu::drawWorlds()
{
    drawLabelCentered("Select World", kScreenWidth * 0.5f, 16.0f, 0.7f, kInk, true);

    const int rows = rowCount(worlds_.size());
    const float rowX = 20.0f;
    const float rowWidth = kScreenWidth - 2.0f * rowX;

    for (int i = 0; i < kVisibleRows; ++i) {
        const int index = worldScroll_ + i;
        if (index >= rows) {
            break;
        }
        const Rect rect{rowX, kRowsTop + float(i) * (kRowHeight + kRowGap), rowWidth,
                        kRowHeight};
        const bool selected = index == worldCursor_;

        if (index == 0) {
            drawButton(rect, "+ Create New World", selected, true);
            continue;
        }

        // A world row is a button with two texts on it rather than a centred
        // label, so the name and when it was last played both fit.
        drawButton(rect, "", selected, true);
        const world::WorldEntry& entry = worlds_[usize(index - 1)];

        // Wide enough for the format's worst case rather than its usual one:
        // tm_year is an int and gmtime_r will hand back a five-digit year for a
        // level.dat with a nonsense LastPlayed in it.
        char when[64];
        formatWhen(entry.lastPlayed, when, sizeof(when));

        // The date's column is fixed, and the name gets whatever is left.
        constexpr float kWhenWidth = 100.0f;
        drawLabelClipped(entry.name.c_str(), rect.x + 10.0f, rect.y + 3.0f, 0.55f, kInk,
                         rect.w - 20.0f - kWhenWidth);
        drawLabel(when, rect.x + rect.w - 10.0f, rect.y + 6.0f, 0.4f, kInkDim, C2D_AlignRight,
                  true);
    }

    drawListChrome(rows, worldScroll_);
}

int Menu::drawListChrome(int rows, int scroll)
{
    // Which way there is more list. Cheaper than a scrollbar and it answers the
    // only question a player has here.
    if (scroll > 0) {
        drawLabelCentered("^", kScreenWidth * 0.5f, kRowsTop - 10.0f, 0.5f, kInkDim, true);
    }
    if (scroll + kVisibleRows < rows) {
        drawLabelCentered("v", kScreenWidth * 0.5f, kScreenHeight - 10.0f, 0.5f, kInkDim,
                          true);
    }
    const int left = rows - scroll;
    return left < kVisibleRows ? (left < 0 ? 0 : left) : kVisibleRows;
}

void Menu::drawOptions()
{
    drawLabelCentered("Options", kScreenWidth * 0.5f, 16.0f, 0.8f, kInk, true);

    char distance[48];
    std::snprintf(distance, sizeof(distance), "Render distance: %d  (max %d)", renderDistance_,
                  maxDistance_);

    char autosave[48];
    autosaveLabel(autosaveSeconds_, autosave, sizeof(autosave));

    // A pack name comes off a card and can be as long as FAT allows, so the row
    // is a button with a clipped label on it rather than a centred one that
    // would draw off both edges of the screen.
    //
    // Four rows now rather than three, so they start higher and the heading
    // moved up with them: 240 pixels does not stretch, and the alternative was
    // a scrolling options screen for four items.
    const float x = (kScreenWidth - kButtonWidth) * 0.5f;
    drawButton(Rect{x, 54.0f, kButtonWidth, kButtonHeight}, distance, optionsCursor_ == 0,
               true);
    drawButton(Rect{x, 90.0f, kButtonWidth, kButtonHeight}, autosave, optionsCursor_ == 1,
               true);

    const Rect packRow{x, 126.0f, kButtonWidth, kButtonHeight};
    drawButton(packRow, "", optionsCursor_ == 2, true);
    drawLabel("Texture Pack:", packRow.x + 8.0f, packRow.y + 6.0f, 0.5f, kInkDim,
              C2D_AlignLeft, true);
    drawLabelClipped(packLabel(), packRow.x + 96.0f, packRow.y + 5.0f, 0.5f, kInk,
                     packRow.w - 104.0f);

    drawButton(Rect{x, 162.0f, kButtonWidth, kButtonHeight}, "Back", optionsCursor_ == 3,
               true);

    drawLabelCentered(isNew3DS_ ? "New 3DS" : "Old 3DS", kScreenWidth * 0.5f, 206.0f, 0.45f,
                      kInkDim, true);
}

const char* Menu::packLabel() const
{
    return packName_.empty() ? "Dev Art" : packName_.c_str();
}

void Menu::drawTexturePacks()
{
    drawLabelCentered("Texture Pack", kScreenWidth * 0.5f, 16.0f, 0.7f, kInk, true);

    const int rows = int(packs_.size()) + 1;
    const float rowX = 20.0f;
    const float rowWidth = kScreenWidth - 2.0f * rowX;

    const int visible = drawListChrome(rows, packScroll_);
    for (int i = 0; i < visible; ++i) {
        const int index = packScroll_ + i;
        const Rect rect{rowX, kRowsTop + float(i) * (kRowHeight + kRowGap), rowWidth,
                        kRowHeight};
        const bool selected = index == packCursor_;

        if (index == 0) {
            drawButton(rect, "+ Extract from a jar...", selected, true);
            continue;
        }

        const texture::PackEntry& pack = packs_[usize(index - 1)];
        drawButton(rect, "", selected, true);

        // Which pack is live, as a mark on the row rather than a separate line:
        // "selected" here means the cursor, and the player needs to see both at
        // once.
        const bool active = pack.builtIn ? packName_.empty() : pack.name == packName_;
        if (active) {
            drawLabel("*", rect.x + 8.0f, rect.y + 5.0f, 0.55f, kInkWarn, C2D_AlignLeft, true);
        }

        char detail[32];
        if (pack.builtIn) {
            std::snprintf(detail, sizeof(detail), "built in");
        } else {
            // How much of the a1.1.2 layout the pack carries, out of the 58
            // names a real client jar holds. A partial pack still works -- only
            // terrain.png is drawn -- and this is what says so at a glance.
            std::snprintf(detail, sizeof(detail), "%d/%d files", pack.textureCount,
                          texture::kA112FileCount);
        }

        constexpr float kDetailWidth = 84.0f;
        drawLabelClipped(pack.builtIn ? "Dev Art" : pack.name.c_str(), rect.x + 22.0f,
                         rect.y + 3.0f, 0.55f, kInk, rect.w - 32.0f - kDetailWidth);
        drawLabel(detail, rect.x + rect.w - 10.0f, rect.y + 6.0f, 0.4f, kInkDim,
                  C2D_AlignRight, true);
    }
}

void Menu::drawPickJar()
{
    drawLabelCentered("Extract from a jar", kScreenWidth * 0.5f, 16.0f, 0.7f, kInk, true);

    if (jars_.empty()) {
        drawLabelCentered("No .jar on the card", kScreenWidth * 0.5f, 100.0f, 0.6f, kInkDim,
                          true);
        drawLabelCentered("Copy your own Minecraft jar into", kScreenWidth * 0.5f, 130.0f,
                          0.45f, kInkDim, true);
        drawLabelCentered("3dalpha/packs and come back.", kScreenWidth * 0.5f, 150.0f, 0.45f,
                          kInkDim, true);
        return;
    }

    const float rowX = 20.0f;
    const float rowWidth = kScreenWidth - 2.0f * rowX;

    const int visible = drawListChrome(int(jars_.size()), jarScroll_);
    for (int i = 0; i < visible; ++i) {
        const int index = jarScroll_ + i;
        const Rect rect{rowX, kRowsTop + float(i) * (kRowHeight + kRowGap), rowWidth,
                        kRowHeight};
        drawButton(rect, "", index == jarCursor_, true);

        const texture::JarEntry& jar = jars_[usize(index)];

        // The size is what tells a 900 KB alpha jar apart from a modern one at
        // a glance, which is the one thing a player can judge from this screen.
        char size[24];
        std::snprintf(size, sizeof(size), "%.1f MB", double(jar.bytes) / (1024.0 * 1024.0));

        constexpr float kSizeWidth = 70.0f;
        drawLabelClipped(jar.name.c_str(), rect.x + 10.0f, rect.y + 3.0f, 0.55f, kInk,
                         rect.w - 20.0f - kSizeWidth);
        drawLabel(size, rect.x + rect.w - 10.0f, rect.y + 6.0f, 0.4f, kInkDim, C2D_AlignRight,
                  true);
    }
}

void Menu::drawConfirmDeleteJar()
{
    drawLabelCentered("Delete the jar?", kScreenWidth * 0.5f, 44.0f, 0.8f, kInk, true);

    char summary[80];
    std::snprintf(summary, sizeof(summary), "%d textures extracted and checked", importedCount_);
    drawLabelCentered(summary, kScreenWidth * 0.5f, 80.0f, 0.45f, kInkDim, true);

    const usize slash = importedPack_.find_last_of('/');
    drawLabelClipped(slash == std::string::npos ? importedPack_.c_str()
                                                : importedPack_.c_str() + slash + 1,
                     30.0f, 100.0f, 0.55f, kInkWarn, kScreenWidth - 60.0f);

    drawLabelCentered("The pack works without it.", kScreenWidth * 0.5f, 134.0f, 0.45f, kInkDim,
                      true);

    // "Keep" is drawn as the highlighted answer even though neither is under a
    // cursor: this deletes a file the player brought to the card themselves,
    // and the layout should not suggest that deleting is the expected reply.
    const float half = kButtonWidth * 0.5f;
    drawButton(Rect{kScreenWidth * 0.5f - half - 8.0f, 170.0f, half, kButtonHeight},
               "A Delete", false, true);
    drawButton(Rect{kScreenWidth * 0.5f + 8.0f, 170.0f, half, kButtonHeight}, "B Keep", true,
               true);
}

void Menu::drawConfirmDelete()
{
    // The world the settings screen was opened on, held by name rather than by
    // list index -- the list is re-read on every refresh.
    const char* name = selectedWorldName_.c_str();

    drawLabelCentered("Delete this world?", kScreenWidth * 0.5f, 60.0f, 0.8f, kInk, true);
    drawLabelCentered(name, kScreenWidth * 0.5f, 100.0f, 0.6f, kInkWarn, true);
    drawLabelCentered("Everything in it goes with it.", kScreenWidth * 0.5f, 130.0f, 0.45f,
                      kInkDim, true);

    const float half = kButtonWidth * 0.5f;
    drawButton(Rect{kScreenWidth * 0.5f - half - 8.0f, 170.0f, half, kButtonHeight}, "A Delete",
               true, true);
    drawButton(Rect{kScreenWidth * 0.5f + 8.0f, 170.0f, half, kButtonHeight}, "B Keep", false,
               true);
}

void Menu::drawButton(const Rect& rect, const char* label, bool selected, bool enabled)
{
    const u32 fill = !enabled ? kFillDisabled : (selected ? kFillSelected : kFill);

    C2D_DrawRectSolid(rect.x - 2.0f, rect.y - 2.0f, 0.1f, rect.w + 4.0f, rect.h + 4.0f,
                      kOutline);
    C2D_DrawRectSolid(rect.x, rect.y, 0.2f, rect.w, rect.h, fill);
    C2D_DrawRectSolid(rect.x, rect.y, 0.3f, rect.w, 2.0f, kBevelLight);
    C2D_DrawRectSolid(rect.x, rect.y + rect.h - 2.0f, 0.3f, rect.w, 2.0f, kBevelDark);

    if (label[0] != '\0') {
        drawLabelCentered(label, rect.x + rect.w * 0.5f, rect.y + rect.h * 0.5f, 0.55f,
                          enabled ? kInk : kInkDim, true);
    }
}

void Menu::drawLabel(const char* text, float x, float y, float scale, u32 color, u32 flags,
                     bool shadow)
{
    if (font_.ready()) {
        // The pack's font. `x` means what the alignment flag says it means, so
        // the width has to be known before anything is drawn -- which costs a
        // walk of the string and no glyph buffer at all.
        const int pixels = fontScale(scale);
        const float width = float(font_.measure(text) * pixels);
        float left = x;
        if ((flags & C2D_AlignMask) == C2D_AlignCenter) {
            left = x - width * 0.5f;
        } else if ((flags & C2D_AlignMask) == C2D_AlignRight) {
            left = x - width;
        }
        font_.draw(text, float(int(left + 0.5f)), fontTop(y, scale, pixels), pixels, color,
                   shadow, 0.4f);
        return;
    }

    C2D_Text parsed;
    C2D_TextParse(&parsed, textBuf_, text);
    C2D_TextOptimize(&parsed);

    if (shadow) {
        C2D_DrawText(&parsed, C2D_WithColor | flags, x + 1.0f, y + 1.0f, 0.4f, scale, scale,
                     kShadow);
    }
    C2D_DrawText(&parsed, C2D_WithColor | flags, x, y, 0.5f, scale, scale, color);
}

void Menu::drawLabelClipped(const char* text, float x, float y, float scale, u32 color,
                            float maxWidth)
{
    if (font_.ready()) {
        // Exact rather than estimated, unlike the system-font path below: the
        // widths are a table, so how much fits is arithmetic and the ellipsis
        // goes exactly where the last glyph that fits ended.
        const int pixels = fontScale(scale);
        char clipped[128];
        const std::string_view shown =
            font_.clip(text, int(maxWidth) / pixels, clipped, sizeof(clipped));
        font_.draw(shown, float(int(x + 0.5f)), fontTop(y, scale, pixels), pixels, color,
                   /*shadow=*/true, 0.4f);
        return;
    }

    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "%s", text);

    C2D_Text parsed;
    C2D_TextParse(&parsed, textBuf_, buffer);

    float width = 0.0f;
    float height = 0.0f;
    C2D_TextGetDimensions(&parsed, scale, scale, &width, &height);

    // **One corrective step, not a loop.** Every parse spends glyphs out of the
    // frame's text buffer, so a shrink-until-it-fits loop over a pathological
    // name would empty it and take the rest of the screen with it. The font is
    // proportional, so the estimate is not exact -- but it is always shorter
    // than what did not fit, and one visibly clipped name is better than a
    // frame with no text in it at all.
    const usize length = std::strlen(buffer);
    if (width > maxWidth && length > 4) {
        usize keep = usize(float(length) * (maxWidth / width));
        if (keep + 3 >= length) {
            keep = length - 4;
        }
        if (keep < 1) {
            keep = 1;
        }
        std::snprintf(buffer + keep, sizeof(buffer) - keep, "...");
        C2D_TextParse(&parsed, textBuf_, buffer);
    }

    C2D_TextOptimize(&parsed);
    C2D_DrawText(&parsed, C2D_WithColor | C2D_AlignLeft, x + 1.0f, y + 1.0f, 0.4f, scale, scale,
                 kShadow);
    C2D_DrawText(&parsed, C2D_WithColor | C2D_AlignLeft, x, y, 0.5f, scale, scale, color);
}

void Menu::drawLabelCentered(const char* text, float cx, float cy, float scale, u32 color,
                             bool shadow)
{
    if (font_.ready()) {
        const int pixels = fontScale(scale);
        const float width = float(font_.measure(text) * pixels);
        const float top = cy - float(texture::kFontCellPixels * pixels) * 0.5f;
        font_.draw(text, float(int(cx - width * 0.5f + 0.5f)), float(int(top + 0.5f)), pixels,
                   color, shadow, 0.4f);
        return;
    }

    C2D_Text parsed;
    C2D_TextParse(&parsed, textBuf_, text);
    C2D_TextOptimize(&parsed);

    // C2D_AlignCenter handles x; y is the *top* of the line, so the height has
    // to come back out of the buffer to centre it.
    float width = 0.0f;
    float height = 0.0f;
    C2D_TextGetDimensions(&parsed, scale, scale, &width, &height);
    const float y = cy - height * 0.5f;

    if (shadow) {
        C2D_DrawText(&parsed, C2D_WithColor | C2D_AlignCenter, cx + 1.0f, y + 1.0f, 0.4f,
                     scale, scale, kShadow);
    }
    C2D_DrawText(&parsed, C2D_WithColor | C2D_AlignCenter, cx, y, 0.5f, scale, scale, color);
}

}  // namespace mc::ctr
