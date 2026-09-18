#include "platform/ctr/menu.hpp"

#include "platform/ctr/local_link.hpp"
#include "platform/ctr/network.hpp"

#include "platform/ctr/hud.hpp"
#include "platform/ctr/overlay.hpp"
#include "platform/ctr/renderer.hpp"

#include "core/audio/vorbis_stream.hpp"
#include "core/gui/settings_list.hpp"
#include "core/gui/text.hpp"
#include "core/settings/world_settings.hpp"
#include "core/texture/background.hpp"
#include "core/texture/jar_import.hpp"
#include "core/util/java_random.hpp"
#include "core/util/seed_text.hpp"
#include "core/world/any_storage.hpp"
#include "core/world/format/converter.hpp"
#include "core/world/spawn_point.hpp"
#include "core/world/world_format.hpp"
#include "core/world/world_transfer.hpp"

#include "version_config.hpp"
#include "version_slots.hpp"

#include <3ds.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace mc::ctr {

// `GuiChat`'s own cap: a1.1.2 stops taking characters at a hundred.
constexpr int kMaxChatChars = 100;

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

// The rows pinned above the worlds themselves: "+ Create New World" and
// "+ Import World". A world's place in `worlds_` is its row minus this, and
// every index on the world screen goes through one of the two names below
// rather than through a literal 1.
enum WorldListRow {
    kWorldRowCreate = 0,
    kWorldRowImport = 1,
    kWorldRowFirst = 2,
};

// Anything above this and a2 is `- empty -` on a1.1.2's own screen; here it is
// only the point at which the list scrolls.
int rowCount(usize worlds)
{
    return int(worlds) + kWorldRowFirst;
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
        std::snprintf(out, size, "Off");
    } else if (seconds < 60) {
        std::snprintf(out, size, "%ds", seconds);
    } else if (seconds % 60 == 0) {
        std::snprintf(out, size, "%dm", seconds / 60);
    } else {
        std::snprintf(out, size, "%dm %ds", seconds / 60, seconds % 60);
    }
}

// The order the gamemode row steps through. **Survival first, because it is
// the one a1.1.2 has** and the one a new world starts in -- see
// `settings::kNewWorldGamemode` -- then Creative, then Spectator, which is the
// furthest from the game and the only one of the three that is not playing it.
// All three are live; the greying below is kept for a mode added later -- see
// settings::gamemodeImplemented.
//
// **Live modes stay adjacent.** Stepping the row is one button, so a disabled
// mode between two live ones would make every switch between them pass through
// a state that refuses.
constexpr settings::Gamemode kGamemodeOrder[] = {
    settings::Gamemode::Survival,
    settings::Gamemode::Creative,
    settings::Gamemode::Spectator,
};
constexpr int kGamemodeCount = int(sizeof(kGamemodeOrder) / sizeof(kGamemodeOrder[0]));

// The order the difficulty row steps through, which is the game's own: the
// four are a scale and a player reads left to right.
constexpr settings::Difficulty kDifficultyOrder[] = {
    settings::Difficulty::Peaceful,
    settings::Difficulty::Easy,
    settings::Difficulty::Normal,
    settings::Difficulty::Hard,
};
constexpr int kDifficultyCount =
    int(sizeof(kDifficultyOrder) / sizeof(kDifficultyOrder[0]));

int difficultyIndex(settings::Difficulty level)
{
    for (int i = 0; i < kDifficultyCount; ++i) {
        if (kDifficultyOrder[i] == level) {
            return i;
        }
    }
    return int(settings::Difficulty::Normal);
}

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

// **The Options screen's rows, top to bottom, in the groups they are drawn in**:
// what the world looks like, then the three sound rows, then saving, then Back
// on its own. Sound used to be a sub-screen, because this was a fixed column
// pitched to fill 240 pixels and its explanation needed room a value row does
// not have. The list scrolls now and the explanation is on the bottom screen,
// so both reasons are gone.
enum OptionsRow {
    kOptDistance = 0,
    // **Next to the distance, not down with the card rows.** The two of them
    // are what the player sees and how they see it; the pack and the skin are
    // what it is made of.
    kOptSensitivity,
    kOptPack,
    kOptSkin,
    kOptAudio,
    kOptMusic,
    kOptSound,
    kOptAutosave,
    kOptBack,
    kOptCount,
};
constexpr u8 kOptionsGroups[kOptCount] = {0, 0, 0, 0, 1, 1, 1, 2, 3};

// The rows of the World Settings screen. **World Info is a row that does
// nothing on the top screen**: it is where everything that is a fact about the
// world rather than a choice goes, and its explanation on the bottom screen is
// those facts. In game there is a world open and streaming behind this, so
// copying, deleting and converting it are not on offer -- the three of them
// all mean rewriting files something else holds.
enum WorldSettingsRow {
    kRowInfo = 0,
    kRowGamemode,
    kRowDifficulty,
    kRowFormat,
    kRowCopy,
    // **Copy, to another console.** It sits under Copy rather than under the
    // format rows because that is what it is: the same file-for-file duplicate,
    // with a radio in the middle instead of a second folder. Out of a game for
    // the same reason the three above it are -- it reads every file this world
    // holds, and a world that is open and streaming holds them.
    kRowExport,
    kRowDelete,
    kRowExtra,
    kRowBack,
    kRowCount,
};
constexpr u8 kWorldSettingsRows[] = {kRowInfo,   kRowGamemode, kRowDifficulty, kRowFormat,
                                     kRowCopy,   kRowExport,   kRowDelete,     kRowExtra,
                                     kRowBack};
constexpr u8 kWorldSettingsGroups[] = {0, 1, 1, 2, 2, 2, 2, 3, 4};
constexpr u8 kWorldSettingsRowsInGame[] = {kRowInfo, kRowGamemode, kRowDifficulty, kRowBack};
constexpr u8 kWorldSettingsGroupsInGame[] = {0, 1, 1, 3};

// **The Extra Settings screen's rows, in the order they were asked for.**
// Nothing on this screen is a1.1.2 and the screen says so in its own tooltip;
// see core/settings/world_settings.hpp for why the choices live in the world
// and not in `3ds.ini`.
//
// The three groups are what the rows are about rather than what they are made
// of, which is why the two bug fixes are not together: the first group changes
// what the ground is made of, the second changes how the world is *shown*, and
// bedrock is on its own because it is the one fix that is not retroactive at
// all -- it only touches ground that has not been generated yet.
enum ExtraRow {
    kExtraSeed = 0,
    kExtraOreFix,
    kExtraSecret,
    kExtraPack,
    kExtraPanorama,
    kExtraBedrockFix,
    kExtraFencePlacement,
    kExtraBack,
    kExtraCount,
};
constexpr u8 kExtraRows[] = {kExtraSeed,       kExtraOreFix,         kExtraSecret,
                             kExtraPack,       kExtraPanorama,       kExtraBedrockFix,
                             kExtraFencePlacement, kExtraBack};
constexpr u8 kExtraGroups[] = {0, 0, 0, 1, 1, 2, 3, 4};

// **The same screen reached from Create World**, where three of the rows have
// nothing to act on: the seed and the secret roll are level.dat values and
// there is no level.dat yet -- the Create screen has its own rows for both --
// and the panorama is a picture taken of ground that has not been generated.
// What is left is the four choices that are about the world rather than about
// the file, in the order they were asked for.
constexpr u8 kExtraRowsNew[] = {kExtraPack, kExtraOreFix, kExtraBedrockFix,
                                kExtraFencePlacement, kExtraBack};
constexpr u8 kExtraGroupsNew[] = {0, 1, 1, 2, 3};

// **The Create World screen's rows.** What a world is called and what it is
// generated from first, then how it is played, then the three things that are
// decided once and never again, then the button that does it.
//
// The generation group is on this screen and not only on Extra Settings
// because this is the moment it costs nothing: a fix that changes what a chunk
// generates cannot touch a chunk that already exists, and at Create there are
// none. SnowCovered is there too because a1.1.2 itself rolls it exactly here.
//
// Format is offered rather than assumed for the same reason Extra Settings
// exists at all: Packed is right for the hardware and is the default, but a
// player who is going to carry the save to a PC should not have to make a
// world and then convert it.
enum CreateRow {
    kNewName = 0,
    kNewSeed,
    kNewGamemode,
    kNewDifficulty,
    kNewFormat,
    kNewSecret,
    kNewExtra,
    kNewCreate,
    kNewBack,
    kNewCount,
};
constexpr u8 kCreateGroups[kNewCount] = {0, 0, 1, 1, 2, 2, 3, 4, 5};

struct WorldSettingsLayout {
    const u8* rows;
    const u8* groups;
    int count;
};

WorldSettingsLayout worldSettingsLayout(bool inGame)
{
    if (inGame) {
        return {kWorldSettingsRowsInGame, kWorldSettingsGroupsInGame,
                int(sizeof(kWorldSettingsRowsInGame))};
    }
    return {kWorldSettingsRows, kWorldSettingsGroups, int(sizeof(kWorldSettingsRows))};
}

WorldSettingsLayout extraSettingsLayout(bool creating)
{
    if (creating) {
        return {kExtraRowsNew, kExtraGroupsNew, int(sizeof(kExtraRowsNew))};
    }
    return {kExtraRows, kExtraGroups, int(sizeof(kExtraRows))};
}

// **The two settings lists' geometry.** Buttons are 22 pixels with a 27-pixel
// pitch, so drawButton's two-pixel outline leaves a pixel of backdrop between
// neighbours, and a group starts 8 pixels further down than a row would. The
// window runs from under the heading to 234, where the last outline ends two
// pixels later and six above the bottom edge.
constexpr float kSettingsWidth = 240.0f;
constexpr float kSettingsValueX = 118.0f;
constexpr float kSettingsBottom = 234.0f;
constexpr float kOptionsTop = 40.0f;
constexpr float kWorldSettingsTop = 50.0f;
// The same as World Settings': both draw a world's name under the heading, so
// both lists start at the same place.
constexpr float kExtraSettingsTop = 50.0f;
// Ten rows and no world name under the heading, so this list starts higher and
// gets one more row on screen for it.
constexpr float kCreateWorldTop = 40.0f;

gui::ListGeometry settingsGeometry(float top)
{
    gui::ListGeometry geometry;
    geometry.rowHeight = 22;
    geometry.pitch = 27;
    geometry.groupGap = 8;
    geometry.viewHeight = int(kSettingsBottom - top);
    return geometry;
}

// **The bottom screen under a settings list**, 320 x 240 and painted rather
// than printed: the row's name along the top, a tooltip box under it, a message
// if there is one, and the controls along the bottom. Glyphs are 8 pixels on a
// 10-pixel line, so fifteen lines fill the box before it turns into pages.
//
// The box is the dark face and violet edge of a later version's item tooltip.
// a1.1.2 has no tooltips at all, so there is no original to copy, and that is
// the look a player recognises as one.
constexpr int kBottomWidth = hud::kScreenWidth;
constexpr int kHeadingY = 8;
constexpr int kTooltipX = 8;
constexpr int kTooltipY = 24;
constexpr int kTooltipWidth = kBottomWidth - 2 * kTooltipX;
constexpr int kTooltipPad = 6;
constexpr int kTooltipLineHeight = 10;
constexpr int kTooltipLinesPerPage = 15;
constexpr int kMessageY = 200;
constexpr int kControlsY = 226;
constexpr u32 kTooltipFace = 0x100010;
constexpr u32 kTooltipEdge = 0x2C0A5E;

__attribute__((format(printf, 2, 3)))
void appendf(std::string* out, const char* format, ...)
{
    char buffer[512];
    va_list args;
    va_start(args, format);
    const int written = std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (written > 0) {
        out->append(buffer, usize(written) < sizeof(buffer) ? usize(written) : sizeof(buffer) - 1);
    }
}

const char* optionsTitle(int row)
{
    switch (row) {
    case kOptDistance: return "Render Distance";
    case kOptSensitivity: return "Camera Sensitivity";
    case kOptPack:     return "Texture Pack";
    case kOptSkin:     return "Skin";
    case kOptAudio:    return "Audio";
    case kOptMusic:    return "Music";
    case kOptSound:    return "Sound";
    case kOptAutosave: return "Autosave";
    default:           return "Back";
    }
}

const char* worldSettingsTitle(int row)
{
    switch (row) {
    case kRowInfo:       return "World Info";
    case kRowGamemode:   return "Gamemode";
    case kRowDifficulty: return "Difficulty";
    case kRowFormat:     return "Format";
    case kRowCopy:       return "Copy";
    case kRowExport:     return "Export";
    case kRowDelete:     return "Delete";
    case kRowExtra:      return "Extra Settings";
    default:             return "Back";
    }
}

const char* createWorldTitle(int row)
{
    switch (row) {
    case kNewName:        return "Name";
    case kNewSeed:        return "Seed";
    case kNewGamemode:    return "Gamemode";
    case kNewDifficulty:  return "Difficulty";
    case kNewFormat:      return "Format";
    case kNewSecret:      return "Secret World";
    case kNewExtra:       return "Extra Settings";
    case kNewCreate:      return "Create World";
    default:              return "Back";
    }
}

const char* extraSettingsTitle(int row)
{
    switch (row) {
    case kExtraSeed:        return "Set Seed";
    case kExtraOreFix:      return "Fix Ore Generation Bug";
    case kExtraSecret:      return "Secret World";
    case kExtraPack:        return "World Texture Pack";
    case kExtraPanorama:    return "Move Panorama";
    case kExtraBedrockFix:  return "Fix Bedrock Hole Bug";
    case kExtraFencePlacement: return "Improved Fence Placement";
    default:                return "Back";
    }
}

// On and Off rather than Yes and No for the two fixes, because a fix is a
// switch and not an answer; Secret World is the other way round and is the one
// row that reads as a question.
const char* onOff(bool value)
{
    return value ? "On" : "Off";
}

const char* yesNo(bool value)
{
    return value ? "Yes" : "No";
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

    // A menu without its previews is still a menu: the three screens keep the
    // console if there is no memory for a second render target.
    preview_.reset(new MenuPreview());
    if (!preview_->init(isNew3DS)) {
        preview_.reset();
    }
    previewClockMs_ = 0;
    syncPreview();
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

    // After the park, for the same reason citro2d's program waits for it: the
    // previews own a program too. Their worker is joined in here, before a
    // world can be opened, and the bottom screen goes back to the console.
    if (preview_ != nullptr) {
        preview_->shutdown();
        preview_.reset();
    }

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
    // The same idea for the other thing that stages: a console switched off
    // part way through an import leaves a dot-directory that is not a world and
    // never will be. Cheap when there is nothing there, which is every boot but
    // one. See core/world/world_transfer.hpp.
    world::discardStagedImport(fs_, kSavesDir);

    world::listWorlds(fs_, kSavesDir, &worlds_);

    // **The cursor follows the world, not the row.** `listWorlds` sorts by last
    // played, so the world you just came out of is now the first one and
    // everything that was above it has moved down -- an index kept across that
    // is a different world. A name that is no longer on the card (it was
    // deleted, or renamed by a copy) falls back to the row, clamped.
    if (!worldCursorName_.empty()) {
        bool found = false;
        for (usize i = 0; i < worlds_.size(); ++i) {
            if (worlds_[i].name == worldCursorName_) {
                worldCursor_ = int(i) + kWorldRowFirst;
                found = true;
                break;
            }
        }
        if (!found) {
            worldCursorName_.clear();
        }
    }

    const int rows = rowCount(worlds_.size());
    if (worldCursor_ >= rows) {
        worldCursor_ = rows - 1;
    }
    if (worldCursor_ < 0) {
        worldCursor_ = 0;
    }
    // Both ends of the window, so a cursor that moved to a row below it is
    // scrolled to rather than merely clamped at the top.
    if (worldScroll_ > worldCursor_) {
        worldScroll_ = worldCursor_;
    }
    if (worldCursor_ >= worldScroll_ + kVisibleRows) {
        worldScroll_ = worldCursor_ - kVisibleRows + 1;
    }
    if (worldScroll_ < 0) {
        worldScroll_ = 0;
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
    if (packOverridden_) {
        // The world that has just been left was drawn with a pack of its own.
        // The menu is the console's again, so the live image is thrown away
        // and rebuilt below from `packName_` -- which applyWorldPack never
        // touched, precisely so this is a rebuild and not a guess.
        packOverridden_ = false;
        atlas_.rgba.clear();
    }
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

    // **After the atlas and not inside it.** `buildAtlas` fills the player's
    // page from the *active pack*, which is what Default means; a saved choice
    // of anything else is one extra file read on top, and only the file it
    // names. That is the whole reason the key carries a name rather than an
    // index -- applying it here must not cost a walk of the packs folder.
    applySavedSkin();

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
    // **The new pack brought its own `char.png` with it**, which is right for
    // Default and wrong for every other choice: a player who picked a skin
    // should keep it across a pack change. One file read, and nothing at all
    // when the choice is Default.
    applySavedSkin();
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
        // On the same pass and for the same reason: one more root file, read
        // when the pack changes and never in a frame.
        texture::buildParticleSheet(fs_, path, &particleSheet_);
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
    skinKey_ = saved.skin;
    autosaveSeconds_ = saved.autosaveSeconds;
    chunkCacheMB_ = saved.chunkCacheMB;

    // -1 is "not chosen yet"; the menu fills in a1.1.2's own default of 1.0F
    // rather than the settings file inventing a number. Anything else is
    // clamped, because the file can be hand-edited on a PC.
    audioEnabled_ = saved.audio != 0;
    musicVolume_ = saved.musicVolume < 0 ? 100 : (saved.musicVolume > 100 ? 100
                                                                         : saved.musicVolume);
    soundVolume_ = saved.soundVolume < 0 ? 100 : (saved.soundVolume > 100 ? 100
                                                                         : saved.soundVolume);
    // The same convention, and the same reason for clamping: a1.1.2's slider
    // sits at the middle, which here is the rate this port already had.
    lookSensitivity_ = saved.lookSensitivity < 0
                           ? settings::kDefaultSensitivity
                           : settings::clampSensitivity(saved.lookSensitivity);

    // The engine is handed the effect volume here rather than only when the
    // row moves, because this runs on every visit to the menu and the shell's
    // copy of the setting is the one that was read at boot. Music is not done
    // here: `setMusicVolume` stops a playing track when it reaches zero, and
    // re-applying the same value on every menu entry would be a stop nobody
    // asked for.
    if (sound_ != nullptr) {
        sound_->setSoundVolume(float(soundVolume_) / 100.0f);
    }
}

void Menu::saveSettings()
{
    settings::GameSettings current;
    current.renderDistance = renderDistance_;
    current.texturePack = packName_;
    current.skin = skinKey_;
    current.autosaveSeconds = autosaveSeconds_;
    current.chunkCacheMB = chunkCacheMB_;
    current.audio = audioEnabled_ ? 1 : 0;
    current.musicVolume = musicVolume_;
    current.soundVolume = soundVolume_;
    current.lookSensitivity = lookSensitivity_;

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
    // The measurement belongs to the screen that shows it. Leaving drops it
    // rather than leaving a thread walking the card while a world is opened,
    // which is the one thing on this menu that wants every FS round trip it
    // can get.
    // The Extra Settings subtree is part of this screen for the purpose of the
    // walk: a player who steps into it and back has not left the world, and
    // starting the stat-per-chunk-file walk again would be a second minute of
    // card traffic for a number that was already on screen.
    const auto inWorldSettings = [](Screen s) {
        return s == Screen::WorldSettings || s == Screen::ExtraSettings
               || s == Screen::WorldPack || s == Screen::MovePanorama;
    };
    if (inWorldSettings(screen_) && !inWorldSettings(screen)) {
        sizeScan_.cancel();
    }
    screen_ = screen;
    consoleDirty_ = true;
    infoPage_ = 0;
    syncPreview();
}

// The one thing a silent console most needs to be told, shortest first. The four
// causes -- no DSP firmware, a DSP something else holds, no resources folder, no
// click in it -- look identical from outside, so the line says which.
const char* Menu::soundProblem() const
{
    if (!audioEnabled_) {
        return "Audio is turned off.";
    }
    if (audioBackend_ == nullptr || sound_ == nullptr) {
        return "No audio in this build.";
    }
    switch (audioBackend_->status()) {
        case ctr::AudioStatus::NoFirmware:  return "No DSP firmware. Dump it with Rosalina.";
        case ctr::AudioStatus::Unavailable: return "The sound system could not start.";
        case ctr::AudioStatus::Disabled:    return "Audio is turned off.";
        case ctr::AudioStatus::Ready:       break;
    }
    if (sound_->resources().sounds.size() == 0 && sound_->resources().music.empty()) {
        return "No sound files on the SD card.";
    }
    if (sound_->loadedSamples() == 0) {
        return "The menu click sound is missing.";
    }
    return nullptr;
}

void Menu::paintSettingInfo(const char* title, RowKind kind)
{
    gui::Surface surface;
    if (!hud::bottomSurface(&surface)) {
        return;
    }

    if (fontImage_.empty() && consoleFont_.empty()) {
        const PrintConsole* console = consoleGetDefault();
        gui::fontFromBitmap(console->font.gfx, console->font.asciiOffset,
                            console->font.numChars, &consoleFont_);
    }
    const texture::FontImage& font = fontImage_.empty() ? consoleFont_ : fontImage_;

    // The same darkened dirt the top screen tiles, and the HUD in game.
    const bool haveTile = backgroundTile_.size() == texture::kBackgroundBytes
                          && texture::kBackgroundEdge == hud::kTileEdge;
    if (haveTile) {
        const usize texels = usize(hud::kTileEdge) * hud::kTileEdge;
        bottomTile_.resize(texels);
        for (usize i = 0; i < texels; ++i) {
            bottomTile_[i] = gui::rgb565(int(backgroundTile_[i * 4]),
                                         int(backgroundTile_[i * 4 + 1]),
                                         int(backgroundTile_[i * 4 + 2]));
        }
    }
    hud::drawBackdrop(surface, haveTile ? bottomTile_.data() : nullptr);

    const int lineWidth = kTooltipWidth - 2 * kTooltipPad;
    texture::wrapText(font.widths, infoBody_, lineWidth, &infoLines_);
    infoPages_ = gui::listPageCount(int(infoLines_.size()), kTooltipLinesPerPage);
    if (infoPage_ >= infoPages_) {
        infoPage_ = infoPages_ - 1;
    }
    if (infoPage_ < 0) {
        infoPage_ = 0;
    }
    const bool paged = infoPages_ > 1;

    const u32 white = 0xFFFFFF;
    const u32 yellow = texture::fontColour(14);
    const u32 grey = texture::fontColour(7);

    // The row's name, between the arrows the selected button also gets.
    gui::drawText(surface, (kBottomWidth - texture::textWidth(font.widths, title)) / 2,
                  kHeadingY, font, title, white, true);
    if (paged) {
        gui::drawText(surface, kTooltipX + 2, kHeadingY, font, "<", yellow, true);
        gui::drawText(surface,
                      kBottomWidth - kTooltipX - 2 - texture::textWidth(font.widths, ">"),
                      kHeadingY, font, ">", yellow, true);
    }

    const int first = infoPage_ * kTooltipLinesPerPage;
    const int left = int(infoLines_.size()) - first;
    const int shown = left < kTooltipLinesPerPage ? left : kTooltipLinesPerPage;
    if (shown > 0) {
        const int height = shown * kTooltipLineHeight - 2 + 2 * kTooltipPad;
        gui::fillRect(surface, kTooltipX, kTooltipY, kTooltipWidth, height,
                      gui::rgb565(kTooltipFace));
        gui::frameRect(surface, kTooltipX + 1, kTooltipY + 1, kTooltipWidth - 2, height - 2,
                       gui::rgb565(kTooltipEdge));
        for (int i = 0; i < shown; ++i) {
            gui::drawText(surface, kTooltipX + kTooltipPad,
                          kTooltipY + kTooltipPad + i * kTooltipLineHeight, font,
                          infoLines_[usize(first + i)], white, true);
        }
        if (paged) {
            char pageText[16];
            std::snprintf(pageText, sizeof(pageText), "%d/%d", infoPage_ + 1, infoPages_);
            gui::drawText(surface,
                          kTooltipX + kTooltipWidth - texture::textWidth(font.widths, pageText),
                          kTooltipY + height + 3, font, pageText, grey, true);
        }
    }

    if (message_ != nullptr) {
        std::vector<std::string> lines;
        texture::wrapText(font.widths, message_, kTooltipWidth, &lines);
        for (usize i = 0; i < lines.size() && i < 2; ++i) {
            gui::drawText(surface, kTooltipX, kMessageY + int(i) * kTooltipLineHeight, font,
                          lines[i], texture::fontColour(12), true);
        }
    }

    char controls[96];
    std::snprintf(controls, sizeof(controls), "%s%sB: Back%s",
                  kind == RowKind::Value ? "Left/Right: Change   " : "",
                  kind == RowKind::Action ? "A: Select   " : "",
                  paged ? (kind == RowKind::Value ? "   L/R: Page" : "   Left/Right: Page") : "");
    gui::drawText(surface, (kBottomWidth - texture::textWidth(font.widths, controls)) / 2,
                  kControlsY, font, controls, grey, true);

    // The CPU has just written a buffer the LCD reads by DMA.
    gfxFlushBuffers();
}

void Menu::buildOptionsInfo(int row)
{
    std::string& out = infoBody_;
    out.clear();

    switch (row) {
    case kOptDistance:
        appendf(&out, "How far you can see.\n");
        appendf(&out, "§7Higher values can lower the frame rate.\n");
        appendf(&out, "§7Up to %d on this console.", maxDistance_);
        break;
    case kOptPack:
        appendf(&out, "Changes how the game looks.\n");
        appendf(&out, "§7Current: §f%s\n", packLabel());
        appendf(&out, "§7Add packs to %s/", texture::kPacksDir);
        break;
    case kOptSkin:
        appendf(&out, "Changes your player skin.\n");
        appendf(&out, "§7Current: §f%s\n", skinLabel());
        appendf(&out, "§7Add skins to %s/", texture::kSkinsDir);
        break;
    case kOptSensitivity: {
        appendf(&out, "How fast the view turns, for both the\n");
        appendf(&out, "touch drag and the C-stick.\n");
        // **The gain, not just the slider.** a1.1.2's curve is a cube, so the
        // number on the row and what the camera actually does are two different
        // percentages -- 200% on the slider is four times the speed, not twice
        // -- and a player dialling this in wants the second one.
        appendf(&out, "§7100%% is a1.1.2's own default.\n");
        appendf(&out, "§7Turning at §f%d%%§7 of that speed.",
                int(settings::sensitivityGain(lookSensitivity_) * 100.0f + 0.5f));
        break;
    }
    case kOptAudio:
        appendf(&out, "Turns all game audio on or off.\n");
        appendf(&out, "§7Takes effect after a restart.");
        if (audioEnabled_) {
            if (const char* problem = soundProblem()) {
                appendf(&out, "\n§c%s", problem);
            }
        }
        break;
    case kOptMusic:
        appendf(&out, "Music volume.");
        if (const char* problem = soundProblem()) {
            appendf(&out, "\n§c%s", problem);
        } else if (sound_ != nullptr && sound_->resources().music.empty()) {
            appendf(&out, "\n§cNo music on the SD card.");
        }
        break;
    case kOptSound:
        appendf(&out, "Volume of blocks, footsteps and menus.\n");
        appendf(&out, "§7Press A to test.");
        if (const char* problem = soundProblem()) {
            appendf(&out, "\n§c%s", problem);
        }
        break;
    case kOptAutosave:
        appendf(&out, "How often your world is saved.\n");
        appendf(&out, "§7It is always saved when you pause or quit.");
        break;
    default:
        appendf(&out, "Return to the %s.", inGame_ ? "game menu" : "title screen");
        break;
    }
}

void Menu::buildWorldSettingsInfo(int row)
{
    std::string& out = infoBody_;
    out.clear();

    switch (row) {
    case kRowInfo: {
        appendf(&out, "§e%s\n", selectedWorldName_.c_str());
        if (selectedLevelKnown_) {
            appendf(&out, "§7Seed: §f%lld\n", (long long)selectedSeed_);
            if (!inGame_) {
                char when[64];
                formatWhen(selectedLastPlayed_, when, sizeof(when));
                appendf(&out, "§7Last played: §f%s\n", when);
            }
        }
        appendf(&out, "§7Format: §f%s\n", world::formatName(selectedFormat_));
        if (!inGame_) {
            if (selectedSizeKnown_) {
                char onDisk[24];
                formatBytes(selectedSize_.onDiskBytes, onDisk, sizeof(onDisk));
                appendf(&out, "§7Size: §f%s\n", onDisk);
            } else if (sizeScan_.running()) {
                // A folder world is a stat per chunk file, so the number
                // arrives a moment after the screen does. Saying so beats a
                // row that is missing and then is not.
                appendf(&out, "§7Size: §7loading...\n");
            }
        }
        appendf(&out, "§7Gamemode: §f%s\n", settings::gamemodeLabel(worldSettings_.gamemode));
        appendf(&out, "§7Difficulty: §f%s", settings::difficultyLabel(worldSettings_.difficulty));
        if (inGame_) {
            appendf(&out, "\n§7Copy, delete, format and the extra options\n");
            appendf(&out, "§7are on the world list.");
        }
        break;
    }
    case kRowGamemode:
        switch (kGamemodeOrder[gamemodeCursor_]) {
        case settings::Gamemode::Spectator:
            appendf(&out, "Fly through the world freely.\n");
            appendf(&out, "§7No collision, no building.");
            break;
        case settings::Gamemode::Creative:
            appendf(&out, "Build with every block.\n");
            appendf(&out, "§7Break and place blocks freely.");
            break;
        case settings::Gamemode::Survival:
            appendf(&out, "Mine, craft and stay alive.\n");
            appendf(&out, "§7Blocks take time to break and drop items.");
            break;
        }
        break;
    case kRowDifficulty:
        if (kDifficultyOrder[difficultyCursor_] == settings::Difficulty::Peaceful) {
            appendf(&out, "No monsters.");
        } else {
            appendf(&out, "Monsters spawn in the dark.\n");
            appendf(&out, "§7Harder settings make their hits hurt more.");
        }
        break;
    case kRowFormat:
        if (selectedFormat_ == world::WorldFormat::Packed) {
            appendf(&out, "Packed for this console.\n");
            appendf(&out, "§7Only 3DAlpha can open it.\n");
            appendf(&out, "§7Press A to unpack it.");
        } else {
            appendf(&out, "Standard Minecraft save.\n");
            appendf(&out, "§7Opens on a PC.\n");
            appendf(&out, "§7Press A to pack it for this console.");
        }
        break;
    case kRowExtra:
        appendf(&out, "Some Extra Options that might not be\n");
        appendf(&out, "fully vanilla or increase/decrease\n");
        appendf(&out, "parity with \"vanilla\".\n");
        appendf(&out, "§7Applies only to this World.");
        break;
    case kRowCopy:
        appendf(&out, "Makes a copy of this world.");
        break;
    case kRowExport:
        appendf(&out, "Sends a copy of this world to\n");
        appendf(&out, "another 3DS in the room.\n");
        appendf(&out, "§7This world is only read:\n");
        appendf(&out, "§7the copy is the one that moves.");
        break;
    case kRowDelete:
        appendf(&out, "Deletes this world.\n");
        appendf(&out, "§cThis cannot be undone.");
        break;
    default:
        appendf(&out, "Return to the %s.", inGame_ ? "game menu" : "world list");
        break;
    }
}

void Menu::printConsoleHelp()
{
    if (!consoleDirty_ && printedScreen_ == screen_) {
        return;
    }
    printedScreen_ = screen_;
    consoleDirty_ = false;

    // **The two settings screens paint the whole bottom screen** rather than
    // printing to it, so the console is neither cleared nor written here. The
    // next screen that prints clears the paint away with its own \x1b[2J.
    if (screen_ == Screen::Options) {
        buildOptionsInfo(optionsCursor_);
        const RowKind kind = optionsCursor_ == kOptPack || optionsCursor_ == kOptSkin ||
                                     optionsCursor_ == kOptBack
                                 ? RowKind::Action
                                 : RowKind::Value;
        paintSettingInfo(optionsTitle(optionsCursor_), kind);
        return;
    }
    if (screen_ == Screen::CreateWorld) {
        buildCreateWorldInfo(createCursor_);
        const RowKind kind = createCursor_ == kNewName || createCursor_ == kNewSeed ||
                                     createCursor_ == kNewExtra ||
                                     createCursor_ == kNewCreate || createCursor_ == kNewBack
                                 ? RowKind::Action
                                 : RowKind::Value;
        paintSettingInfo(createWorldTitle(createCursor_), kind);
        return;
    }
    if (screen_ == Screen::ExtraSettings) {
        const WorldSettingsLayout layout = extraSettingsLayout(extraForNewWorld_);
        const int row = layout.rows[extraCursor_ < layout.count ? extraCursor_ : 0];
        buildExtraSettingsInfo(row);
        const RowKind kind = row == kExtraSeed || row == kExtraPack ||
                                     row == kExtraPanorama || row == kExtraBack
                                 ? RowKind::Action
                                 : RowKind::Value;
        paintSettingInfo(extraSettingsTitle(row), kind);
        return;
    }
    if (screen_ == Screen::WorldSettings) {
        const WorldSettingsLayout layout = worldSettingsLayout(inGame_);
        const int row = layout.rows[worldSettingsCursor_ < layout.count ? worldSettingsCursor_
                                                                          : 0];
        buildWorldSettingsInfo(row);
        const RowKind kind = row == kRowInfo ? RowKind::Info
                             : row == kRowGamemode || row == kRowDifficulty ? RowKind::Value
                                                                             : RowKind::Action;
        paintSettingInfo(worldSettingsTitle(row), kind);
        return;
    }

    // **The three preview screens draw the bottom screen on the GPU** on the
    // main menu, and say what they need to under the picture.
    if (preview_ != nullptr && preview_->screen() != PreviewScreen::None) {
        return;
    }

    std::printf("\x1b[2J\x1b[1;1H");
    std::printf("\x1b[32m3DAlpha %s\x1b[0m\n\n", mcver::kDisplay);

    switch (screen_) {
    case Screen::Title:
        std::printf("Up/Down  choose\n");
        std::printf("A        select\n");
        std::printf("START    exit to the home menu\n");
        break;
    case Screen::Multiplayer:
        std::printf("Up/Down  move\n");
        std::printf("Left/Rt  Host or Join\n");
        std::printf("A        select\n");
        std::printf("X        edit or delete a server\n");
        std::printf("B        back\n\n");
        std::printf("Playing as \x1b[33m%s\x1b[0m, the name\n", username_.c_str());
        std::printf("in this console's friend list.\n\n");
        if (scanned_) {
            std::printf("\x1b[33m%d\x1b[0m session(s) found nearby.\n", int(sessions_.size()));
            std::printf("Press Join again to look again.\n\n");
        } else {
            std::printf("Host opens one of your worlds\n");
            std::printf("to another 3DS in the room;\n");
            std::printf("Join looks for one.\n\n");
        }
        std::printf("Servers below the line run\n");
        std::printf("a1.1.2's protocol (server 0.2.1)\n");
        std::printf("with online-mode=false. The list\n");
        std::printf("is kept at:\n");
        std::printf("  \x1b[33m%s\x1b[0m\n", net::kServerListPath);
        break;
    case Screen::Session:
        std::printf("B        leave the session\n\n");
        std::printf("The two consoles are talking:\n");
        std::printf("this screen is the handshake and\n");
        std::printf("the player list.\n\n");
        std::printf("The world itself does not cross\n");
        std::printf("the link yet -- that is the next\n");
        std::printf("piece of work.\n");
        break;
    case Screen::NetMode:
        std::printf("Up/Down  choose\n");
        std::printf("A        select\n");
        std::printf("B        back\n\n");
        std::printf("\x1b[33mLocal\x1b[0m is the 3DS's own wireless:\n");
        std::printf("the other console has to be in\n");
        std::printf("the same room, and neither needs\n");
        std::printf("a network or the internet.\n\n");
        std::printf("Turn the wireless switch on\n");
        std::printf("before either console tries.\n");
        break;
    case Screen::ImportScan:
        std::printf("Up/Down  choose\n");
        std::printf("A        take that world\n");
        std::printf("X        look again\n");
        std::printf("B        back\n\n");
        std::printf("On the other console:\n");
        std::printf("  world list, \x1b[33mX\x1b[0m on a world,\n");
        std::printf("  then \x1b[33mExport\x1b[0m.\n\n");
        std::printf("Both consoles need the wireless\n");
        std::printf("switch on and have to be in the\n");
        std::printf("same room.\n");
        break;
    case Screen::Transfer:
        std::printf("B        stop\n");
        std::printf("A        close when it is done\n\n");
        std::printf("The world is copied file for\n");
        std::printf("file, in whichever format it is\n");
        std::printf("already in. A transfer is a\n");
        std::printf("backup that travelled, not a\n");
        std::printf("conversion.\n\n");
        std::printf("\x1b[33mThe original is only read.\x1b[0m\n");
        std::printf("Nothing is written to the world\n");
        std::printf("being sent, and a transfer that\n");
        std::printf("stops half way leaves nothing\n");
        std::printf("behind on either console.\n");
        break;
    case Screen::EditServer:
        std::printf("Up/Down  choose\n");
        std::printf("A        change or select\n");
        std::printf("B        back without saving\n\n");
        std::printf("An address is a host name or an\n");
        std::printf("IP. Port is %u unless the server\n", unsigned(net::kDefaultPort));
        std::printf("was moved off it.\n\n");
        std::printf("A host:port typed into Address\n");
        std::printf("fills both rows.\n");
        break;
    case Screen::ConfirmDeleteServer:
        std::printf("A        delete it from the list\n");
        std::printf("B        keep it\n");
        break;
    case Screen::Disconnected:
        std::printf("A/B      back to the server list\n");
        break;
    case Screen::Pause:
        std::printf("Up/Down  choose\n");
        std::printf("A        select\n");
        std::printf("B/START  back to the world\n\n");
        if (multiplayer_) {
            std::printf("The server's world goes on\n");
            std::printf("while this is up.\n\n");
            std::printf("Disconnect leaves the server;\n");
            std::printf("the world stays there.\n");
            break;
        }
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
    case Screen::ExtraSettings:
    case Screen::CreateWorld:
        break;  // painted above, not printed
    case Screen::WorldPack:
        std::printf("Up/Down  choose a pack\n");
        std::printf("A        use it for this world\n");
        std::printf("B        back\n\n");
        std::printf("\x1b[33mDefault\x1b[0m is not a pack: it means this\n");
        std::printf("world follows whatever the console is\n");
        std::printf("set to on the Options screen, which is\n");
        std::printf("what every world does until it is told\n");
        std::printf("otherwise.\n\n");
        std::printf("The choice is written into this world's\n");
        std::printf("3dalpha.ini and applies to nothing else.\n");
        break;
    case Screen::MovePanorama:
        break;  // the diorama has the bottom screen
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
    case Screen::Skins:
        std::printf("Up/Down     choose a skin\n");
        std::printf("A           use it\n");
        std::printf("B           back\n\n");
        std::printf("The arm you see holding nothing is\n");
        std::printf("the only thing a1.1.2 draws a player\n");
        std::printf("skin on, so this is what it changes.\n\n");
        std::printf("\x1b[33mDefault\x1b[0m is the texture pack's own\n");
        std::printf("char.png, or a black silhouette when\n");
        std::printf("the pack has none.\n\n");
        std::printf("Drop skins as .png in:\n");
        std::printf("  \x1b[33m%s/\x1b[0m\n\n", texture::kSkinsDir);
        std::printf("64x32 and 64x64 are both read; the\n");
        std::printf("lower half of a 64x64 is a later\n");
        std::printf("version's and is not drawn. A skin\n");
        std::printf("made for the \x1b[33mslim\x1b[0m body is marked as\n");
        std::printf("such and still drawn on the wide arm\n");
        std::printf("-- the narrow one is 1.8's, not this\n");
        std::printf("version's.\n");
        break;
    case Screen::Options:
        break;  // painted above, not printed
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
        // **Between the frame that said "Searching" and the search.** The scan
        // holds the radio for about a second, so it is asked for on one frame
        // and done at the top of the next, with the message already drawn.
        if (scanPending_) {
            scanPending_ = false;
            refreshLocalSessions();
        }
        if (offerScanPending_) {
            offerScanPending_ = false;
            refreshWorldOffers();
        }
        pumpSession();
        pumpTransfer();

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
        case Screen::Skins:
            handleSkins(down);
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
        case Screen::CreateWorld:
            done = handleCreateWorld(down, &choice);
            break;
        case Screen::ExtraSettings:
            handleExtraSettings(down);
            break;
        case Screen::WorldPack:
            handleWorldPack(down);
            break;
        case Screen::MovePanorama:
            handleMovePanorama(down);
            break;
        case Screen::ConfirmConvert:
            handleConfirmConvert(down);
            break;
        case Screen::Multiplayer:
            done = handleMultiplayer(down, &choice);
            break;
        case Screen::NetMode:
            done = handleNetMode(down, &choice);
            break;
        case Screen::Session:
            done = handleSession(down, &choice);
            break;
        case Screen::ImportScan:
            handleImportScan(down);
            break;
        case Screen::Transfer:
            handleTransfer(down);
            break;
        case Screen::EditServer:
            handleEditServer(down);
            break;
        case Screen::ConfirmDeleteServer:
            handleConfirmDeleteServer(down);
            break;
        case Screen::Disconnected:
            handleDisconnected(down);
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
            choice.lookSensitivity = lookSensitivity_;
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
    choice.lookSensitivity = lookSensitivity_;
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

    beginPause(worldName, worldPath, renderDistance);

    while (aptMainLoop()) {
        hidScanInput();
        if (stepPause(hidKeysDown())) {
            break;
        }
        drawFrame();
    }

    return endPause();
}

// **The three pieces `runPause` is made of**, so that a caller which cannot
// give up its frame loop -- a multiplayer session, where stopping means
// stopping for everybody else too -- runs the same menu a frame at a time.
void Menu::beginPause(const char* worldName, const char* worldPath, int renderDistance)
{
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
    // A server's world has no settings file on this card to read.
    if (!multiplayer_) {
        openWorldSettings(pauseWorldName_, worldPath != nullptr ? worldPath : "");
    }
    pauseCursor_ = 0;
    resumeScreen_ = screen_;
    setScreen(Screen::Pause);

    pauseRevisionAtEntry_ = packRevision_;

    // Exit, not Resume, if no step ever answers: the only way past aptMainLoop
    // is the system taking the application away, and the caller's answer to
    // that has to be to close the world rather than to carry on playing a
    // frame at a time into a shutdown.
    pauseChoice_ = PauseChoice{};
    pauseChoice_.action = PauseChoice::Action::ExitWorld;
}

bool Menu::stepPause(u32 down)
{
    // **Between the frame that said "Searching" and the search.** The scan
    // holds the radio for about a second, so it is asked for on one frame
    // and done at the top of the next, with the message already drawn.
    if (scanPending_) {
        scanPending_ = false;
        refreshLocalSessions();
    }
    pumpSession();

    bool done = false;
    switch (screen_) {
    case Screen::Pause:
        done = handlePause(down, &pauseChoice_);
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
    case Screen::Skins:
        handleSkins(down);
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
        return true;
    }

    presentState();
    return false;
}

PauseChoice Menu::endPause()
{
    inGame_ = false;
    backdrop_ = PauseBackdrop{};
    pauseWorldName_ = "";
    // What the caller applies to the world it still has open. The file was
    // written the moment the row changed; this is the copy in memory.
    pauseChoice_.gamemode = worldSettings_.gamemode;
    pauseChoice_.difficulty = worldSettings_.difficulty;
    pauseChoice_.improvedFencePlacement = worldSettings_.improvedFencePlacement;
    // Back to where the main menu was standing when this world was opened, so
    // Exit World returns to the world list rather than to the pause menu.
    setScreen(resumeScreen_);

    pauseChoice_.renderDistance = renderDistance_;
    pauseChoice_.autosaveSeconds = autosaveSeconds_;
    pauseChoice_.lookSensitivity = lookSensitivity_;
    pauseChoice_.atlasChanged = packRevision_ != pauseRevisionAtEntry_;
    return pauseChoice_;
}

void Menu::present()
{
    presentState();
    drawFrame();
}

void Menu::presentState()
{
    pollWorldSize();
    updatePreview();
    printConsoleHelp();
}

namespace {

// The circle pad reports through the same button mask as the d-pad, so both
// work everywhere without a second code path.
constexpr u32 kUp = KEY_DUP | KEY_CPAD_UP;
constexpr u32 kDown = KEY_DDOWN | KEY_CPAD_DOWN;
constexpr u32 kLeft = KEY_DLEFT | KEY_CPAD_LEFT;
constexpr u32 kRight = KEY_DRIGHT | KEY_CPAD_RIGHT;

int stepCursor(u32 down, int cursor, int count)
{
    if (down & kUp) {
        cursor = cursor == 0 ? count - 1 : cursor - 1;
    }
    if (down & kDown) {
        cursor = cursor + 1 >= count ? 0 : cursor + 1;
    }
    return cursor;
}

// `random/click.ogg` under `sound/` or `newsound/` -- both feed one pool -- keyed
// the way `eb.a(String, File)` keys it. The one sound this port can currently
// make, and the only one a menu needs. In a real resources folder it is usually
// the `newsound/` copy that exists.
constexpr char kClickSound[] = "random.click";

}  // namespace

void Menu::playClick()
{
    if (sound_ != nullptr) {
        sound_->playSoundFX(kClickSound, 1.0f, 1.0f);
    }
}

void Menu::playMoveClick()
{
    if (sound_ != nullptr) {
        sound_->playSoundFX(kClickSound, 0.3f, 0.5f);
    }
}

int Menu::step(u32 down, int cursor, int count)
{
    const int moved = stepCursor(down, cursor, count);
    // Only a cursor that actually went somewhere. A one-row list and a press
    // that wrapped onto itself are the same key with nothing to show for it,
    // and clicking for them would be the menu talking back about nothing.
    if (moved != cursor) {
        playMoveClick();
    }
    return moved;
}

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

    // a1.1.2 clicks for a button that is *enabled* -- `bh.a` plays the sound
    // only once `fk.c` has said the press landed -- and every row here is.
    playClick();

    switch (titleCursor_) {
    case 0:
        message_ = nullptr;
        refreshWorlds();
        setScreen(Screen::Worlds);
        break;
    case 1:
        message_ = nullptr;
        pickingHost_ = false;
        refreshServers();
        setScreen(Screen::Multiplayer);
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
    playClick();

    // Multiplayer's second row is Chat where single player's is World
    // Settings; the other three are the same rows in the same places. Sending
    // a line returns to the world, as a1.1.2's chat closes onto it.
    if (multiplayer_ && pauseCursor_ == 1) {
        std::string text;
        if (askServerText("Chat", std::string(), kMaxChatChars, &text) && !text.empty()) {
            choice->chat = text;
            choice->action = PauseChoice::Action::Resume;
            return true;
        }
        return false;
    }

    switch (pauseCursor_) {
    case 0:
        choice->action = PauseChoice::Action::Resume;
        return true;
    case 1:
        message_ = nullptr;
        worldSettingsCursor_ = 0;
        worldSettingsScroll_ = 0;
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

    // Remembered by name on every move, so the list can be re-read and
    // re-sorted underneath it -- which is what playing a world does.
    worldCursorName_ = worldCursor_ >= kWorldRowFirst
                               && usize(worldCursor_ - kWorldRowFirst) < worlds_.size()
                           ? worlds_[usize(worldCursor_ - kWorldRowFirst)].name
                           : std::string();

    if (down & KEY_B) {
        message_ = nullptr;
        // Back to wherever this list was opened from: the title screen, or the
        // multiplayer screen when it is being used to pick a world to host.
        const bool hosting = pickingHost_;
        pickingHost_ = false;
        setScreen(hosting ? Screen::Multiplayer : Screen::Title);
        return false;
    }

    // X is the world's own screen rather than a delete confirmation. Delete is
    // still one press further in, behind the same confirmation it always had;
    // what changed is that X now also reaches size, format and copy, which had
    // nowhere to live when it went straight to a yes/no.
    if ((down & KEY_X) != 0 && worldCursor_ >= kWorldRowFirst
        && usize(worldCursor_ - kWorldRowFirst) < worlds_.size()) {
        const world::WorldEntry& entry = worlds_[usize(worldCursor_ - kWorldRowFirst)];
        message_ = nullptr;
        worldSettingsCursor_ = 0;
        worldSettingsScroll_ = 0;
        playClick();
        openWorldSettings(entry.name, entry.path);
        setScreen(Screen::WorldSettings);
        return false;
    }

    if ((down & KEY_A) == 0) {
        return false;
    }
    playClick();

    if (worldCursor_ == kWorldRowCreate || worldCursor_ == kWorldRowImport) {
        if (pickingHost_) {
            // Getting a world is a thing to do on the way into single player,
            // not on the way into a session: the others are waiting, and the
            // world they were told about is one that exists.
            message_ = worldCursor_ == kWorldRowCreate
                           ? "Make the world first, then host it."
                           : "Import the world first, then host it.";
            consoleDirty_ = true;
            return false;
        }
        if (worldCursor_ == kWorldRowCreate) {
            // **A screen, not two keyboards.** This used to open askWorldName
            // and then askSeed back to back, with no way to see the first
            // answer again and nothing else asked at all.
            openCreateWorld();
            setScreen(Screen::CreateWorld);
        } else {
            openImport();
        }
        return false;
    }

    const world::WorldEntry& entry = worlds_[usize(worldCursor_ - kWorldRowFirst)];
    // **The same world, opened for company.** Hosting is not a different way
    // of playing a world -- the host plays it exactly as they would alone --
    // so everything below this line is the single-player path, and the only
    // difference is the action the caller is handed.
    choice->action = pickingHost_ ? MenuChoice::Action::Host : MenuChoice::Action::Play;
    choice->link = pickingHost_ ? MenuChoice::Link::Local : MenuChoice::Link::Internet;
    choice->username = username_;
    pickingHost_ = false;
    choice->worldPath = entry.path;
    choice->worldName = entry.name;

    // Read here rather than left to the caller: a per-world setting has to
    // start from the world it is about, and this is the one place a world is
    // chosen. Reading it fresh is also what stops the last world's gamemode
    // leaking into the next one.
    settings::WorldSettings worldSettings;
    settings::loadWorldSettings(fs_, entry.path, &worldSettings);
    choice->gamemode = worldSettings.gamemode;
    choice->difficulty = worldSettings.difficulty;

    // **Its texture pack too, and this is the last moment for it**: the atlas
    // the caller is handed is `atlas_`, built a moment later, and a world that
    // names a pack of its own wants that one rather than the console's. The
    // member is what applyWorldPack reads.
    worldSettings_ = worldSettings;
    applyWorldPack();
    return true;
}

void Menu::turnInfoPage(u32 down, bool arrowsTurn)
{
    if (infoPages_ <= 1) {
        return;
    }
    const u32 back = KEY_L | (arrowsTurn ? kLeft : 0u);
    const u32 forward = KEY_R | (arrowsTurn ? kRight : 0u);
    int page = infoPage_;
    if ((down & back) != 0) {
        page = page == 0 ? infoPages_ - 1 : page - 1;
    }
    if ((down & forward) != 0) {
        page = page + 1 >= infoPages_ ? 0 : page + 1;
    }
    if (page != infoPage_) {
        infoPage_ = page;
        consoleDirty_ = true;
        playMoveClick();
    }
}

// Volumes apply live, as `of.a()` does in the original: moving Music to OFF
// stops the track that is playing rather than leaving it running silently, and
// the engine is told on every change rather than on the way out. The Audio
// row is the exception -- ndsp is a process-scoped service and bringing it up
// or down under a playing track is not worth the complication, so it takes
// effect on the next launch and its explanation says so.
void Menu::handleOptions(u32 down)
{
    const int before = optionsCursor_;
    optionsCursor_ = step(down, optionsCursor_, kOptCount);
    optionsScroll_ = gui::listScrollFor(kOptionsGroups, kOptCount, optionsCursor_,
                                        optionsScroll_, settingsGeometry(kOptionsTop));
    if (optionsCursor_ != before) {
        infoPage_ = 0;
    }
    // Any press can change what the explanation says -- a value, a row, the
    // audio state -- and reprinting forty columns is cheaper than knowing which.
    if (down != 0) {
        consoleDirty_ = true;
    }

    // Ten points a press, which is one press per audible step and ten presses
    // end to end. The original's slider is continuous; a d-pad is not.
    constexpr int kVolumeStep = 10;

    switch (optionsCursor_) {
    case kOptDistance: {
        const int previous = renderDistance_;
        if ((down & kLeft) != 0 && renderDistance_ > 2) {
            --renderDistance_;
        }
        if ((down & kRight) != 0 && renderDistance_ < maxDistance_) {
            ++renderDistance_;
        }
        if (renderDistance_ != previous) {
            playClick();
            saveSettings();
        }
        break;
    }
    case kOptAutosave: {
        const int previous = autosaveSeconds_;
        int index = autosaveIndex(autosaveSeconds_);
        if ((down & kLeft) != 0 && index > 0) {
            --index;
        }
        if ((down & kRight) != 0 && index < kAutosaveStepCount - 1) {
            ++index;
        }
        autosaveSeconds_ = kAutosaveSteps[index];
        if (autosaveSeconds_ != previous) {
            playClick();
            saveSettings();
        }
        break;
    }
    case kOptSensitivity: {
        const int previous = lookSensitivity_;
        if ((down & kLeft) != 0) {
            lookSensitivity_ = settings::clampSensitivity(lookSensitivity_
                                                          - settings::kSensitivityStep);
        }
        if ((down & kRight) != 0) {
            lookSensitivity_ = settings::clampSensitivity(lookSensitivity_
                                                          + settings::kSensitivityStep);
        }
        if (lookSensitivity_ != previous) {
            playClick();
            saveSettings();
        }
        break;
    }
    case kOptAudio:
        if ((down & (kLeft | kRight | KEY_A)) != 0) {
            audioEnabled_ = !audioEnabled_;
            // The click still plays when the row is switched to OFF, and that
            // is right rather than sloppy: the toggle takes effect on the next
            // launch, so ndsp is still up and a silent press here would say the
            // setting had already bitten when it has not.
            playClick();
            saveSettings();
        }
        break;
    case kOptMusic: {
        const int previous = musicVolume_;
        if ((down & kLeft) != 0) {
            musicVolume_ = musicVolume_ - kVolumeStep < 0 ? 0 : musicVolume_ - kVolumeStep;
        }
        if ((down & kRight) != 0) {
            musicVolume_ = musicVolume_ + kVolumeStep > 100 ? 100 : musicVolume_ + kVolumeStep;
        }
        if (musicVolume_ != previous) {
            if (sound_ != nullptr) {
                sound_->setMusicVolume(float(musicVolume_) / 100.0f);
            }
            playClick();
            saveSettings();
        }
        break;
    }
    case kOptSound: {
        // A on the Sound row is a test press, and it is a1.1.2's behaviour
        // rather than an addition: `fu` (GuiSlider) is a `fk` (GuiButton), so
        // clicking a slider in the original plays the click even when the value
        // does not move. Here it is also the one way to answer "is anything
        // wrong with my card" without waiting twenty minutes for a music track.
        if ((down & KEY_A) != 0) {
            playClick();
        }
        const int previous = soundVolume_;
        if ((down & kLeft) != 0) {
            soundVolume_ = soundVolume_ - kVolumeStep < 0 ? 0 : soundVolume_ - kVolumeStep;
        }
        if ((down & kRight) != 0) {
            soundVolume_ = soundVolume_ + kVolumeStep > 100 ? 100 : soundVolume_ + kVolumeStep;
        }
        if (soundVolume_ != previous) {
            // **In that order.** The engine is told first so the click below is
            // played at the volume the row now shows, which is the only way a
            // player can hear what they are setting -- the slider is otherwise
            // a number with nothing attached to it. a1.1.2 gets this for free:
            // its slider is clicked with a mouse, and `bh.a` plays the click
            // after `fr.a` has already written the new value.
            if (sound_ != nullptr) {
                sound_->setSoundVolume(float(soundVolume_) / 100.0f);
            }
            playClick();
            saveSettings();
        }
        break;
    }
    case kOptPack:
        if ((down & KEY_A) != 0) {
            message_ = nullptr;
            playClick();
            refreshPacks();
            setScreen(Screen::TexturePacks);
            return;
        }
        break;
    case kOptSkin:
        if ((down & KEY_A) != 0) {
            message_ = nullptr;
            playClick();
            refreshSkins();
            setScreen(Screen::Skins);
            return;
        }
        break;
    default:
        break;
    }

    // Only on a press that did not also move the cursor: the page count is the
    // row the cursor has just left.
    if (optionsCursor_ == before) {
        const bool valueless =
            optionsCursor_ == kOptPack || optionsCursor_ == kOptSkin || optionsCursor_ == kOptBack;
        turnInfoPage(down, valueless);
    }

    if ((down & KEY_B) != 0 || ((down & KEY_A) != 0 && optionsCursor_ == kOptBack)) {
        if ((down & KEY_A) != 0) {
            playClick();  // the Back button. B is Escape, and Escape is silent.
        }
        setScreen(inGame_ ? Screen::Pause : Screen::Title);
    }
}

// Everything the screen needs off the card, taken once on the way in.
//
// **Nothing here happens in a draw, and the long one does not happen here
// either.** Reading a settings file is one open; measuring a folder world is a
// stat per chunk file across up to 4,096 leaf directories, so that one is
// started on a thread and picked up in present().
void Menu::openWorldSettings(const std::string& name, const std::string& path)
{
    selectedWorldName_ = name;
    selectedWorldPath_ = path;
    selectedSize_ = world::WorldSize();
    selectedSizeKnown_ = false;

    // The list has already read this world's level.dat, and in game it is the
    // list the world was opened from. Not found -- a world made a moment ago
    // and not listed since -- leaves the two lines off World Info.
    selectedLevelKnown_ = false;
    for (const world::WorldEntry& entry : worlds_) {
        if (entry.path == path) {
            selectedSeed_ = entry.seed;
            selectedLastPlayed_ = entry.lastPlayed;
            selectedLevelKnown_ = true;
            break;
        }
    }

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
    difficultyCursor_ = difficultyIndex(worldSettings_.difficulty);

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

    // **Nothing is waited for here.** This used to be the walk itself, with a
    // frame drawn first saying what was happening, and on a big folder world
    // that was a menu that stopped for seconds. Now the thread is started and
    // the screen carries on; the size row says "loading..." until it answers.
    selectedSize_ = world::WorldSize();
    selectedSizeKnown_ = false;
    sizeScan_.start(fs_, selectedWorldPath_);
    consoleDirty_ = true;
}

void Menu::pollWorldSize()
{
    if (selectedSizeKnown_) {
        return;
    }
    world::WorldSize size;
    if (!sizeScan_.result(&size)) {
        // Still walking, or it failed -- and a walk that failed leaves the row
        // off rather than showing a total nothing stands behind.
        return;
    }
    selectedSize_ = size;
    selectedSizeKnown_ = true;
    // The bottom screen is painted from infoBody_, which is only rebuilt when
    // this is set, so without it the row would say "loading..." until the
    // cursor moved.
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
    const WorldSettingsLayout layout = worldSettingsLayout(inGame_);
    if (worldSettingsCursor_ >= layout.count) {
        worldSettingsCursor_ = 0;
    }
    const int before = worldSettingsCursor_;
    worldSettingsCursor_ = step(down, worldSettingsCursor_, layout.count);
    worldSettingsScroll_ =
        gui::listScrollFor(layout.groups, layout.count, worldSettingsCursor_,
                           worldSettingsScroll_, settingsGeometry(kWorldSettingsTop));
    if (worldSettingsCursor_ != before) {
        infoPage_ = 0;
    }
    if (down != 0) {
        consoleDirty_ = true;
    }
    const int row = layout.rows[worldSettingsCursor_];
    const bool moved = worldSettingsCursor_ != before;

    if (row == kRowGamemode) {
        const int previous = gamemodeCursor_;
        if ((down & kLeft) != 0 && gamemodeCursor_ > 0) {
            --gamemodeCursor_;
        }
        if ((down & kRight) != 0 && gamemodeCursor_ < kGamemodeCount - 1) {
            ++gamemodeCursor_;
        }
        // **The row moves onto a disabled mode; the world does not.** It is the
        // value that is refused, not the movement -- a player can put the row
        // on Survival, see it greyed out and read why on the bottom screen,
        // which is the whole reason the unimplemented mode is listed rather
        // than hidden. A dead arrow key would say nothing at all.
        if (gamemodeCursor_ != previous
            && settings::gamemodeImplemented(kGamemodeOrder[gamemodeCursor_])) {
            worldSettings_.gamemode = kGamemodeOrder[gamemodeCursor_];
            playClick();
            saveWorldSettings();
        }
    }

    // **Every difficulty is implemented**, unlike gamemode, so there is no
    // greyed-out value here: Peaceful removes monsters, the other three change
    // what a hit costs and whether a big slime may spawn.
    if (row == kRowDifficulty) {
        const int previous = difficultyCursor_;
        if ((down & kLeft) != 0 && difficultyCursor_ > 0) {
            --difficultyCursor_;
        }
        if ((down & kRight) != 0 && difficultyCursor_ < kDifficultyCount - 1) {
            ++difficultyCursor_;
        }
        if (difficultyCursor_ != previous) {
            worldSettings_.difficulty = kDifficultyOrder[difficultyCursor_];
            playClick();
            saveWorldSettings();
        }
    }

    if (!moved && ((down & KEY_A) != 0 || (down & (kLeft | kRight)) != 0)) {
        switch (row) {
        case kRowFormat:
            // Left, Right and A all mean the same thing here, because there
            // are exactly two formats: whichever one this world is not.
            playClick();
            beginConvert();
            return;
        case kRowCopy:
            if ((down & KEY_A) != 0) {
                playClick();
                copySelectedWorld();
                return;
            }
            break;
        case kRowExport:
            if ((down & KEY_A) != 0) {
                playClick();
                message_ = nullptr;
                netPurpose_ = NetPurpose::Export;
                netModeCursor_ = 0;
                setScreen(Screen::NetMode);
                return;
            }
            break;
        case kRowDelete:
            if ((down & KEY_A) != 0) {
                playClick();
                setScreen(Screen::ConfirmDelete);
                return;
            }
            break;
        case kRowExtra:
            if ((down & KEY_A) != 0) {
                playClick();
                // level.dat is read here rather than on every frame of the
                // screen: it is a compressed NBT file, and the two rows that
                // show what is in it want one number each.
                extraForNewWorld_ = false;
                openExtraSettings();
                extraCursor_ = 0;
                extraScroll_ = 0;
                setScreen(Screen::ExtraSettings);
                return;
            }
            break;
        default:
            break;
        }
    }

    if (!moved) {
        // World Info's A turns forward as well: it is the only thing the row
        // can be pressed for.
        const u32 turn = row == kRowInfo && (down & KEY_A) != 0 ? down | KEY_R : down;
        const bool valueless = row == kRowInfo || row == kRowCopy || row == kRowExport ||
                               row == kRowDelete || row == kRowExtra || row == kRowBack;
        turnInfoPage(turn, valueless);
    }

    if ((down & KEY_B) != 0 || ((down & KEY_A) != 0 && row == kRowBack)) {
        if ((down & KEY_A) != 0) {
            playClick();
        }
        setScreen(inGame_ ? Screen::Pause : Screen::Worlds);
    }
}

// ---------------------------------------------------------------------------
// Extra Settings
//
// **The one screen on this menu that is deliberately not a1.1.2.** Everything
// else here either reproduces a screen the original had or exists because a
// console needs it; these six rows either undo a bug the original shipped or
// offer a choice it never had, and each of them belongs to one world rather
// than to the console. Two are level.dat values, three are `3dalpha.ini` ones
// and one opens a list; the split is invisible from the screen and is entirely
// about which file a real Minecraft client would also read.
// ---------------------------------------------------------------------------

void Menu::openExtraSettings()
{
    extraLevelKnown_ = false;
    extraSeed_ = 0;
    extraSnowCovered_ = false;
    if (selectedWorldPath_.empty()) {
        return;
    }

    // **readLevel, not open and not peek.** Opening a world writes a lock and
    // a fresh lastPlayed into it, and merely looking at a value must not touch
    // the card. Peek is the other trap and is the one this screen fell into:
    // on a packed world it answers out of the manifest's metadata block, which
    // carries the seed and nothing else, so `SnowCovered` came back at its
    // default and every world -- including every winter world -- read as No.
    // Every world this port makes is packed.
    world::AnyStorage storage(fs_);
    world::LevelData level;
    if (!storage.readLevel(selectedWorldPath_, &level)) {
        return;
    }
    extraLevelKnown_ = true;
    extraSeed_ = level.randomSeed;
    extraSnowCovered_ = level.snowCovered;

    panoramaTileX_ = worldSettings_.panoramaTileX;
    panoramaTileZ_ = worldSettings_.panoramaTileZ;
}

void Menu::openExtraSettingsForNewWorld()
{
    // **Nothing is read and nothing is written.** The three rows that would
    // need a level.dat are not on this screen's Create World layout, so there
    // is no file to open -- which is the point: at Create the world is still a
    // `NewWorld` in memory and stays one until the Create row is pressed.
    extraForNewWorld_ = true;
    extraLevelKnown_ = false;
    extraCursor_ = 0;
    extraScroll_ = 0;
    infoPage_ = 0;
    message_ = nullptr;
}

settings::WorldSettings& Menu::extraSettings()
{
    return extraForNewWorld_ ? newWorld_.settings : worldSettings_;
}

const settings::WorldSettings& Menu::extraSettings() const
{
    return extraForNewWorld_ ? newWorld_.settings : worldSettings_;
}

void Menu::saveExtraSettings()
{
    // A world that does not exist has nothing to write to; `createWorld`
    // writes these settings out with the rest of it.
    if (!extraForNewWorld_) {
        saveWorldSettings();
    }
}

bool Menu::saveExtraLevel()
{
    if (selectedWorldPath_.empty() || !extraLevelKnown_) {
        message_ = "this world's level.dat could not be read";
        consoleDirty_ = true;
        return false;
    }

    // The diorama may be reading this very world, and the size walk is in the
    // same position -- the same pair `beginConvert` quiesces, and for the same
    // reason: what follows claims the world.
    if (preview_ != nullptr) {
        preview_->quiesce(selectedWorldPath_);
    }
    sizeScan_.cancel();

    const i64 now = nowMillis();
    world::AnyStorage storage(fs_);
    if (storage.open(selectedWorldPath_, now) != world::OpenResult::Ok) {
        message_ = "could not open this world";
        consoleDirty_ = true;
        return false;
    }

    // **Read-modify-write, not write.** `open` has just decoded the whole of
    // level.dat, unmodelled tags included, and saving it back is what carries
    // them across -- the same promise core/nbt/preserved.hpp makes everywhere
    // else. Only the two fields this screen owns are touched.
    storage.level().randomSeed = extraSeed_;
    storage.level().snowCovered = extraSnowCovered_;
    const bool saved = storage.saveLevel();
    storage.close(now);

    if (!saved) {
        message_ = "could not write level.dat";
        consoleDirty_ = true;
        return false;
    }

    // The world list caches the seed it showed on the World Info row, and it
    // has just changed underneath it.
    for (world::WorldEntry& entry : worlds_) {
        if (entry.path == selectedWorldPath_) {
            entry.seed = extraSeed_;
            break;
        }
    }
    selectedSeed_ = extraSeed_;

    message_ = nullptr;
    consoleDirty_ = true;
    return true;
}

void Menu::askNewSeed()
{
    constexpr int kMaxText = 64;

    SwkbdState swkbd;
    swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, kMaxText - 1);
    swkbdSetHintText(&swkbd, "Seed for chunks not generated yet");
    swkbdSetFeatures(&swkbd, SWKBD_DARKEN_TOP_SCREEN);
    swkbdSetValidation(&swkbd, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);

    // Pre-filled with the seed the world has, so a player who opens this to
    // read it and changes their mind can cancel out with the number intact --
    // and so a small edit is an edit rather than a retype.
    char initial[kMaxText];
    std::snprintf(initial, sizeof(initial), "%lld", (long long)extraSeed_);
    swkbdSetInitialText(&swkbd, initial);

    char text[kMaxText];
    // **The session is told before the applet starts, not after.** This
    // suspends the whole application for as long as somebody is typing; see
    // `ctr::linkPausing`.
    linkPausing(net::link::kAppletAwayMs);
    const SwkbdButton pressed = swkbdInputText(&swkbd, text, sizeof(text));

    consoleInit(GFX_BOTTOM, nullptr);
    consoleDirty_ = true;
    C2D_Prepare();

    if (pressed != SWKBD_BUTTON_CONFIRM) {
        return;
    }

    i64 seed = 0;
    if (!seedFromText(text, &seed)) {
        // Unlike the create screen, blank is not "roll one" here: this row is
        // reached by a player who came to type a specific number, and rolling
        // a random one over the world they already have is not what an empty
        // box asks for.
        message_ = "that is not a seed";
        consoleDirty_ = true;
        return;
    }
    if (seed == extraSeed_) {
        return;
    }

    const i64 previous = extraSeed_;
    extraSeed_ = seed;
    if (!saveExtraLevel()) {
        extraSeed_ = previous;  // the row goes on showing what is on the card
    }
}

int Menu::worldPackRows() const
{
    // Default, pinned above the list, then every pack `listPacks` found --
    // which already has Dev Art first among them.
    return int(packs_.size()) + 1;
}

const char* Menu::worldPackLabel() const
{
    const std::string& chosen = extraSettings().texturePack;
    if (chosen.empty()) {
        return "Default";
    }
    if (chosen == settings::kWorldPackDevArt) {
        return "Dev Art";
    }
    return chosen.c_str();
}

int Menu::worldPackRow() const
{
    const std::string& chosen = extraSettings().texturePack;
    if (chosen.empty()) {
        return 0;
    }
    for (usize i = 0; i < packs_.size(); ++i) {
        const texture::PackEntry& pack = packs_[i];
        const bool match = pack.builtIn ? chosen == settings::kWorldPackDevArt
                                        : chosen == pack.name;
        if (match) {
            return int(i) + 1;
        }
    }
    // A pack this console does not have. The list cannot show the row, so it
    // opens on Default -- but the setting is not touched: see the note in
    // core/settings/world_settings.cpp about a card carried between consoles.
    return 0;
}

void Menu::openWorldPack()
{
    // The full read of every zip on the card, paid on the way in and nowhere
    // else -- the same price Options' own pack list pays.
    refreshPacks();
    worldPackCursor_ = worldPackRow();
    worldPackScroll_ = 0;
    if (worldPackCursor_ >= kVisibleRows) {
        worldPackScroll_ = worldPackCursor_ - kVisibleRows + 1;
    }
}

void Menu::applyWorldPack()
{
    if (worldSettings_.texturePack.empty()) {
        // Follow the console. If a previous world overrode it, the menu's own
        // atlas has already been put back by the time this runs.
        return;
    }

    const std::string path = worldSettings_.texturePack == settings::kWorldPackDevArt
                                 ? std::string()
                                 : texture::packPath(texture::kPacksDir,
                                                     worldSettings_.texturePack);

    // Into a scratch image, exactly as selectPack does and for the same
    // reason: a world whose pack has been deleted from the card must open with
    // the console's art rather than with half an atlas.
    texture::AtlasImage loaded;
    if (texture::buildAtlas(fs_, path, &loaded) != texture::PackError::Ok) {
        message_ = "this world's texture pack could not be read";
        consoleDirty_ = true;
        return;
    }

    atlas_ = std::move(loaded);
    ++packRevision_;
    applySavedSkin();

    // **The menu's own choice is not written over.** `packName_` and 3ds.ini
    // still say what the console is set to; only the live image changed, and
    // this flag is what makes the next visit to the menu build the console's
    // again. The menu's font and backdrop are deliberately left alone: they
    // belong to the menu, which the player is leaving.
    packOverridden_ = true;
}

void Menu::commitPanorama()
{
    worldSettings_.panoramaTileX = panoramaTileX_;
    worldSettings_.panoramaTileZ = panoramaTileZ_;
    saveWorldSettings();

    // The file is the only channel between this screen and the worker that
    // builds the table, so the grid it already has must go before it will read
    // the new corner. Everything streams in again from there.
    if (preview_ != nullptr && !selectedWorldPath_.empty()) {
        preview_->forgetWorld(selectedWorldPath_);
    }
}

void Menu::buildExtraSettingsInfo(int row)
{
    std::string& out = infoBody_;
    out.clear();

    switch (row) {
    case kExtraSeed:
        if (!extraLevelKnown_) {
            appendf(&out, "§cThis world's level.dat could not be read.");
            break;
        }
        appendf(&out, "Changes the seed of this world.\n");
        appendf(&out, "§7Chunks already on the card keep the\n");
        appendf(&out, "§7shape they were generated with; this is\n");
        appendf(&out, "§7the seed everything past the edge is\n");
        appendf(&out, "§7generated from, so the two will not\n");
        appendf(&out, "§7line up at the seam.\n");
        appendf(&out, "§7Press A to type one.");
        break;
    case kExtraOreFix:
        appendf(&out, "%s.\n", onOff(extraSettings().fixOreGeneration));
        appendf(&out, "§7a1.1.2 rounds an ore vein's bounds\n");
        appendf(&out, "§7towards zero instead of downwards, so a\n");
        appendf(&out, "§7vein at negative X or Z loses a slice of\n");
        appendf(&out, "§7itself and those quadrants come out with\n");
        appendf(&out, "§7less ore than the positive one.\n");
        appendf(&out, "§7On rounds downwards everywhere.\n");
        if (extraForNewWorld_) {
            appendf(&out, "§7Nothing is generated yet, so this is\n");
            appendf(&out, "§7the whole world either way.");
        } else {
            appendf(&out, "§7Generation only: chunks already on the\n");
            appendf(&out, "§7card are not changed.");
        }
        break;
    case kExtraSecret:
        if (!extraLevelKnown_) {
            appendf(&out, "§cThis world's level.dat could not be read.");
            break;
        }
        appendf(&out, "%s.\n", yesNo(extraSnowCovered_));
        appendf(&out, "§7a1.1.2 rolls a one-in-four chance when a\n");
        appendf(&out, "§7world is made and never rolls again: the\n");
        appendf(&out, "§7world it makes freezes its seas and lays\n");
        appendf(&out, "§7snow. This is that roll, after the fact.\n");
        appendf(&out, "§7It does change the world you have -- ice\n");
        appendf(&out, "§7and snow spread over ground that is\n");
        appendf(&out, "§7already there as it is ticked -- but it\n");
        appendf(&out, "§7does not take them away again.");
        break;
    case kExtraPack:
        appendf(&out, "%s.\n", worldPackLabel());
        appendf(&out, "§7The pack this world is drawn with.\n");
        appendf(&out, "§7Default follows the console's own choice,\n");
        appendf(&out, "§7which is what every world does until it is\n");
        appendf(&out, "§7told otherwise.\n");
        appendf(&out, "§7Press A to choose one.");
        break;
    case kExtraPanorama:
        appendf(&out, "X %ld, Z %ld.\n", (long)extraSettings().panoramaTileX,
                (long)extraSettings().panoramaTileZ);
        appendf(&out, "§7Where the little world on the world list\n");
        appendf(&out, "§7is taken from. It stands on the 128-block\n");
        appendf(&out, "§7grid the map draws in red, three squares\n");
        appendf(&out, "§7across, and this moves it one square at a\n");
        appendf(&out, "§7time.\n");
        appendf(&out, "§7Press A to move it.");
        break;
    case kExtraBedrockFix:
        appendf(&out, "%s.\n", onOff(extraSettings().fixBedrockHole));
        appendf(&out, "§7a1.1.2 rolls for bedrock at every height\n");
        appendf(&out, "§7of every column, and one column in six\n");
        appendf(&out, "§7comes back with nothing at all at the\n");
        appendf(&out, "§7bottom -- a hole out of the world.\n");
        appendf(&out, "§7On lays bedrock at the lowest level\n");
        appendf(&out, "§7whatever the roll said.\n");
        if (extraForNewWorld_) {
            appendf(&out, "§7Nothing is generated yet, so this is\n");
            appendf(&out, "§7the whole world either way.");
        } else {
            appendf(&out, "§cGeneration only: it does not fill in a\n");
            appendf(&out, "§chole that is already on the card.");
        }
        break;
    case kExtraFencePlacement:
        appendf(&out, "%s.\n", onOff(extraSettings().improvedFencePlacement));
        appendf(&out, "§7a1.1.2 only lets a fence be placed on\n");
        appendf(&out, "§7solid ground: never on another fence and\n");
        appendf(&out, "§7never in the air.\n");
        appendf(&out, "§7On lets a fence go anywhere a block can.\n");
        appendf(&out, "§7Fences already built are not changed.");
        break;
    default:
        appendf(&out, extraForNewWorld_ ? "Return to Create World."
                                        : "Return to World Settings.");
        break;
    }
}

void Menu::handleExtraSettings(u32 down)
{
    const WorldSettingsLayout layout = extraSettingsLayout(extraForNewWorld_);
    if (extraCursor_ >= layout.count) {
        extraCursor_ = 0;
    }
    const int before = extraCursor_;
    extraCursor_ = step(down, extraCursor_, layout.count);
    extraScroll_ = gui::listScrollFor(layout.groups, layout.count, extraCursor_, extraScroll_,
                                      settingsGeometry(kExtraSettingsTop));
    if (extraCursor_ != before) {
        infoPage_ = 0;
    }
    if (down != 0) {
        consoleDirty_ = true;
    }
    const int row = layout.rows[extraCursor_];
    const bool moved = extraCursor_ != before;
    const bool pressed = !moved && (down & (KEY_A | kLeft | kRight)) != 0;

    if (pressed) {
        switch (row) {
        case kExtraSeed:
            // A only. Left and Right have nothing to step through -- there is
            // no next seed -- so they turn the explanation's page instead.
            if ((down & KEY_A) != 0 && extraLevelKnown_) {
                playClick();
                askNewSeed();
                return;
            }
            break;
        case kExtraOreFix:
            // **Both arrows and A do the same thing, because there are two
            // states.** A row with two values and a Left that means "the other
            // one" and a Right that means "the other one" is what every other
            // two-state row on this menu does.
            playClick();
            extraSettings().fixOreGeneration = !extraSettings().fixOreGeneration;
            saveExtraSettings();
            return;
        case kExtraSecret:
            if (extraLevelKnown_) {
                playClick();
                extraSnowCovered_ = !extraSnowCovered_;
                if (!saveExtraLevel()) {
                    extraSnowCovered_ = !extraSnowCovered_;
                }
            }
            return;
        case kExtraPack:
            if ((down & KEY_A) != 0) {
                playClick();
                openWorldPack();
                setScreen(Screen::WorldPack);
                return;
            }
            break;
        case kExtraPanorama:
            if ((down & KEY_A) != 0) {
                playClick();
                panoramaTileX_ = worldSettings_.panoramaTileX;
                panoramaTileZ_ = worldSettings_.panoramaTileZ;
                panoramaUndoX_ = panoramaTileX_;
                panoramaUndoZ_ = panoramaTileZ_;
                setScreen(Screen::MovePanorama);
                return;
            }
            break;
        case kExtraBedrockFix:
            playClick();
            extraSettings().fixBedrockHole = !extraSettings().fixBedrockHole;
            saveExtraSettings();
            return;
        case kExtraFencePlacement:
            playClick();
            extraSettings().improvedFencePlacement = !extraSettings().improvedFencePlacement;
            saveExtraSettings();
            return;
        default:
            break;
        }
    }

    if (!moved) {
        // The two rows an arrow cannot change, plus Back: on those the arrows
        // turn the explanation instead, which is what `turnInfoPage` means by
        // `arrowsTurn`.
        const bool valueless = row == kExtraSeed || row == kExtraPack ||
                               row == kExtraPanorama || row == kExtraBack;
        turnInfoPage(down, valueless);
    }

    if ((down & KEY_B) != 0 || ((down & KEY_A) != 0 && row == kExtraBack)) {
        if ((down & KEY_A) != 0) {
            playClick();
        }
        if (extraForNewWorld_) {
            extraForNewWorld_ = false;
            setScreen(Screen::CreateWorld);
            return;
        }
        setScreen(Screen::WorldSettings);
    }
}

void Menu::handleWorldPack(u32 down)
{
    const int rows = worldPackRows();
    worldPackCursor_ = step(down, worldPackCursor_, rows);

    if (worldPackCursor_ < worldPackScroll_) {
        worldPackScroll_ = worldPackCursor_;
    }
    if (worldPackCursor_ >= worldPackScroll_ + kVisibleRows) {
        worldPackScroll_ = worldPackCursor_ - kVisibleRows + 1;
    }

    if ((down & KEY_B) != 0) {
        message_ = nullptr;
        setScreen(Screen::ExtraSettings);
        return;
    }
    if ((down & KEY_A) == 0) {
        return;
    }
    playClick();

    if (worldPackCursor_ == 0) {
        extraSettings().texturePack.clear();
    } else {
        const texture::PackEntry& pack = packs_[usize(worldPackCursor_ - 1)];
        // Dev Art has no file name, so it gets a token of its own -- empty
        // already means "follow the console" here. See world_settings.hpp.
        extraSettings().texturePack = pack.builtIn ? std::string(settings::kWorldPackDevArt)
                                                   : pack.name;
    }
    saveExtraSettings();
    message_ = nullptr;
    consoleDirty_ = true;
}

void Menu::handleMovePanorama(u32 down)
{
    // One map tile a press, which is a third of the table's width -- the step
    // the red grid on the bottom map is drawn at, and the step the diorama's
    // own tiles are read in.
    i32 dx = 0;
    i32 dz = 0;
    if ((down & kLeft) != 0) {
        dx = -1;
    }
    if ((down & kRight) != 0) {
        dx = 1;
    }
    if ((down & kUp) != 0) {
        dz = -1;
    }
    if ((down & kDown) != 0) {
        dz = 1;
    }

    if (dx != 0 || dz != 0) {
        panoramaTileX_ += dx;
        panoramaTileZ_ += dz;
        playMoveClick();
        // **Written on every step rather than on the way out**, because the
        // picture under the player's thumb is built by a worker that reads the
        // file -- so the file is how the picture is asked to move. Saving is
        // one small atomic write to a file beside the world.
        commitPanorama();
        consoleDirty_ = true;
    }

    if ((down & (KEY_A | KEY_START)) != 0) {
        playClick();
        commitPanorama();
        setScreen(Screen::ExtraSettings);
        return;
    }

    if ((down & KEY_B) != 0) {
        // B puts the world back where it was found, and puts it back the same
        // way every other step did -- through the file, so the picture on the
        // way out is the picture that was there on the way in.
        if (panoramaTileX_ != panoramaUndoX_ || panoramaTileZ_ != panoramaUndoZ_) {
            panoramaTileX_ = panoramaUndoX_;
            panoramaTileZ_ = panoramaUndoZ_;
            commitPanorama();
        }
        setScreen(Screen::ExtraSettings);
    }
}

void Menu::drawExtraSettings()
{
    const WorldSettingsLayout layout = extraSettingsLayout(extraForNewWorld_);
    drawLabelCentered("Extra Settings", kScreenWidth * 0.5f, 10.0f, 0.7f, kInk, true);
    // The world these belong to, which at Create is the one being filled in.
    drawLabelClipped(extraForNewWorld_ ? newWorld_.name.c_str() : selectedWorldName_.c_str(),
                     40.0f, 28.0f, 0.5f, kInkDim, kScreenWidth - 80.0f);

    char seed[32];
    char panorama[32];
    if (extraLevelKnown_) {
        std::snprintf(seed, sizeof(seed), "%lld", (long long)extraSeed_);
    } else {
        std::snprintf(seed, sizeof(seed), "unreadable");
    }
    std::snprintf(panorama, sizeof(panorama), "%ld, %ld", (long)extraSettings().panoramaTileX,
                  (long)extraSettings().panoramaTileZ);

    SettingRowView rows[kExtraCount];
    for (int i = 0; i < layout.count; ++i) {
        SettingRowView& view = rows[i];
        switch (layout.rows[i]) {
        case kExtraSeed:
            view.name = "Seed:";
            view.value = seed;
            // A seed that could not be read is not a choice the row is
            // offering, so it is dimmed for the same reason an unimplemented
            // gamemode is: the row still works and says why on the bottom.
            view.dimValue = !extraLevelKnown_;
            break;
        case kExtraOreFix:
            view.name = "Fix Ore Gen:";
            view.value = onOff(extraSettings().fixOreGeneration);
            view.dimValue = !extraSettings().fixOreGeneration;
            break;
        case kExtraSecret:
            view.name = "Secret World:";
            view.value = extraLevelKnown_ ? yesNo(extraSnowCovered_) : "unreadable";
            view.dimValue = !extraLevelKnown_ || !extraSnowCovered_;
            break;
        case kExtraPack:
            view.name = "Texture Pack:";
            view.value = worldPackLabel();
            view.dimValue = extraSettings().texturePack.empty();
            break;
        case kExtraPanorama:
            view.name = "Panorama:";
            view.value = panorama;
            break;
        case kExtraBedrockFix:
            view.name = "Fix Bedrock:";
            view.value = onOff(extraSettings().fixBedrockHole);
            view.dimValue = !extraSettings().fixBedrockHole;
            break;
        case kExtraFencePlacement:
            view.name = "Fences:";
            view.value = extraSettings().improvedFencePlacement ? "Improved" : "a1.1.2";
            view.dimValue = !extraSettings().improvedFencePlacement;
            break;
        default:
            view.name = "Back";
            break;
        }
    }

    drawSettingsRows(rows, layout.groups, layout.count, extraCursor_, extraScroll_,
                     kExtraSettingsTop);
}

void Menu::drawWorldPack()
{
    drawLabelCentered("World Texture Pack", kScreenWidth * 0.5f, 16.0f, 0.7f, kInk, true);

    const int rows = worldPackRows();
    const float rowX = 20.0f;
    const float rowWidth = kScreenWidth - 2.0f * rowX;
    const int chosen = worldPackRow();

    const int visible = drawListChrome(rows, worldPackScroll_);
    for (int i = 0; i < visible; ++i) {
        const int index = worldPackScroll_ + i;
        const Rect rect{rowX, kRowsTop + float(i) * (kRowHeight + kRowGap), rowWidth,
                        kRowHeight};
        const bool selected = index == worldPackCursor_;

        drawButton(rect, "", selected, true);
        // Which one the world is set to, as a mark on the row: "selected" here
        // means the cursor, and both have to be visible at once.
        if (index == chosen) {
            drawLabel("*", rect.x + 8.0f, rect.y + 5.0f, 0.55f, kInkWarn, C2D_AlignLeft, true);
        }

        const char* name = nullptr;
        char detail[32];
        if (index == 0) {
            name = "Default";
            std::snprintf(detail, sizeof(detail), "%s",
                          packName_.empty() ? "Dev Art" : packName_.c_str());
        } else {
            const texture::PackEntry& pack = packs_[usize(index - 1)];
            name = pack.builtIn ? "Dev Art" : pack.name.c_str();
            if (pack.builtIn) {
                std::snprintf(detail, sizeof(detail), "built in");
            } else {
                std::snprintf(detail, sizeof(detail), "%d/%d files", pack.textureCount,
                              texture::kA112FileCount);
            }
        }

        constexpr float kDetailWidth = 84.0f;
        drawLabelClipped(name, rect.x + 22.0f, rect.y + 3.0f, 0.55f, kInk,
                         rect.w - 32.0f - kDetailWidth);
        drawLabel(detail, rect.x + rect.w - 10.0f, rect.y + 6.0f, 0.4f, kInkDim,
                  C2D_AlignRight, true);
    }
}

void Menu::drawMovePanorama()
{
    drawLabelCentered("Move Panorama", kScreenWidth * 0.5f, 16.0f, 0.7f, kInk, true);
    drawLabelClipped(selectedWorldName_.c_str(), 40.0f, 40.0f, 0.5f, kInkDim,
                     kScreenWidth - 80.0f);

    // **The table's own corner, in blocks**, rather than the tile numbers the
    // d-pad steps: a player looking for a particular place in their world
    // knows where it is in blocks and has never heard of a tile.
    i32 blockX = 0;
    i32 blockZ = 0;
    preview::dioramaOrigin(&blockX, &blockZ, panoramaTileX_, panoramaTileZ_);

    char line[96];
    std::snprintf(line, sizeof(line), "tile %ld, %ld", (long)panoramaTileX_,
                  (long)panoramaTileZ_);
    drawLabelCentered(line, kScreenWidth * 0.5f, 72.0f, 0.6f, kInk, true);

    std::snprintf(line, sizeof(line), "blocks %ld..%ld  x  %ld..%ld", (long)blockX,
                  (long)(blockX + preview::kDioramaBlocks - 1), (long)blockZ,
                  (long)(blockZ + preview::kDioramaBlocks - 1));
    drawLabelCentered(line, kScreenWidth * 0.5f, 96.0f, 0.42f, kInkDim, true);

    drawLabelCentered("Each step is one square of the map's", kScreenWidth * 0.5f, 126.0f,
                      0.42f, kInkDim, true);
    drawLabelCentered("red 128-block grid.", kScreenWidth * 0.5f, 142.0f, 0.42f, kInkDim,
                      true);

    if (message_ != nullptr) {
        drawLabelCentered(message_, kScreenWidth * 0.5f, 166.0f, 0.45f, kInkWarn, true);
    }

    const float half = kButtonWidth * 0.5f;
    drawButton(Rect{kScreenWidth * 0.5f - half - 8.0f, 192.0f, half, kButtonHeight},
               "A/START Save", true, true);
    drawButton(Rect{kScreenWidth * 0.5f + 8.0f, 192.0f, half, kButtonHeight}, "B Cancel",
               false, true);
}

bool Menu::beginConvert()
{
    if (inGame_ || selectedWorldPath_.empty()) {
        return false;
    }
    // The diorama may be reading this very world; a conversion rewrites it.
    // The size walk is in the same position, and a total taken across a
    // conversion would be about neither shape.
    if (preview_ != nullptr) {
        preview_->quiesce(selectedWorldPath_);
    }
    sizeScan_.cancel();
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
        playClick();
        runConvert();
    }
}

void Menu::copySelectedWorld()
{
    if (inGame_ || selectedWorldPath_.empty()) {
        return;
    }

    if (preview_ != nullptr) {
        preview_->quiesce(selectedWorldPath_);
    }
    // A copy reads every file this would stat; the card serves one of them
    // faster than both.
    sizeScan_.cancel();

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
    playClick();

    // The world this screen is about is the one the settings screen was opened
    // on, held by path rather than by list index: the list is re-read on every
    // refresh, and an index would be pointing at a different row.
    if (!selectedWorldPath_.empty()) {
        if (preview_ != nullptr) {
            preview_->quiesce(selectedWorldPath_);
        }
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
    playClick();

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
        return;  // an empty list has no button to land on, so no click either
    }
    playClick();
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
    playClick();

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

bool Menu::askWorldName(std::string_view current, std::string* out)
{
    constexpr int kMaxText = 64;

    // **What the row is already holding**, so a name typed once and opened
    // again to fix a letter is there to fix. Empty falls back to the first free
    // "World<n>", which is what the screen itself opens with.
    char initial[kMaxText];
    if (current.empty()) {
        const std::string suggestion = world::defaultWorldName(worlds_);
        std::snprintf(initial, sizeof(initial), "%s", suggestion.c_str());
    } else {
        std::snprintf(initial, sizeof(initial), "%.*s", int(current.size()), current.data());
    }

    SwkbdState swkbd;
    swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, kMaxText - 1);
    swkbdSetInitialText(&swkbd, initial);
    swkbdSetHintText(&swkbd, "World name");
    swkbdSetFeatures(&swkbd, SWKBD_DARKEN_TOP_SCREEN);
    swkbdSetValidation(&swkbd, SWKBD_NOTEMPTY_NOTBLANK, SWKBD_FILTER_CALLBACK, 0);
    gExistingWorlds = &worlds_;
    swkbdSetFilterCallback(&swkbd, validateWorldName, nullptr);

    char text[kMaxText];
    // **The session is told before the applet starts, not after.** This
    // suspends the whole application for as long as somebody is typing; see
    // `ctr::linkPausing`.
    linkPausing(net::link::kAppletAwayMs);
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
    // **The session is told before the applet starts, not after.** This
    // suspends the whole application for as long as somebody is typing; see
    // `ctr::linkPausing`.
    linkPausing(net::link::kAppletAwayMs);
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

// False means the player backed out and neither output is touched. True means
// they confirmed: `*chosen` is false for a blank box, which is the Random the
// screen draws and which `createWorld` rolls for, and true with `*out` filled
// in otherwise.
//
// **The roll is not done here**, which it used to be. A keyboard that rolled
// its own seed could not tell "random" from "the number I typed", so the row
// had to show a number the player never chose -- and the two states are the
// whole of what the row is for.
bool Menu::askSeed(bool* chosen, i64* out)
{
    constexpr int kMaxText = 64;

    SwkbdState swkbd;
    swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, kMaxText - 1);
    swkbdSetHintText(&swkbd, "Seed -- leave blank for a random one");
    swkbdSetFeatures(&swkbd, SWKBD_DARKEN_TOP_SCREEN);
    // No validation: **blank is a valid answer here** and means "roll one",
    // which is what a1.1.2 does every time because it never asks at all.
    swkbdSetValidation(&swkbd, SWKBD_ANYTHING, 0, 0);

    // Whatever the row already holds, so opening this to look at a seed and
    // backing out of it costs nothing. A blank box for Random, which is also
    // how it is cleared again.
    char initial[kMaxText];
    if (*chosen) {
        std::snprintf(initial, sizeof(initial), "%lld", (long long)*out);
    } else {
        initial[0] = '\0';
    }
    swkbdSetInitialText(&swkbd, initial);

    char text[kMaxText];
    // **The session is told before the applet starts, not after.** This
    // suspends the whole application for as long as somebody is typing; see
    // `ctr::linkPausing`.
    linkPausing(net::link::kAppletAwayMs);
    const SwkbdButton pressed = swkbdInputText(&swkbd, text, sizeof(text));

    consoleInit(GFX_BOTTOM, nullptr);
    consoleDirty_ = true;
    C2D_Prepare();

    if (pressed != SWKBD_BUTTON_CONFIRM) {
        return false;
    }

    i64 seed = 0;
    // `seedFromText` returns false only for a blank box: a word is hashed the
    // way later Minecraft hashes one, and a typed 0 is 0.
    *chosen = seedFromText(text, &seed);
    if (*chosen) {
        *out = seed;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Create World
//
// **The screen that replaced two keyboards opening back to back.** Making a
// world used to be `askWorldName` and then `askSeed`, with no way to see the
// first answer again, no way to change anything else, and nothing between
// pressing A on the world list and a world existing on the card.
//
// It is laid out like World Settings because it is the same set of questions
// asked one moment earlier -- and three of them can *only* be asked here.
// Gamemode and difficulty can be changed at any time; a seed, a format and the
// two generation fixes are about ground that has not been made yet, and the
// screen is the last moment at which that is true of the whole world.
//
// **Nothing is written until Create.** Every row edits `newWorld_`, so B
// leaves having made nothing, and a failed Create leaves the screen exactly as
// the player filled it in.
// ---------------------------------------------------------------------------

void Menu::openCreateWorld()
{
    newWorld_ = NewWorld();
    // The first free "World<n>" -- which is what a1.1.2's five fixed slots are
    // called, and so what a player expects to be offered.
    newWorld_.name = world::defaultWorldName(worlds_);
    // A world made here starts in Survival, which is not what an absent
    // settings file means -- see settings::kNewWorldGamemode.
    newWorld_.settings.gamemode = settings::kNewWorldGamemode;

    // **The two shared cursors are pointed at this world, not left where the
    // last screen put them.** They belong to the menu rather than to a screen
    // -- World Settings uses the same pair -- so without this a world created
    // after browsing another world's settings would draw that world's gamemode
    // beside a `NewWorld` that is still holding the default.
    gamemodeCursor_ = gamemodeIndex(newWorld_.settings.gamemode);
    difficultyCursor_ = difficultyIndex(newWorld_.settings.difficulty);

    createCursor_ = 0;
    createScroll_ = 0;
    message_ = nullptr;
}

void Menu::buildCreateWorldInfo(int row)
{
    std::string& out = infoBody_;
    out.clear();

    switch (row) {
    case kNewName:
        appendf(&out, "§e%s\n", newWorld_.name.c_str());
        appendf(&out, "§7The folder this world gets on the card,\n");
        appendf(&out, "§7and what the world list shows.\n");
        appendf(&out, "§7Press A to type one.");
        break;
    case kNewSeed:
        if (newWorld_.seedChosen) {
            appendf(&out, "§e%lld\n", (long long)newWorld_.seed);
            appendf(&out, "§7The same seed makes the same world.\n");
        } else {
            appendf(&out, "Random.\n");
            appendf(&out, "§7One is drawn from the clock when the\n");
            appendf(&out, "§7world is made, which is what a1.1.2\n");
            appendf(&out, "§7does every time -- it never asks.\n");
        }
        appendf(&out, "§7Press A to type one, blank for random.");
        break;
    case kNewGamemode:
        switch (kGamemodeOrder[gamemodeCursor_]) {
        case settings::Gamemode::Spectator:
            appendf(&out, "Fly through the world freely.\n");
            appendf(&out, "§7No collision, no building.");
            break;
        case settings::Gamemode::Creative:
            appendf(&out, "Build with every block.\n");
            appendf(&out, "§7Break and place blocks freely.");
            break;
        case settings::Gamemode::Survival:
            appendf(&out, "Mine, craft and stay alive.\n");
            appendf(&out, "§7Blocks take time to break and drop items.");
            break;
        }
        break;
    case kNewDifficulty:
        if (kDifficultyOrder[difficultyCursor_] == settings::Difficulty::Peaceful) {
            appendf(&out, "No monsters.");
        } else {
            appendf(&out, "Monsters spawn in the dark.\n");
            appendf(&out, "§7Harder settings make their hits hurt more.");
        }
        break;
    case kNewFormat:
        if (newWorld_.format == world::WorldFormat::Packed) {
            appendf(&out, "Packed for this console.\n");
            appendf(&out, "§7One file per region instead of one per\n");
            appendf(&out, "§7chunk, which is what suits a card whose\n");
            appendf(&out, "§7clusters are 16 KB.\n");
            appendf(&out, "§7Only 3DAlpha can open it.");
        } else {
            appendf(&out, "Standard Minecraft save.\n");
            appendf(&out, "§7A level.dat and the base36 chunk\n");
            appendf(&out, "§7folders, so it opens on a PC.\n");
            appendf(&out, "§7Slower on this hardware.");
        }
        appendf(&out, "\n§7Either can be converted afterwards.");
        break;
    case kNewSecret:
        switch (newWorld_.secret) {
        case NewWorld::Secret::Roll:
            appendf(&out, "Roll for it -- one chance in four.\n");
            appendf(&out, "§7Which is what a1.1.2 does: it flips\n");
            appendf(&out, "§7this once when a world is made and\n");
            appendf(&out, "§7never again.");
            break;
        case NewWorld::Secret::Yes:
            appendf(&out, "A winter world.\n");
            appendf(&out, "§7Every sea is frozen at the waterline\n");
            appendf(&out, "§7and snow lies on the ground.");
            break;
        case NewWorld::Secret::No:
            appendf(&out, "Never.\n");
            appendf(&out, "§7No ice, no snow.");
            break;
        }
        break;
    case kNewExtra:
        appendf(&out, "Four choices a1.1.2 never had.\n");
        appendf(&out, "§7This world's texture pack, the two\n");
        appendf(&out, "§7generation bugs, and where a fence may\n");
        appendf(&out, "§7be placed.\n");
        appendf(&out, "§7They are offered here because this is\n");
        appendf(&out, "§7the moment the generation ones cost\n");
        appendf(&out, "§7nothing: no chunk exists yet to be\n");
        appendf(&out, "§7generated the other way.\n");
        appendf(&out, "§7Press A to open them.");
        break;
    case kNewCreate:
        appendf(&out, "Makes the world and opens it.\n");
        appendf(&out, "§7Nothing above is written to the card\n");
        appendf(&out, "§7until this is pressed.\n");
        appendf(&out, "§7More options are on World Settings once\n");
        appendf(&out, "§7the world exists.");
        break;
    default:
        appendf(&out, "Return to the world list.\n");
        appendf(&out, "§7Nothing is made.");
        break;
    }
}

bool Menu::handleCreateWorld(u32 down, MenuChoice* choice)
{
    const int before = createCursor_;
    createCursor_ = step(down, createCursor_, kNewCount);
    createScroll_ = gui::listScrollFor(kCreateGroups, kNewCount, createCursor_, createScroll_,
                                       settingsGeometry(kCreateWorldTop));
    if (createCursor_ != before) {
        infoPage_ = 0;
    }
    if (down != 0) {
        consoleDirty_ = true;
    }
    const bool moved = createCursor_ != before;

    // The two value rows share their cursors with World Settings, so that a
    // gamemode browsed on one screen is the gamemode shown on the other. They
    // are pointed at this world on the way in, by openCreateWorld through
    // NewWorld's defaults.
    if (!moved && createCursor_ == kNewGamemode) {
        const int previous = gamemodeCursor_;
        if ((down & kLeft) != 0 && gamemodeCursor_ > 0) {
            --gamemodeCursor_;
        }
        if ((down & kRight) != 0 && gamemodeCursor_ < kGamemodeCount - 1) {
            ++gamemodeCursor_;
        }
        // The row moves onto a disabled mode; the world does not -- the same
        // bargain the World Settings row takes, and for the same reason.
        if (gamemodeCursor_ != previous
            && settings::gamemodeImplemented(kGamemodeOrder[gamemodeCursor_])) {
            newWorld_.settings.gamemode = kGamemodeOrder[gamemodeCursor_];
            playClick();
        }
    }

    if (!moved && createCursor_ == kNewDifficulty) {
        const int previous = difficultyCursor_;
        if ((down & kLeft) != 0 && difficultyCursor_ > 0) {
            --difficultyCursor_;
        }
        if ((down & kRight) != 0 && difficultyCursor_ < kDifficultyCount - 1) {
            ++difficultyCursor_;
        }
        if (difficultyCursor_ != previous) {
            newWorld_.settings.difficulty = kDifficultyOrder[difficultyCursor_];
            playClick();
        }
    }

    const bool pressed = !moved && (down & (KEY_A | kLeft | kRight)) != 0;
    if (pressed) {
        switch (createCursor_) {
        case kNewName:
            if ((down & KEY_A) != 0) {
                playClick();
                std::string name;
                if (askWorldName(newWorld_.name, &name)) {
                    newWorld_.name = std::move(name);
                    message_ = nullptr;
                }
            }
            return false;
        case kNewSeed:
            if ((down & KEY_A) != 0) {
                playClick();
                // **Blank is an answer here, not a cancel** -- it means Random,
                // and askSeed reports the two apart so the row can show which
                // one it got. `seedFromText` returns false only for a blank
                // box; a word is hashed the way later Minecraft hashes one.
                if (askSeed(&newWorld_.seedChosen, &newWorld_.seed)) {
                    message_ = nullptr;
                }
            }
            return false;
        case kNewFormat:
            // Two formats, so Left, Right and A all mean the other one -- the
            // same as the World Settings Format row.
            playClick();
            newWorld_.format = newWorld_.format == world::WorldFormat::Packed
                                   ? world::WorldFormat::Folder
                                   : world::WorldFormat::Packed;
            return false;
        case kNewSecret: {
            // Three states, so the arrows step rather than toggle and A goes
            // forward: Roll, Yes, No.
            playClick();
            int index = int(newWorld_.secret);
            if ((down & kLeft) != 0) {
                index = index == 0 ? 2 : index - 1;
            } else {
                index = index == 2 ? 0 : index + 1;
            }
            newWorld_.secret = NewWorld::Secret(index);
            return false;
        }
        case kNewExtra:
            if ((down & KEY_A) != 0) {
                playClick();
                openExtraSettingsForNewWorld();
                setScreen(Screen::ExtraSettings);
            }
            return false;
        case kNewCreate:
            if ((down & KEY_A) != 0) {
                playClick();
                return createWorld(choice);
            }
            break;
        default:
            break;
        }
    }

    if (!moved) {
        const bool valueless = createCursor_ == kNewName || createCursor_ == kNewSeed ||
                               createCursor_ == kNewExtra || createCursor_ == kNewCreate ||
                               createCursor_ == kNewBack;
        turnInfoPage(down, valueless);
    }

    if ((down & KEY_B) != 0 || ((down & KEY_A) != 0 && createCursor_ == kNewBack)) {
        if ((down & KEY_A) != 0) {
            playClick();
        }
        message_ = nullptr;
        setScreen(Screen::Worlds);
    }
    return false;
}

void Menu::drawCreateWorld()
{
    drawLabelCentered("Create World", kScreenWidth * 0.5f, 12.0f, 0.7f, kInk, true);

    char seed[32];
    if (newWorld_.seedChosen) {
        std::snprintf(seed, sizeof(seed), "%lld", (long long)newWorld_.seed);
    } else {
        std::snprintf(seed, sizeof(seed), "Random");
    }

    const settings::Gamemode mode = kGamemodeOrder[gamemodeCursor_];

    SettingRowView rows[kNewCount];
    for (int i = 0; i < kNewCount; ++i) {
        SettingRowView& view = rows[i];
        switch (i) {
        case kNewName:
            view.name = "Name:";
            view.value = newWorld_.name.c_str();
            break;
        case kNewSeed:
            view.name = "Seed:";
            view.value = seed;
            // Random is a real answer and not a missing one, but it is not a
            // number the player chose, so it is drawn as the quieter of the
            // two states.
            view.dimValue = !newWorld_.seedChosen;
            break;
        case kNewGamemode:
            view.name = "Gamemode:";
            view.value = settings::gamemodeLabel(mode);
            view.dimValue = !settings::gamemodeImplemented(mode);
            break;
        case kNewDifficulty:
            view.name = "Difficulty:";
            view.value = settings::difficultyLabel(kDifficultyOrder[difficultyCursor_]);
            break;
        case kNewFormat:
            view.name = "Format:";
            view.value = world::formatName(newWorld_.format);
            break;
        case kNewSecret:
            view.name = "Secret World:";
            view.value = newWorld_.secret == NewWorld::Secret::Roll  ? "Roll (1 in 4)"
                         : newWorld_.secret == NewWorld::Secret::Yes ? "Yes"
                                                                     : "No";
            view.dimValue = newWorld_.secret == NewWorld::Secret::Roll;
            break;
        case kNewExtra:
            view.name = "Extra Settings...";
            break;
        case kNewCreate:
            view.name = "Create";
            break;
        default:
            view.name = "Back";
            break;
        }
    }

    drawSettingsRows(rows, kCreateGroups, kNewCount, createCursor_, createScroll_,
                     kCreateWorldTop);
}

bool Menu::createWorld(MenuChoice* choice)
{
    // **Everything that can be refused is refused before anything is written.**
    // The name arrives from a keyboard that already validated it, but the row
    // can also still be holding the suggestion this screen opened with, and
    // the world list can have changed underneath it.
    std::string name;
    if (!world::sanitizeWorldName(newWorld_.name, &name)) {
        message_ = "that name has nothing a card can store";
        consoleDirty_ = true;
        return false;
    }
    for (const world::WorldEntry& entry : worlds_) {
        if (entry.name == name) {
            message_ = "there is already a world with that name";
            consoleDirty_ = true;
            return false;
        }
    }

    if (!fs_.makeDirectories(kSavesDir)) {
        message_ = "could not make the saves folder on the card";
        consoleDirty_ = true;
        return false;
    }

    // **One clock-seeded stream for both draws.** Two `JavaRandom`s built from
    // `clockSeed()` in the same call would be built from the same clock and
    // would agree with each other, which is not what "roll twice" means. The
    // order below is fixed rather than conditional for the same reason it is
    // written down at all: neither draw is reproducible, so the only property
    // worth having is that one call cannot make them correlate.
    JavaRandom random(clockSeed());

    // a1.1.2's own answer for a seed nobody typed: `new World(File, String)`
    // seeds itself with `new Random().nextLong()`. Java's no-argument Random
    // draws from a process-wide uniquifier mixed with nanoTime, which a console
    // cannot reproduce and which is not worth reproducing -- what matters is
    // that it is a nextLong off a clock-seeded LCG.
    const i64 seed = newWorld_.seedChosen ? newWorld_.seed : random.nextLong();

    // **SnowCovered is decided here because a1.1.2 decides it here.** In `cn`'s
    // constructor, the branch taken when there is no level.dat reads
    // `this.snowCovered = this.rand.nextInt(4) == 0`, and `rand` is the World's
    // **unseeded** `new Random()` -- a one-in-four coin flip at creation, not a
    // function of the seed, then persisted and never rolled again. The
    // generator reads it: it puts ice at sea level - 1 across every ocean.
    // Storage::create cannot do this itself because core has no clock, and the
    // harnesses want it off and deterministic.
    //
    // Roll is that flip and is the default. Yes and No are the screen's
    // deviation, and are the same value the Extra Settings row edits later.
    const bool roll = random.nextInt(4) == 0;
    const bool snowCovered = newWorld_.secret == NewWorld::Secret::Roll
                                 ? roll
                                 : newWorld_.secret == NewWorld::Secret::Yes;

    const std::string path = world::worldPath(kSavesDir, name);
    const i64 now = nowMillis();

    // **Packed is the default and not the only choice.** It is the format that
    // suits the hardware -- one file per region instead of one per chunk, on a
    // card whose clusters are 16 KB and whose file operations are IPC round
    // trips. A player who wants a save a PC can open picks Folder on the row
    // above; either can be converted afterwards, losslessly, both ways.
    world::AnyStorage storage(fs_);
    if (storage.create(path, seed, now, newWorld_.format) != world::OpenResult::Ok) {
        message_ = "could not create the world -- is the card full or locked?";
        consoleDirty_ = true;
        return false;
    }

    storage.level().snowCovered = snowCovered;

    // **Where the world starts the player**, which `cn`'s constructor decides
    // on this same branch -- the one taken when there is no level.dat -- by
    // walking away from (0, 0) until the top of a column is sand. It is the
    // reason an Alpha world begins on a beach, and without it every world
    // begins at x = 0, z = 0 with `spawnY` at 64 whatever the ground there is
    // doing, which in Survival is as likely to be the inside of a hill as the
    // top of one. The same `random` the snow flip came off, because the
    // original walks on the World's own unseeded `Random` too.
    //
    // **This generates terrain, here, now**, a column at a time until the walk
    // accepts one -- a median of about twenty and a tail into the low hundreds.
    // It is the one place in the shell that makes a player wait for the
    // generator on the main thread, and it is paid once in a world's life. See
    // core/world/spawn_point.hpp.
    world::chooseFreshSpawn(seed, snowCovered, random, &storage.level());

    const bool saved = storage.saveLevel();
    storage.close(now);
    if (!saved) {
        message_ = "could not write level.dat";
        consoleDirty_ = true;
        return false;
    }

    // The one place a world comes into existence, so the one place its
    // settings file starts out -- and now the one place the generation fixes
    // can be set before they mean anything, since they only touch chunks that
    // do not exist yet and at this moment none do. Written rather than left
    // absent so a player who opens the folder on a PC finds it and can see
    // what it holds.
    settings::saveWorldSettings(fs_, path, newWorld_.settings);

    message_ = nullptr;
    // **Where the menu is standing when this world opens**, which is where
    // `runPause` puts it back on the way out. The Create screen is not a place
    // to come back to -- its rows are about a world that now exists -- so the
    // world list is, with the new world under the cursor.
    worldCursorName_ = name;
    setScreen(Screen::Worlds);

    choice->action = MenuChoice::Action::Play;
    choice->worldPath = path;
    choice->worldName = name;
    choice->created = true;
    choice->gamemode = newWorld_.settings.gamemode;
    choice->difficulty = newWorld_.settings.difficulty;
    // **Its texture pack too**, for the reason `handleWorlds` gives: the atlas
    // the caller is handed is `atlas_`, and a world that named a pack of its
    // own on the Extra Settings screen wants that one rather than the
    // console's. applyWorldPack reads the member, so it is set first.
    worldSettings_ = newWorld_.settings;
    applyWorldPack();
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

    // **Bounded, because this is where a wedged GPU gets handed over.**
    // `C3D_FRAME_SYNCDRAW` ends in an unbounded `gxCmdQueueWait`, and the
    // thread it blocks is also `aptMainLoop` -- so a list the GPU never
    // finishes takes the menu, both screens and HOME with it, with no dump and
    // nothing on the card. `Renderer::shutdown` drains before handing the
    // screen over, but that drain has a deadline and the case where it expires
    // is exactly the case where the GPU is already gone: the guard that mattered
    // was missing from the one frame that inherits the problem.
    //
    // On expiry no frame was opened, so there is nothing to draw into and
    // nothing to end. The menu simply holds its last image; input, `aptMainLoop`
    // and HOME keep running, which is the whole difference between a console
    // the player can back out of and one they have to hold the power button on.
    if (!beginFrameBounded(kFrameWatchdogSeconds)) {
        return;
    }
    prepare2D();
    C2D_TargetClear(target_, C2D_Color32(0x18, 0x14, 0x10, 0xFF));
    C2D_SceneBegin(target_);
    drawScreen();
    drawPreviewScreen();
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
    case Screen::CreateWorld:
        drawCreateWorld();
        break;
    case Screen::ExtraSettings:
        drawExtraSettings();
        break;
    case Screen::WorldPack:
        drawWorldPack();
        break;
    case Screen::MovePanorama:
        drawMovePanorama();
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
    case Screen::Skins:
        drawSkins();
        break;
    case Screen::PickJar:
        drawPickJar();
        break;
    case Screen::ConfirmDeleteJar:
        drawConfirmDeleteJar();
        break;
    case Screen::Multiplayer:
        drawMultiplayer();
        break;
    case Screen::NetMode:
        drawNetMode();
        break;
    case Screen::Session:
        drawSession();
        break;
    case Screen::ImportScan:
        drawImportScan();
        break;
    case Screen::Transfer:
        drawTransfer();
        break;
    case Screen::EditServer:
        drawEditServer();
        break;
    case Screen::ConfirmDeleteServer:
        drawConfirmDeleteServer();
        break;
    case Screen::Disconnected:
        drawDisconnected();
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
        drawButton(rect, labels[i], titleCursor_ == i, true);
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
    const char* const singleLabels[] = {"Resume", "World Settings", "Options", "Exit World"};
    const char* const multiLabels[] = {"Resume", "Chat", "Options", "Disconnect"};
    const char* const* labels = multiplayer_ ? multiLabels : singleLabels;
    for (int i = 0; i < 4; ++i) {
        const Rect rect{x, 80.0f + float(i) * (kButtonHeight + 4.0f), kButtonWidth,
                        kButtonHeight};
        drawButton(rect, labels[i], pauseCursor_ == i, true);
    }

    drawLabelCentered(multiplayer_ ? "The server keeps the world." : "Exiting saves the world.",
                      kScreenWidth * 0.5f, 206.0f, 0.45f, kInkDim, true);
}

void Menu::drawWorldSettings()
{
    drawLabelCentered("World Settings", kScreenWidth * 0.5f, 10.0f, 0.7f, kInk, true);
    // Clipped rather than centred, for the same reason every other name here
    // is: it came off a card and can be anything a PC let somebody type.
    drawLabelClipped(selectedWorldName_.c_str(), 40.0f, 28.0f, 0.5f, kInkDim,
                     kScreenWidth - 80.0f);

    const WorldSettingsLayout layout = worldSettingsLayout(inGame_);

    char gamemode[32];
    char difficulty[32];
    char format[32];
    const settings::Gamemode mode = kGamemodeOrder[gamemodeCursor_];
    std::snprintf(gamemode, sizeof(gamemode), "%s", settings::gamemodeLabel(mode));
    std::snprintf(difficulty, sizeof(difficulty), "%s",
                  settings::difficultyLabel(kDifficultyOrder[difficultyCursor_]));
    std::snprintf(format, sizeof(format), "%s", world::formatName(selectedFormat_));

    SettingRowView rows[kRowCount];
    for (int i = 0; i < layout.count; ++i) {
        SettingRowView& view = rows[i];
        switch (layout.rows[i]) {
        case kRowInfo:
            view.name = "World Info";
            break;
        case kRowGamemode:
            view.name = "Gamemode:";
            view.value = gamemode;
            // The value, not the button, is what says a mode is unavailable:
            // the row itself still works, and greying the whole button would
            // read as "this row is broken".
            view.dimValue = !settings::gamemodeImplemented(mode);
            break;
        case kRowDifficulty:
            view.name = "Difficulty:";
            view.value = difficulty;
            break;
        case kRowFormat:
            view.name = "Format:";
            view.value = format;
            break;
        case kRowExtra:
            view.name = "Extra Settings...";
            break;
        case kRowCopy:
            view.name = "Copy...";
            break;
        case kRowExport:
            view.name = "Export...";
            break;
        case kRowDelete:
            view.name = "Delete...";
            break;
        default:
            view.name = "Back";
            break;
        }
    }

    drawSettingsRows(rows, layout.groups, layout.count, worldSettingsCursor_,
                     worldSettingsScroll_, kWorldSettingsTop);
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
    drawLabelCentered(pickingHost_ ? "Host a World" : "Select World", kScreenWidth * 0.5f,
                      16.0f, 0.7f, kInk, true);

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

        if (index == kWorldRowCreate) {
            drawButton(rect, "+ Create New World", selected, true);
            continue;
        }
        if (index == kWorldRowImport) {
            drawButton(rect, "+ Import World", selected, true);
            continue;
        }

        // A world row is a button with two texts on it rather than a centred
        // label, so the name and when it was last played both fit.
        drawButton(rect, "", selected, true);
        const world::WorldEntry& entry = worlds_[usize(index - kWorldRowFirst)];

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

void Menu::drawSettingsRows(const SettingRowView* rows, const u8* groups, int count, int cursor,
                            int scroll, float top)
{
    const gui::ListGeometry geometry = settingsGeometry(top);
    const float x = (kScreenWidth - kSettingsWidth) * 0.5f;
    const int visible = gui::listVisibleCount(groups, count, scroll, geometry);
    const int origin = gui::listRowOffset(groups, scroll, geometry);

    for (int k = 0; k < visible; ++k) {
        const int i = scroll + k;
        const SettingRowView& row = rows[i];
        const float y = top + float(gui::listRowOffset(groups, i, geometry) - origin);
        const Rect rect{x, y, kSettingsWidth, float(geometry.rowHeight)};
        const bool selected = i == cursor;

        if (row.value == nullptr) {
            drawButton(rect, row.name, selected, true);
        } else {
            // A name off a card can be as long as FAT allows, so the value is
            // clipped to the button rather than allowed to run off its edge.
            drawButton(rect, "", selected, true);
            drawLabel(row.name, rect.x + 14.0f, rect.y + 4.0f, 0.5f, kInkDim, C2D_AlignLeft,
                      true);
            drawLabelClipped(row.value, rect.x + kSettingsValueX, rect.y + 4.0f, 0.5f,
                             row.dimValue ? kInkDim : kInk,
                             rect.w - kSettingsValueX - 14.0f);
        }

        // The bottom screen has more than a page about this row: arrows either
        // side of it, the same pair its name sits between down there.
        if (selected && infoPages_ > 1) {
            drawLabel("<", rect.x + 4.0f, rect.y + 4.0f, 0.5f, kInkWarn, C2D_AlignLeft, true);
            drawLabel(">", rect.x + rect.w - 4.0f, rect.y + 4.0f, 0.5f, kInkWarn,
                      C2D_AlignRight, true);
        }
    }

    // Which way there is more list, beside it rather than above and below: the
    // heading is where "above" would be.
    const float caretX = x + kSettingsWidth + 14.0f;
    if (scroll > 0) {
        drawLabelCentered("^", caretX, top + 11.0f, 0.5f, kInkDim, true);
    }
    if (scroll + visible < count) {
        drawLabelCentered("v", caretX, top + float(geometry.viewHeight) - 11.0f, 0.5f, kInkDim,
                          true);
    }
}

void Menu::drawOptions()
{
    drawLabelCentered("Options", kScreenWidth * 0.5f, 16.0f, 0.8f, kInk, true);

    char distance[32];
    std::snprintf(distance, sizeof(distance), "%d  (max %d)", renderDistance_, maxDistance_);

    char autosave[24];
    autosaveLabel(autosaveSeconds_, autosave, sizeof(autosave));

    // a1.1.2's own labels for a volume: a percentage, or the word OFF. Worth
    // copying exactly -- "0%" and "OFF" are the same number and different
    // sentences.
    char music[16];
    char effects[16];
    std::snprintf(music, sizeof(music), musicVolume_ > 0 ? "%d%%" : "OFF", musicVolume_);
    std::snprintf(effects, sizeof(effects), soundVolume_ > 0 ? "%d%%" : "OFF", soundVolume_);

    // a1.1.2's own label for the slider, `*yawn*` and `HYPERSPEED!!!` included.
    char sensitivity[24];
    settings::sensitivityLabel(lookSensitivity_, sensitivity, sizeof(sensitivity));

    SettingRowView rows[kOptCount];
    rows[kOptDistance] = {"Render distance:", distance, false};
    rows[kOptSensitivity] = {"Sensitivity:", sensitivity, false};
    rows[kOptPack] = {"Texture Pack:", packLabel(), false};
    rows[kOptSkin] = {"Skin:", skinLabel(), false};
    rows[kOptAudio] = {"Audio:", audioEnabled_ ? "On" : "Off", !audioEnabled_};
    rows[kOptMusic] = {"Music:", music, false};
    rows[kOptSound] = {"Sound:", effects, false};
    rows[kOptAutosave] = {"Autosave:", autosave, false};
    rows[kOptBack] = {"Back", nullptr, false};

    drawSettingsRows(rows, kOptionsGroups, kOptCount, optionsCursor_, optionsScroll_,
                     kOptionsTop);
}

const char* Menu::packLabel() const
{
    return packName_.empty() ? "Dev Art" : packName_.c_str();
}

// **Off the key rather than off the list**, because the Options row is drawn
// long before anything has listed the card -- and listing it to put a name on a
// button would be a full read of every zip for a label. The key already carries
// the name; this only has to drop the prefix and, for a file, the extension.
const char* Menu::skinLabel() const
{
    if (skinKey_.empty()) {
        return "Default";
    }
    const usize colon = skinKey_.find(':');
    if (colon == std::string::npos) {
        return skinKey_.c_str();
    }
    skinLabel_.assign(skinKey_, colon + 1, std::string::npos);
    if (skinKey_.compare(0, colon, "file") == 0) {
        const usize dot = skinLabel_.rfind('.');
        if (dot != std::string::npos) {
            skinLabel_.erase(dot);
        }
    }
    return skinLabel_.c_str();
}

void Menu::refreshSkins()
{
    // **Made, not just listed.** The console help on this screen tells the
    // player which folder to drop skins into, and a path that does not exist is
    // a worse instruction than one that does -- a card mounted on a PC shows an
    // empty `skins/` and there is nothing left to work out. It costs one
    // directory create on the way into a screen a player opens rarely.
    fs_.makeDirectories(texture::kSkinsDir);

    // The pack list first, because a skin row exists for every pack carrying a
    // `char.png` and `hasSkin` is filled in there. Costly on the console -- see
    // the header -- and paid once, on the way into this screen.
    refreshPacks();
    texture::listSkins(fs_, packs_, texture::kSkinsDir, &skins_);

    skinCursor_ = texture::findSkin(skins_, skinKey_);
    // A saved skin that is no longer on the card falls back to Default, and the
    // *setting* follows the fallback rather than being left pointing at
    // something that is gone.
    if (skinCursor_ == 0 && !skinKey_.empty()) {
        skinKey_.clear();
        saveSettings();
        applySavedSkin();
    }
    skinScroll_ = 0;
    if (skinCursor_ >= kVisibleRows) {
        skinScroll_ = skinCursor_ - kVisibleRows + 1;
    }
}

void Menu::applySavedSkin()
{
    if (atlas_.entityRgba.empty()) {
        return;
    }
    std::string path;
    bool fromPack = false;
    // Default: `buildEntitySkins` has already put the active pack's own
    // `char.png` -- or the black silhouette -- in the page, so there is nothing
    // to do and nothing to read.
    if (!texture::skinPathForKey(skinKey_, texture::kPacksDir, texture::kSkinsDir, &path,
                                 &fromPack)) {
        return;
    }
    if (!texture::applyPlayerSkin(fs_, path, fromPack, &atlas_.entityRgba)) {
        // The file went away between being chosen and being read. The page
        // keeps the Default that is already in it, which is a skin rather than
        // a hole, and the row will fall back the next time the screen is opened.
        return;
    }
    // The renderer holds its own upload of this sheet, so it has to be told
    // that the bytes behind it changed even though the pack did not.
    ++packRevision_;
}

void Menu::handleSkins(u32 down)
{
    const int rows = int(skins_.size());
    if (rows > 0) {
        skinCursor_ = step(down, skinCursor_, rows);
        if (skinCursor_ < skinScroll_) {
            skinScroll_ = skinCursor_;
        }
        if (skinCursor_ >= skinScroll_ + kVisibleRows) {
            skinScroll_ = skinCursor_ - kVisibleRows + 1;
        }
    }

    if (down & KEY_B) {
        message_ = nullptr;
        setScreen(Screen::Options);
        return;
    }
    if ((down & KEY_A) == 0 || rows == 0) {
        return;
    }
    playClick();

    // **The atlas is rebuilt rather than patched**, and that is not laziness.
    // The page currently holds whatever the *last* choice put there, so going
    // from one skin back to Default has nothing to restore it from: Default is
    // the active pack's own file, and the only thing that reads it is
    // `buildEntitySkins`. Rebuilding is one pack read on a button press a
    // player makes a handful of times.
    skinKey_ = skins_[usize(skinCursor_)].key;
    const std::string path =
        packName_.empty() ? std::string() : texture::packPath(texture::kPacksDir, packName_);
    texture::AtlasImage rebuilt;
    if (texture::buildAtlas(fs_, path, &rebuilt) == texture::PackError::Ok) {
        atlas_ = std::move(rebuilt);
        ++packRevision_;
    }
    applySavedSkin();
    saveSettings();
    consoleDirty_ = true;
}

void Menu::drawSkins()
{
    drawLabelCentered("Skin", kScreenWidth * 0.5f, 16.0f, 0.7f, kInk, true);

    const int rows = int(skins_.size());
    const float rowX = 20.0f;
    const float rowWidth = kScreenWidth - 2.0f * rowX;

    const int visible = drawListChrome(rows, skinScroll_);
    for (int i = 0; i < visible; ++i) {
        const int index = skinScroll_ + i;
        const texture::SkinEntry& skin = skins_[usize(index)];
        const Rect rect{rowX, kRowsTop + float(i) * (kRowHeight + kRowGap), rowWidth,
                        kRowHeight};
        drawButton(rect, "", index == skinCursor_, true);

        // Which skin is live, as a mark on the row: "selected" is the cursor,
        // and the player needs to see both at once. The same idiom the pack
        // list uses.
        if (skin.key == skinKey_) {
            drawLabel("*", rect.x + 8.0f, rect.y + 5.0f, 0.55f, kInkWarn, C2D_AlignLeft, true);
        }

        // **What the row says about itself**, and the slim mark is the one that
        // earns its place: a player whose Alex skin looks a texel too wide
        // should be told why on the screen that offered it, not left to guess.
        char detail[40];
        switch (skin.source) {
        case texture::SkinSource::Default:
            std::snprintf(detail, sizeof(detail), "%s",
                          packName_.empty() ? "black" : "texture pack");
            break;
        case texture::SkinSource::Pack:
        case texture::SkinSource::File:
            std::snprintf(detail, sizeof(detail), "%dx%d%s", skin.width, skin.height,
                          skin.model == texture::SkinModel::Slim ? "  slim" : "");
            break;
        }

        constexpr float kDetailWidth = 84.0f;
        drawLabelClipped(skin.name.c_str(), rect.x + 22.0f, rect.y + 3.0f, 0.55f, kInk,
                         rect.w - 32.0f - kDetailWidth);
        drawLabel(detail, rect.x + rect.w - 10.0f, rect.y + 6.0f, 0.4f, kInkDim,
                  C2D_AlignRight, true);
    }
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

// ---------------------------------------------------------------------------
// The bottom-screen previews
// ---------------------------------------------------------------------------

PreviewScreen Menu::previewScreenFor(Screen screen) const
{
    if (inGame_) {
        return PreviewScreen::None;
    }
    switch (screen) {
    case Screen::Skins:
        return PreviewScreen::Skins;
    case Screen::TexturePacks:
        return PreviewScreen::Packs;
    case Screen::Worlds:
    // **The same preview, pointed at the same world.** Move Panorama is
    // reached from World Settings, which is reached from the world list, so
    // `worldCursor_` is already on the world being moved -- and the picture the
    // player is aiming is then literally the picture the world list will show.
    case Screen::MovePanorama:
        return PreviewScreen::Worlds;
    default:
        return PreviewScreen::None;
    }
}

void Menu::syncPreview()
{
    if (preview_ == nullptr) {
        return;
    }
    const PreviewScreen screen = previewScreenFor(screen_);
    if (screen != PreviewScreen::None) {
        preview_->setActivePack(atlas_, packRevision_);
    }
    // The lists are handed over on the way into their screen, which is where
    // each of them is read -- never per frame.
    switch (screen) {
    case PreviewScreen::Skins:
        preview_->setSkinList(skins_);
        break;
    case PreviewScreen::Packs:
        preview_->setPackList(packs_);
        break;
    case PreviewScreen::Worlds:
        preview_->setWorldList(worlds_);
        break;
    case PreviewScreen::None:
        break;
    }
    preview_->setScreen(screen);
    if (preview_->screen() == PreviewScreen::None) {
        consoleDirty_ = true;
    }
}

void Menu::updatePreview()
{
    if (preview_ == nullptr || preview_->screen() == PreviewScreen::None) {
        previewClockMs_ = 0;
        return;
    }
    const u64 now = osGetTime();
    float seconds = previewClockMs_ == 0 ? 0.0f : float(now - previewClockMs_) / 1000.0f;
    if (seconds > 0.1f) {
        seconds = 0.1f;  // a stall is not a reason to jump the animation
    }
    previewClockMs_ = now;

    // A pack chosen on this screen changes the atlas under the table and the
    // Default skin; cheap when it has not.
    preview_->setActivePack(atlas_, packRevision_);
    preview_->setSkinCursor(skinCursor_);
    preview_->setPackCursor(packCursor_);
    preview_->setWorldCursor(worldCursor_);
    preview_->update(seconds);
}

void Menu::drawPreviewScreen()
{
    if (preview_ == nullptr || preview_->screen() == PreviewScreen::None
        || preview_->target() == nullptr) {
        return;
    }
    C3D_RenderTarget* bottom = preview_->target();

    // The top screen's batch goes first: from here on the bottom one is drawn.
    C2D_Flush();
    C2D_TargetClear(bottom, C2D_Color32(0x18, 0x14, 0x10, 0xFF));
    C2D_SceneBegin(bottom);
    // The backdrop writes colour and no depth, so every fragment of the
    // preview in front of it passes.
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    if (background_.ready()) {
        background_.draw(320.0f, 240.0f, 0.0f);
    }
    C2D_Flush();

    preview_->draw();

    // citro2d's state back over the preview's, for the labels and for the next
    // frame's top screen.
    prepare2D();
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C2D_SceneBegin(bottom);
    drawPreviewLabels();
    C2D_Flush();
}

void Menu::drawPreviewLabels()
{
    constexpr float kCentre = 160.0f;
    const char* name = nullptr;
    const char* note = nullptr;
    const char* hint = "A: Select   B: Back";
    char panoramaNote[32];

    switch (screen_) {
    case Screen::Skins:
        if (usize(skinCursor_) < skins_.size()) {
            const texture::SkinEntry& skin = skins_[usize(skinCursor_)];
            name = skin.name.c_str();
            note = skin.key == skinKey_ ? "In use" : nullptr;
        }
        hint = "A: Use   B: Back";
        break;
    case Screen::TexturePacks:
        if (packCursor_ == 0) {
            name = "Extract from a jar";
        } else if (usize(packCursor_ - 1) < packs_.size()) {
            const texture::PackEntry& pack = packs_[usize(packCursor_ - 1)];
            name = pack.builtIn ? "Dev Art" : pack.name.c_str();
            const bool active = pack.builtIn ? packName_.empty() : pack.name == packName_;
            note = active ? "In use" : nullptr;
        }
        hint = "A: Use   B: Back";
        break;
    case Screen::Worlds:
        if (worldCursor_ == kWorldRowCreate) {
            name = "New World";
            hint = "A: Create   B: Back";
        } else if (worldCursor_ == kWorldRowImport) {
            name = "Import World";
            hint = "A: Import   B: Back";
        } else if (usize(worldCursor_ - kWorldRowFirst) < worlds_.size()) {
            name = worlds_[usize(worldCursor_ - kWorldRowFirst)].name.c_str();
            hint = "A: Play   X: Settings   B: Back";
        }
        break;
    case Screen::MovePanorama:
        name = selectedWorldName_.c_str();
        // The tiles stream in again from the new corner after every step, so
        // the table is often half table for a moment. Saying which square it
        // is standing on is what makes that read as moving rather than as
        // broken.
        std::snprintf(panoramaNote, sizeof(panoramaNote), "tile %ld, %ld",
                      (long)panoramaTileX_, (long)panoramaTileZ_);
        note = panoramaNote;
        hint = "D-Pad: Move   A/START: Save   B: Cancel";
        break;
    default:
        return;
    }

    if (name != nullptr) {
        drawLabelCentered(name, kCentre, 12.0f, 0.6f, kInk, true);
    }
    if (note != nullptr) {
        drawLabelCentered(note, kCentre, 28.0f, 0.45f, kInkDim, true);
    }
    if (message_ != nullptr) {
        drawLabelCentered(message_, kCentre, 204.0f, 0.45f, kInkWarn, true);
    }
    drawLabelCentered(hint, kCentre, 226.0f, 0.45f, kInkDim, true);
}

// ---- multiplayer ------------------------------------------------------------

namespace {

// **The screen's shape.** Host and Join sit side by side at the top; the list
// starts well below them, with a rule between, because the two are different
// questions: the buttons start or find a session between two of these
// consoles, and the list is somewhere to go that already exists.
constexpr int kMpHost = 0;
constexpr int kMpJoin = 1;
constexpr int kMpFirstListRow = 2;

// The buttons, the line under them, and where the list begins: 40..66 for the
// two buttons, a caption centred at 74, the rule at 85, and four rows from 98
// ending at 222, which leaves the last line of the screen free.
constexpr float kMpButtonsTop = 40.0f;
constexpr float kMpRuleY = 85.0f;
constexpr float kMpListTop = 98.0f;
constexpr int kMpVisibleRows = 4;

enum class MpRow : u8 {
    Host,
    Join,
    Session,    // one found in the room
    AddServer,
    Server,     // one saved on the card
};

// What the row at `index` is, and its index within its own list.
MpRow mpRowAt(int index, int sessions, int* sub)
{
    *sub = 0;
    if (index <= kMpJoin) {
        return index == kMpHost ? MpRow::Host : MpRow::Join;
    }
    const int listRow = index - kMpFirstListRow;
    if (listRow < sessions) {
        *sub = listRow;
        return MpRow::Session;
    }
    if (listRow == sessions) {
        return MpRow::AddServer;
    }
    *sub = listRow - sessions - 1;
    return MpRow::Server;
}

}  // namespace

void Menu::refreshServers()
{
    net::loadServerList(fs_, net::kServerListPath, &servers_);
    if (username_.empty()) {
        username_ = ctr::loginName();
    }
    const int rows = multiplayerRows();
    if (serverCursor_ >= rows) {
        serverCursor_ = rows - 1;
    }
    const int listRow = serverCursor_ - kMpFirstListRow;
    if (serverScroll_ > listRow) {
        serverScroll_ = listRow > 0 ? listRow : 0;
    }
    consoleDirty_ = true;
}

void Menu::showDisconnected(const std::string& title, const std::string& detail)
{
    disconnectTitle_ = title;
    disconnectDetail_ = detail;
    refreshServers();
    // Not setScreen: this is called between visits, before init() has made
    // the things a screen change touches.
    screen_ = Screen::Disconnected;
    consoleDirty_ = true;
}

void Menu::showMultiplayer()
{
    refreshServers();
    screen_ = Screen::Multiplayer;
    consoleDirty_ = true;
}

bool Menu::askServerText(const char* hint, const std::string& current, int maxChars,
                         std::string* out, bool numeric)
{
    constexpr int kMaxText = 128;
    const int limit = maxChars < kMaxText - 1 ? maxChars : kMaxText - 1;

    SwkbdState swkbd;
    swkbdInit(&swkbd, numeric ? SWKBD_TYPE_NUMPAD : SWKBD_TYPE_NORMAL, 2, limit);
    swkbdSetInitialText(&swkbd, current.c_str());
    swkbdSetHintText(&swkbd, hint);
    swkbdSetFeatures(&swkbd, SWKBD_DARKEN_TOP_SCREEN);

    char text[kMaxText];
    // **The session is told before the applet starts, not after.** This
    // suspends the whole application for as long as somebody is typing; see
    // `ctr::linkPausing`.
    linkPausing(net::link::kAppletAwayMs);
    const SwkbdButton pressed = swkbdInputText(&swkbd, text, sizeof(text));

    // See askWorldName: the applet had both screens.
    consoleInit(GFX_BOTTOM, nullptr);
    consoleDirty_ = true;
    C2D_Prepare();

    if (pressed != SWKBD_BUTTON_CONFIRM) {
        return false;
    }
    std::string_view typed(text);
    while (!typed.empty() && (typed.front() == ' ' || typed.front() == '\t')) {
        typed.remove_prefix(1);
    }
    while (!typed.empty() && (typed.back() == ' ' || typed.back() == '\t')) {
        typed.remove_suffix(1);
    }
    *out = std::string(typed);
    return true;
}

bool Menu::handleMultiplayer(u32 down, MenuChoice* choice)
{
    const int rows = multiplayerRows();

    // **Two cursors' worth of movement in one.** The buttons are side by side
    // and the list is vertical, so Left and Right choose between Host and Join
    // while the cursor is up there, and Up and Down step through everything.
    // Down from either button lands on the first row of the list, which is the
    // whole point of the gap between them.
    const int before = serverCursor_;
    if (serverCursor_ < kMpFirstListRow) {
        if ((down & kLeft) != 0) {
            serverCursor_ = kMpHost;
        }
        if ((down & kRight) != 0) {
            serverCursor_ = kMpJoin;
        }
        if ((down & kDown) != 0 && rows > kMpFirstListRow) {
            serverCursor_ = kMpFirstListRow;
        }
        if ((down & kUp) != 0) {
            serverCursor_ = rows - 1;
        }
    } else {
        if ((down & kUp) != 0) {
            serverCursor_ = serverCursor_ == kMpFirstListRow ? kMpHost : serverCursor_ - 1;
        }
        if ((down & kDown) != 0) {
            serverCursor_ = serverCursor_ + 1 >= rows ? kMpHost : serverCursor_ + 1;
        }
    }
    if (serverCursor_ != before) {
        playMoveClick();
    }

    // The list scrolls under the buttons, which never move.
    const int listRow = serverCursor_ - kMpFirstListRow;
    if (listRow >= 0) {
        if (listRow < serverScroll_) {
            serverScroll_ = listRow;
        }
        if (listRow >= serverScroll_ + kMpVisibleRows) {
            serverScroll_ = listRow - kMpVisibleRows + 1;
        }
    }

    if (down & KEY_B) {
        message_ = nullptr;
        setScreen(Screen::Title);
        return false;
    }

    int sub = 0;
    const MpRow kind = mpRowAt(serverCursor_, int(sessions_.size()), &sub);

    if ((down & KEY_X) != 0 && kind == MpRow::Server) {
        playClick();
        message_ = nullptr;
        editServerIndex_ = sub;
        editServer_ = servers_[usize(sub)];
        editServerCursor_ = 0;
        setScreen(Screen::EditServer);
        return false;
    }

    if ((down & KEY_A) == 0) {
        return false;
    }
    playClick();
    message_ = nullptr;

    switch (kind) {
    case MpRow::Host:
    case MpRow::Join:
        netPurpose_ = kind == MpRow::Host ? NetPurpose::Host : NetPurpose::Join;
        netModeCursor_ = 0;
        setScreen(Screen::NetMode);
        return false;

    case MpRow::Session: {
        const LocalSession& session = sessions_[usize(sub)];
        if (!session.compatible) {
            message_ = "that console is running a different version of 3DAlpha";
            consoleDirty_ = true;
            return false;
        }
        if (session.maxPlayers != 0 && session.players >= session.maxPlayers) {
            message_ = "that session is full";
            consoleDirty_ = true;
            return false;
        }
        beginLocalJoin(sub);
        return false;
    }

    case MpRow::AddServer:
        editServerIndex_ = -1;
        editServer_ = net::ServerEntry{"Minecraft Server", std::string(), net::kDefaultPort};
        // A new row's address is the thing it does not have yet.
        editServerCursor_ = 1;
        setScreen(Screen::EditServer);
        return false;

    case MpRow::Server:
        break;
    }

    const net::ServerEntry& entry = servers_[usize(sub)];
    std::string host;
    u16 port = entry.port;
    if (!net::parseAddress(entry.address, &host, &port, entry.port)) {
        message_ = "that address is not a host or host:port";
        consoleDirty_ = true;
        return false;
    }

    choice->action = MenuChoice::Action::Join;
    choice->link = MenuChoice::Link::Internet;
    choice->serverName = entry.name.empty() ? entry.address : entry.name;
    choice->serverHost = host;
    choice->serverPort = port;
    choice->username = username_;
    choice->worldName = choice->serverName;
    choice->worldPath.clear();
    choice->created = false;
    // What a protocol-2 client is: it breaks blocks at their real speed, and it
    // cannot be hurt because nothing in the protocol carries health.
    choice->gamemode = settings::Gamemode::Survival;
    return true;
}

// Local or Internet, for whichever button asked. **Internet is a row rather
// than nothing at all** because a player who has just pressed Host deserves to
// be told that the answer is "not yet" rather than left to guess why only one
// choice exists.
//
// **Four buttons ask it now, not two.** Import and Export are not multiplayer
// and have nothing to do with a session, but the question in front of them is
// word for word the one Host and Join ask -- which radio -- so they ask it on
// this screen rather than on a second one that would have to be kept in step.
bool Menu::handleNetMode(u32 down, MenuChoice* choice)
{
    (void)choice;
    netModeCursor_ = step(down, netModeCursor_, 2);

    if (down & KEY_B) {
        message_ = nullptr;
        // Back to whichever screen asked. A transfer is reached from the world
        // list and from a world's own settings, neither of which is anywhere
        // near the multiplayer screen.
        switch (netPurpose_) {
        case NetPurpose::Import: setScreen(Screen::Worlds); break;
        case NetPurpose::Export: setScreen(Screen::WorldSettings); break;
        default:                 setScreen(Screen::Multiplayer); break;
        }
        return false;
    }
    if ((down & KEY_A) == 0) {
        return false;
    }
    playClick();

    if (netModeCursor_ == 1) {
        switch (netPurpose_) {
        case NetPurpose::Host:
            message_ = "Hosting over the internet is not in this build yet.";
            break;
        case NetPurpose::Import:
        case NetPurpose::Export:
            message_ = "Sending a world over the internet is not in this build yet.";
            break;
        default:
            message_ = "3DAlpha sessions over the internet are not in this build yet.";
            break;
        }
        consoleDirty_ = true;
        return false;
    }

    if (!localWirelessReady() && !startLocalWireless(&localError_)) {
        message_ = localError_.c_str();
        consoleDirty_ = true;
        return false;
    }

    message_ = nullptr;
    if (netPurpose_ == NetPurpose::Export) {
        startExport();
        return false;
    }
    if (netPurpose_ == NetPurpose::Import) {
        // The scan happens on the next frame, with "Searching" already on
        // screen, for the reason the join scan below gives.
        offers_.clear();
        offerCursor_ = 0;
        offerScanPending_ = true;
        message_ = "Searching for a world nearby...";
        setScreen(Screen::ImportScan);
        return false;
    }
    if (netPurpose_ == NetPurpose::Host) {
        // The world list, doing the other job it can do: choosing which of
        // this console's worlds the others are going to be playing in.
        pickingHost_ = true;
        refreshWorlds();
        setScreen(Screen::Worlds);
        return false;
    }

    // The scan happens on the next frame, with "Searching" already on screen:
    // the radio takes about a second and a menu that freezes without saying
    // why looks like a menu that has crashed.
    scanPending_ = true;
    message_ = "Searching for sessions nearby...";
    setScreen(Screen::Multiplayer);
    return false;
}

void Menu::refreshLocalSessions()
{
    message_ = nullptr;
    localError_.clear();
    std::vector<LocalSession> found;
    if (!scanLocalSessions(&found, &localError_)) {
        message_ = localError_.c_str();
        sessions_.clear();
    } else {
        // **Games only.** The same scan finds consoles that have pressed Export
        // and are waiting to hand a world over; those are not sessions and
        // there is nothing on this screen that could do anything with one. See
        // `refreshWorldOffers` for the other half of the same filter.
        sessions_.clear();
        for (LocalSession& session : found) {
            if (session.kind == LocalKind::Session) {
                sessions_.push_back(std::move(session));
            }
        }
        message_ = sessions_.empty() ? "No sessions nearby. Ask them to press Host." : nullptr;
    }
    scanned_ = true;
    serverCursor_ = sessions_.empty() ? kMpJoin : kMpFirstListRow;
    serverScroll_ = 0;
    consoleDirty_ = true;
}

void Menu::beginLocalJoin(int index)
{
    const LocalSession session = sessions_[usize(index)];
    if (!guest_) {
        guest_ = std::make_unique<GuestPlay>();
    }
    message_ = nullptr;
    localError_.clear();
    if (username_.empty()) {
        username_ = ctr::loginName();
    }

    if (!guest_->join(session, username_, &localError_)) {
        message_ = localError_.c_str();
        consoleDirty_ = true;
        return;
    }
    message_ = nullptr;
    setScreen(Screen::Session);
}

// Once a frame, whatever the player is pressing: a link that is not read is a
// link that times out.
void Menu::pumpSession()
{
    if (screen_ != Screen::Session || !guest_ || !guest_->active()) {
        return;
    }
    if (guest_->pumpLobby()) {
        consoleDirty_ = true;
    }
    if (guest_->finished()) {
        const std::string reason = guest_->reason();
        guest_->leave("the session ended");
        showDisconnected("Session ended", reason);
    }
}

void Menu::endSession(const std::string& reason)
{
    if (guest_) {
        guest_->leave(reason);
        guest_.reset();
    }
}

bool Menu::handleSession(u32 down, MenuChoice* choice)
{
    // **The lobby ends by itself.** Nothing on this screen starts the game: the
    // host decides when a guest gets a world, and the moment its Login arrives
    // there is one to be in. See GuestPlay::ready.
    if (guest_ && guest_->ready()) {
        playClick();
        choice->action = MenuChoice::Action::Join;
        choice->link = MenuChoice::Link::Local;
        choice->worldName = guest_->worldName();
        choice->worldPath.clear();
        choice->serverName = guest_->hostName();
        choice->username = username_;
        choice->created = false;
        // **The host's world is played the host's way.** Without this a joiner
        // gets `MenuChoice`'s default, which is Spectator -- the right answer
        // for a world with no settings file and the wrong one for somebody
        // joining a world that has an owner. See `net::link::WorldRules`.
        choice->gamemode = guest_->gamemode();
        choice->difficulty = guest_->difficulty();
        multiplayer_ = true;
        message_ = nullptr;
        setScreen(Screen::Multiplayer);
        return true;
    }

    if ((down & (KEY_B | KEY_START)) == 0) {
        return false;
    }
    playClick();
    endSession("left the session");
    message_ = nullptr;
    setScreen(Screen::Multiplayer);
    return false;
}

void Menu::drawSession()
{
    const GuestPlay* session = guest_.get();
    const bool joining = session == nullptr
                         || session->state() == net::link::GuestSession::State::Joining;

    drawLabelCentered(joining ? "Joining..." : session->worldName().c_str(),
                      kScreenWidth * 0.5f, 16.0f, 0.7f, kInk, true);

    if (session != nullptr && !joining) {
        char line[128];
        std::snprintf(line, sizeof(line), "hosted by %s   %d in the session   %u ms",
                      session->hostName().c_str(), session->playerCount(),
                      unsigned(session->rttMs()));
        drawLabelCentered(line, kScreenWidth * 0.5f, 40.0f, 0.45f, kInkDim, true);
    }

    // **What this console is waiting for.** The link is up and the two consoles
    // are talking; what has not happened yet is the host deciding to send a
    // world, which is the moment this screen goes away by itself.
    drawLabelCentered(joining ? "asking the other console to let this one in"
                              : "connected -- waiting for the host to send the world",
                      kScreenWidth * 0.5f, 64.0f, 0.42f, kInkWarn, true);

    if (!joining && session != nullptr && session->terrainAnswered() != 0) {
        char work[96];
        std::snprintf(work, sizeof(work), "generating terrain for the host: %u columns",
                      unsigned(session->terrainAnswered()));
        drawLabelCentered(work, kScreenWidth * 0.5f, 82.0f, 0.42f, kInkDim, true);
    }

    float y = 96.0f;
    if (session != nullptr) {
        for (const std::string& text : session->lines()) {
            drawLabelCentered(text.c_str(), kScreenWidth * 0.5f, y, 0.45f, kInk, true);
            y += 18.0f;
        }
    }

    drawButton(Rect{(kScreenWidth - kButtonWidth) * 0.5f, 202.0f, kButtonWidth, kButtonHeight},
               "B  Leave the session", true, true);
}

// ---------------------------------------------------------------------------
// Import and Export: one world crossing the room.
//
// **Two screens for four states**, because a transfer is symmetrical. The
// exporting console puts a beacon up and waits; the importing one scans, picks
// a beacon and connects. After that both are watching the same bar, and the
// only thing that differs between them is which one is reading the card. See
// platform/ctr/world_transfer.hpp.

namespace {

// How many consoles offering a world the Import screen shows. It is the height
// of the list, not a limit on the radio: see the note in `refreshWorldOffers`.
constexpr int kMaxOfferRows = 5;

}  // namespace

void Menu::openImport()
{
    // **The name first, and on a keyboard, before the radio exists.** An applet
    // suspends this console outright -- see `ctr::linkPausing` -- and a link
    // that has not been opened yet cannot be told to expect it. So the one
    // question a player has to type the answer to is asked here, with nothing
    // on the air.
    std::string name;
    if (!askImportName(&name)) {
        return;
    }
    importName_ = name;
    message_ = nullptr;
    netPurpose_ = NetPurpose::Import;
    netModeCursor_ = 0;
    setScreen(Screen::NetMode);
}

bool Menu::askImportName(std::string* out)
{
    constexpr int kMaxText = 64;

    // The first free "World<n>", for the reason `askCopyName` gives: the
    // validator refuses a name already on the card, and the name the other
    // console uses is one this card may well already have.
    char initial[kMaxText];
    const std::string suggestion = world::defaultWorldName(worlds_);
    std::snprintf(initial, sizeof(initial), "%s", suggestion.c_str());

    SwkbdState swkbd;
    swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, kMaxText - 1);
    swkbdSetInitialText(&swkbd, initial);
    swkbdSetHintText(&swkbd, "Name for the world you are importing");
    swkbdSetFeatures(&swkbd, SWKBD_DARKEN_TOP_SCREEN);
    swkbdSetValidation(&swkbd, SWKBD_NOTEMPTY_NOTBLANK, SWKBD_FILTER_CALLBACK, 0);
    // The same validation a new world and a copy use, so an imported world
    // cannot land on a name either of those could not have.
    gExistingWorlds = &worlds_;
    swkbdSetFilterCallback(&swkbd, validateWorldName, nullptr);

    char text[kMaxText];
    linkPausing(net::link::kAppletAwayMs);
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

void Menu::refreshWorldOffers()
{
    message_ = nullptr;
    localError_.clear();
    offers_.clear();
    offerCursor_ = 0;

    std::vector<LocalSession> found;
    if (!scanLocalSessions(&found, &localError_)) {
        message_ = localError_.c_str();
        consoleDirty_ = true;
        return;
    }
    // **Only the consoles offering a world.** The same scan finds game
    // sessions, and a player who pressed Import is not looking for one.
    //
    // Cut to what the screen draws rather than scrolled: a room with more than
    // five consoles all waiting to hand a world over is not a case worth a
    // scrollbar, and a row the cursor could reach but the screen could not show
    // would be worse than one that is not offered.
    for (LocalSession& session : found) {
        if (session.kind == LocalKind::WorldOffer && offers_.size() < usize(kMaxOfferRows)) {
            offers_.push_back(std::move(session));
        }
    }
    message_ = offers_.empty() ? "No world nearby. Ask them to press Export." : nullptr;
    consoleDirty_ = true;
}

void Menu::startExport()
{
    if (inGame_ || selectedWorldPath_.empty()) {
        return;
    }

    // The diorama reads this world off the card on its own thread, and so does
    // the size scan. Both are stopped for the same reason a copy stops them:
    // the card serves one reader faster than three.
    if (preview_ != nullptr) {
        preview_->quiesce(selectedWorldPath_);
    }
    sizeScan_.cancel();

    if (username_.empty()) {
        username_ = ctr::loginName();
    }

    transfer_ = std::make_unique<WorldTransfer>(fs_);
    localError_.clear();
    if (!transfer_->offer(selectedWorldPath_, selectedWorldName_, username_, &localError_)) {
        transfer_.reset();
        message_ = localError_.c_str();
        consoleDirty_ = true;
        setScreen(Screen::WorldSettings);
        return;
    }
    message_ = nullptr;
    setScreen(Screen::Transfer);
}

void Menu::startImport(int index)
{
    if (usize(index) >= offers_.size()) {
        return;
    }
    const LocalSession offer = offers_[usize(index)];
    if (!offer.compatible) {
        message_ = "that console is on a different build of 3DAlpha";
        consoleDirty_ = true;
        return;
    }

    transfer_ = std::make_unique<WorldTransfer>(fs_);
    localError_.clear();
    if (!transfer_->accept(offer, kSavesDir, importName_, &localError_)) {
        transfer_.reset();
        message_ = localError_.c_str();
        consoleDirty_ = true;
        return;
    }
    message_ = nullptr;
    setScreen(Screen::Transfer);
}

void Menu::pumpTransfer()
{
    if (screen_ != Screen::Transfer || !transfer_) {
        return;
    }
    if (transfer_->pump()) {
        consoleDirty_ = true;
    }
}

void Menu::endTransfer(const char* why)
{
    const bool imported = transfer_ && !transfer_->sending() && transfer_->succeeded();
    message_ = nullptr;
    if (transfer_) {
        transfer_->stop(why);
        // **Carried off this screen into the next one.** The reason lives in
        // the transfer, which is about to be destroyed, and `message_` is a
        // borrowed pointer -- so it is copied into the member that already
        // outlives every screen that shows one.
        if (!transfer_->succeeded() && !transfer_->reason().empty()) {
            localError_ = transfer_->reason();
            message_ = localError_.c_str();
        }
        transfer_.reset();
    }

    if (imported) {
        // The world arrived, so the list has one more row in it -- and the
        // cursor should be on the new world rather than where it was.
        message_ = nullptr;
        worldCursorName_ = importName_;
        refreshWorlds();
        setScreen(Screen::Worlds);
        return;
    }
    // An export goes back to the world it was about. **An import that did not
    // arrive goes back to the list of offers, not to the world list**: the name
    // it was going to have is still typed in, the other console is still on the
    // air, and that screen is one of the two that can actually show the reason
    // -- the world list's bottom screen is the diorama and has nowhere to put
    // a line of text.
    setScreen(netPurpose_ == NetPurpose::Export ? Screen::WorldSettings : Screen::ImportScan);
}

void Menu::handleImportScan(u32 down)
{
    const int rows = int(offers_.size());
    if (rows > 0) {
        const int before = offerCursor_;
        offerCursor_ = step(down, offerCursor_, rows);
        if (offerCursor_ != before) {
            playMoveClick();
        }
    }

    if (down & KEY_B) {
        message_ = nullptr;
        setScreen(Screen::Worlds);
        return;
    }
    // X looks again rather than B-and-back-in: a console that has just pressed
    // Export in the next room is one scan away, and leaving the screen to find
    // it would mean typing the name again.
    if ((down & KEY_X) != 0) {
        playClick();
        offerScanPending_ = true;
        message_ = "Searching for a world nearby...";
        consoleDirty_ = true;
        return;
    }
    if ((down & KEY_A) != 0 && rows > 0) {
        playClick();
        startImport(offerCursor_);
    }
}

void Menu::handleTransfer(u32 down)
{
    if (!transfer_) {
        setScreen(Screen::Worlds);
        return;
    }

    // **The screen stays up when the transfer ends.** A bar that vanished the
    // moment the last file landed would never have said whether it worked, and
    // on the sending console it is the only place the far end's verdict is ever
    // shown. A press is what closes it.
    if ((down & (KEY_A | KEY_B | KEY_START)) == 0) {
        return;
    }
    if (transfer_->finished()) {
        playClick();
        endTransfer("the transfer was closed");
        return;
    }
    // A is not a way out of a transfer that is still going: the one button that
    // stops one is the one that means "back" everywhere else.
    if ((down & (KEY_B | KEY_START)) == 0) {
        return;
    }
    playClick();
    endTransfer("the other console cancelled");
}

void Menu::drawImportScan()
{
    drawLabelCentered("Import World", kScreenWidth * 0.5f, 16.0f, 0.7f, kInk, true);

    char subtitle[128];
    std::snprintf(subtitle, sizeof(subtitle), "it will be called \"%s\" on this console",
                  importName_.c_str());
    drawLabelCentered(subtitle, kScreenWidth * 0.5f, 38.0f, 0.42f, kInkDim, true);

    const float rowX = 20.0f;
    const float rowWidth = kScreenWidth - 2.0f * rowX;
    const int rows = int(offers_.size());
    for (int i = 0; i < rows; ++i) {
        const LocalSession& offer = offers_[usize(i)];
        const Rect rect{rowX, 60.0f + float(i) * (kRowHeight + kRowGap), rowWidth, kRowHeight};
        drawButton(rect, "", i == offerCursor_, offer.compatible);
        drawLabelClipped(offer.worldName.c_str(), rect.x + 10.0f, rect.y + 3.0f, 0.55f,
                         offer.compatible ? kInk : kInkDim, rect.w - 140.0f);
        drawLabel(offer.compatible ? offer.hostName.c_str() : "different build",
                  rect.x + rect.w - 10.0f, rect.y + 6.0f, 0.4f,
                  offer.compatible ? kInkDim : kInkWarn, C2D_AlignRight, true);
    }

    if (rows == 0) {
        drawLabelCentered("Nothing is being offered in this room.", kScreenWidth * 0.5f,
                          100.0f, 0.45f, kInkDim, true);
        drawLabelCentered("On the other console: World Settings -> Export.",
                          kScreenWidth * 0.5f, 122.0f, 0.42f, kInkDim, true);
    }

    drawButton(Rect{(kScreenWidth - kButtonWidth) * 0.5f, 202.0f, kButtonWidth, kButtonHeight},
               "X  Look again", rows == 0, true);

    if (message_ != nullptr) {
        drawLabelCentered(message_, kScreenWidth * 0.5f, 190.0f, 0.42f, kInkWarn, true);
    }
}

void Menu::drawTransfer()
{
    const bool sending = transfer_ && transfer_->sending();
    drawLabelCentered(sending ? "Export World" : "Import World", kScreenWidth * 0.5f, 16.0f,
                      0.7f, kInk, true);

    if (!transfer_) {
        return;
    }
    const net::copy::Progress progress = transfer_->progress();

    drawLabelCentered(transfer_->worldName().c_str(), kScreenWidth * 0.5f, 42.0f, 0.55f, kInk,
                      true);
    // **What it will be called here**, which is not what the other console
    // calls it: the name was typed on this card, against this card's world
    // list, and the line above is the sending console's own.
    if (!sending && !importName_.empty() && importName_ != transfer_->worldName()) {
        char as[96];
        std::snprintf(as, sizeof(as), "saved here as \"%s\"", importName_.c_str());
        drawLabelCentered(as, kScreenWidth * 0.5f, 84.0f, 0.4f, kInkDim, true);
    }

    // **What the console is doing, in the words of the thing it is waiting
    // for.** "Offering" on an export is a console sitting on the air with
    // nobody there yet, and saying so is what stops it reading as a hang.
    const char* state = net::copy::describeStage(progress.stage);
    if (sending && (progress.stage == net::copy::Stage::Offering
                    || progress.stage == net::copy::Stage::Waiting)) {
        state = "Waiting for the other console to take it";
    }
    drawLabelCentered(state, kScreenWidth * 0.5f, 66.0f, 0.45f,
                      progress.stage == net::copy::Stage::Failed ? kInkWarn : kInkDim, true);

    // The bar. Files rather than bytes when the total is not known yet, which
    // on the receiving end is everything before the offer lands.
    const float barX = 40.0f;
    const float barW = kScreenWidth - 2.0f * barX;
    const Rect bar{barX, 100.0f, barW, 18.0f};
    drawButton(bar, "", false, false);
    if (progress.bytesTotal > 0) {
        const double done = double(progress.bytesDone) / double(progress.bytesTotal);
        const float filled = float(done > 1.0 ? 1.0 : done) * (barW - 4.0f);
        if (filled > 0.0f) {
            C2D_DrawRectSolid(barX + 2.0f, 102.0f, 0.3f, filled, 14.0f, kFillSelected);
        }
    }

    char line[128];
    if (progress.bytesTotal > 0) {
        char done[24];
        char total[24];
        formatBytes(progress.bytesDone, done, sizeof(done));
        formatBytes(progress.bytesTotal, total, sizeof(total));
        std::snprintf(line, sizeof(line), "%s of %s   %u / %u files", done, total,
                      unsigned(progress.filesDone), unsigned(progress.filesTotal));
    } else {
        std::snprintf(line, sizeof(line), "nothing has crossed yet");
    }
    drawLabelCentered(line, kScreenWidth * 0.5f, 126.0f, 0.42f, kInkDim, true);

    // **The promise, said on the screen it is about.** The world being sent is
    // only ever read; what moves is the copy.
    drawLabelCentered(sending ? "This world stays on this console, untouched."
                              : "The world stays on the other console too.",
                      kScreenWidth * 0.5f, 150.0f, 0.4f, kInkDim, true);

    if (transfer_->finished()) {
        if (transfer_->succeeded()) {
            drawLabelCentered(sending ? "Sent." : "Imported.", kScreenWidth * 0.5f, 174.0f,
                              0.5f, kInk, true);
        } else {
            drawLabelCentered(transfer_->reason().empty() ? "The transfer stopped."
                                                          : transfer_->reason().c_str(),
                              kScreenWidth * 0.5f, 174.0f, 0.42f, kInkWarn, true);
        }
    }

    drawButton(Rect{(kScreenWidth - kButtonWidth) * 0.5f, 202.0f, kButtonWidth, kButtonHeight},
               transfer_->finished() ? "A  Done" : "B  Stop", true, true);
}

int Menu::multiplayerRows() const
{
    // The two buttons, the sessions a scan found, "+ Add Server", and the
    // saved ones.
    return kMpFirstListRow + int(sessions_.size()) + 1 + int(servers_.size());
}

namespace {

// EditServer's rows. **Port sits under Address** because that is the order the
// two are read in, and it carries 25565 rather than being blank: a server that
// has not been moved off the default is the ordinary case, and a row a player
// never has to touch should say what it is doing anyway.
//
// A new server has no Delete, so its Cancel is the fifth.
constexpr int kEditName = 0;
constexpr int kEditAddress = 1;
constexpr int kEditPort = 2;
constexpr int kEditSave = 3;
constexpr int kEditDelete = 4;
constexpr int kEditCancel = 5;

int editRowAction(int row, bool existing)
{
    return !existing && row >= kEditDelete ? row + 1 : row;
}

}  // namespace

void Menu::handleEditServer(u32 down)
{
    const bool existing = editServerIndex_ >= 0;
    const int rows = existing ? 6 : 5;
    editServerCursor_ = step(down, editServerCursor_, rows);

    if (down & KEY_B) {
        message_ = nullptr;
        setScreen(Screen::Multiplayer);
        return;
    }
    if ((down & KEY_A) == 0) {
        return;
    }
    playClick();

    std::string text;
    std::string host;
    u16 port = 0;
    char typed[16];
    switch (editRowAction(editServerCursor_, existing)) {
    case kEditName:
        if (askServerText("Server name", editServer_.name, 32, &text)) {
            editServer_.name = text;
        }
        break;
    case kEditAddress:
        if (askServerText("Address: host, or host:port", editServer_.address, 96, &text)) {
            // **A `host:port` typed in here fills both rows.** An address is
            // copied off a forum post in one piece, and taking it apart by
            // hand on a touch keyboard is the work the two rows exist to save,
            // not work to hand back to the player.
            port = editServer_.port;
            if (net::parseAddress(text, &host, &port)) {
                editServer_.address = host;
                editServer_.port = port;
                message_ = nullptr;
            } else {
                editServer_.address = text;
                message_ = "that is not a host or host:port";
            }
        }
        break;
    case kEditPort:
        std::snprintf(typed, sizeof(typed), "%u", unsigned(editServer_.port));
        if (askServerText("Port (25565 by default)", typed, 5, &text, true)) {
            message_ = net::parsePort(text, &editServer_.port) ? nullptr
                                                               : "a port is 1 to 65535";
        }
        break;
    case kEditSave:
        if (!net::parseAddress(editServer_.address, &host, &port, editServer_.port)) {
            message_ = "that is not a host or host:port";
            break;
        }
        // The address row may still be holding a `host:port` the player typed
        // and never left; splitting it here is what Save means by "as shown".
        editServer_.address = host;
        editServer_.port = port;
        if (editServer_.name.empty()) {
            editServer_.name = editServer_.address;
        }
        if (existing && usize(editServerIndex_) < servers_.size()) {
            servers_[usize(editServerIndex_)] = editServer_;
        } else {
            servers_.push_back(editServer_);
            // Onto the row it just made, which is the last one on the screen.
            serverCursor_ = multiplayerRows() - 1;
            const int listRow = serverCursor_ - kMpFirstListRow;
            if (listRow >= serverScroll_ + kMpVisibleRows) {
                serverScroll_ = listRow - kMpVisibleRows + 1;
            }
        }
        fs_.makeDirectories(kRootDir);
        message_ = net::saveServerList(fs_, net::kServerListPath, servers_)
                       ? nullptr
                       : "could not write the server list";
        setScreen(Screen::Multiplayer);
        break;
    case kEditDelete:
        setScreen(Screen::ConfirmDeleteServer);
        break;
    case kEditCancel:
    default:
        message_ = nullptr;
        setScreen(Screen::Multiplayer);
        break;
    }
    consoleDirty_ = true;
}

void Menu::handleConfirmDeleteServer(u32 down)
{
    if (down & KEY_B) {
        setScreen(Screen::EditServer);
        return;
    }
    if ((down & KEY_A) == 0) {
        return;
    }
    playClick();

    if (editServerIndex_ >= 0 && usize(editServerIndex_) < servers_.size()) {
        servers_.erase(servers_.begin() + editServerIndex_);
        message_ = net::saveServerList(fs_, net::kServerListPath, servers_)
                       ? nullptr
                       : "could not write the server list";
    }
    // Back onto the list rather than up on the buttons: the row that was being
    // deleted was down here.
    editServerIndex_ = -1;
    serverCursor_ = kMpFirstListRow;
    serverScroll_ = 0;
    setScreen(Screen::Multiplayer);
}

void Menu::handleDisconnected(u32 down)
{
    if ((down & (KEY_A | KEY_B)) == 0) {
        return;
    }
    playClick();
    refreshServers();
    setScreen(Screen::Multiplayer);
}

void Menu::drawMultiplayer()
{
    // `gc`'s own heading.
    drawLabelCentered("Play Multiplayer", kScreenWidth * 0.5f, 16.0f, 0.7f, kInk, true);

    // The two buttons, side by side and above everything else.
    const float half = (kButtonWidth + 40.0f) * 0.5f;
    drawButton(Rect{kScreenWidth * 0.5f - half - 6.0f, kMpButtonsTop, half, kButtonHeight},
               "Host Game", serverCursor_ == kMpHost, true);
    drawButton(Rect{kScreenWidth * 0.5f + 6.0f, kMpButtonsTop, half, kButtonHeight},
               "Join Game", serverCursor_ == kMpJoin, true);
    drawLabelCentered("start or find a session on another 3DS", kScreenWidth * 0.5f,
                      kMpButtonsTop + kButtonHeight + 8.0f, 0.4f, kInkDim, true);

    // **The rule is the separation the list was asked for.** Everything above
    // it is a session between two consoles; everything below it is a list of
    // somewhere to go.
    C2D_DrawRectSolid(28.0f, kMpRuleY, 0.3f, kScreenWidth - 56.0f, 1.0f, kBevelDark);
    C2D_DrawRectSolid(28.0f, kMpRuleY + 1.0f, 0.3f, kScreenWidth - 56.0f, 1.0f, kBevelLight);

    const int rows = multiplayerRows();
    const int listRows = rows - kMpFirstListRow;
    const float rowX = 20.0f;
    const float rowWidth = kScreenWidth - 2.0f * rowX;

    for (int i = 0; i < kMpVisibleRows; ++i) {
        const int listRow = serverScroll_ + i;
        if (listRow >= listRows) {
            break;
        }
        const int index = listRow + kMpFirstListRow;
        const Rect rect{rowX, kMpListTop + float(i) * (kRowHeight + kRowGap), rowWidth,
                        kRowHeight};
        const bool selected = index == serverCursor_;

        int sub = 0;
        const MpRow kind = mpRowAt(index, int(sessions_.size()), &sub);
        if (kind == MpRow::AddServer) {
            drawButton(rect, "+ Add Server", selected, true);
            continue;
        }

        drawButton(rect, "", selected, true);
        if (kind == MpRow::Session) {
            // A session's row says the world on the left and who is hosting it
            // and how full it is on the right, which is the same shape a saved
            // server's row has and the same shape a world's row has.
            const LocalSession& session = sessions_[usize(sub)];
            constexpr float kHostWidth = 150.0f;
            char who[96];
            if (session.compatible) {
                std::snprintf(who, sizeof(who), "%s  %u/%u", session.hostName.c_str(),
                              unsigned(session.players), unsigned(session.maxPlayers));
            } else {
                std::snprintf(who, sizeof(who), "another version");
            }
            drawLabelClipped(session.worldName.c_str(), rect.x + 10.0f, rect.y + 3.0f, 0.55f,
                             session.compatible ? kInk : kInkDim,
                             rect.w - 30.0f - kHostWidth);
            drawLabelClipped(who, rect.x + rect.w - 10.0f - kHostWidth, rect.y + 6.0f, 0.4f,
                             session.compatible ? kInkDim : kInkWarn, kHostWidth);
            continue;
        }

        // The name where a world's name goes, the address where its date does.
        const net::ServerEntry& entry = servers_[usize(sub)];
        constexpr float kAddressWidth = 150.0f;
        // **The port is shown only when it is not the default**, because a row
        // reading ":25565" on every line says nothing and costs the host name
        // the width it needs.
        char address[128];
        if (entry.port == net::kDefaultPort) {
            std::snprintf(address, sizeof(address), "%s", entry.address.c_str());
        } else {
            std::snprintf(address, sizeof(address), "%s:%u", entry.address.c_str(),
                          unsigned(entry.port));
        }
        drawLabelClipped(entry.name.c_str(), rect.x + 10.0f, rect.y + 3.0f, 0.55f, kInk,
                         rect.w - 30.0f - kAddressWidth);
        drawLabelClipped(address, rect.x + rect.w - 10.0f - kAddressWidth, rect.y + 6.0f, 0.4f,
                         kInkDim, kAddressWidth);
    }

    if (serverScroll_ > 0) {
        drawLabelCentered("^", kScreenWidth * 0.5f, kMpListTop - 10.0f, 0.5f, kInkDim, true);
    }
    // The last line of the screen belongs to whichever of the two has more to
    // say: a message the player needs, or the arrow saying the list goes on.
    if (message_ != nullptr) {
        drawLabelCentered(message_, kScreenWidth * 0.5f, kScreenHeight - 12.0f, 0.42f, kInkWarn,
                          true);
    } else if (serverScroll_ + kMpVisibleRows < listRows) {
        drawLabelCentered("v", kScreenWidth * 0.5f, kScreenHeight - 10.0f, 0.5f, kInkDim,
                          true);
    }
}

void Menu::drawNetMode()
{
    const char* title = "Join a Session";
    const char* question = "Where should this console look?";
    switch (netPurpose_) {
    case NetPurpose::Host:
        title = "Host a Session";
        question = "Who should be able to reach this world?";
        break;
    case NetPurpose::Import:
        title = "Import World";
        question = "Where is the world coming from?";
        break;
    case NetPurpose::Export:
        title = "Export World";
        question = "Where should this world go?";
        break;
    default:
        break;
    }
    drawLabelCentered(title, kScreenWidth * 0.5f, 16.0f, 0.7f, kInk, true);
    drawLabelCentered(question, kScreenWidth * 0.5f, 44.0f, 0.45f, kInkDim, true);

    const float x = (kScreenWidth - kButtonWidth) * 0.5f;
    drawButton(Rect{x, 76.0f, kButtonWidth, kButtonHeight}, "Local", netModeCursor_ == 0,
               true);
    drawLabelCentered("another 3DS in the same room", kScreenWidth * 0.5f, 114.0f, 0.4f,
                      kInkDim, true);

    // Drawn disabled rather than hidden: the row is what says the answer is
    // "not yet" instead of "never".
    drawButton(Rect{x, 136.0f, kButtonWidth, kButtonHeight}, "Internet", netModeCursor_ == 1,
               false);
    drawLabelCentered("not in this build yet", kScreenWidth * 0.5f, 174.0f, 0.4f, kInkDim,
                      true);

    if (message_ != nullptr) {
        drawLabelCentered(message_, kScreenWidth * 0.5f, 208.0f, 0.42f, kInkWarn, true);
    }
}

void Menu::drawEditServer()
{
    const bool existing = editServerIndex_ >= 0;
    drawLabelCentered(existing ? "Edit Server" : "Add Server", kScreenWidth * 0.5f, 16.0f, 0.7f,
                      kInk, true);

    const int rows = existing ? 6 : 5;
    const float rowX = 40.0f;
    const float rowWidth = kScreenWidth - 2.0f * rowX;
    char port[16];
    std::snprintf(port, sizeof(port), "%u", unsigned(editServer_.port));
    for (int i = 0; i < rows; ++i) {
        const int action = editRowAction(i, existing);
        const Rect rect{rowX, kRowsTop + float(i) * (kRowHeight + kRowGap), rowWidth,
                        kRowHeight};
        const bool selected = i == editServerCursor_;

        if (action == kEditName || action == kEditAddress || action == kEditPort) {
            drawButton(rect, "", selected, true);
            const char* label = action == kEditName      ? "Name"
                                 : action == kEditAddress ? "Address"
                                                          : "Port";
            drawLabel(label, rect.x + 10.0f, rect.y + 7.0f, 0.45f, kInkDim, C2D_AlignLeft,
                      true);
            const std::string& text =
                action == kEditName ? editServer_.name : editServer_.address;
            const char* value = action == kEditPort ? port
                                : text.empty()      ? "(none)"
                                                    : text.c_str();
            // The port is dimmed while it is the one nobody chose, the way
            // every other row on this menu dims a value it is only reporting.
            const u32 ink = action == kEditPort
                                ? (editServer_.port == net::kDefaultPort ? kInkDim : kInk)
                                : (text.empty() ? kInkDim : kInk);
            drawLabelClipped(value, rect.x + 80.0f, rect.y + 3.0f, 0.55f, ink, rect.w - 90.0f);
            continue;
        }
        const char* label = action == kEditSave     ? "Save"
                             : action == kEditDelete ? "Delete"
                                                     : "Cancel";
        drawButton(rect, label, selected, true);
    }
}

void Menu::drawConfirmDeleteServer()
{
    drawLabelCentered("Delete this server?", kScreenWidth * 0.5f, 60.0f, 0.8f, kInk, true);
    drawLabelCentered(editServer_.name.c_str(), kScreenWidth * 0.5f, 100.0f, 0.6f, kInkWarn,
                      true);
    drawLabelCentered("Only the row goes. The server is not touched.", kScreenWidth * 0.5f,
                      130.0f, 0.45f, kInkDim, true);

    const float half = kButtonWidth * 0.5f;
    drawButton(Rect{kScreenWidth * 0.5f - half - 8.0f, 170.0f, half, kButtonHeight}, "A Delete",
               true, true);
    drawButton(Rect{kScreenWidth * 0.5f + 8.0f, 170.0f, half, kButtonHeight}, "B Keep", false,
               true);
}

void Menu::drawDisconnected()
{
    drawLabelCentered(disconnectTitle_.c_str(), kScreenWidth * 0.5f, 60.0f, 0.7f, kInk, true);

    // Wrapped by words at a width the label font fits across the screen; a
    // server's kick reason is one line and a socket error is rarely three.
    constexpr usize kLineChars = 52;
    constexpr int kMaxLines = 5;
    std::string_view rest(disconnectDetail_);
    int line = 0;
    while (!rest.empty() && line < kMaxLines) {
        usize take = rest.size();
        if (take > kLineChars) {
            take = kLineChars;
            const usize space = rest.substr(0, kLineChars).rfind(' ');
            if (space != std::string_view::npos && space > 0) {
                take = space;
            }
        }
        const std::string text(rest.substr(0, take));
        drawLabelCentered(text.c_str(), kScreenWidth * 0.5f, 96.0f + float(line) * 16.0f, 0.5f,
                          kInkDim, true);
        rest.remove_prefix(take);
        while (!rest.empty() && rest.front() == ' ') {
            rest.remove_prefix(1);
        }
        ++line;
    }

    drawButton(Rect{(kScreenWidth - kButtonWidth) * 0.5f, 190.0f, kButtonWidth, kButtonHeight},
               "Back to server list", true, true);
}

}  // namespace mc::ctr
