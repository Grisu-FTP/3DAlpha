#pragma once

// **The splash an entity makes the moment it touches water** -- the water
// branch of `kh.y()` (Entity.onEntityUpdate). It is the one piece of the base
// entity tick that every pool here shares and that none of them had, which is
// why it lives in a header of its own rather than in any one of them.
//
// ```java
// if (g_()) {                        // handleWaterMovement -- and it pushes
//     if (!inWater && !firstUpdate) {
//         float f = MathHelper.sqrt_double(motionX * motionX * 0.2
//                 + motionY * motionY + motionZ * motionZ * 0.2) * 0.2F;
//         if (f > 1.0F) f = 1.0F;
//         playSoundAtEntity(this, "random.splash", f,
//                           1.0F + (rand.nextFloat() - rand.nextFloat()) * 0.4F);
//         ... a row of `bubble` and a row of `splash` particles ...
//     }
//     fallDistance = 0.0F;
//     inWater = true;
//     fire = 0;
// } else {
//     inWater = false;
// }
// ```
//
// **Which entities splash is derived, not assumed.** `kh.e_()` -- onUpdate on
// the base class -- is one line, `y()`, so an entity reaches this branch
// exactly when its own `onUpdate` calls `super.onUpdate()`. Disassembling the
// eight entity classes this port has says that is `ge` (so the player and all
// four animals), `dx` (the dropped item), `kg` (the arrow) and `dc` (the boat).
// **`ff`, `jc` and `oc` -- the falling block, the painting and the minecart --
// re-implement `onUpdate` and never call up to it, so those three enter water
// in silence.** That is a1.1.2's, not a gap here; a falling sand block landing
// in a river really is silent in the original.
//
// **The volume is a motion, and it is read before the tick moves anything.**
// `y()` is the first thing `onUpdate` does, so what it squares is the motion
// the *previous* tick left behind. An anvil-drop into still water is loud and a
// player who wades in sideways is very nearly silent, and the two come out of
// the same expression. Note the 0.2 weighting on the two horizontal terms:
// falling in is louder than swimming in.
//
// **The pitch is left to the caller**, because it is a draw and the pools own
// the generators -- the same split `PlayerBody::stepSoundDue` makes with the
// footstep, and for the same reason.

#include "core/util/java_random.hpp"
#include "core/util/math_helper.hpp"

namespace mc::entity {

inline constexpr const char* kSplashSound = "random.splash";

// `(double)0.2f`, which is what the class file holds and is not 0.2.
inline constexpr double kSplashHorizontalWeight = 0.20000000298023224;

// `kh`'s `aV` (inWater) and `c` (firstUpdate). **A loaded entity starts with
// `firstUpdate` set**, which is why reopening a world does not splash
// everything already floating in it -- and why a boat, which is placed *into*
// water, is silent on the tick it is put down.
//
// **`splashed` is ours, and it is the one deviation in this file.** See
// `updateWaterEntry`.
struct WaterEntry {
    bool inWater = false;
    bool firstUpdate = true;
    bool splashed = false;
};

struct WaterEntryResult {
    // The branch was taken: the caller clears its own fall distance and fire,
    // which is the rest of it.
    bool inWater = false;
    // ...and this is the tick it was entered on, so there is a splash.
    bool splash = false;
    float volume = 0.0f;
};

// `inWaterNow` is the caller's own `g_()` answer -- and in the jar that call is
// a mutator, so a caller that wants to be faithful asks
// `block::handleWaterMovement`, not a const material test: an entity in a
// current is carried once here and again in `moveEntityWithHeading`.
//
// `firstUpdate` is cleared here rather than at the end of the caller's `y()`,
// which is where the jar clears it. Nothing between the two reads it.
//
// ---------------------------------------------------------------------------
//
// **`touchingWater` is not in the jar, and the splash is latched on it.** This
// is the one deviation in this file and it is worth the paragraph.
//
// `g_()` hands `handleMaterialAcceleration` the box **shrunk by four tenths top
// and bottom** -- `boundingBox.expand(0.0D, -0.4D, 0.0D)`. For a player, 1.8
// tall, that leaves a metre of box. For anything **shorter than 0.8 of a
// block** it leaves a box whose `minY` is *above* its `maxY`, and the y loop
// inside that method is `for (l1 = floor(minY); l1 < floor(maxY + 1); l1++)` --
// which on an inverted box runs once or not at all depending on where between
// two cells the entity happens to be. Measured over a whole cell of positions
// it sees a cell on **45 % of ticks**.
//
// So a dropped stack (0.25), an arrow (0.5), a chicken (0.4) or a boat (0.6)
// sinking through water has an `inWater` that flickers, and `!inWater` is the
// whole of the splash's condition: it splashes again, with its full row of
// bubbles, every few blocks all the way down. That was reported from play as
// "a block falling into water plays a sound and particle for every water block
// it falls", and it is a1.1.2's arithmetic rather than this port's.
//
// **Only the splash is latched.** `inWater` comes back exactly as the jar
// computes it, so the current still pushes on the ticks the jar pushes on, the
// fall distance still clears on those ticks and fire still goes out on them.
// What changes is that a second splash needs the entity to have left the water
// first -- and "left" is asked of the entity's own box, uninset, because that
// is the question the flicker cannot answer.
//
// `touchingWater` is a predicate rather than a bool because it is only ever
// consulted on a tick that already splashed once and is now dry by the jar's
// reckoning, which is a handful of ticks in an entity's life. Everything else
// pays nothing for it.
template <class TouchingWater>
inline WaterEntryResult updateWaterEntry(WaterEntry& state, bool inWaterNow, double motionX,
                                         double motionY, double motionZ,
                                         TouchingWater touchingWater)
{
    WaterEntryResult out;
    if (inWaterNow) {
        out.inWater = true;
        if (!state.inWater && !state.firstUpdate && !state.splashed) {
            float volume = MathHelper::sqrtDouble(motionX * motionX * kSplashHorizontalWeight
                                                  + motionY * motionY
                                                  + motionZ * motionZ * kSplashHorizontalWeight)
                           * 0.2f;
            if (volume > 1.0f) {
                volume = 1.0f;
            }
            out.splash = true;
            out.volume = volume;
            state.splashed = true;
        }
    } else if (state.splashed && !touchingWater()) {
        // Out of the water by the only measure that does not flicker, so the
        // next entry is a new one.
        state.splashed = false;
    }
    state.inWater = out.inWater;
    state.firstUpdate = false;
    return out;
}

// `1.0F + (rand.nextFloat() - rand.nextFloat()) * 0.4F`. Two draws, so most
// splashes land near 1.0 and few at either edge.
inline float splashPitch(JavaRandom& rand)
{
    return 1.0f + (rand.nextFloat() - rand.nextFloat()) * 0.4f;
}

// **The two rows of particles the same branch throws**, and they are the rest
// of the block quoted at the top of this file.
//
// `1.0F + width * 20` of each, so **how wet an entry looks is the entity's
// width and nothing else**: a dropped item is 0.25 across and makes six of
// each, a player 0.6 and makes thirteen, a boat 1.5 and makes thirty-one. Each
// one is scattered a width either way on the two horizontal axes and placed at
// **the top of the cell the entity's feet are in** -- `floor(boundingBox.minY)
// + 1`, which is the water's surface for a body standing in it, and is why the
// spray comes off the surface rather than off the middle of the entity.
//
// The bubbles are given the entity's motion **minus up to a fifth on y** and
// the splashes are given it whole: the bubbles sink behind and the splashes
// travel with it.
//
// `world` is taken by reference-to-const and the particles go through
// `spawnParticle`, so an entity pool that has no particle sink installed pays
// six to thirty-one calls that return immediately and nothing else.
template <class World>
void waterEntryParticles(const World& world, JavaRandom& rand, double x, double boxMinY,
                         double z, float width, double motionX, double motionY,
                         double motionZ)
{
    // `entity::ParticleKind` as the integer `TickWorld::spawnParticle` takes;
    // see the note on that seam for why it is not the enum. Bubble is 0 and
    // Splash is 5, which is the order `cn.a`'s compare chain has them in.
    constexpr int kBubble = 0;
    constexpr int kSplash = 5;

    const float surface = float(MathHelper::floorDouble(boxMinY));
    const float rows = 1.0f + width * 20.0f;

    for (float i = 0.0f; i < rows; i += 1.0f) {
        const double dx = double((rand.nextFloat() * 2.0f - 1.0f) * width);
        const double dz = double((rand.nextFloat() * 2.0f - 1.0f) * width);
        world.spawnParticle(kBubble, x + dx, double(surface + 1.0f), z + dz, motionX,
                            motionY - double(rand.nextFloat() * 0.2f), motionZ);
    }
    for (float i = 0.0f; i < rows; i += 1.0f) {
        const double dx = double((rand.nextFloat() * 2.0f - 1.0f) * width);
        const double dz = double((rand.nextFloat() * 2.0f - 1.0f) * width);
        world.spawnParticle(kSplash, x + dx, double(surface + 1.0f), z + dz, motionX,
                            motionY, motionZ);
    }
}

}  // namespace mc::entity
