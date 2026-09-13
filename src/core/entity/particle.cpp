// Every `nq` subclass a1.1.2 constructs: its constructor, its `onUpdate` and
// nothing else. See particle.hpp for the three sheets and for why the two
// random streams cannot be reproduced.

#include "core/entity/particle.hpp"

#include "core/block/collision.hpp"
#include "core/block/fluid_flow.hpp"
#include "core/block/registry.hpp"
#include "core/entity/sweep.hpp"
#include "core/item/registry.hpp"
#include "core/tick/tick_world.hpp"
#include "core/util/math_helper.hpp"

namespace mc::entity {
namespace {

// The two liquid materials this version has. `gb.d()` -- `Material.isLiquid` --
// is a field on the material and not a property of the block, and water and
// lava are the only two that set it; so the question "is this material a
// liquid" is exactly "is it one of these two" here.
constexpr u8 kWaterMaterial = mcver::kBlocks[int(mcver::Block::Water)].material;
constexpr u8 kLavaMaterial = mcver::kBlocks[int(mcver::Block::Lava)].material;

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

// `particleRed = f` and the two beside it, as the byte the vertex carries. The
// original multiplies a float in 0..1 into the quad's colour; this is the same
// number in the 0..255 the `DetailVertex` has room for.
u8 tint(float channel)
{
    const float clamped = channel < 0.0f ? 0.0f : (channel > 1.0f ? 1.0f : channel);
    return u8(clamped * 255.0f + 0.5f);
}

// `Entity.moveEntity`, minus the halves that belong to a player: no step up
// (`stepHeight` is zero for every particle), no sneak probe, no fall distance.
// **`noClip` is a whole branch and not a flag inside the sweep**, exactly as it
// is in `kh.c(DDD)`: a flame offsets its box and reads its position back out
// without ever asking the world what is there.
void moveParticle(const tick::TickWorld& world, Particle& p)
{
    if (p.noClip) {
        p.box = p.box.offset(p.motionX, p.motionY, p.motionZ);
        p.x = (p.box.minX + p.box.maxX) / 2.0;
        p.y = p.box.minY + kParticleHalf;
        p.z = (p.box.minZ + p.box.maxZ) / 2.0;
        p.onGround = false;
        return;
    }

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

    // The box is kept and the position read out of it, never the other way
    // round -- see the note on `Particle::box`.
    p.box = box;
    p.x = (box.minX + box.maxX) / 2.0;
    p.y = box.minY + kParticleHalf;
    p.z = (box.minZ + box.maxZ) / 2.0;
    p.onGround = wantY != dy && wantY < 0.0;

    if (p.motionX != dx) p.motionX = 0.0;
    if (p.motionY != dy) p.motionY = 0.0;
    if (p.motionZ != dz) p.motionZ = 0.0;
}

// The `(int)(N / (Math.random() * 0.8 + 0.2))` every subclass but the base uses
// for its lifetime. Written once because it is written eight times in the jar.
int lifeFrom(JavaRandom& rand, double span)
{
    return int(span / (rand.nextDouble() * 0.8 + 0.2));
}

// `7 - particleAge * 8 / particleMaxAge`, the smoke row walked backwards.
// **`maxAge` can be zero** -- `(int)(8.0 / 1.0)` cannot be, but `nl`'s
// `maxAge = (int)(maxAge * scale)` with a scale below one can -- and the jar
// divides by it anyway and throws. The guard is ours; the alternative is a
// crash the original only escapes because no call site passes a small enough
// scale.
u16 smokeTile(int age, int maxAge)
{
    if (maxAge <= 0) {
        return 0;
    }
    const int index = int(kParticleTileSmokeLast) - age * 8 / maxAge;
    return u16(index < 0 ? 0 : index);
}

}  // namespace

// **`nq`'s constructor, and every kind runs it first.** Most then overwrite
// part of what it produced -- three of them replace the motion outright -- but
// the draws happen in this order and in this number whatever follows, which is
// the only thing about the stream that can be copied at all.
Particle* ParticleSystem::construct(const tick::TickWorld& world, double px, double py,
                                    double pz, double mx, double my, double mz)
{
    Particle* slot = particles_.push();
    if (slot == nullptr) {
        ++refused_;
        return nullptr;
    }

    Particle& p = *slot;
    p = Particle{};
    p.setPosition(px, py, pz);

    // The jitter is `(random*2 - 1)` **narrowed to a float** before it is
    // scaled, which is a rounding step and not decoration.
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
    // makes a cloud rise before it falls.
    p.motionX = p.motionX / double(length) * double(speed) * 0.4000000059604645;
    p.motionY = p.motionY / double(length) * double(speed) * 0.4000000059604645
                + 0.10000000149011612;
    p.motionZ = p.motionZ / double(length) * double(speed) * 0.4000000059604645;

    p.jitterU = rand_.nextFloat() * 3.0f;
    p.jitterV = rand_.nextFloat() * 3.0f;

    p.scale = (rand_.nextFloat() * 0.5f + 0.5f) * 2.0f;
    p.birthScale = p.scale;

    // 4 divided by a tenth-to-a-whole, truncated: between 4 and 40 ticks, and
    // heavily weighted to the short end.
    p.maxAge = int(4.0f / (rand_.nextFloat() * 0.9f + 0.1f));
    p.age = 0;

    p.light = packedLightAt(world, px, py, pz);
    return slot;
}

void ParticleSystem::spawn(const tick::TickWorld& world, ParticleKind kind, double x,
                           double y, double z, double motionX, double motionY,
                           double motionZ)
{
    // **Six of the ten named kinds throw the caller's motion away**, because
    // `e.a` passes only the position to their constructors. That is not a
    // simplification here: `cn.a("smoke", x, y, z, mx, my, mz)` really does
    // ignore mx/my/mz in a1.1.2, and a reader checking a call site against the
    // jar should find the same surprise in both.
    const bool takesMotion = kind == ParticleKind::Bubble || kind == ParticleKind::Explode
                             || kind == ParticleKind::Flame
                             || kind == ParticleKind::Splash
                             || kind == ParticleKind::Digging;
    const double mx = takesMotion ? motionX : 0.0;
    const double my = takesMotion ? motionY : 0.0;
    const double mz = takesMotion ? motionZ : 0.0;

    Particle* slot = construct(world, x, y, z, mx, my, mz);
    if (slot == nullptr) {
        return;
    }
    Particle& p = *slot;
    p.kind = kind;

    switch (kind) {
    case ParticleKind::Bubble:
        // `ba`. The motion is rebuilt from the *argument*, so the base's
        // normalised vector is discarded -- a bubble drifts with whatever threw
        // it, at a fifth of that speed, and does not fly off on its own.
        p.tile = kParticleTileBubble;
        p.scale = p.scale * (rand_.nextFloat() * 0.6f + 0.2f);
        p.motionX = motionX * 0.20000000298023224
                    + double(float(rand_.nextDouble() * 2.0 - 1.0)) * 0.02;
        p.motionY = motionY * 0.20000000298023224
                    + double(float(rand_.nextDouble() * 2.0 - 1.0)) * 0.02;
        p.motionZ = motionZ * 0.20000000298023224
                    + double(float(rand_.nextDouble() * 2.0 - 1.0)) * 0.02;
        p.maxAge = lifeFrom(rand_, 8.0);
        break;

    case ParticleKind::Smoke:
    case ParticleKind::LargeSmoke: {
        // `nl`, whose only argument past the position is the scale: 1.0 for
        // `smoke` and 2.5 for `largesmoke`. The **lifetime is scaled too**,
        // which is why a chimney's puff lingers where a furnace's does not.
        const float size = kind == ParticleKind::LargeSmoke ? 2.5f : 1.0f;
        p.motionX *= 0.10000000149011612;
        p.motionY *= 0.10000000149011612;
        p.motionZ *= 0.10000000149011612;
        // A dark grey between black and 0.3 -- smoke is a *shadow* on the
        // sprite, not a colour.
        const u8 grey = tint(float(rand_.nextDouble() * 0.30000001192092896));
        p.red = p.green = p.blue = grey;
        p.scale *= 0.75f;
        p.scale *= size;
        p.birthScale = p.scale;
        p.maxAge = lifeFrom(rand_, 8.0);
        p.maxAge = int(float(p.maxAge) * size);
        p.tile = smokeTile(0, p.maxAge);
        break;
    }

    case ParticleKind::Explode:
        // `dp`. Pale rather than dark, up to seven times the size of a smoke
        // puff, and it lives four times as long.
        p.motionX = motionX + double(float(rand_.nextDouble() * 2.0 - 1.0)) * 0.05;
        p.motionY = motionY + double(float(rand_.nextDouble() * 2.0 - 1.0)) * 0.05;
        p.motionZ = motionZ + double(float(rand_.nextDouble() * 2.0 - 1.0)) * 0.05;
        {
            const u8 pale = tint(rand_.nextFloat() * 0.3f + 0.7f);
            p.red = p.green = p.blue = pale;
        }
        // Two draws multiplied: mostly small, occasionally very large.
        p.scale = rand_.nextFloat() * rand_.nextFloat() * 6.0f + 1.0f;
        p.maxAge = lifeFrom(rand_, 16.0) + 2;
        p.tile = smokeTile(0, p.maxAge);
        break;

    case ParticleKind::Flame:
        // `jb`. **A hundredth of the base's motion plus the caller's**, so a
        // torch flame barely moves and a flame thrown by something keeps that
        // throw.
        p.motionX = p.motionX * 0.009999999776482582 + motionX;
        p.motionY = p.motionY * 0.009999999776482582 + motionY;
        p.motionZ = p.motionZ * 0.009999999776482582 + motionZ;
        // **Six draws that move nothing.** `jb`'s constructor jitters the
        // position into three locals and never calls `setPosition` with them --
        // a dead store in the jar. The draws still happen, so they still happen
        // here; the result is still discarded, because discarding it is what
        // a1.1.2 does and a flame really does sit dead centre on its torch.
        for (int i = 0; i < 3; ++i) {
            (void) (rand_.nextFloat() - rand_.nextFloat());
        }
        p.birthScale = p.scale;
        p.maxAge = lifeFrom(rand_, 8.0) + 4;
        p.noClip = true;
        p.tile = kParticleTileFlame;
        p.lighting = ParticleLight::FadeFromFull;
        break;

    case ParticleKind::Lava:
        // `cq`. The base's lift is replaced by a real upward throw, so a pop
        // arcs out of the surface rather than rising.
        p.motionX *= 0.800000011920929;
        p.motionY *= 0.800000011920929;
        p.motionZ *= 0.800000011920929;
        p.motionY = double(rand_.nextFloat() * 0.4f + 0.05f);
        p.scale *= rand_.nextFloat() * 2.0f + 0.2f;
        p.birthScale = p.scale;
        p.maxAge = lifeFrom(rand_, 16.0);
        p.tile = kParticleTileLava;
        p.lighting = ParticleLight::Full;
        break;

    case ParticleKind::Splash:
    case ParticleKind::Rain:
        // `nf`, and `kq` is `nf` plus three lines. A raindrop is thrown a
        // third as far sideways as the base wanted and always upward.
        p.motionX *= 0.30000001192092896;
        p.motionY = double(float(rand_.nextDouble()) * 0.2f + 0.1f);
        p.motionZ *= 0.30000001192092896;
        p.tile = u16(int(kParticleTileRainFirst) + rand_.nextInt(kParticleTileRainSpan));
        p.gravity = 0.06f;
        p.maxAge = lifeFrom(rand_, 8.0);
        if (kind == ParticleKind::Splash) {
            p.gravity = 0.04f;
            p.tile = u16(p.tile + 1);
            // **The thrower's motion wins if there is any**, which is the whole
            // of `kq`: a splash from a swimming player travels with the player
            // and a splash from still water does not.
            if (motionY != 0.0 || motionX != 0.0 || motionZ != 0.0) {
                p.motionX = motionX;
                p.motionY = motionY + 0.1;
                p.motionZ = motionZ;
            }
        }
        break;

    case ParticleKind::Reddust:
        // `en`. The one tinted sprite: red at 0.7..1.0 with a tenth of green
        // and blue, so a wire's motes read as dust and not as fire.
        p.motionX *= 0.10000000149011612;
        p.motionY *= 0.10000000149011612;
        p.motionZ *= 0.10000000149011612;
        p.red = tint(float(rand_.nextDouble() * 0.30000001192092896) + 0.7f);
        p.green = p.blue = tint(float(rand_.nextDouble() * 0.10000000149011612));
        p.scale *= 0.75f;
        p.maxAge = lifeFrom(rand_, 8.0);
        p.birthScale = p.scale;
        p.tile = smokeTile(0, p.maxAge);
        break;

    case ParticleKind::SnowballPoof:
    case ParticleKind::Slime:
        // `ig`, whose whole difference is which `Item` it was handed.
        //
        // **Its gravity is `Block.snow`'s**, not the item's or its own: the
        // constructor reads `ly.aV.bm`, and `aV` is block 80. Every block in
        // this version has 1.0 there, so the line is indefensible and harmless
        // at once -- and it is copied rather than simplified because a version
        // where the snow block's gravity differs would follow the jar.
        p.tile = item::def(kind == ParticleKind::Slime ? kSlimeballItem : kSnowballItem).icon;
        p.gravity = kParticleGravity;
        p.scale /= 2.0f;
        break;

    case ParticleKind::Digging:
        // Never reached through `spawn` by anything in the jar -- see
        // `spawnDigging`, which is what `EffectRenderer` calls. Left whole
        // rather than unreachable so a caller that wants one gets a fleck of
        // stone rather than nothing.
        p.tile = block::def(block::BlockId(mcver::Block::Stone)).texture;
        p.gravity = kParticleGravity;
        p.red = p.green = p.blue = tint(0.6f);
        p.scale /= 2.0f;
        break;
    }

}

// `iw`, the one `nq` subclass `EffectRenderer` constructs itself. Its texture
// and its gravity both come off the block it was cut from, and it is the only
// kind whose colour is a flat darkening: **0.6 on all three channels**, which
// is what stops a cloud of flecks reading as brighter than the block it came
// off.
void ParticleSystem::spawnDigging(const tick::TickWorld& world, double px, double py,
                                  double pz, double mx, double my, double mz,
                                  block::BlockId block)
{
    Particle* slot = construct(world, px, py, pz, mx, my, mz);
    if (slot == nullptr) {
        return;
    }
    Particle& p = *slot;
    p.kind = ParticleKind::Digging;
    p.tile = block::def(block).texture;
    p.gravity = kParticleGravity;
    p.red = p.green = p.blue = tint(0.6f);
    // `particleScale /= 2.0F`, and the base already doubled it -- so this is
    // the same as not doubling it, written the way the jar writes it because
    // the base class's value is what every other kind starts from.
    p.scale /= 2.0f;
    p.birthScale = p.scale;
}

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
                spawnDigging(world, px, py, pz,
                             px - double(x) - 0.5, py - double(y) - 0.5,
                             pz - double(z) - 0.5, block);
            }
        }
    }
}

void ParticleSystem::addBlockHit(const tick::TickWorld& world, i32 x, int y, i32 z,
                                 int face)
{
    const block::BlockId block = world.blockAt(x, y, z);
    if (block == block::kAir) {
        return;
    }

    // `bq.a(IIII)`'s inset. A tenth of a block in from each edge of the block's
    // *render* bounds, so a fleck off a slab comes off the slab and not off the
    // cell it sits in.
    constexpr double kInset = 0.10000000149011612;
    const AABB bounds = block::selectionBox(block, world.dataAt(x, y, z));

    double px = double(x) + rand_.nextDouble() * (bounds.maxX - bounds.minX - kInset * 2.0)
                + kInset + bounds.minX;
    double py = double(y) + rand_.nextDouble() * (bounds.maxY - bounds.minY - kInset * 2.0)
                + kInset + bounds.minY;
    double pz = double(z) + rand_.nextDouble() * (bounds.maxZ - bounds.minZ - kInset * 2.0)
                + kInset + bounds.minZ;

    // ...and then pushed a tenth of a block *outside* the struck face, which is
    // what makes the chips come towards the player rather than out of the
    // block's middle. The face order is the usual one.
    switch (face) {
    case 0: py = double(y) + bounds.minY - kInset; break;
    case 1: py = double(y) + bounds.maxY + kInset; break;
    case 2: pz = double(z) + bounds.minZ - kInset; break;
    case 3: pz = double(z) + bounds.maxZ + kInset; break;
    case 4: px = double(x) + bounds.minX - kInset; break;
    case 5: px = double(x) + bounds.maxX + kInset; break;
    default: break;
    }

    const int before = particles_.size();
    spawnDigging(world, px, py, pz, 0.0, 0.0, 0.0, block);
    if (particles_.size() == before) {
        return;
    }
    Particle& p = particles_[before];

    // `nq.b(0.2F)` -- slow it, **about the lift rather than about zero**: the
    // +0.1 the base added is taken out, scaled and put back, so a chip that was
    // rising still rises.
    constexpr float kSlow = 0.2f;
    p.motionX *= double(kSlow);
    p.motionY = (p.motionY - 0.10000000149011612) * double(kSlow) + 0.10000000149011612;
    p.motionZ *= double(kSlow);
    // `nq.d(0.6F)` -- and shrink it. The box shrinks in the jar too; nothing
    // here reads a particle's box for anything but its own sweep, and a sweep
    // at 0.12 rather than 0.2 would let a chip through a gap the jar's chip
    // also fits through, so the box is left alone and only the quad changes.
    p.scale *= 0.6f;
    p.birthScale = p.scale;
}

void ParticleSystem::tick(const tick::TickWorld& world)
{
    int live = 0;
    for (int i = 0; i < particles_.size(); ++i) {
        Particle& p = particles_[i];

        p.prevX = p.x;
        p.prevY = p.y;
        p.prevZ = p.z;

        bool dead = false;
        switch (p.kind) {
        case ParticleKind::Bubble:
            // `ba.e_()`. No age at all: it rises, slows hard, and dies the tick
            // it finds itself outside water.
            p.motionY += 0.002;
            moveParticle(world, p);
            p.motionX *= 0.8500000238418579;
            p.motionY *= 0.8500000238418579;
            p.motionZ *= 0.8500000238418579;
            {
                const i32 bx = MathHelper::floorDouble(p.x);
                const int by = int(MathHelper::floorDouble(p.y));
                const i32 bz = MathHelper::floorDouble(p.z);
                if (block::def(world.blockAt(bx, by, bz)).material != kWaterMaterial) {
                    dead = true;
                }
            }
            // **Pre-decrement compare**: `if (f-- <= 0)`, so a bubble whose
            // countdown reaches zero is drawn on the tick that takes it there.
            if (p.maxAge-- <= 0) {
                dead = true;
            }
            break;

        case ParticleKind::Smoke:
        case ParticleKind::LargeSmoke:
        case ParticleKind::Reddust:
            // `nl.e_()` and `en.e_()`, which differ only in whether there is a
            // rise: smoke has 0.004 of one and dust has none.
            if (p.age++ >= p.maxAge) {
                dead = true;
                break;
            }
            p.tile = smokeTile(p.age, p.maxAge);
            if (p.kind != ParticleKind::Reddust) {
                p.motionY += 0.004;
            }
            moveParticle(world, p);
            // **Did not move vertically, so spread sideways instead.** This is
            // what makes a puff trapped under a ceiling crawl outward rather
            // than pile up, and it compounds at 1.1 a tick.
            if (p.y == p.prevY) {
                p.motionX *= 1.1;
                p.motionZ *= 1.1;
            }
            p.motionX *= 0.9599999785423279;
            p.motionY *= 0.9599999785423279;
            p.motionZ *= 0.9599999785423279;
            if (p.onGround) {
                p.motionX *= kParticleGroundDrag;
                p.motionZ *= kParticleGroundDrag;
            }
            break;

        case ParticleKind::Explode:
            if (p.age++ >= p.maxAge) {
                dead = true;
                break;
            }
            p.tile = smokeTile(p.age, p.maxAge);
            p.motionY += 0.004;
            moveParticle(world, p);
            p.motionX *= 0.8999999761581421;
            p.motionY *= 0.8999999761581421;
            p.motionZ *= 0.8999999761581421;
            if (p.onGround) {
                p.motionX *= kParticleGroundDrag;
                p.motionZ *= kParticleGroundDrag;
            }
            break;

        case ParticleKind::Flame:
            // `jb.e_()`. No gravity, no rise and no tile walk -- a flame is one
            // sprite that shrinks, and `noClip` keeps it inside its torch.
            if (p.age++ >= p.maxAge) {
                dead = true;
                break;
            }
            moveParticle(world, p);
            p.motionX *= 0.9599999785423279;
            p.motionY *= 0.9599999785423279;
            p.motionZ *= 0.9599999785423279;
            if (p.onGround) {
                p.motionX *= kParticleGroundDrag;
                p.motionZ *= kParticleGroundDrag;
            }
            break;

        case ParticleKind::Lava:
            // `cq.e_()`, and the interesting line is the smoke: the chance of
            // throwing one is **1 minus the fraction of its life elapsed**, so
            // a fresh pop smokes every tick and an old one hardly ever does.
            // That is the trail behind a lava spit, and it is why this kind is
            // the only one that spawns another.
            if (p.age++ >= p.maxAge) {
                dead = true;
                break;
            }
            {
                const float elapsed = p.maxAge > 0 ? float(p.age) / float(p.maxAge) : 1.0f;
                if (rand_.nextFloat() > elapsed) {
                    // **Appended to the pool this loop is walking, on purpose.**
                    // `EffectRenderer.updateEffects` iterates one list against a
                    // live `size()` and `addEffect` appends to it, so the smoke
                    // a pop throws is ticked on the tick it was thrown -- and a
                    // `SegmentedPool` reference survives a push into the same
                    // pool, which is what makes `p` safe to keep using here.
                    spawn(world, ParticleKind::Smoke, p.x, p.y, p.z, p.motionX, p.motionY,
                          p.motionZ);
                }
            }
            p.motionY -= 0.03;
            moveParticle(world, p);
            p.motionX *= 0.9990000128746033;
            p.motionY *= 0.9990000128746033;
            p.motionZ *= 0.9990000128746033;
            if (p.onGround) {
                p.motionX *= kParticleGroundDrag;
                p.motionZ *= kParticleGroundDrag;
            }
            break;

        case ParticleKind::Rain:
        case ParticleKind::Splash:
            // `nf.e_()`. A drop falls under its own small gravity, has half a
            // chance of dying the moment it lands, and dies outright if it
            // finds itself below the surface of the fluid it fell into.
            p.motionY -= double(p.gravity);
            moveParticle(world, p);
            p.motionX *= kParticleDrag;
            p.motionY *= kParticleDrag;
            p.motionZ *= kParticleDrag;
            if (p.maxAge-- <= 0) {
                dead = true;
            }
            if (p.onGround) {
                if (rand_.nextDouble() < 0.5) {
                    dead = true;
                }
                p.motionX *= kParticleGroundDrag;
                p.motionZ *= kParticleGroundDrag;
            }
            {
                const i32 bx = MathHelper::floorDouble(p.x);
                const int by = int(MathHelper::floorDouble(p.y));
                const i32 bz = MathHelper::floorDouble(p.z);
                const block::BlockDef& def = block::def(world.blockAt(bx, by, bz));
                const bool liquid =
                    def.material == kWaterMaterial || def.material == kLavaMaterial;
                if (liquid || def.solid) {
                    // `BlockFluid.getPercentAir(metadata)` -- how far below the
                    // cell's top the surface sits. A solid block reports the
                    // level of whatever metadata it happens to carry, which is
                    // the jar's behaviour and is harmless: the drop is already
                    // stopped on top of it.
                    const double top =
                        double(by + 1)
                        - double(block::fluidPercentAir(world.dataAt(bx, by, bz)));
                    if (p.y < top) {
                        dead = true;
                    }
                }
            }
            break;

        case ParticleKind::SnowballPoof:
        case ParticleKind::Slime:
        case ParticleKind::Digging:
            // `nq.e_()` itself: the three kinds that never override it.
            if (p.age++ >= p.maxAge) {
                dead = true;
                break;
            }
            p.motionY -= kParticlePull * double(p.gravity);
            moveParticle(world, p);
            p.motionX *= kParticleDrag;
            p.motionY *= kParticleDrag;
            p.motionZ *= kParticleDrag;
            if (p.onGround) {
                // Only the two horizontal axes: a particle that has landed
                // skids to a stop rather than bouncing.
                p.motionX *= kParticleGroundDrag;
                p.motionZ *= kParticleGroundDrag;
            }
            break;
        }

        if (dead) {
            continue;  // not copied down, which is how it is removed
        }

        Particle& kept = particles_[i];
        kept.light = packedLightAt(world, kept.x, kept.y, kept.z);

        if (live != i) {
            particles_[live] = kept;
        }
        ++live;
    }
    particles_.truncate(live);
    particles_.trim();
}

void bindParticles(tick::TickWorld& world, ParticleSystem* system)
{
    if (system == nullptr) {
        world.setParticleSink(nullptr, nullptr);
        return;
    }
    world.setParticleSink(
        [](void* ctx, int kind, double x, double y, double z, double mx, double my,
           double mz) {
            // The seam carries the kind as an integer so `core/tick` need not
            // include this file; it is put back here, where the enum lives.
            if (kind < 0 || kind >= kParticleKindCount) {
                return;
            }
            auto* pool = static_cast<ParticleSystem*>(ctx);
            pool->spawn(*pool->world_, ParticleKind(kind), x, y, z, mx, my, mz);
        },
        system);
    system->world_ = &world;
}

}  // namespace mc::entity
