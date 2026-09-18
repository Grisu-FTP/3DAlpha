#pragma once

// `<world>/3dalpha.ini` -- the settings that belong to one world and that the
// Alpha level format has nowhere to put.
//
// **Why a file of our own, beside a world rather than inside it.** A setting
// like gamemode has two homes it must not have:
//
//   * Not `level.dat`. A key no version of a1.1.2 ever wrote would travel back
//     to a PC copy of the world, where the real game would carry it around
//     without meaning it. Round-tripping other people's tags verbatim is a
//     promise this project makes (see core/nbt/preserved.hpp); inventing tags
//     of our own is the other side of that promise.
//   * Not `3ds.ini`. That file is the console's, and a per-world setting kept
//     there would be one value shared by every world on the card.
//
// So it is a plain file in the world folder, which a real Minecraft client
// never reads: the Alpha loader looks for `level.dat` and the base36 chunk
// tree and enumerates nothing else. An unknown sibling file is inert -- it is
// not read, not rewritten and not deleted -- which is exactly the "does not
// interfere" this needs.
//
// **Absent is the normal case, not an error.** Every world that already exists
// has no such file, and so does every Alpha save copied in from a PC. Absent
// means defaults, and the file is written when something changes rather than
// on sight, so merely listing worlds never writes to the card.
//
// The format is the `key=value` shape `3ds.ini` uses, sharing its parser --
// see core/settings/ini.hpp. Saving rewrites the file from the keys below, so
// **a key this build does not know is dropped**.

#include "core/io/file_system.hpp"
#include "core/util/types.hpp"

#include <string>
#include <string_view>

namespace mc::settings {

// The file's name inside a world folder. The same in both storage formats: it
// is ours, so unlike a stray file from a third-party tool the packer does not
// have to stash it -- it stays a plain readable file on both sides, which is
// also what lets the world list read a gamemode without opening a container.
inline constexpr char kWorldSettingsName[] = "3dalpha.ini";

// **a1.1.2 has no gamemode at all** -- there is one way to play and the word is
// not in the client. These are the names a player coming from a later version
// expects to find, and only one of them means anything yet.
enum class Gamemode {
    Spectator,
    Survival,
    Creative,
};

// **What Create World starts on, which is not what an absent file means.**
// a1.1.2 has one way to play and this is it, so a world a player makes today
// begins in Survival; `WorldSettings::gamemode` below stays Spectator because
// that is what every world written before this file existed was played as, and
// reading one of those as Survival would drop a player into terrain they had
// been flying through. Two answers to two different questions.
inline constexpr Gamemode kNewWorldGamemode = Gamemode::Survival;

// **`cn.l` -- the world's difficulty**, and unlike gamemode this one *is* in
// a1.1.2: `World.difficulty` is read by `dq.e_()` (which removes every monster
// on Peaceful), by `ma.a()Z` (which refuses a big slime on it) and by
// `dm.a(Lkh;I)Z` (which scales incoming damage). The client keeps it in
// `options.txt` rather than in the world, which is a file this port does not
// have; it goes here because everything else per-world does.
//
// The ordinals are the jar's, so a table indexed by one stays in step.
enum class Difficulty {
    Peaceful = 0,
    Easy = 1,
    Normal = 2,
    Hard = 3,
};

// **The world's texture pack, which has one more state than the console's.**
// `GameSettings::texturePack` is a name or empty-for-Dev-Art; a world needs a
// third answer -- "whatever the console is set to" -- and that is what the
// menu draws as Default and what an absent key means. Dev Art therefore
// cannot be the empty string here, so it is a token of its own.
inline constexpr char kWorldPackDevArt[] = "dev-art";

// **What the world diorama's table is centred on**, before `panoramaTile*`
// steps it away. Three places a world has that are worth standing on, rather
// than one fixed coordinate: the table is 384 blocks a side and a1.1.2 puts
// spawn wherever its sand walk lands, so on a real save block 0, 0 is usually
// somewhere nobody has ever been.
//
// Stable words in the file, like Gamemode and Difficulty above: a file written
// by a later build that adds an anchor is readable here instead of being an
// ordinal that silently means something else.
enum class PanoramaAnchor : u8 {
    // `level.dat`'s SpawnX/SpawnZ -- where a1.1.2 starts a player, and where
    // the chunks around them were generated first. The default, because it is
    // the one anchor every world has.
    Spawn,
    // Block 0, 0. Where the table stood before there was a choice, and the
    // right answer for a world whose interesting part is the origin.
    Origin,
    // Where the player was standing when the world was last closed, out of
    // level.dat's Player compound. **A world can be without one** -- a
    // server-made level.dat has no Player at all -- and then this anchor is not
    // offered, rather than quietly meaning 0, 0, 0.
    Player,
};

// The token written to the file, the label the menu draws, and a parse that
// leaves the caller's default alone when the word is not one of these.
const char* panoramaAnchorToken(PanoramaAnchor anchor);
const char* panoramaAnchorLabel(PanoramaAnchor anchor);
bool panoramaAnchorFromToken(std::string_view token, PanoramaAnchor* out);

struct WorldSettings {
    // **Spectator, which is this port's default and not a1.1.2's** -- the
    // original has one way to play and it is Survival. It stays the default
    // because it is also what a world with no settings file gets, and every
    // world that predates this file was played as a free-flying camera: reading
    // an old world as Survival would drop a player into terrain they had been
    // flying through. The Create World screen offers all three.
    Gamemode gamemode = Gamemode::Spectator;

    // **Normal, which is a1.1.2's own default** -- `Minecraft.difficulty` is
    // initialised to 2 and the options screen starts there.
    Difficulty difficulty = Difficulty::Normal;

    // ---- Extra Settings ------------------------------------------------
    //
    // Everything below this line is **deliberately not a1.1.2**. The rest of
    // this port exists to reproduce the version, bugs included, and the rows
    // on the Extra Settings screen are the one place a player is offered a
    // choice the original never had. They live here rather than in `3ds.ini`
    // for the reason everything else here does: a fix that changes what a
    // chunk generates is a property of the world it generated, not of the
    // console that happened to generate it. Carry the card to another console
    // and the world keeps generating the way its first chunks did.
    //
    // **Every one of them defaults to off**, so a world that predates this
    // screen -- which is every world that exists -- reads as plain a1.1.2.

    // `cu.a` computes its bounding box with `(int)` casts, which truncate
    // toward zero rather than floor. At negative x or z that shifts the box a
    // block away from the vein's low end, and the slice that falls outside it
    // is never written -- so the negative quadrants get measurably less ore
    // than the positive one out of the same generator. True floors instead.
    // See docs/worldgen-a1.1.2.md.
    bool fixOreGeneration = false;

    // `nw.b`'s bedrock test is `y <= random.nextInt(6) - 1`, so y = 0 is left
    // as stone on the one draw in six that comes back 0 -- a hole through the
    // bottom of the world. True lays bedrock at y = 0 unconditionally, and
    // **still makes the draw**, because the draw is what every column after
    // this one is in step with.
    //
    // Generation only: it is not applied to chunks already on the card.
    bool fixBedrockHole = false;

    // `fh.a(Lcn;III)Z` refuses a fence on top of another fence and over any
    // material that is not solid, so a1.1.2's fences stand on the ground and
    // nowhere else. True lets one go anywhere a plain block could.
    //
    // Placement only, and applied the moment it changes: a fence has no
    // `canBlockStay`, so nothing already standing is affected either way.
    bool improvedFencePlacement = false;

    // The pack this world is drawn with, overriding the console's choice.
    // Empty -- the ordinary state -- means "follow the console", which is what
    // the menu draws as Default; `kWorldPackDevArt` means the built-in art;
    // anything else is a pack file name inside the packs folder, the same
    // spelling `GameSettings::texturePack` uses.
    std::string texturePack;

    // **What the main menu's world diorama stands on**, and how far from it.
    //
    // The anchor is a place in the world that is worth putting a table on --
    // see `PanoramaAnchor` -- and the tiles are 128-block map steps away from
    // it. The diorama is three tiles square, so one step of those moves it by a
    // third of its own width. See core/preview/diorama.hpp.
    //
    // **Changing the anchor is not the same as moving the table**: the offset
    // is relative to the anchor, so the two are stored apart and the menu zeroes
    // the offset when the anchor changes. A world written before this key
    // existed reads as Spawn with whatever offset it had, which is where its
    // table already stood.
    PanoramaAnchor panoramaAnchor = PanoramaAnchor::Spawn;
    i32 panoramaTileX = 0;
    i32 panoramaTileZ = 0;
};

// The token written to the file. Stable across builds -- a word rather than an
// ordinal, so a file written by a later build that adds a mode is readable
// instead of being a number that silently means something else here.
const char* gamemodeToken(Gamemode mode);

// What the menu draws.
const char* gamemodeLabel(Gamemode mode);

// **True for all three now.** It stays as a function rather than becoming a
// constant because the answer is what the menu greys a row out on, and a mode
// added later starts out unimplemented: a row that is missing reads as an
// oversight, one that is greyed out reads as a plan. See the .cpp.
bool gamemodeImplemented(Gamemode mode);

// False when the token is not one of the three; the caller keeps its default.
bool gamemodeFromToken(std::string_view token, Gamemode* out);

// The same three for difficulty: a stable word in the file, a label for the
// menu, and a parse that leaves the default alone when it does not recognise
// the word.
const char* difficultyToken(Difficulty level);
const char* difficultyLabel(Difficulty level);
bool difficultyFromToken(std::string_view token, Difficulty* out);

// The token a bool is written as, and the parse that reads one back. `true`
// and `false` are what is written; `1`, `0`, `yes` and `no` are accepted
// because this is a file a player edits by hand on a PC.
const char* boolToken(bool value);
bool boolFromToken(std::string_view token, bool* out);

// `<worldDir>/3dalpha.ini`.
std::string worldSettingsPath(std::string_view worldDir);

// False when there is no file, which is the ordinary state of every world that
// predates this feature; *out is left at its defaults either way. An
// unrecognised value is ignored rather than failing the load.
bool loadWorldSettings(io::FileSystem& fs, std::string_view worldDir, WorldSettings* out);

// Writes through writeFileAtomic, so a console switched off mid-save keeps the
// settings it had. Creates nothing but the file: the world folder is already
// there by the time anyone can change a setting in it.
bool saveWorldSettings(io::FileSystem& fs, std::string_view worldDir,
                       const WorldSettings& settings);

}  // namespace mc::settings
