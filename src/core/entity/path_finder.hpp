#pragma once

// **How a mob decides where to put its feet** -- `cz` (PathFinder), `bl`
// (PathEntity) and `a` (PathPoint), transcribed.
//
// a1.1.2's `EntityCreature.updatePlayerActionState` does not walk towards a
// point; it asks the world for a *path* to one and then follows it a node at a
// time. Without that a pig walks into the first wall it meets and stays there,
// which is what "the animals are stuck on the fence" would look like.
//
// The search is an ordinary A* over block cells, with four neighbours (no
// diagonals), a step up of one and a drop of at most four. Two things about it
// are worth knowing before reading the code, and both come out of the class
// file rather than out of a description of it:
//
// **1. The entity's size is not used, and that is a bug in the original.**
// `cz.a(Lkh;IIILa;)I` -- getVerticalOffset -- loops `i` over `x .. x+size.x`,
// `j` over `y .. y+size.y` and `k` over `z .. z+size.z`, and then asks
// `world.getBlockMaterial(x, y, z)` **with the parameters, not with the loop
// variables**. So every iteration tests the same single cell and the loop is
// dead: a1.1.2 paths a cow as though it were a point. Later versions pass
// `i, j, k` and that is when mobs started refusing to path through gaps they do
// not fit in.
//
// It is reproduced here rather than fixed, and that is a deliberate choice
// against the usual rule (docs/status.md, *Faithful to the game, not its
// limits*): this one is not a technical limit the hardware imposed, it is what
// the search actually explores, so "fixing" it would change every path a mob
// takes and cost the search a volume test per node on a 268 MHz ARM11 for the
// privilege. `kSizeIsIgnored` names it so a reader meets the decision rather
// than a mystery.
//
// **2. Identity is by coordinate here and by a colliding hash in the jar.**
// `a`'s constructor packs `x | y << 10 | z << 20` into a field and `equals`
// compares *only that*, so two cells 1024 apart in x -- or any cell with a
// negative coordinate -- are the same node to the original. That one is a
// limitation: it is a hash written as an identity, it makes a search near the
// world's edge quietly refuse, and this project tests negative coordinates on
// purpose. The table below hashes the same way and then compares the three
// coordinates, which is the same answer everywhere the jar's is meaningful.
//
// **Nothing here allocates while it runs.** The node arena, the open heap and
// the hash table are taken once, at construction, and reused by every search --
// the same shape `SegmentedPool` uses and for the same reason: this runs inside
// the 20 Hz tick, and a mob asking for a path must not reach the allocator.
// A search that runs out of arena stops and answers with the best node it
// reached, which is what the original does when it runs out of *candidates*.

#include "core/util/aabb.hpp"
#include "core/util/types.hpp"

#include <vector>

namespace mc::tick {
class TickWorld;
}

namespace mc::entity {

// See the header. The original's loop over the entity's size tests one cell.
inline constexpr bool kSizeIsIgnored = true;

// How far a node may drop before the search gives up on it -- `j >= 4` in
// `getSafePoint`. Four blocks is also a1.1.2's fall-damage threshold, which is
// probably not a coincidence.
inline constexpr int kPathMaxDrop = 4;

// **What a found path holds.** `bl` is an array of points and an index into it;
// this is the same thing with a ceiling, because a path lives inside a mob and
// a mob lives in a pool.
//
// **The ceiling is ours.** a1.1.2's array is however long the search made it.
// An animal asks for a path to a point it drew within six blocks of itself, so
// 32 nodes is already more than a straight line to the far corner of that box;
// a longer path is truncated at 32 and the mob re-paths when it runs out, which
// is the same thing it does when it arrives. Sixteen bytes a node, 32 nodes, so
// a path costs half a kilobyte and a mob that has never pathed costs it too.
struct PathRoute {
    static constexpr int kMaxSteps = 32;

    struct Step {
        i32 x = 0;
        i32 z = 0;
        i16 y = 0;
    };

    Step steps[kMaxSteps] = {};
    u8 count = 0;
    u8 index = 0;

    bool empty() const { return count == 0; }

    // `bl.b()` -- isFinished.
    bool finished() const { return index >= count; }

    // `bl.a()` -- incrementPathIndex.
    void advance()
    {
        if (index < kMaxSteps) {
            ++index;
        }
    }

    void clear()
    {
        count = 0;
        index = 0;
    }

    // `bl.a(Lkh;)Laj;` -- getPosition. The node's cell, centred by **half the
    // integer part of `width + 1`** rather than by half a block: for a pig
    // (0.9 wide) `(int)(1.9) * 0.5` is 0.5, and for anything two blocks wide it
    // would be 1.0. The original's arithmetic exactly, truncation and all.
    void position(float width, double* x, double* y, double* z) const;
};

// One node. `f`, `g` and `h` are the original's field names for cost so far,
// heuristic to the target and sort key; `previous` and `heapIndex` are indices
// into the arena and the heap rather than pointers, so the arena can be one
// flat vector that never moves anything.
struct PathPoint {
    i32 x = 0;
    i32 z = 0;
    i16 y = 0;
    bool visited = false;  // `j`, isFirst -- already expanded
    float total = 0.0f;    // `f`
    float toTarget = 0.0f; // `g`
    float sort = 0.0f;     // `h`
    int previous = -1;
    int heapIndex = -1;    // `e`, index in the open heap; -1 is "not queued"
};

class PathFinder {
public:
    // The arena, taken once. **1,024 nodes** is the ceiling on one search, and
    // it is a budget rather than a transcription: the original's `IntHashMap`
    // grows without bound and a search that explores a thousand cells has
    // already spent more of a 20 Hz tick than a wandering pig is worth. A
    // search that fills it stops and answers with the nearest node it reached.
    static constexpr int kMaxNodes = 1024;

    PathFinder();

    // `cz.a(Lkh;DDDF)Lbl;`. `box` is the mob's bounding box -- the search
    // starts at the **floor of its minimum corner**, not at the mob's centre,
    // which is the original's and matters for a box that straddles a cell edge.
    //
    // `maxDistance` is `EntityCreature`'s 10: a node further than this from the
    // target is never expanded, so the search is bounded around the *goal* and
    // not around the mob.
    //
    // Returns false when nothing was found, which is `null` in the original and
    // is what sends a mob back to `EntityLiving`'s aimless wander.
    bool find(const tick::TickWorld& world, const AABB& box, float width, float height,
              double targetX, double targetY, double targetZ, float maxDistance,
              PathRoute* out);

    // How many searches have run and how many of those hit `kMaxNodes`. Read
    // by the debug page; the second one going up means the budget is the thing
    // shaping mob movement rather than the world is.
    u32 searches() const { return searches_; }
    u32 exhausted() const { return exhausted_; }
    // The largest node count any one search has needed, so the budget above can
    // be checked against a real world rather than argued about.
    int highWater() const { return highWater_; }

private:
    // `cz.a(III)La;` -- openPoint: the node for this cell, made on first ask.
    // -1 when the arena is full.
    int openPoint(i32 x, int y, i32 z);

    // `cz.a(Lkh;IIILa;)I` -- getVerticalOffset. 1 is free, 0 is blocked,
    // -1 is a liquid, and see the header for the loop that is not there.
    int verticalOffset(const tick::TickWorld& world, i32 x, int y, i32 z) const;

    // `cz.a(Lkh;IIILa;I)La;` -- getSafePoint: the node reached by stepping to
    // this cell, lifted by `step` if it has to be and dropped onto the ground
    // under it if it can be. -1 when there is no such node.
    int safePoint(const tick::TickWorld& world, i32 x, int y, i32 z, int step);

    // The open set, a binary heap of arena indices keyed on `sort`.
    void heapClear() { heap_.clear(); }
    void heapPush(int node);
    int heapPop();
    void heapUpdate(int node, float sort);
    void heapUp(int at);
    void heapDown(int at);

    // Open addressing on the original's `x | y << 10 | z << 20`, compared by
    // coordinate. See the header.
    static constexpr int kTableSize = 2048;  // a power of two > kMaxNodes
    void tableClear();
    int* tableSlot(i32 x, int y, i32 z);

    std::vector<PathPoint> nodes_;
    std::vector<int> heap_;
    std::vector<int> table_;

    u32 searches_ = 0;
    u32 exhausted_ = 0;
    int highWater_ = 0;
};

}  // namespace mc::entity
