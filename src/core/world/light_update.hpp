#pragma once

// Sky and block light repaired after a block changes, a few cells at a time.
//
// **The gap this closes.** `LightEngine` solves a whole column against a 3x3
// window and runs once, on the generation worker, when a column is first
// finalised. Nothing recomputed light after that, so every block a tick wrote
// left the stored light exactly as it was: lava flowed and the world it flowed
// through stayed dark, a roof broken by a falling block stayed shaded, and
// `TickWorld::lightValue` -- which grass, crops, saplings, ice and the snow
// pass all read -- kept answering with values that no longer described the
// world. `TickWorld::refreshHeight` maintained the height map through all of
// this, which is why `canSeeSky` stayed right while the light did not.
//
// **Why this is allowed to work differently from LightEngine.** The argument is
// already written down in lighting.hpp: a1.1.2's update rule is a monotone
// operator with a strictly positive decrement, so it has exactly one fixed
// point and every algorithm that finds it finds the same numbers. LightEngine
// gets there with a bucketed breadth-first sweep of a 48x128x48 window; this
// gets there with the standard incremental removal-then-addition pair over
// however few cells the change actually disturbed. Same fixed point, and
// `tests/light_update_test.cpp` asserts it cell by cell against
// `LightEngine::computeCentre`, which is itself checked against a real JVM.
//
// **Why incremental rather than re-running the engine.** LightEngine holds
// 576 KB and settles 294,912 cells; a flowing fluid changes hundreds of blocks
// a second and each change disturbs a handful of cells. Running the whole-column
// engine per edit would cost more than the fluid does. The trade is that this
// has to be written correctly by hand, which is what the oracle test is for.
//
// **Why it is on the main thread.** It reads and writes the resident columns,
// which the mesher and the tick also touch, and this project's rule is that the
// grid belongs to the main thread. It is budgeted instead: `drain` settles at
// most a stated number of cells per call and carries the rest to the next
// frame, so a large edit costs latency rather than a frame.
//
// **Light is baked into vertices**, so every cell whose stored value moves also
// means a remesh. That is reported through `sectionLit` rather than inferred,
// because the set of sections a light change touches is not the set the block
// change touched -- light travels.

#include "core/block/registry.hpp"
#include "core/util/types.hpp"
#include "core/world/chunk.hpp"

#include <cstddef>
#include <vector>

namespace mc::world {

// Where the relighter reaches columns, and what it tells the renderer when one
// changes. Plain function pointers and a context, the same shape
// `tick::TickAccess` uses and for the same reason: core owns no grid.
struct LightAccess {
    void* ctx = nullptr;

    // The resident column, or null. Never generates, never blocks, never
    // touches the card. A null answer is a column that is not loaded, and light
    // simply stops at it -- which is what the original does when it lights
    // against an unloaded neighbour.
    ChunkColumn* (*column)(void* ctx, i32 chunkX, i32 chunkZ) = nullptr;

    // A section's stored light changed and its mesh is now wrong. Called once
    // per section per drain, not once per cell.
    void (*sectionLit)(void* ctx, i32 chunkX, int sectionY, i32 chunkZ) = nullptr;
};

class LightUpdater {
public:
    static constexpr int kHeight = ChunkColumn::kHeight;

    // How many entries each of the four queues holds. A block change usually
    // disturbs a few dozen cells; the expensive case is removing an isolated
    // light-15 source in open air, which can reach a few thousand.
    //
    // Past this, entries are dropped and counted rather than the queue grown:
    // growing would allocate on the frame path, and the consequence of dropping
    // is a patch of world holding light one edit out of date, which is a wrong
    // shade rather than a wrong world. `stats().dropped` is on the debug page
    // so it is not silent.
    static constexpr usize kQueueCapacity = 2048;

    explicit LightUpdater(LightAccess access);

    // A block at (x, y, z) has just been written. Enqueues everything the
    // change can have disturbed, for both light types. Cheap: it looks at the
    // cell, its six neighbours and the 128 cells of its own column's sky
    // exposure, and does no propagation itself.
    void blockChanged(i32 x, int y, i32 z);

    // Settles at most `budget` cells and returns how many it settled. Anything
    // left stays queued for the next call.
    u32 drain(u32 budget);

    // Whether there is anything left to settle.
    bool idle() const;

    // Throws away everything queued. For a world closing or the grid being
    // rebuilt under it -- the queued coordinates would name columns that are no
    // longer resident, and while that is harmless (they read as absent) it is
    // pointless work.
    void reset();

    struct Stats {
        u64 cellsSettled = 0;   // cells whose stored light actually moved
        u64 cellsVisited = 0;   // cells popped, including no-ops
        u64 edits = 0;          // blockChanged calls
        u64 dropped = 0;        // queue entries there was no room for
    };
    const Stats& stats() const { return stats_; }
    usize pending() const;

private:
    // Which of the two stored planes is being solved. They differ only in what
    // counts as a source, so the propagation below is written once.
    enum class Kind : u8 { Sky = 0, Block = 1 };

    struct Cell {
        i32 x = 0;
        i32 z = 0;
        i16 y = 0;
        u8 level = 0;  // in a removal queue, the value the cell used to hold
    };

    // A fixed-capacity FIFO over a vector reserved once. Not a deque, because a
    // deque allocates a block per chunk of entries and this must not allocate
    // once the world is open.
    struct Queue {
        std::vector<Cell> items;
        usize head = 0;

        // Twice the live capacity, because entries are popped by advancing
        // `head` and the dead prefix is only reclaimed periodically -- so the
        // vector holds up to one and a half times the live bound and must not
        // reallocate at that point.
        void reserve() { items.reserve(kQueueCapacity * 2); }
        bool empty() const { return head >= items.size(); }
        usize size() const { return items.size() - head; }
        Cell pop()
        {
            const Cell cell = items[head++];
            if (head >= items.size()) {
                // Drained: the cheap and much the commonest reclaim.
                items.clear();
                head = 0;
            } else if (head > kQueueCapacity / 2) {
                items.erase(items.begin(), items.begin() + static_cast<std::ptrdiff_t>(head));
                head = 0;
            }
            return cell;
        }
        // False when there was no room; the caller counts the drop.
        bool push(const Cell& cell)
        {
            if (items.size() - head >= kQueueCapacity) return false;
            items.push_back(cell);
            return true;
        }
        void clear()
        {
            items.clear();
            head = 0;
        }
    };

    // ---- column access, with the same one-entry cache the tick uses --------
    ChunkColumn* columnAtBlock(i32 x, i32 z);
    void invalidateColumnCache() { cachedValid_ = false; }

    u8 lightAt(Kind kind, i32 x, int y, i32 z);
    void setLightAt(Kind kind, i32 x, int y, i32 z, u8 value);

    // The value this cell holds *of itself*, before any neighbour is consulted:
    // for block light the block's own emission, for sky light 15 wherever the
    // column's height map says the cell can see the sky. Zero everywhere else.
    u8 sourceAt(Kind kind, i32 x, int y, i32 z);

    u8 opacityAt(i32 x, int y, i32 z);

    void push(Queue* queue, i32 x, int y, i32 z, u8 level);
    void noteSectionLit(i32 x, int y, i32 z);

    // One pop from the removal queue, then one from the addition queue. Split
    // out because the two are the same walk over six neighbours with different
    // tests.
    bool stepRemoval(Kind kind);
    bool stepAddition(Kind kind);

    LightAccess access_;

    // Removal first, then addition, per light type. Removal has to finish
    // before addition for that type, or a cell zeroed by the removal can be
    // refilled from a neighbour that is itself about to be zeroed.
    Queue remove_[2];
    Queue add_[2];

    ChunkColumn* cached_ = nullptr;
    i32 cachedX_ = 0;
    i32 cachedZ_ = 0;
    bool cachedValid_ = false;

    // Sections reported this drain, so `sectionLit` fires once each rather than
    // once per cell. A short array with a linear scan: a single edit's light
    // reaches a handful of sections, and a set would allocate.
    struct LitSection {
        i32 chunkX;
        i32 chunkZ;
        i16 sectionY;
    };
    static constexpr usize kLitSectionCapacity = 64;
    LitSection litSections_[kLitSectionCapacity];
    usize litSectionCount_ = 0;
    void flushLitSections();

    Stats stats_;
};

}  // namespace mc::world
