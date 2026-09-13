// `jd` -- EntityTNTPrimed. See primed_tnt.hpp for the transcription and for
// what a1.1.2 does and does not give this entity.

#include "core/entity/primed_tnt.hpp"

#include "core/entity/explosion.hpp"
#include "core/entity/mob.hpp"
#include "core/entity/particle.hpp"
#include "core/entity/fire_entry.hpp"
#include "core/entity/sweep.hpp"
#include "core/tick/tick_world.hpp"
#include "core/util/math_helper.hpp"

namespace mc::entity {
namespace {

// The same `(sky << 4) | block` byte every mesh vertex carries.
u8 packedLightAt(const tick::TickWorld& world, double x, double y, double z)
{
    const i32 bx = MathHelper::floorDouble(x);
    const int by = int(MathHelper::floorDouble(y));
    const i32 bz = MathHelper::floorDouble(z);
    return u8((world.skyLightAt(bx, by, bz) << 4) | world.blockLightAt(bx, by, bz));
}

}  // namespace

PrimedTnt* PrimedTntSystem::allocate()
{
    PrimedTnt* e = items_.push();
    if (e == nullptr) {
        ++refused_;
    }
    return e;
}

void PrimedTntSystem::removeAt(int index)
{
    items_.swapRemove(index);
}

bool PrimedTntSystem::spawn(const tick::TickWorld& world, i32 x, int y, i32 z, int fuse)
{
    PrimedTnt* e = allocate();
    if (e == nullptr) {
        return false;
    }

    // `new jd(world, i + 0.5F, j + 0.5F, k + 0.5F)` -- the halves are floats in
    // the class file and the constructor widens them, which for these values is
    // exact.
    e->setPosition(double(x) + 0.5, double(y) + 0.5, double(z) + 0.5);
    e->fuse = fuse;
    e->onGround = false;
    e->active = true;

    // **`Math.random()`, not the world's generator**, and then the angle is
    // converted to radians a second time:
    //
    // ```
    // float f = (float)(Math.random() * Math.PI * 2D);
    // motionX = -MathHelper.sin(f * (float)Math.PI / 180F) * 0.02F;
    // motionY = 0.20000000298023224D;
    // motionZ = -MathHelper.cos(f * (float)Math.PI / 180F) * 0.02F;
    // ```
    //
    // The argument `f` is already radians, so the `* PI / 180` shrinks a full
    // turn to a **0.11-radian arc**: sin stays under 0.11 and cos stays over
    // 0.994, which means every primed block in a1.1.2 hops very slightly
    // towards -Z and hardly at all on x. It reads as a straight hop, not a
    // scatter, and that is what the jar does -- the double conversion is in
    // the class file, at offsets 33-38 and 60-65 of `jd.<init>(cn,FFF)`.
    const float f = float(rand_.nextDouble() * 3.1415927410125732 * 2.0);
    const float angle = f * 3.1415927f / 180.0f;
    e->motionX = double(-MathHelper::sin(angle) * kPrimedTntScatter);
    e->motionY = kPrimedTntLaunch;
    e->motionZ = double(-MathHelper::cos(angle) * kPrimedTntScatter);

    e->light = packedLightAt(world, e->x, e->y, e->z);
    return true;
}

void PrimedTntSystem::tick(tick::TickWorld& world, MobSystem* mobs,
                           const MobSurroundings* around)
{
    for (int i = 0; i < items_.size();) {
        PrimedTnt& e = items_[i];
        // Restored entities can precede the chunks beneath them.
        if (!world.chunkResident(MathHelper::floorDouble(e.x) >> 4,
                                 MathHelper::floorDouble(e.z) >> 4)) {
            ++i;
            continue;
        }

        e.prevX = e.x;
        e.prevY = e.y;
        e.prevZ = e.z;
        e.motionY -= kPrimedTntPull;

        // `moveEntity`, through the sweep the player, the particles and the
        // falling blocks all share. No step up: `stepHeight` is the base
        // class's zero.
        AABB box = e.box;
        double dx = e.motionX;
        double dy = e.motionY;
        double dz = e.motionZ;
        const double wantY = dy;

        const BlockRange range = sweepRange(box.extend(dx, dy, dz));
        dy = clipAxis(world, range, box, kAxisY, dy);
        box = box.offset(0.0, dy, 0.0);
        dx = clipAxis(world, range, box, kAxisX, dx);
        box = box.offset(dx, 0.0, 0.0);
        dz = clipAxis(world, range, box, kAxisZ, dz);
        box = box.offset(0.0, 0.0, dz);

        e.box = box;
        e.x = (box.minX + box.maxX) / 2.0;
        e.y = (box.minY + box.maxY) / 2.0;
        e.z = (box.minZ + box.maxZ) / 2.0;
        e.onGround = wantY != dy && wantY < 0.0;

        if (e.motionX != dx) e.motionX = 0.0;
        if (e.motionY != dy) e.motionY = 0.0;
        if (e.motionZ != dz) e.motionZ = 0.0;


        // **`moveEntity`'s tail**, which for primed TNT is the counter and the
        // hiss and nothing else: `kh.a(Lkh;I)Z` is `return false` and `jd`
        // does not override it, so fire chars a lit block without harming it.
        // See core/entity/fire_entry.hpp.
        {
            const FireEntryResult burn = updateFireEntry(
                &e.fire, boundingBoxBurning(world, e.box), fireWetProbe(world, e.box));
            if (burn.fizz) {
                world.playSoundAt(kFizzSound, e.x, e.y, e.z, 0.7f, fizzPitch(rand_));
            }
        }

        e.motionX *= kPrimedTntDrag;
        e.motionY *= kPrimedTntDrag;
        e.motionZ *= kPrimedTntDrag;

        // **Every tick it is on the ground, not once on the tick it lands.**
        // The bounce is what makes TNT dropped from a height hop twice before
        // settling, and the 0.7 is what stops it sliding off a slope.
        if (e.onGround) {
            e.motionX *= kPrimedTntLandDrag;
            e.motionZ *= kPrimedTntLandDrag;
            e.motionY *= kPrimedTntBounce;
        }

        e.light = packedLightAt(world, e.x, e.y, e.z);

        // `if (fuse-- <= 0)` -- the **old** value decides, so a fuse of 80
        // smokes eighty times and blows on the eighty-first tick.
        const int remaining = e.fuse--;
        if (remaining > 0) {
            // `spawnParticle("smoke", posX, posY + 0.5, posZ, 0, 0, 0)`.
            world.spawnParticle(int(ParticleKind::Smoke), e.x, e.y + 0.5, e.z);
            ++i;
            continue;
        }

        // `F()` then `i()` -- dead first, then the blast, and the order is
        // why a chain does not double-count: the entity is gone before
        // anything it lights can exist.
        const double bx = e.x;
        const double by = e.y;
        const double bz = e.z;
        removeAt(i);

        // `cn.a(Lkh;DDDF)V` with a null exploder and `f = 4.0F`. Phase two is
        // handed the same targets a creeper's is; with neither set this is
        // `createExplosion` and moves blocks only.
        //
        // **Phase three can land back in this pool.** A recorded cell holding
        // TNT re-primes rather than detonating, so the push is a fresh slot at
        // the end and the loop below reaches it on this same pass -- which is
        // a1.1.2's own behaviour, since `World.updateEntities` walks
        // `loadedEntityList` by index and an entity spawned mid-loop is
        // appended to it. Its fuse is 10..29, so nothing goes off twice in a
        // tick and the recursion is one level deep.
        Explosion blast(bx, by, bz, kPrimedTntStrength);
        blast.cast(world);
        applyBlast(world, blast, mobs, around);
        blast.destroy(world);
    }
    items_.trim();
}

}  // namespace mc::entity
