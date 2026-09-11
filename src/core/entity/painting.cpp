// See painting.hpp. `jc.b(int)` (setDirection), `jc.i()` (onValidSurface) and
// the constructor's art draw, transcribed.

#include "core/entity/painting.hpp"

#include "core/block/registry.hpp"
#include "core/tick/tick_world.hpp"
#include "core/util/math_helper.hpp"

#include "items.hpp"  // generated; see tools/configure.py

namespace mc::entity {
namespace {

// `c(int)` -- the original calls it `offs`, and it is **not** `size % 32 == 0`
// however much it looks like it. The class file tests `== 32` and `== 64`
// explicitly and answers 0 for everything else, so a 48-texel painting -- the
// Skeleton and the Donkey Kong -- gets no offset where a modulo reading would
// give it one. Three blocks tall is an odd number of blocks and centres on a
// block; two and four are even and centre on a boundary.
float offs(int sizeTexels)
{
    if (sizeTexels == 32 || sizeTexels == 64) {
        return 0.5f;
    }
    return 0.0f;
}

u8 packedLightAt(const tick::TickWorld& world, double x, double y, double z)
{
    const i32 bx = MathHelper::floorDouble(x);
    const int by = int(MathHelper::floorDouble(y));
    const i32 bz = MathHelper::floorDouble(z);
    return u8((world.skyLightAt(bx, by, bz) << 4) | world.blockLightAt(bx, by, bz));
}

// `gb.a()` -- Material.isSolid, which `onValidSurface` is the only caller of
// here. Note this is the *material* question and not the collision one: glass
// and leaves are solid to a painting.
bool solidAt(const tick::TickWorld& world, i32 x, int y, i32 z)
{
    return block::def(world.blockAt(x, y, z)).solid;
}

// The per-cell light, sampled where `RenderPainting` samples it: at the centre
// of each 16 x 16 cell, in the plane of the canvas.
void sampleCellLight(const tick::TickWorld& world, Painting* p)
{
    const PaintingArt& art = p->artwork();
    const int wide = art.blocksWide();
    const int tall = art.blocksTall();
    const bool alongX = p->direction == 0 || p->direction == 2;

    for (int j = 0; j < tall && j < 4; ++j) {
        for (int i = 0; i < wide && i < 4; ++i) {
            // The cell's centre, offset from the canvas centre by whole blocks.
            const double along = double(i) - double(wide - 1) / 2.0;
            const double up = double(j) - double(tall - 1) / 2.0;
            // Model +x runs along the wall; which world axis that is depends on
            // the facing, and the sign is the one `setPaintingDirection` uses.
            const double cx = p->x + (alongX ? along : 0.0);
            const double cz = p->z + (alongX ? 0.0 : -along);
            p->cellLight[j * 4 + i] = packedLightAt(world, cx, p->y + up, cz);
        }
    }
}

}  // namespace

const PaintingArt& paintingArt(int index)
{
    const int clamped = index >= 0 && index < kPaintingArtCount ? index : 0;
    return mcver::kPaintings[clamped];
}

void setPaintingDirection(Painting* painting, int direction)
{
    painting->direction = direction;

    const PaintingArt& art = painting->artwork();

    // The three half-extents, in texels before the /32. Two of them are the
    // art's size and the third is the canvas thickness -- **which of the two
    // horizontal axes is which depends on the facing**, and that is the whole
    // reason this cannot be written as one expression.
    float halfX = float(art.width);
    const float halfY = float(art.height);
    float halfZ = float(art.width);
    if (direction == 0 || direction == 2) {
        halfZ = 0.5f;
    } else {
        halfX = 0.5f;
    }
    const double ex = double(halfX) / 32.0;
    const double ey = double(halfY) / 32.0;
    const double ez = double(halfZ) / 32.0;

    // The centre of the tile the painting was hung on...
    double px = double(painting->tileX) + 0.5;
    double py = double(painting->tileY) + 0.5;
    double pz = double(painting->tileZ) + 0.5;

    // ...pushed out of it by nine sixteenths, so the canvas stands off the wall
    // rather than inside it.
    if (direction == 0) {
        pz -= kPaintingStandoff;
    }
    if (direction == 1) {
        px -= kPaintingStandoff;
    }
    if (direction == 2) {
        pz += kPaintingStandoff;
    }
    if (direction == 3) {
        px += kPaintingStandoff;
    }

    // ...and slid half a block along the wall when the picture is an even
    // number of blocks wide, so that it centres on a block boundary instead of
    // on a block. The sign differs per direction, which is what keeps the
    // picture growing away from the corner it was clicked in rather than
    // towards it.
    const float slide = offs(int(art.width));
    if (direction == 0) {
        px -= double(slide);
    }
    if (direction == 1) {
        pz += double(slide);
    }
    if (direction == 2) {
        px += double(slide);
    }
    if (direction == 3) {
        pz -= double(slide);
    }
    py += double(offs(int(art.height)));

    painting->x = px;
    painting->y = py;
    painting->z = pz;

    // The box is a hair *smaller* than the canvas -- see kPaintingBoxInset. A
    // painting flush against a wall whose box was exact would overlap the block
    // behind it and refuse itself.
    painting->box = AABB{px - ex - kPaintingBoxInset, py - ey - kPaintingBoxInset,
                         pz - ez - kPaintingBoxInset, px + ex + kPaintingBoxInset,
                         py + ey + kPaintingBoxInset, pz + ez + kPaintingBoxInset};
}

bool paintingFits(const tick::TickWorld& world, const Painting& painting,
                  const PaintingSystem* existing)
{
    const PaintingArt& art = painting.artwork();
    const int wide = art.blocksWide();
    const int tall = art.blocksTall();

    // The bottom-left corner of the wall the picture covers, worked back out of
    // the position. **The original recomputes this from `posX`/`posZ` rather
    // than keeping it**, which matters because the half-block slide above has
    // already been applied and the corner has to include it.
    i32 cornerX = painting.tileX;
    i32 cornerZ = painting.tileZ;
    if (painting.direction == 0 || painting.direction == 2) {
        cornerX = MathHelper::floorDouble(painting.x - double(art.width) / 32.0);
    } else {
        cornerZ = MathHelper::floorDouble(painting.z - double(art.width) / 32.0);
    }
    const int cornerY = int(MathHelper::floorDouble(painting.y - double(art.height) / 32.0));

    for (int i = 0; i < wide; ++i) {
        for (int j = 0; j < tall; ++j) {
            const bool alongX = painting.direction == 0 || painting.direction == 2;
            const i32 bx = alongX ? cornerX + i : painting.tileX;
            const int by = cornerY + j;
            const i32 bz = alongX ? painting.tileZ : cornerZ + i;
            if (!solidAt(world, bx, by, bz)) {
                return false;
            }
        }
    }

    // ...and nothing else already hanging there. The original asks the world
    // for every entity in the box and refuses if any of them is a painting;
    // this asks the pool, which is the same question with the same answer and
    // no allocation.
    if (existing != nullptr) {
        for (int i = 0; i < existing->count(); ++i) {
            const Painting& other = (*existing)[i];
            if (other.alive && other.box.intersects(painting.box)) {
                return false;
            }
        }
    }
    return true;
}

bool PaintingSystem::place(const tick::TickWorld& world, i32 tileX, i32 tileY, i32 tileZ,
                           int face)
{
    // `od.a(...)`'s own face table. The two horizontal faces refuse outright --
    // a painting cannot lie on the floor or hang from a ceiling in this
    // version.
    int direction = 0;
    if (face == 0 || face == 1) {
        return false;
    }
    if (face == 4) {
        direction = 1;
    }
    if (face == 3) {
        direction = 2;
    }
    if (face == 5) {
        direction = 3;
    }

    Painting candidate{};
    candidate.tileX = tileX;
    candidate.tileY = tileY;
    candidate.tileZ = tileZ;
    candidate.alive = true;

    // **Every art in declaration order, and the ones that fit are the ones you
    // can get.** This is the constructor: it sets `art` to each in turn, calls
    // `setDirection`, asks `onValidSurface`, and collects the passes. That is
    // why a one-block gap gives one of the seven small pictures and why the
    // order of the generated table is not decorative.
    int fitting[kPaintingArtCount];
    int fits = 0;
    for (int i = 0; i < kPaintingArtCount; ++i) {
        candidate.art = i;
        setPaintingDirection(&candidate, direction);
        if (paintingFits(world, candidate, this)) {
            fitting[fits++] = i;
        }
    }
    if (fits == 0) {
        // The original still returns true here -- `onItemUse` means "I handled
        // the click" -- but it spawns nothing and takes nothing off the stack,
        // which is what this false says.
        return false;
    }

    candidate.art = fitting[rand_.nextInt(fits)];
    setPaintingDirection(&candidate, direction);
    candidate.light = packedLightAt(world, candidate.x, candidate.y, candidate.z);
    sampleCellLight(world, &candidate);
    candidate.checkIn = kPaintingCheckInterval;

    // Refused only when the heap would not hold another; see
    // core/util/segmented_pool.hpp.
    Painting* slot = paintings_.push();
    if (slot == nullptr) {
        ++refused_;
        return false;
    }
    *slot = candidate;
    return true;
}

void PaintingSystem::tick(const tick::TickWorld& world)
{
    for (int i = 0; i < paintings_.size();) {
        Painting& p = paintings_[i];

        // **Only when the wall is loaded.** A painting whose column has been
        // streamed out has no wall to find, and answering "the wall is gone"
        // there would delete every painting a player walked away from -- the
        // same rule the dropped item's tick already follows and for the same
        // reason: our entities outlive the columns under them and a1.1.2's do
        // not.
        if (!world.chunkResident(p.tileX >> 4, p.tileZ >> 4)) {
            ++i;
            continue;
        }

        // **Light every tick, the wall every hundred.** The original resamples
        // the lightmap once a *frame* and checks the wall on its hundred-tick
        // counter; putting the light on the same counter would leave a painting
        // holding five seconds of stale shading after a torch was placed beside
        // it, which is exactly the sort of thing that reads as a broken
        // renderer.
        p.light = packedLightAt(world, p.x, p.y, p.z);
        sampleCellLight(world, &p);

        if (--p.checkIn > 0) {
            ++i;
            continue;
        }
        p.checkIn = kPaintingCheckInterval;

        if (paintingFits(world, p, nullptr)) {
            ++i;
            continue;
        }

        // The wall went. `jc.e_()` spawns the item at the painting's own
        // position -- no offset and no spread, which is what makes it fall out
        // of the middle of where the picture was rather than out of a block
        // corner.
        dropAndRemove(world, i);
    }
    paintings_.trim();
}

bool PaintingSystem::attack(const tick::TickWorld& world, int index)
{
    if (index < 0 || index >= paintings_.size() || !paintings_[index].alive) {
        return false;
    }
    // `jc.a(Lkh;I)Z` is `setEntityDead(); spawn(new EntityItem(... Item.painting))`
    // and a bare `return true` -- the damage argument is read by nothing.
    dropAndRemove(world, index);
    return true;
}

void PaintingSystem::dropAndRemove(const tick::TickWorld& world, int index)
{
    const Painting& p = paintings_[index];
    world.spawnItem(p.x, p.y, p.z, u16(mcver::Item::Painting), 1);
    // Swap the last one into the hole rather than shuffling, which is what
    // every other pool here does. The caller's loop index must not advance.
    paintings_.swapRemove(index);
}

}  // namespace mc::entity
