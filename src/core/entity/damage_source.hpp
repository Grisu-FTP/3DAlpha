#pragma once

// **What dealt a hit to the player**, as far as a1.1.2 ever asks.
//
// Its own header because three pools need to say it and none of them should
// have to know about the player's health to do so: a mob's fist and a blast
// leave through `MobSurroundings::hurtPlayer`, an arrow through
// `ArrowTargets::hurtPlayer`, and core/entity/player_vitals.hpp is what reads
// it on the other side.
//
// `EntityPlayer`'s method asks exactly one question of its attacker --
// the type test for the two hostile classes -- and scales the hit by difficulty
// when the
// answer is yes. `EntityLiving`'s method asks one more, whether there was an attacker at
// all, and that is what knocks the player back.

#include "core/util/types.hpp"

namespace mc::entity {

enum class DamageSource : u8 {
    // `attackEntityFrom(null, n)`: fire, lava, the void, drowning, suffocation,
    // a cactus and a fall. No knockback.
    World,
    // A `EntityMob` -- zombie, skeleton, creeper, spider -- whether by its fist or, for
    // the creeper, by its blast. Scaled by difficulty.
    Monster,
    // A `EntityArrow`, whoever fired it. Scaled by difficulty.
    Arrow,
    // Any other entity: a slime (`ma` is not a `EntityMob`), primed TNT's blast.
    Other,
};

}  // namespace mc::entity
