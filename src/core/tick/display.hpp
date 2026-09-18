#pragma once

// **`Block.randomDisplayTick`, and the loop that drives it** -- `cn.m(III)V`,
// which `Minecraft.i()` calls once a tick round the player.
//
// This is where almost every particle in ordinary play comes from. Nothing
// breaks, nothing explodes and nothing falls in water on a quiet evening, and
// yet a torch smokes, a fire smoulders, redstone glitters and lava spits --
// because a1.1.2 throws **a thousand darts a tick** at a 33-block cube round
// the player and asks whatever each one hits whether it would like a particle.
//
// ```java
// public void m(int x, int y, int z) {          // World.doVoidFogParticles
//     int r = 16;
//     Random rand = new Random();
//     for (int i = 0; i < 1000; i++) {
//         int px = x + this.rand.nextInt(r) - this.rand.nextInt(r);
//         ...                                    // and the same for y and z
//         int id = getBlockId(px, py, pz);
//         if (id > 0) Block.blocksList[id].randomDisplayTick(this, px, py, pz, rand);
//     }
// }
// ```
//
// **The offsets come out of the world's own generator and that is observable.**
// `this.n` is the same `Random` the random block ticks draw from, so in a1.1.2
// the display loop shifts the block-tick stream by 6,000 draws every tick. It
// is copied rather than given a generator of its own: a stream that diverges
// from the jar's in a way the jar's own client does not is not the more
// faithful of the two. The `new Random()` handed to the block, on the other
// hand, is a fresh time-seeded one per call and reproduces nothing in either.
//
// **The distribution is a difference of two uniforms**, not a uniform: `x +
// nextInt(16) - nextInt(16)` is triangular over -15..15 and peaks at the
// player. Six draws per dart, so the cube is sampled densely near the eye and
// thinly at its corners, which is exactly the density a particle effect wants
// and is not something anybody would arrive at by choosing a radius.
//
// **Seven blocks answer.** Torch, redstone torch, furnace, redstone wire,
// redstone ore, fire, and the two fluids -- everything else inherits `ly`'s
// empty method and costs one switch. Which class each id belongs to was read
// off `ly`'s static initialiser (`new mj(50, 80)`, `new ku(62, true)` and so
// on) rather than guessed from the name, which matters for the four ids that
// come in lit/unlit pairs: the flag is a *constructor argument*, so the lit
// furnace smokes and the unlit one does not, and the same field on the
// redstone ore and the redstone torch means the same thing.
//
// Two of the seven also play a **sound** here and not only a particle -- fire
// crackles, water trickles -- which is why this takes the world rather than a
// particle pool: `randomDisplayTick` is a block behaviour that happens to be
// client-side, and the world is what a block behaviour talks to.

#include "core/block/block_def.hpp"
#include "core/util/java_random.hpp"
#include "core/util/types.hpp"

namespace mc::tick {

class TickWorld;

// `cn.m(III)V`'s reach and its count, as the jar writes them.
inline constexpr int kDisplayTickRadius = 16;
inline constexpr int kDisplayTickDarts = 1000;

// `ai.i(Lcn;III)V` -- **redstone ore's glitter**, six motes thrown one per
// face and each kept only if the face it came off is open. It is declared here
// rather than left inside the display tick because two different things run it
// and only one of them is a display tick: `ai.b(Lcn;IIILjava/util/Random;)V`
// asks for it once a tick while the ore is lit, and `ai.h(Lcn;III)V` -- the
// glow a touch, a punch or a footstep causes -- runs it **before** the block
// becomes the lit one, so the sparkle happens on the dull ore too. See
// `tick::redstoneOreActivated`.
void redstoneOreSparkle(const TickWorld& world, i32 x, int y, i32 z, JavaRandom& rand);

// `ly.b(Lcn;IIILjava/util/Random;)V` -- one block's own display tick. `self` is
// the block already read at those coordinates, because every caller has it.
//
// Silent for the sixty-odd ids with no override, which is the common case and
// is one compare.
void blockDisplayTick(const TickWorld& world, i32 x, int y, i32 z, block::BlockId self,
                      JavaRandom& rand);

// `cn.m(III)V` -- the thousand darts, round the block the player is standing
// in. `rand` is the block's generator, the `new Random()` the jar makes once
// per call; the offsets are drawn from the world's own.
//
// **Does nothing at all without a particle sink**, which is what makes this
// free on a headless tick: the host world harness installs none.
void displayTick(TickWorld& world, i32 centreX, int centreY, i32 centreZ,
                 JavaRandom& rand);

}  // namespace mc::tick
