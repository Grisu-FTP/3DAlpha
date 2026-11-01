#pragma once

// **The footsteps of a body another console moves**, paid out in this world.
//
// a1.1.2's server moves every player it hears from through `moveEntity`, and
// the tail of `moveEntity` is where the world finds out somebody walked on it:
// `Block.onEntityWalking` once per step, on the cell underfoot. Two blocks
// answer it -- farmland, which tramples back to dirt, and redstone ore, which
// lights -- and both are world writes, so on a host they have to happen in the
// host's world. A guest's own console runs the same rule over its copy of the
// blocks, and its edit is reverted when the host never confirms it (see
// `WorldStreamer::stepTicks`'s remote branch); this is what confirms it.
//
// **The rule is `PlayerBody`'s, fed with where the guest says they are**:
// `distanceWalkedModified += sqrt(dx*dx + dz*dz) * 0.6`, a step whenever it
// passes `nextStepDistance` over a cell that is not air, the counter
// incremented rather than reset, and nothing while crouched or riding. See
// core/entity/player_body.cpp's footstep block.
//
// **Ours: a jump of more than a few blocks is not walking.** The host hears a
// guest's position, not their moves, so a respawn or a teleport arrives as one
// enormous step -- which `moveEntity` never sees, because a1.1.2 places a
// teleported entity with `setPosition`. It is banked as nothing.

#include "core/block/registry.hpp"
#include "core/util/math_helper.hpp"
#include "core/util/types.hpp"

#include <cmath>

namespace mc::entity {

// Further than this between two ticks is a placement, not a walk: a sprinting
// player covers well under one block a tick, and a host hears from a guest at
// least every few ticks.
inline constexpr double kRemoteWalkJump = 4.0;

struct RemoteWalk {
    i32 entityId = 0;
    bool placed = false;
    double x = 0.0, z = 0.0;
    float walked = 0.0f;
    int nextStep = 1;

    // One tick of the body at `(px, feetY, pz)`. True when it pays out a step,
    // with the cell in `bx, by, bz` for `tick::entityWalkedOnBlock`.
    template <class Access>
    bool step(const Access& world, double px, double feetY, double pz, bool crouched,
              bool riding, i32* bx, int* by, i32* bz)
    {
        if (!placed) {
            placed = true;
            x = px;
            z = pz;
            return false;
        }
        const double dx = px - x;
        const double dz = pz - z;
        x = px;
        z = pz;
        const double moved = std::sqrt(dx * dx + dz * dz);
        if (moved > kRemoteWalkJump || riding) {
            return false;
        }
        walked += float(moved * 0.6);
        if (crouched) {
            return false;
        }
        // `posY - 0.2 - yOffset` is the feet less 0.2f widened.
        const i32 cx = MathHelper::floorDouble(px);
        const int cy = int(MathHelper::floorDouble(feetY - 0.20000000298023224));
        const i32 cz = MathHelper::floorDouble(pz);
        if (walked <= float(nextStep) || world.blockAt(cx, cy, cz) == block::kAir) {
            return false;
        }
        ++nextStep;
        *bx = cx;
        *by = cy;
        *bz = cz;
        return true;
    }
};

}  // namespace mc::entity
