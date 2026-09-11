// `EntityDiggingFX` and the cloud of them a broken block makes. See
// particle.hpp for why the two random streams cannot be reproduced.

#include "core/entity/particle.hpp"

#include "core/block/registry.hpp"
#include "core/entity/sweep.hpp"
#include "core/tick/tick_world.hpp"
#include "core/util/math_helper.hpp"

namespace mc::entity {
namespace {

// `EntityFX.getBrightness` asks `world.getLightBrightness` at the cell it is
// standing in, and does it every frame. Here it is once a tick, in the same
// `(sky << 4) | block` byte every mesh vertex carries -- so a particle is lit
// by the shader that lights the block it came off, and a cloud thrown into a
// cave darkens as it flies into it.
u8 packedLightAt(const tick::TickWorld& world, double x, double y, double z)
{
    const i32 bx = MathHelper::floorDouble(x);
    const int by = int(MathHelper::floorDouble(y));
    const i32 bz = MathHelper::floorDouble(z);
    return u8((world.skyLightAt(bx, by, bz) << 4) | world.blockLightAt(bx, by, bz));
}

}  // namespace

void ParticleSystem::addBlockDestroy(const tick::TickWorld& world, i32 x, int y, i32 z)
{
    const block::BlockId block = world.blockAt(x, y, z);
    if (block == block::kAir) {
        return;
    }

    // Four along each axis. The position is the sub-cell's centre and the
    // velocity handed to the particle is **that position minus the block's
    // middle** -- so a fleck from a corner is thrown at the corner and one from
    // the centre barely moves before the jitter takes over.
    constexpr int kPerAxis = 4;
    for (int a = 0; a < kPerAxis; ++a) {
        for (int b = 0; b < kPerAxis; ++b) {
            for (int c = 0; c < kPerAxis; ++c) {
                const double px = double(x) + (double(a) + 0.5) / double(kPerAxis);
                const double py = double(y) + (double(b) + 0.5) / double(kPerAxis);
                const double pz = double(z) + (double(c) + 0.5) / double(kPerAxis);
                spawn(world, px, py, pz,
                      px - double(x) - 0.5, py - double(y) - 0.5, pz - double(z) - 0.5,
                      block);
            }
        }
    }
}

void ParticleSystem::spawn(const tick::TickWorld& world, double px, double py, double pz,
                           double mx, double my, double mz, block::BlockId block)
{
    Particle* slot = particles_.push();
    if (slot == nullptr) {
        ++refused_;
        return;
    }

    Particle& p = *slot;
    p.setPosition(px, py, pz);

    // `EntityFX`'s constructor, in its order. The jitter is `(random*2 - 1)`
    // **narrowed to a float** before it is scaled, which is a rounding step and
    // not decoration.
    p.motionX = mx + double(float(rand_.nextDouble() * 2.0 - 1.0)) * 0.4;
    p.motionY = my + double(float(rand_.nextDouble() * 2.0 - 1.0)) * 0.4;
    p.motionZ = mz + double(float(rand_.nextDouble() * 2.0 - 1.0)) * 0.4;

    // Two draws added together and a constant: a triangular distribution, so
    // most flecks get a middling speed and few get an extreme one.
    const float speed = float(rand_.nextDouble() + rand_.nextDouble() + 1.0) * 0.15f;
    const float length = MathHelper::sqrtDouble(p.motionX * p.motionX
                                                + p.motionY * p.motionY
                                                + p.motionZ * p.motionZ);

    // Normalise, scale to that speed, then **lift**: the +0.1 on y is what
    // makes the cloud rise before it falls.
    p.motionX = p.motionX / double(length) * double(speed) * 0.4000000059604645;
    p.motionY = p.motionY / double(length) * double(speed) * 0.4000000059604645
                + 0.10000000149011612;
    p.motionZ = p.motionZ / double(length) * double(speed) * 0.4000000059604645;

    p.jitterU = rand_.nextFloat() * 3.0f;
    p.jitterV = rand_.nextFloat() * 3.0f;

    // `(nextFloat * 0.5 + 0.5) * 2`, then halved again by `EntityDiggingFX` --
    // which is the same as not doubling it in the first place, and is written
    // as the original writes it because the base class's value is what every
    // other particle type would use.
    p.scale = ((rand_.nextFloat() * 0.5f + 0.5f) * 2.0f) / 2.0f;

    // 4 divided by a tenth-to-a-whole, truncated: between 4 and 40 ticks, and
    // heavily weighted to the short end.
    p.maxAge = int(4.0f / (rand_.nextFloat() * 0.9f + 0.1f));
    p.age = 0;

    p.tile = block::def(block).texture;
    p.light = packedLightAt(world, px, py, pz);
    p.onGround = false;
}

void ParticleSystem::tick(const tick::TickWorld& world)
{
    int live = 0;
    for (int i = 0; i < particles_.size(); ++i) {
        Particle& p = particles_[i];

        p.prevX = p.x;
        p.prevY = p.y;
        p.prevZ = p.z;

        // `if (particleAge++ >= particleMaxAge) setDead();` -- post-increment,
        // so a particle whose maxAge is 4 is drawn on five ticks.
        if (p.age++ >= p.maxAge) {
            continue;  // dead: not copied down, which is how it is removed
        }

        p.motionY -= kParticlePull * double(kParticleGravity);

        // `moveEntity`, minus the halves that belong to a player: no step up
        // (`stepHeight` is zero), no sneak probe, no fall distance.
        AABB box = p.box;
        double dx = p.motionX;
        double dy = p.motionY;
        double dz = p.motionZ;
        const double wantY = dy;

        const BlockRange range = sweepRange(box.extend(dx, dy, dz));
        dy = clipAxis(world, range, box, kAxisY, dy);
        box = box.offset(0.0, dy, 0.0);
        dx = clipAxis(world, range, box, kAxisX, dx);
        box = box.offset(dx, 0.0, 0.0);
        dz = clipAxis(world, range, box, kAxisZ, dz);
        box = box.offset(0.0, 0.0, dz);

        // The box is kept and the position read out of it, never the other
        // way round -- see the note on `Particle::box`.
        p.box = box;
        p.x = (box.minX + box.maxX) / 2.0;
        p.y = box.minY + kParticleHalf;
        p.z = (box.minZ + box.maxZ) / 2.0;
        p.onGround = wantY != dy && wantY < 0.0;

        if (p.motionX != dx) p.motionX = 0.0;
        if (p.motionY != dy) p.motionY = 0.0;
        if (p.motionZ != dz) p.motionZ = 0.0;

        p.motionX *= kParticleDrag;
        p.motionY *= kParticleDrag;
        p.motionZ *= kParticleDrag;
        if (p.onGround) {
            // Only the two horizontal axes: a particle that has landed skids to
            // a stop rather than bouncing.
            p.motionX *= kParticleGroundDrag;
            p.motionZ *= kParticleGroundDrag;
        }

        p.light = packedLightAt(world, p.x, p.y, p.z);

        if (live != i) {
            particles_[live] = p;
        }
        ++live;
    }
    particles_.truncate(live);
    particles_.trim();
}

}  // namespace mc::entity
