#pragma once

// **Which way a fluid is going, and what that does to anything standing in
// it.** `jp.e(nm,III)` -- BlockFluid.getFlowVector -- and the two callers that
// turn its answer into something visible.
//
// The flow field had exactly one reader for a long time: the mesher spins a
// flowing block's top texture about the direction it is running (see
// core/mesh/fluid.cpp), and that was the whole of it. The *other* caller is
// `cn.a(cf,gb,kh)Z` -- World.handleMaterialAcceleration -- which sums the
// vector over every water cell an entity's box touches and adds four
// thousandths of the normalised total to its motion every tick. That is the
// entire mechanism by which a river carries a player, and it was missing: an
// entity in water sank and stayed put.
//
// **So the transcription lives here rather than in either caller.** It is one
// method in the jar and it has to stay one method here; two copies against two
// different world types is exactly the pair that drifts, and the drift would be
// silent -- water that pushes one way and paints its texture the other.
//
// **Templated on the accessor, not on an interface.** The mesher asks a
// `MeshScratch`, which is a flat neighbourhood copy addressed in local
// coordinates; an entity asks a `tick::TickWorld`, which reaches live columns
// in world coordinates. Both answer the same two questions and nothing else --
// `blockAt(x, y, z)` and `dataAt(x, y, z)` -- so that pair is the seam. A
// virtual interface would put a call through a pointer in the mesher's inner
// loop for no gain, and CONTRIBUTING keeps virtuals for the platform and
// storage seams.
//
// core/block rather than core/mesh or core/entity because it is below both:
// the only thing it knows about the world is the block table.

#include "core/block/block_def.hpp"
#include "core/block/registry.hpp"
#include "core/util/aabb.hpp"
#include "core/util/math_helper.hpp"
#include "core/util/types.hpp"

#include <cmath>

namespace mc::block {

// `jp.b(I)F` -- getPercentAir. **Ninths, not eighths**: a source block's
// surface sits at 1 - 1/9 of the block. Levels 8..15 are the falling flag and
// read as a source.
inline float fluidPercentAir(int level)
{
    if (level >= 8) {
        level = 0;
    }
    return static_cast<float>(level + 1) / 9.0f;
}

// `aj.b()` -- Vec3D.normalize, with the original's own guard and its **float**
// square root of a double length (`eo.a(D)F`). Both details are load-bearing:
// the guard is what stops a still fluid from normalising zero, and the float
// sqrt is what the flow angle in the mesher was pinned against.
inline void normaliseFlow(double* x, double* y, double* z)
{
    const double length = static_cast<float>(std::sqrt(*x * *x + *y * *y + *z * *z));
    if (length < 1.0e-4) {
        *x = *y = *z = 0.0;
        return;
    }
    *x /= length;
    *y /= length;
    *z /= length;
}

// `jp.c(nm,IIII)Z` -- shouldSideBeRendered, for a **side** face only.
//
// The flow vector asks it eight times and every one of them names a side, so
// the top-face branch (which answers true unconditionally, even under stone)
// is not part of this. `neighbour` is the block the face looks at.
inline bool fluidSideVisible(BlockId neighbour, u8 material)
{
    const BlockDef& d = def(neighbour);
    if (d.material == material) {
        return false;
    }
    // Ice, by singleton identity in the jar -- there is no property behind it,
    // which is why the generator has to name it. Compiled out entirely for a
    // version with no ice, where the constant is 0 and no constructed block
    // takes material 0.
    if (mcver::kIceMaterial != 0 && d.material == mcver::kIceMaterial) {
        return false;
    }
    // `ly.c(nm,IIII)Z` otherwise, which for a full-cube bounding box -- and a
    // fluid block's box is 0..1 on every axis while it renders -- reduces to
    // "is the neighbour an opaque cube".
    return !d.opaque;
}

// `jp.b(nm,III)I` -- getFlowDecay: the cell's flow level, or **-1 when it is
// not this fluid at all**. Levels 8..15 are the falling flag and read as a
// source.
template <class Access>
int fluidFlowDecay(const Access& world, i32 x, int y, i32 z, u8 material)
{
    if (def(world.blockAt(x, y, z)).material != material) {
        return -1;
    }
    const int level = int(world.dataAt(x, y, z));
    return level >= 8 ? 0 : level;
}

// The normalised direction the fluid at (x, y, z) is running in. All zero for
// a fluid that is standing still, which is what both callers branch on.
struct FlowVector {
    double x = 0.0, y = 0.0, z = 0.0;

    bool still() const { return x == 0.0 && y == 0.0 && z == 0.0; }
};

// `jp.e(nm,III)Laj;` -- getFlowVector.
template <class Access>
FlowVector fluidFlowVector(const Access& world, i32 x, int y, i32 z, u8 material)
{
    FlowVector v;

    const int mine = fluidFlowDecay(world, x, y, z, material);

    // **-X, -Z, +X, +Z**, which is not the face order anything else in this
    // project walks. Kept as the class file has it, because the *sign* of the
    // result depends on the order the four deltas are applied in.
    for (int i = 0; i < 4; ++i) {
        i32 px = x;
        i32 pz = z;
        if (i == 0) px -= 1;
        if (i == 1) pz -= 1;
        if (i == 2) px += 1;
        if (i == 3) pz += 1;

        int decay = fluidFlowDecay(world, px, y, pz, material);

        if (decay < 0) {
            // Not this fluid. If it is something you can walk through, the
            // fluid one step *down* still pulls -- which is what makes water
            // aim at the lip of a drop rather than at the drop's far wall.
            //
            // The predicate is `Material.blocksMovement` (`gb.c()`), and it is
            // identical to `Material.isSolid` (`gb.a()`) for every one of
            // a1.1.2's four material classes: air, liquid and the
            // no-collision material override both to false, the base overrides
            // neither. So the `solid` column answers both.
            if (!def(world.blockAt(px, y, pz)).solid) {
                decay = fluidFlowDecay(world, px, y - 1, pz, material);
                if (decay >= 0) {
                    const int weight = decay - (mine - 8);
                    v.x += double((px - x) * weight);
                    v.z += double((pz - z) * weight);
                }
            }
        } else {
            const int weight = decay - mine;
            v.x += double((px - x) * weight);
            v.z += double((pz - z) * weight);
        }
    }

    // A falling column with anything open around it -- at its own level or the
    // one above -- points almost straight down, so it stops carrying whatever
    // horizontal imbalance it happened to have.
    if (world.dataAt(x, y, z) >= 8) {
        const i32 around[8][3] = {
            {x, y, z - 1},     {x, y, z + 1},     {x - 1, y, z},     {x + 1, y, z},
            {x, y + 1, z - 1}, {x, y + 1, z + 1}, {x - 1, y + 1, z}, {x + 1, y + 1, z},
        };

        bool falling = false;
        for (const auto& cell : around) {
            if (fluidSideVisible(world.blockAt(cell[0], int(cell[1]), cell[2]), material)) {
                falling = true;
                break;
            }
        }

        if (falling) {
            normaliseFlow(&v.x, &v.y, &v.z);
            v.y += -6.0;
        }
    }

    normaliseFlow(&v.x, &v.y, &v.z);
    return v;
}

// `cn.a(cf,gb,kh)Z` -- **World.handleMaterialAcceleration**, both halves.
//
// Answers "does this material reach into the box" *and* adds its push to the
// three motion components. The two are one method in the original and are one
// method here, because the caller that wants the answer is the caller that
// wants the push: `Entity.handleWaterMovement` is the condition on
// `moveEntityWithHeading`'s swimming branch.
//
// **The surface test compares against the loop bound, not against the cell.**
// `(double)l >= d1`, where `l` is the box's top *in cells* -- so every fluid
// cell in the box is measured against one height and not against its own.
// Reading it as the cell's own y instead makes every fluid cell count, and is
// the obvious wrong answer.
template <class Access>
bool handleMaterialAcceleration(const Access& world, const AABB& probe, u8 material,
                                double* motionX, double* motionY, double* motionZ)
{
    const i32 x0 = MathHelper::floorDouble(probe.minX);
    const i32 x1 = MathHelper::floorDouble(probe.maxX + 1.0);
    const int y0 = MathHelper::floorDouble(probe.minY);
    const int y1 = MathHelper::floorDouble(probe.maxY + 1.0);
    const i32 z0 = MathHelper::floorDouble(probe.minZ);
    const i32 z1 = MathHelper::floorDouble(probe.maxZ + 1.0);

    bool reaches = false;
    double sumX = 0.0;
    double sumY = 0.0;
    double sumZ = 0.0;

    for (i32 bx = x0; bx < x1; ++bx) {
        for (int by = y0; by < y1; ++by) {
            for (i32 bz = z0; bz < z1; ++bz) {
                if (def(world.blockAt(bx, by, bz)).material != material) {
                    continue;
                }
                const double surface =
                    double(float(by + 1) - fluidPercentAir(int(world.dataAt(bx, by, bz))));
                if (double(y1) < surface) {
                    continue;
                }
                reaches = true;

                // `ly.a(cn,IIILkh;Laj;)V` is empty on the base class and adds
                // the flow vector on `jp`. Only fluids carry a fluid material,
                // so reaching here *is* the override.
                const FlowVector flow = fluidFlowVector(world, bx, by, bz, material);
                sumX += flow.x;
                sumY += flow.y;
                sumZ += flow.z;
            }
        }
    }

    // `if (vec3d.lengthVector() > 0.0D)`. The sum of unit vectors is normalised
    // a second time, so a wide box in a big river is pushed no harder than a
    // narrow one -- what the cells decide is the *direction*, never the speed.
    const double length = double(static_cast<float>(std::sqrt(sumX * sumX + sumY * sumY
                                                             + sumZ * sumZ)));
    if (length > 0.0) {
        normaliseFlow(&sumX, &sumY, &sumZ);
        constexpr double kPush = 0.0040000000000000001;
        *motionX += sumX * kPush;
        *motionY += sumY * kPush;
        *motionZ += sumZ * kPush;
    }
    return reaches;
}

// `cn.b(Lcf;Lgb;)Z` -- **World.isAABBInMaterial**, which is the plain question
// `handleMaterialAcceleration` asks on its way to the push: does any cell of
// this box hold this material?
//
// It is not the same test and the difference is visible. `handleMaterialAcceleration`
// measures every fluid cell's surface against **the top of the box** -- one
// height for all of them -- and this measures each cell's own surface against
// **the bottom** of the box. So a box resting on the surface of a lake is "in
// water" here and is not there, which is exactly the distinction a boat needs:
// it floats *because* its lowest slice counts as wet while its highest does
// not.
//
// The one caller is `EntityBoat.onUpdate`'s buoyancy, which slices its own box
// into five and asks this of each.
template <class Access>
bool isMaterialInBox(const Access& world, const AABB& box, u8 material)
{
    const i32 x0 = MathHelper::floorDouble(box.minX);
    const i32 x1 = MathHelper::floorDouble(box.maxX + 1.0);
    const int y0 = MathHelper::floorDouble(box.minY);
    const int y1 = MathHelper::floorDouble(box.maxY + 1.0);
    const i32 z0 = MathHelper::floorDouble(box.minZ);
    const i32 z1 = MathHelper::floorDouble(box.maxZ + 1.0);

    for (i32 bx = x0; bx < x1; ++bx) {
        for (int by = y0; by < y1; ++by) {
            for (i32 bz = z0; bz < z1; ++bz) {
                if (def(world.blockAt(bx, by, bz)).material != material) {
                    continue;
                }
                // The fluid's surface, and **metadata 8 or more is a falling
                // column**, which the original treats as full-height rather
                // than as level 8 of 8.
                const int meta = int(world.dataAt(bx, by, bz));
                double surface = double(by + 1);
                if (meta < 8) {
                    surface = double(by + 1) - double(meta) / 8.0;
                }
                if (surface >= box.minY) {
                    return true;
                }
            }
        }
    }
    return false;
}

// `kh.g_()Z` -- **Entity.handleWaterMovement**, which is the whole of the
// method: shrink the box by four tenths top and bottom, and hand it to
// `handleMaterialAcceleration` with the water material.
//
// The inset is why a puddle at the ankles is not water to swim in, and why the
// same puddle does not push. There is no lava counterpart: `kh.G()` --
// handleLavaMovement -- calls `isMaterialInBB`, which has no vector in it at
// all, so **lava does not carry anything in this version**.
inline constexpr double kWaterProbeInset = -0.4000000059604645;

template <class Access>
bool handleWaterMovement(const Access& world, const AABB& box, u8 waterMaterial,
                         double* motionX, double* motionY, double* motionZ)
{
    return handleMaterialAcceleration(world, box.expand(0.0, kWaterProbeInset, 0.0),
                                      waterMaterial, motionX, motionY, motionZ);
}

}  // namespace mc::block
