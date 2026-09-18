#pragma once

// **the block's random display tick, and the loop that drives it** --
// `World`'s method,
// which `Minecraft.i()` calls once a tick round the player.
//
// This is where almost every particle in ordinary play comes from. Nothing
// breaks, nothing explodes and nothing falls in water on a quiet evening, and
// yet a torch smokes, a fire smoulders, redstone glitters and lava spits --
// because a1.1.2 throws **a thousand darts a tick** at a 33-block cube round
// the player and asks whatever each one hits whether it would like a particle.
//
// Void fog: from the player's cell it takes 1000 samples, each offset on every axis by the difference of two draws below 16. If the sampled cell holds a real block, that block's random display tick runs for it, using a fresh random generator.
//
// **The offsets come out of the world's own generator and that is observable.**
// `this.n` is the same `Random` the random block ticks draw from, so in a1.1.2
// the display loop shifts the block-tick stream by 6,000 draws every tick. It
// is copied rather than given a generator of its own: a stream that diverges
// from the jar's in a way the jar's own client does not is not the more
// faithful of the two. The an unseeded random generator handed to the block,
// on the other
// hand, is a fresh time-seeded one per call and reproduces nothing in either.
//
// **The distribution is a difference of two uniforms**, not a uniform: `x +
// nextInt(16) - nextInt(16)` is triangular over -15..15 and peaks at the
// player. Six draws per dart, so the cube is sampled densely near the eye and
// thinly at its corners, which is exactly the density a particle effect wants
// and is not something anybody would arrive at by choosing a radius.
//
// **Seven blocks answer.** Torch, redstone torch, furnace, redstone wire,
// redstone ore, fire, and the two fluids -- everything else inherits `Block`'s
// empty method and costs one switch. Which class each id belongs to was read
// off `Block`'s static initialiser (`new mj(50, 80)`, `new ku(62, true)` and so
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

// `World`'s method's reach and its count, as the jar writes them.
inline constexpr int kDisplayTickRadius = 16;
inline constexpr int kDisplayTickDarts = 1000;

// a method of another original class -- **redstone ore's glitter**, six motes thrown one per
// face and each kept only if the face it came off is open. It is declared here
// rather than left inside the display tick because two different things run it
// and only one of them is a display tick: a method of another original class
// asks for it once a tick while the ore is lit, and a method of another original class -- the
// glow a touch, a punch or a footstep causes -- runs it **before** the block
// becomes the lit one, so the sparkle happens on the dull ore too. See
// `tick::redstoneOreActivated`.
void redstoneOreSparkle(const TickWorld& world, i32 x, int y, i32 z, JavaRandom& rand);

// `Block`'s method -- one block's own display tick. `self` is
// the block already read at those coordinates, because every caller has it.
//
// Silent for the sixty-odd ids with no override, which is the common case and
// is one compare.
void blockDisplayTick(const TickWorld& world, i32 x, int y, i32 z, block::BlockId self,
                      JavaRandom& rand);

// `World`'s method -- the thousand darts, round the block the player is standing
// in. `rand` is the block's generator, the an unseeded random generator the
// jar makes once
// per call; the offsets are drawn from the world's own.
//
// **Does nothing at all without a particle sink**, which is what makes this
// free on a headless tick: the host world harness installs none.
void displayTick(TickWorld& world, i32 centreX, int centreY, i32 centreZ,
                 JavaRandom& rand);

}  // namespace mc::tick
