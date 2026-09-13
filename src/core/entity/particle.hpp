#pragma once

// **Every particle a1.1.2 has**, as one pool. `nq` -- EntityFX -- and the ten
// subclasses of it that this version constructs.
//
// **Twelve kinds, and ten of them have a name.** `cn.a(String, DDDDDD)` --
// `World.spawnParticle` -- is a chain of ten string compares and the name is
// the whole API: everything in the jar that wants a puff of smoke calls it with
// `"smoke"`. `ParticleKind` is that list, in the jar's own order, so a call
// site here reads as the line it was derived from. The two without a name are
// the two nothing spawns through it: the digging fleck (`iw`, which
// `EffectRenderer` builds directly) and the raindrop (`nf`, built by
// `EntityRenderer`).
//
// **A particle is an `Entity` and moves like one.** `nq.e_` calls `moveEntity`,
// so a fleck lands on the ground, slides along it and stops in a corner -- see
// core/entity/sweep.hpp, which the player and the particles share. It also
// means a particle is exactly as expensive to move as the player is, which is
// what the pool size below is really about. The one exception is the flame,
// which sets `noClip` and passes through the torch it sits on.
//
// **Three sheets, because the original has three lists.** `nq.c()` answers a
// layer and `EffectRenderer` binds one texture per layer:
//
//   | layer | texture           | kinds                                     |
//   |-------|-------------------|-------------------------------------------|
//   | 0     | `particles.png`   | the eight that are a sprite               |
//   | 1     | `terrain.png`     | `Digging` -- a quarter of a block's tile  |
//   | 2     | `gui/items.png`   | `SnowballPoof`, `Slime` -- an item icon   |
//
// Layer 3 is `cd`, the pickup animation, which draws an entity *model* rather
// than a quad and is not a particle in any sense this file can hold.
//
// **Two random streams, and neither can be reproduced.** The original draws its
// velocity jitter from `Math.random()` -- a global, time-seeded generator --
// and its texture, scale and lifetime from the entity's own `new Random()`,
// also time-seeded. Nothing about either is deterministic in the original
// either, so what this copies is the *distribution* and not the sequence: one
// `JavaRandom`, seeded by the caller, drawn in the original's order.
//
// Drawing is elsewhere on purpose (core/render/particle_mesh.hpp): a particle
// is a position, an age and a colour here, and what a camera makes of that is
// the renderer's business. That split is also what lets the whole thing be
// tested on a machine with no GPU.

#include "core/block/block_def.hpp"
#include "core/item/item_def.hpp"
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
//
// **Two kinds are smaller and say so.** `ba` (bubble) calls `setSize(0.02F)`
// and `nf` (rain) calls `setSize(0.01F)`, both after the base constructor has
// already placed the box -- so the box they end up with is the 0.2 one, and the
// only thing the smaller size changes is `width`, which the water branch of
// `Entity.onEntityUpdate` reads to decide how many bubbles to throw. Nothing
// here re-boxes them, because the jar does not either.
inline constexpr double kParticleSize = 0.2;
inline constexpr double kParticleHalf = kParticleSize / 2.0;

// `Block.blockParticleGravity`, and it is **1.0 for all seventy blocks** in
// a1.1.2 -- measured by reading the field off every constructed block rather
// than assumed, which is why it is a constant here and not a table column. Both
// the digging fleck (`iw`, off the block it came from) and the breaking fleck
// (`ig`, off `Block.snow` for no reason anybody can defend) take their gravity
// from it.
inline constexpr float kParticleGravity = 1.0f;

// `EntityFX.onUpdate`'s three constants: the pull, the drag it applies every
// tick, and the extra drag on the two horizontal axes once it has landed. The
// ground drag is the one number every kind shares -- all ten subclasses repeat
// it verbatim -- where the pull and the air drag are overridden freely.
inline constexpr double kParticlePull = 0.04;
inline constexpr double kParticleDrag = 0.9800000190734863;
inline constexpr double kParticleGroundDrag = 0.699999988079071;

// **The ten names `cn.a(String, DDDDDD)` knows, and the two it does not.**
//
// The order is the jar's compare chain, which is worth keeping because it is
// the order a reader checking this against `e.class` will meet them in. The
// values are an implementation detail: nothing saved or sent has ever held one.
enum class ParticleKind : u8 {
    // `ba` -- a rising bubble that dies the moment it leaves the water.
    Bubble,
    // `nl` at scale 1. Eight frames of the smoke row, fading as it ages.
    Smoke,
    // `dp` -- the same eight frames, paler, larger and longer-lived.
    Explode,
    // `jb` -- a torch flame. Does not collide and starts full bright.
    Flame,
    // `cq` -- a lava pop, which throws `Smoke` behind it as it falls.
    Lava,
    // `kq` -- `nf` with the thrower's motion, one frame further along the
    // splash row.
    Splash,
    // `nl` at scale 2.5. Not a kind of its own in the jar either -- the
    // constructor argument is the whole difference -- but the call sites name
    // it, so this does too.
    LargeSmoke,
    // `en` -- the redstone dust mote. Red, and the only tinted sprite.
    Reddust,
    // `ig` with `Item.snowball`. An item icon, so layer 2.
    SnowballPoof,
    // `ig` with `Item.slimeball`. The same, in green.
    Slime,

    // **Not reachable by name.** `iw`, built by `EffectRenderer` itself, and
    // `nf`, built by `EntityRenderer.addRainParticles`.
    //
    // **And nothing spawns `Rain` in a1.1.2 either.**
    // `EntityRenderer.addRainParticles` is guarded by `Minecraft.J`, a field
    // initialised to `false` and **assigned nowhere in the jar** -- so the
    // whole method is dead code in this version, which is a large part of why
    // a1.1.2 has no weather to speak of. `Rain` is here because `Splash` is its
    // subclass and inherits the whole of its tick; it is spawned by nothing,
    // exactly as in the original.
    Digging,
    Rain,
};
inline constexpr int kParticleKindCount = 12;

// Which of `EffectRenderer`'s three quad layers a kind belongs to -- `nq.c()`.
enum class ParticleSheet : u8 {
    Particles,  // layer 0, `/particles.png`
    Terrain,    // layer 1, `/terrain.png`
    Items,      // layer 2, `/gui/items.png`
};
inline constexpr int kParticleSheetCount = 3;

constexpr ParticleSheet sheetOf(ParticleKind kind)
{
    return kind == ParticleKind::Digging          ? ParticleSheet::Terrain
           : (kind == ParticleKind::SnowballPoof
              || kind == ParticleKind::Slime)     ? ParticleSheet::Items
                                                  : ParticleSheet::Particles;
}

// **`particles.png` is a 16 x 16 grid, exactly as terrain.png is.** `nq`'s
// render is `(b % 16) / 16` and `(b / 16) / 16` with a span of 0.0624375, so
// the sheet is indexed in whole tiles of a sixteenth -- 8 px each in the
// original's 128 x 128 image. The four tiles below are the ones a1.1.2 names.
//
// **The smoke row is eight frames and is walked backwards.** `nl`, `dp` and
// `en` all set `b = 7 - age * 8 / maxAge` every tick, so a puff starts at tile
// 7 -- the densest -- and thins to tile 0 as it dies.
inline constexpr u16 kParticleTileSmokeLast = 7;
inline constexpr u16 kParticleTileBubble = 32;
inline constexpr u16 kParticleTileFlame = 48;
inline constexpr u16 kParticleTileLava = 49;
// `nf` draws `19 + rand.nextInt(4)`; `kq` adds one to whatever `nf` chose.
inline constexpr u16 kParticleTileRainFirst = 19;
inline constexpr int kParticleTileRainSpan = 4;

// **The two items `ig` is ever constructed with.** `di.aB` is `new bp(76)` and
// `di.aK` is `new di(85)`, and an `Item`'s id is its constructor argument plus
// 256 -- so 332 and 341, which is the snowball and the slimeball. Written as
// the ids they are because `ig` takes an `Item`, not a name.
inline constexpr item::ItemId kSnowballItem = 332;
inline constexpr item::ItemId kSlimeballItem = 341;

// How a kind's brightness is found. `nq.a(float)` is the world's light at the
// particle for nine of the twelve; two override it.
enum class ParticleLight : u8 {
    // `nq.a(float)` -- the light of the cell it is standing in.
    World,
    // `cq.a(float)` returns 1.0F for its whole life: a lava pop is its own
    // light source and a dark cave does not dim it.
    Full,
    // `jb.a(float)` is `world * f + (1 - f)` with `f` the fraction of its life
    // elapsed -- so a flame is born full bright and settles onto the world's
    // light as it burns out.
    FadeFromFull,
};

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

    // `particleScale`, the live value the quad is built from. Four kinds
    // rewrite it every frame out of `birthScale` and their age; the rest never
    // touch it after the constructor.
    float scale = 1.0f;

    // **`particleScale` as the constructor left it**, which four of the
    // subclasses keep a private copy of (`nl.a`, `en.a`, `jb.a`, `cq.a`)
    // because their render overrides scale *from* it rather than compounding.
    // A field rather than a recomputation for the same reason it is a field in
    // the jar: the ramp is not invertible.
    float birthScale = 1.0f;

    // `Entity.particleGravity` -- `h`. **Zero on the base class**, which is the
    // thing to notice: smoke, flame, reddust and the bubble have no gravity at
    // all, and the ones that fall do so because their own constructor set this.
    float gravity = 0.0f;

    // Where in the tile this fleck came from: 0..3 in quarters, as
    // `particleTextureJitterX/Y`. **Only the two sheets that show a quarter of
    // a tile read these** -- `iw` and `ig` -- because only they are cut out of
    // an image whose tiles are a whole block or a whole icon. A sprite off
    // `particles.png` shows its tile whole.
    float jitterU = 0.0f, jitterV = 0.0f;

    int age = 0;

    // `particleMaxAge`. **Three kinds count it down instead of comparing
    // against it**: `ba` and `nf` (and so `kq`) never touch `particleAge` at
    // all and die on `--particleMaxAge <= 0`. Their `age` therefore stays 0,
    // which is correct and is why nothing may assume `age` is a clock.
    int maxAge = 0;

    // The tile this frame shows. A block's `blockIndexInTexture` for `Digging`,
    // an `Item.getIconIndex` for the two off the item sheet, and a tile of
    // `particles.png` for the rest -- rewritten every tick by the three that
    // walk the smoke row.
    u16 tile = 0;

    // `particleRed/Green/Blue`, which is a tint the quad is multiplied by and
    // not a colour it is drawn in. 0xFF on all three is the base class's 1.0.
    u8 red = 0xFF, green = 0xFF, blue = 0xFF;

    // `(sky << 4) | block` at the particle, resampled every tick as the
    // original resamples it every frame.
    u8 light = 0;

    ParticleKind kind = ParticleKind::Digging;
    ParticleLight lighting = ParticleLight::World;

    bool onGround = false;

    // `Entity.noClip`. **True for exactly one kind**: `jb` sets it so a torch
    // flame is not stopped by the torch. Everything else sweeps.
    bool noClip = false;

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

    ParticleSheet sheet() const { return sheetOf(kind); }
};

// **No cap**: `EffectRenderer.addEffect` (`bq.a(Lnq;)V`) is one `List.add`.
//
// **The first 512 -- eight blocks' worth -- are held from construction.** A
// break is 64 particles, the longest a digging fleck can live is 40 ticks
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

    // **`cn.a(String, DDDDDD)`** -- the one entry point everything in the jar
    // that wants a particle goes through. The three motions are a *hint*: the
    // base constructor adds its own jitter to them and then renormalises, and
    // several kinds scale or discard them outright.
    //
    // The original also drops any particle further than 16 blocks from the
    // camera (`256.0` squared, in `e.a`). That is a render-distance decision
    // and the caller here owns the camera, so it is not made in this file.
    void spawn(const tick::TickWorld& world, ParticleKind kind, double x, double y,
               double z, double motionX = 0.0, double motionY = 0.0, double motionZ = 0.0);

    // `bq.a(III)V` -- 64 particles from the block at these coordinates, which
    // must still be **there**: the original spawns before it clears the cell,
    // and a particle needs the block's texture.
    void addBlockDestroy(const tick::TickWorld& world, i32 x, int y, i32 z);

    // `bq.a(IIII)V` -- **one** fleck off the face a pick is chewing on, which
    // `Minecraft.clickMouse` throws on every tick the button is held.
    //
    // It is not `addBlockDestroy` with a smaller count: the fleck starts at a
    // random point inside the block's *render* bounds, is pushed a tenth of a
    // block clear of the struck face, and is then slowed to a fifth
    // (`nq.b(0.2F)`) and shrunk to three fifths (`nq.d(0.6F)`). `face` is the
    // struck side in the usual 0..5 order.
    //
    // **Nothing calls this yet, and the reason is not an oversight.**
    // `Minecraft.clickMouse` throws one of these per tick on the *hold* path --
    // the ticks a block is being chewed on and has not broken. This build has
    // no break progress at all (a press is a broken block in every mode; see
    // the note in `editBlocks`), so there are no such ticks to throw one on. It
    // is written because it is the other half of `EffectRenderer`'s digging
    // pair, and because break progress is what will want it.
    void addBlockHit(const tick::TickWorld& world, i32 x, int y, i32 z, int face);

    // One 20 Hz tick of the whole pool -- each kind's own `onUpdate`.
    void tick(const tick::TickWorld& world);

    void clear() { particles_.clear(); }

    int count() const { return particles_.size(); }
    const Particle& operator[](int i) const { return particles_[i]; }

    // How many spawns the pool has refused since it was made -- only ever
    // because the heap would not hold more. On the debug page.
    u32 refused() const { return refused_; }

    JavaRandom& random() { return rand_; }

private:
    // The base `nq` constructor: the jitter, the renormalise and the lift. Every
    // kind runs it, and most then overwrite part of what it produced.
    Particle* construct(const tick::TickWorld& world, double px, double py, double pz,
                        double mx, double my, double mz);

    void spawnDigging(const tick::TickWorld& world, double px, double py, double pz,
                      double mx, double my, double mz, block::BlockId block);

    // **The world this pool is bound to**, or null. Only `bindParticles` sets
    // it, and only the sink it installs reads it: `TickWorld::spawnParticle`'s
    // signature has no world in it because the world *is* the caller, and the
    // light lookup at spawn needs one.
    const tick::TickWorld* world_ = nullptr;
    friend void bindParticles(tick::TickWorld& world, ParticleSystem* system);

    SegmentedPool<Particle, kInitialCapacity> particles_;
    u32 refused_ = 0;
    JavaRandom rand_;
};

// **Installs `system` as the world's `cn.a(String, DDDDDD)`.**
//
// Everything in `core/tick` and every other entity pool reaches the particles
// through `TickWorld::spawnParticle`, exactly as everything in the jar reaches
// them through `World.spawnParticle` -- so this is the one place the pool and
// the world are tied together, and a harness that installs nothing gets a world
// whose blocks and entities run their full logic and throw no particles.
//
// The pool must outlive the world, which it does: both belong to the frame
// loop.
void bindParticles(tick::TickWorld& world, ParticleSystem* system);

}  // namespace mc::entity
