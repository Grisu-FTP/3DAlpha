#pragma once

// **The block-breaking particles** -- `EffectRenderer.addBlockDestroyEffects`
// and the `EntityDiggingFX` it spawns, which are `Entity`s and move like one.
//
// Sixty-four per broken block: the cell is cut into a 4x4x4 grid and one
// particle starts at each sub-cell's centre, thrown outward along the offset
// from the block's middle. That is why the cloud comes apart in the shape of
// the block rather than puffing straight up.
//
// **They collide.** `EntityFX.onUpdate` calls `moveEntity`, so a particle lands
// on the ground, slides along it, and stops in a corner -- see
// core/entity/sweep.hpp, which the player and the particles now share. It also
// means a particle is exactly as expensive to move as the player is, which is
// what the pool size below is really about.
//
// **Two random streams, and neither can be reproduced.** The original draws its
// velocity jitter from `Math.random()` -- a global, time-seeded generator --
// and its texture, scale and lifetime from the entity's own `new Random()`,
// also time-seeded. Nothing about either is deterministic in the original
// either, so what this copies is the *distribution* and not the sequence: one
// `JavaRandom`, seeded by the caller, drawn in the original's order.
//
// Drawing is elsewhere on purpose (core/render/particle_mesh.hpp): a particle
// is a position and an age here, and what a camera makes of that is the
// renderer's business. That split is also what lets the whole thing be tested
// on a machine with no GPU.

#include "core/block/block_def.hpp"
#include "core/util/aabb.hpp"
#include "core/util/java_random.hpp"
#include "core/util/segmented_pool.hpp"
#include "core/util/types.hpp"

namespace mc::tick {
class TickWorld;
}

namespace mc::entity {

// `EntityFX` is 0.2 on a side, and its `yOffset` is half its height -- so the
// position is the *centre* of the box, not its floor. Everything here follows
// from that: the box is a pure function of the position and is rebuilt rather
// than stored.
inline constexpr double kParticleSize = 0.2;
inline constexpr double kParticleHalf = kParticleSize / 2.0;

// `Block.blockParticleGravity`, and it is **1.0 for all seventy blocks** in
// a1.1.2 -- measured by reading the field off every constructed block rather
// than assumed, which is why it is a constant here and not a table column.
inline constexpr float kParticleGravity = 1.0f;

// `EntityFX.onUpdate`'s three constants: the pull, the drag it applies every
// tick, and the extra drag on the two horizontal axes once it has landed.
inline constexpr double kParticlePull = 0.04;
inline constexpr double kParticleDrag = 0.9800000190734863;
inline constexpr double kParticleGroundDrag = 0.699999988079071;

struct Particle {
    // **The box is the authority and the position is derived from it**, which
    // is the way round `Entity` has it -- `moveEntity` clips the box and then
    // reads the position back out of it.
    //
    // Deriving the other way costs a particle its floor. The clip lands the
    // box's bottom exactly on the block's top; rebuilding that box next tick
    // from a rounded centre puts it a fraction of an ulp *below*, and the very
    // next `calculateYOffset` -- whose guard is `mover.minY >= block.maxY` --
    // then declines to stop it and the fleck falls through the world. That is
    // not a hypothetical: it is what the first version of this did, and the
    // test that caught it is `particles_fall_land_and_stop`.
    AABB box{};

    // `prev` is last tick's position, kept so a frame between two ticks can
    // interpolate rather than stepping at 20 Hz.
    double x = 0.0, y = 0.0, z = 0.0;
    double prevX = 0.0, prevY = 0.0, prevZ = 0.0;
    double motionX = 0.0, motionY = 0.0, motionZ = 0.0;

    // `particleScale`, already halved by `EntityDiggingFX`'s constructor. The
    // quad drawn is `0.1f * scale` to a side.
    float scale = 1.0f;

    // Where in the block's own 16x16 tile this fleck came from: 0..3 in
    // quarters, as `particleTextureJitterX/Y`. A digging particle shows a
    // quarter of a tile, which is what makes the cloud look like the block.
    float jitterU = 0.0f, jitterV = 0.0f;

    int age = 0;
    int maxAge = 0;

    // The block's `blockIndexInTexture` -- one tile, not a face.
    u16 tile = 0;

    // `(sky << 4) | block` at the particle, resampled every tick as the
    // original resamples it every frame.
    u8 light = 0;

    bool onGround = false;

    // `Entity.setPosition`: the box is centred on x and z and hangs half its
    // height either side of y, because `EntityFX`'s `yOffset` is half its
    // height. Called once, at spawn; after that the box moves and the position
    // follows it.
    void setPosition(double px, double py, double pz)
    {
        x = prevX = px;
        y = prevY = py;
        z = prevZ = pz;
        box = AABB{px - kParticleHalf, py - kParticleHalf, pz - kParticleHalf,
                   px + kParticleHalf, py + kParticleHalf, pz + kParticleHalf};
    }
};

// **No cap**: `EffectRenderer.addEffect` (`bq.a(Lnq;)V`) is one `List.add`.
//
// **The first 512 -- eight blocks' worth -- are held from construction.** A
// break is 64 particles, the longest a particle can live is 40 ticks
// (`4.0f / 0.1f`), and the edit path repeats every 5 ticks, so a player holding
// the break button has at most eight clouds in the air at once and never
// allocates. Past that the pool grows until the heap says stop (see
// core/util/segmented_pool.hpp); only then is the *newest* refused and counted:
// dropping the oldest would make a cloud vanish mid-flight, which looks like a
// bug, where refusing a new one looks like nothing at all.
class ParticleSystem {
public:
    // Held from construction, so ordinary play never allocates; not a limit.
    static constexpr int kInitialCapacity = 512;

    explicit ParticleSystem(i64 seed) : rand_(seed) {}

    // `bq.a(III)V` -- 64 particles from the block at these coordinates, which
    // must still be **there**: the original spawns before it clears the cell,
    // and a particle needs the block's texture.
    void addBlockDestroy(const tick::TickWorld& world, i32 x, int y, i32 z);

    // One 20 Hz tick of `EntityFX.onUpdate` for every live particle.
    void tick(const tick::TickWorld& world);

    void clear() { particles_.clear(); }

    int count() const { return particles_.size(); }
    const Particle& operator[](int i) const { return particles_[i]; }

    // How many spawns the pool has refused since it was made -- only ever
    // because the heap would not hold more. On the debug page.
    u32 refused() const { return refused_; }

private:
    void spawn(const tick::TickWorld& world, double px, double py, double pz,
               double mx, double my, double mz, block::BlockId block);

    SegmentedPool<Particle, kInitialCapacity> particles_;
    u32 refused_ = 0;
    JavaRandom rand_;
};

}  // namespace mc::entity
