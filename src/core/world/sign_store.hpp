#pragma once

// **The text on a sign** -- `ob`, which is `TileEntitySign`.
//
// "Signs don't render and don't open the keyboard on placing" is one bug with
// three halves, and the first is why the other two were invisible: **a sign is
// render type -1 in a1.1.2.** The mesher is asked to draw block 63 and answers
// with nothing, correctly, because the whole of a sign -- board, post and text
// -- is drawn by `in` (TileEntitySignRenderer) from a tile entity. A build with
// no tile entities has nothing to draw and nothing to type into.
//
// **A tile entity is not an entity**, which is why this lives in core/world/
// rather than beside the boats: it belongs to a block, it has no position of
// its own beyond that block's, and it does not tick. What it shares with the
// entity pools here is the shape -- a fixed array, no allocation, and no
// persistence.
//
// **The text survives the world closing**, which it did not until the
// `TileEntities` list stopped being an opaque blob. `readSigns` takes a
// column's decoded tile entities as it joins the world and `writeSigns` puts
// them back just before it is saved; `core/world/tile_entity.hpp` is the list
// and the argument for modelling all four of its tenants at once.
//
// The format is one line: `Text1` through `Text4`, four TAG_Strings, beside
// the `x`/`y`/`z` and the `id` of `"Sign"` that every tile entity carries.

#include "core/util/segmented_pool.hpp"
#include "core/util/types.hpp"

#include <string_view>

namespace mc::world {

// Four lines, and **fifteen characters each**. The limit is the editor's, not
// the tile entity's: `GuiEditSign` refuses a keystroke past 15 and the renderer
// would happily draw more. Sixteen bytes so a line is always NUL-terminated.
inline constexpr int kSignLines = 4;
inline constexpr int kSignLineLength = 15;
inline constexpr int kSignLineBytes = kSignLineLength + 1;

struct SignText {
    i32 x = 0;
    int y = 0;
    i32 z = 0;

    // **Which of the two blocks it belongs to.** A post turns by a sixteenth
    // and stands on the ground; a wall sign takes one of four faces and hangs
    // off it. They are different blocks with different metadata meanings, and
    // the renderer needs to know which before it can place the board.
    bool wall = false;

    // A copy of the block's metadata at the moment it was written, so the
    // renderer does not have to reach into the world for it. Refreshed on
    // every write.
    u8 metadata = 0;

    char lines[kSignLines][kSignLineBytes] = {};

    // `(sky << 4) | block` at the sign's own block, resampled on the world's
    // clock. A sign is a tile entity and does not tick, so this is refreshed by
    // whoever owns the store -- see `refreshLight`.
    u8 light = 0xF0;

    bool used = false;
};

// **No cap**: a sign is a tile entity, and a1.1.2 keeps as many as are built.
// The first sixty-four are held from construction; past that the store grows
// until the heap says stop (core/util/segmented_pool.hpp), and only then is a
// sign refused -- `refused()` counts them and the debug page shows it. What is
// *drawn* is a separate, nearest-first budget: see core/render/sign_mesh.hpp.
class SignStore {
public:
    // Held from construction, so ordinary play never allocates; not a limit.
    static constexpr int kInitialCapacity = 64;

    // Creates or replaces the sign at this block. Returns the index, or -1 when
    // the heap would not hold another.
    int put(i32 x, int y, i32 z, bool wall, u8 metadata);

    // The sign at this block, or -1.
    int find(i32 x, int y, i32 z) const;

    // Forgets the sign at this block, if there is one. Called when the block
    // goes; a store that kept it would put the old text on the next sign built
    // in the same hole.
    void erase(i32 x, int y, i32 z);

    // Forgets every sign in a column. The column has left the loaded world and
    // its text is already on its way to the card; keeping it would grow the
    // store for the length of the session and hand stale text to whatever is
    // built in those coordinates next.
    void eraseColumn(i32 chunkX, i32 chunkZ);

    // Writes one line, truncated to fifteen characters and always terminated.
    void setLine(int index, int line, std::string_view text);

    // Resamples every sign's light. Cheap -- one block read a sign -- and
    // called from the tick loop rather than from a draw, so a torch put
    // beside a sign brightens it on the next tick rather than the next frame.
    //
    // Templated on the world so this header does not pull in the tick world,
    // which is the same trick core/block/fluid_flow.hpp uses.
    template <class Access>
    void refreshLight(const Access& world)
    {
        for (int i = 0; i < signs_.size(); ++i) {
            SignText& s = signs_[i];
            if (!s.used) {
                continue;
            }
            s.light = u8((world.skyLightAt(s.x, s.y, s.z) << 4)
                         | world.blockLightAt(s.x, s.y, s.z));
        }
    }

    // **The sign the last placement made**, taken once and cleared. A placement
    // has to tell the caller to put a keyboard up, and the caller is the only
    // thing that has one; `item::rightClick` returns a bool and cannot say
    // which sign.
    //
    // `ArrowSystem::struckLastTick` used to be the other seam of this shape and
    // is gone: a sound needs no caller, because `TickWorld::playSoundAt` is
    // const and the arrow knows where it is. A keyboard does.
    int takeJustPlaced()
    {
        const int index = justPlaced_;
        justPlaced_ = -1;
        return index;
    }

    void clear() { signs_.clear(); }

    int count() const { return signs_.size(); }
    const SignText& operator[](int i) const { return signs_[i]; }
    u32 refused() const { return refused_; }

private:
    SegmentedPool<SignText, kInitialCapacity> signs_;
    int justPlaced_ = -1;
    u32 refused_ = 0;
};

class ChunkColumn;

// **The text of every sign in a column**, taken out of its decoded
// `TileEntities` as the column joins the loaded world -- `cn.b(Lcu;)V`.
//
// The board's shape is not in the NBT: `ob` stores four strings and nothing
// else, and whether a sign is a post or hangs on a wall, and which way it
// turns, is the block and its metadata. Both are read off the column here,
// which is the only place that has all three at once. Returns how many were
// taken; a sign whose block is no longer a sign is skipped, the way the
// original's lazy `getChunkBlockTileEntity` would never have built one.
int readSigns(const ChunkColumn& column, SignStore& store);

// ...and back, just before the column is written. Only signs inside this
// column are touched. Returns how many entries were written.
int writeSigns(const SignStore& store, ChunkColumn& column);

}  // namespace mc::world
