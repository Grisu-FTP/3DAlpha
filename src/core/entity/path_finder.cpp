// See path_finder.hpp. `cz.a(Lkh;La;La;La;F)Lbl;` and the four private methods
// under it, transcribed.

#include "core/entity/path_finder.hpp"

#include "core/block/registry.hpp"
#include "core/tick/tick_world.hpp"
#include "core/util/math_helper.hpp"

#include "blocks.hpp"  // generated; see tools/configure.py

#include <cmath>

namespace mc::entity {
namespace {

constexpr u8 kWaterMaterial = mcver::kBlocks[int(mcver::Block::Water)].material;
constexpr u8 kLavaMaterial = mcver::kBlocks[int(mcver::Block::Lava)].material;

// `a.a(La;)F` -- distanceTo, and it is the **square root** rather than the
// squared distance: the heuristic and the step cost are both real distances, so
// an A* here admits diagonal shortcuts across a four-neighbour grid. That is
// the original's; a Manhattan heuristic would find different paths.
float distanceBetween(const PathPoint& from, i32 x, int y, i32 z)
{
    const float dx = float(x - from.x);
    const float dy = float(y - int(from.y));
    const float dz = float(z - from.z);
    return MathHelper::sqrtFloat(dx * dx + dy * dy + dz * dz);
}

// `eo.d(F)I` -- MathHelper.ceiling_float_int, kept because the size point is
// built from it even though nothing reads the size. See the header.
int ceilFloat(float value)
{
    const int truncated = int(value);
    return float(truncated) < value ? truncated + 1 : truncated;
}

// The original packs the cell into one int and compares only that. We hash the
// same way and compare the coordinates -- see the header.
u32 cellHash(i32 x, int y, i32 z)
{
    const u32 a = u32(x) * 0x9e3779b9u;
    const u32 b = u32(y) * 0x85ebca6bu;
    const u32 c = u32(z) * 0xc2b2ae35u;
    return a ^ (b << 10) ^ (c << 20) ^ (c >> 12);
}

}  // namespace

void PathRoute::position(float width, double* x, double* y, double* z) const
{
    const int step = index < count ? index : (count > 0 ? count - 1 : 0);
    const double centre = double(int(width + 1.0f)) * 0.5;
    *x = double(steps[step].x) + centre;
    *y = double(steps[step].y);
    *z = double(steps[step].z) + centre;
}

PathFinder::PathFinder()
{
    nodes_.reserve(kMaxNodes);
    heap_.reserve(kMaxNodes);
    table_.assign(kTableSize, -1);
}

void PathFinder::tableClear()
{
    for (int& slot : table_) {
        slot = -1;
    }
}

int* PathFinder::tableSlot(i32 x, int y, i32 z)
{
    u32 index = cellHash(x, y, z) & u32(kTableSize - 1);
    for (int probe = 0; probe < kTableSize; ++probe) {
        int& slot = table_[index];
        if (slot < 0) {
            return &slot;
        }
        const PathPoint& node = nodes_[usize(slot)];
        if (node.x == x && int(node.y) == y && node.z == z) {
            return &slot;
        }
        index = (index + 1) & u32(kTableSize - 1);
    }
    return nullptr;
}

int PathFinder::openPoint(i32 x, int y, i32 z)
{
    int* slot = tableSlot(x, y, z);
    if (slot == nullptr) {
        return -1;
    }
    if (*slot >= 0) {
        return *slot;
    }
    if (int(nodes_.size()) >= kMaxNodes) {
        return -1;
    }
    PathPoint point;
    point.x = x;
    point.y = i16(y);
    point.z = z;
    nodes_.push_back(point);
    *slot = int(nodes_.size()) - 1;
    return *slot;
}

int PathFinder::verticalOffset(const tick::TickWorld& world, i32 x, int y, i32 z) const
{
    // **One cell, because the original tests one cell.** See the header: its
    // triple loop reads the parameters rather than the loop variables, so the
    // entity's size never reaches the world.
    const block::BlockDef& def = block::def(world.blockAt(x, y, z));
    if (def.solid) {
        return 0;
    }
    if (def.material == kWaterMaterial || def.material == kLavaMaterial) {
        return -1;
    }
    return 1;
}

int PathFinder::safePoint(const tick::TickWorld& world, i32 x, int y, i32 z, int step)
{
    int node = -1;
    if (verticalOffset(world, x, y, z) > 0) {
        node = openPoint(x, y, z);
    }
    // **The step up**, and `step` is 1 only when the cell above the node being
    // expanded is clear -- a mob under a one-block ceiling cannot climb.
    if (node < 0 && verticalOffset(world, x, y + step, z) > 0) {
        node = openPoint(x, y + step, z);
        y += step;
    }
    if (node < 0) {
        return -1;
    }

    // **The drop.** Walk down while the cell below is free, up to four blocks;
    // deeper than that and the node is refused outright rather than clamped.
    int dropped = 0;
    while (y > 0 && verticalOffset(world, x, y - 1, z) > 0) {
        ++dropped;
        if (dropped >= kPathMaxDrop) {
            return -1;
        }
        --y;
    }
    if (y <= 0) {
        return node;
    }
    return openPoint(x, y, z);
}

void PathFinder::heapPush(int node)
{
    heap_.push_back(node);
    nodes_[usize(node)].heapIndex = int(heap_.size()) - 1;
    heapUp(int(heap_.size()) - 1);
}

int PathFinder::heapPop()
{
    const int top = heap_.front();
    const int last = heap_.back();
    heap_.pop_back();
    nodes_[usize(top)].heapIndex = -1;
    if (!heap_.empty() && last != top) {
        heap_[0] = last;
        nodes_[usize(last)].heapIndex = 0;
        heapDown(0);
    }
    return top;
}

void PathFinder::heapUpdate(int node, float sort)
{
    const float previous = nodes_[usize(node)].sort;
    nodes_[usize(node)].sort = sort;
    const int at = nodes_[usize(node)].heapIndex;
    if (at < 0) {
        return;
    }
    if (sort < previous) {
        heapUp(at);
    } else {
        heapDown(at);
    }
}

void PathFinder::heapUp(int at)
{
    const int node = heap_[usize(at)];
    const float sort = nodes_[usize(node)].sort;
    while (at > 0) {
        const int parent = (at - 1) / 2;
        const int other = heap_[usize(parent)];
        if (sort >= nodes_[usize(other)].sort) {
            break;
        }
        heap_[usize(at)] = other;
        nodes_[usize(other)].heapIndex = at;
        at = parent;
    }
    heap_[usize(at)] = node;
    nodes_[usize(node)].heapIndex = at;
}

void PathFinder::heapDown(int at)
{
    const int node = heap_[usize(at)];
    const float sort = nodes_[usize(node)].sort;
    const int size = int(heap_.size());
    for (;;) {
        int child = at * 2 + 1;
        if (child >= size) {
            break;
        }
        const int right = child + 1;
        if (right < size
            && nodes_[usize(heap_[usize(right)])].sort < nodes_[usize(heap_[usize(child)])].sort) {
            child = right;
        }
        const int other = heap_[usize(child)];
        if (nodes_[usize(other)].sort >= sort) {
            break;
        }
        heap_[usize(at)] = other;
        nodes_[usize(other)].heapIndex = at;
        at = child;
    }
    heap_[usize(at)] = node;
    nodes_[usize(node)].heapIndex = at;
}

bool PathFinder::find(const tick::TickWorld& world, const AABB& box, float width, float height,
                      double targetX, double targetY, double targetZ, float maxDistance,
                      PathRoute* out)
{
    if (out == nullptr) {
        return false;
    }
    out->clear();

    ++searches_;
    nodes_.clear();
    heapClear();
    tableClear();

    // The size point, built and then never read -- see the header. Kept so the
    // transcription is recognisable and so a reader who looks for it finds the
    // note instead of a gap.
    (void)ceilFloat(width + 1.0f);
    (void)ceilFloat(height + 1.0f);

    const int start = openPoint(MathHelper::floorDouble(box.minX),
                                int(MathHelper::floorDouble(box.minY)),
                                MathHelper::floorDouble(box.minZ));
    const int end = openPoint(MathHelper::floorDouble(targetX - double(width / 2.0f)),
                              int(MathHelper::floorDouble(targetY)),
                              MathHelper::floorDouble(targetZ - double(width / 2.0f)));
    if (start < 0 || end < 0) {
        return false;
    }

    const i32 endX = nodes_[usize(end)].x;
    const int endY = int(nodes_[usize(end)].y);
    const i32 endZ = nodes_[usize(end)].z;

    nodes_[usize(start)].total = 0.0f;
    nodes_[usize(start)].toTarget = distanceBetween(nodes_[usize(start)], endX, endY, endZ);
    nodes_[usize(start)].sort = nodes_[usize(start)].toTarget;
    heapPush(start);

    int best = start;
    bool reached = false;

    while (!heap_.empty()) {
        const int current = heapPop();
        if (current == end) {
            best = end;
            reached = true;
            break;
        }
        if (distanceBetween(nodes_[usize(current)], endX, endY, endZ)
            < distanceBetween(nodes_[usize(best)], endX, endY, endZ)) {
            best = current;
        }
        nodes_[usize(current)].visited = true;

        const i32 cx = nodes_[usize(current)].x;
        const int cy = int(nodes_[usize(current)].y);
        const i32 cz = nodes_[usize(current)].z;

        // `cz.b(...)` -- findPathOptions. The step allowance is one only when
        // there is headroom above the node being left.
        const int step = verticalOffset(world, cx, cy + 1, cz) > 0 ? 1 : 0;
        const i32 offsets[4][2] = {{0, 1}, {-1, 0}, {1, 0}, {0, -1}};
        for (const auto& offset : offsets) {
            const int option = safePoint(world, cx + offset[0], cy, cz + offset[1], step);
            if (option < 0 || nodes_[usize(option)].visited) {
                continue;
            }
            // **Bounded around the goal, not around the mob.** A node further
            // than `maxDistance` from the target is dropped, which is what
            // stops a refused path from searching the whole world.
            if (distanceBetween(nodes_[usize(option)], endX, endY, endZ) >= maxDistance) {
                continue;
            }

            const float total = nodes_[usize(current)].total
                                + distanceBetween(nodes_[usize(current)], nodes_[usize(option)].x,
                                                  int(nodes_[usize(option)].y),
                                                  nodes_[usize(option)].z);
            const bool queued = nodes_[usize(option)].heapIndex >= 0;
            if (queued && total >= nodes_[usize(option)].total) {
                continue;
            }
            nodes_[usize(option)].previous = current;
            nodes_[usize(option)].total = total;
            nodes_[usize(option)].toTarget =
                distanceBetween(nodes_[usize(option)], endX, endY, endZ);
            const float sort = nodes_[usize(option)].total + nodes_[usize(option)].toTarget;
            if (queued) {
                heapUpdate(option, sort);
            } else {
                nodes_[usize(option)].sort = sort;
                heapPush(option);
            }
        }

        if (int(nodes_.size()) >= kMaxNodes) {
            // Out of arena. Answer with the nearest node reached, which is
            // what the original does when it runs out of candidates -- the mob
            // walks part of the way and asks again.
            ++exhausted_;
            break;
        }
    }

    if (int(nodes_.size()) > highWater_) {
        highWater_ = int(nodes_.size());
    }

    if (!reached && best == start) {
        return false;
    }

    // `cz.a(La;La;)Lbl;` -- createEntityPath: walk `previous` back to the start
    // and write the nodes out forwards. Longer than `kMaxSteps` keeps the
    // **first** `kMaxSteps` of the path rather than the last, so the mob starts
    // walking the right way and re-paths when it runs out.
    int length = 1;
    for (int node = best; nodes_[usize(node)].previous >= 0;
         node = nodes_[usize(node)].previous) {
        ++length;
    }
    int node = best;
    int write = length - 1;
    while (node >= 0) {
        if (write < PathRoute::kMaxSteps) {
            out->steps[write].x = nodes_[usize(node)].x;
            out->steps[write].y = nodes_[usize(node)].y;
            out->steps[write].z = nodes_[usize(node)].z;
        }
        --write;
        node = nodes_[usize(node)].previous;
    }
    out->count = u8(length < PathRoute::kMaxSteps ? length : PathRoute::kMaxSteps);
    out->index = 0;
    return out->count > 0;
}

}  // namespace mc::entity
