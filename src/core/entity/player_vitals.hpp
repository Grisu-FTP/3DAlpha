#pragma once

// The player's health, and every rule in a1.1.2 that spends it or gives it back:
// `ge` (EntityLiving) and `dm` (EntityPlayer)'s counters, the damage entry point,
// fall damage, fire, lava, the void, suffocation, drowning, Peaceful's
// regeneration and death.
//
// **Separate from `PlayerBody`, and for two reasons rather than tidiness.** The
// body is shared -- `LivingBody` is the same type, and every mob and animal
// moves through it -- and it moves against a `const TickWorld&` so that moving
// can never write a block. Health is the opposite on both counts: it belongs to
// the player alone, and spending it writes the world (a hurt sound, eight
// bubbles, a death's worth of dropped items) and the inventory (armour wears
// out as it absorbs). So the body reports what happened to it -- `landedFall`,
// `inLava`, the water branch -- and this file decides what that costs.
//
// **Everything below is transcribed from the class file, not recalled.** The
// order matters in several places and is the jar's:
//
//   * `dm.a(Lkh;I)Z` checks the invulnerability window *before* it hands the
//     hit to `ge`, so the "a bigger hit still lands" branch every mob takes is
//     unreachable for the player: a second blow inside ten ticks does nothing
//     whatever its size.
//   * Difficulty scales only a hit whose source is a monster (`dq`) or an arrow
//     (`kg`). A fall, lava, a cactus, a slime and a TNT blast are the same on
//     every difficulty -- Peaceful included, where a monster's hit is zero.
//   * Armour absorbs `armourValue` twenty-fifths of every hit and keeps the
//     remainder in `armourCarry` for the next one, and it wears by the scaled
//     amount *before* the reduction. A hit reduced to nothing still wears it.
//   * The drowning blow lands when air is exactly -20, and a head under water
//     also puts out a fire every tick, not only on the tick it went under.
//
// **Creative is `invulnerable`**, which is ours: a1.1.2 has no gamemode. It
// short-circuits `attack` at the top, so nothing in a Creative world hurts,
// wears armour or kills -- and the counters still run, so switching mode mid-
// fall does not bank a stale window. See core/settings/world_settings.hpp.
//
// Nothing here allocates. See docs/physics-a1.1.2.md, *Survival*, for the
// constants beside the bytecode they came from.

#include "core/entity/damage_source.hpp"
#include "core/item/inventory.hpp"
#include "core/util/java_random.hpp"
#include "core/util/types.hpp"

namespace mc::tick {
class TickWorld;
}
namespace mc::world {
struct PlayerData;
}

namespace mc::entity {

struct PlayerBody;
class ItemEntitySystem;

// `dm`'s constructor: `this.E = 20`.
inline constexpr int kPlayerMaxHealth = 20;

// `kh.aU` -- maxAir -- and where `aX` rests out of water.
inline constexpr int kPlayerMaxAir = 300;

// `if (aX == -20)` -- the tick a drowning player takes a blow, after which air
// goes back to zero and counts down again.
inline constexpr int kPlayerDrownAt = -20;
inline constexpr int kPlayerDrownDamage = 2;

// `ge.j` -- maxHurtResistantTime -- and the ten `hurtTime` is set to.
inline constexpr int kPlayerHurtResistantTime = 20;
inline constexpr int kPlayerHurtTime = 10;

// `kh.y()`: a burning entity takes one every twenty ticks, lava takes ten and
// sets the fire to 600, and below y = -64 the void takes four a tick (`ge.E()`).
inline constexpr int kPlayerFireDamageInterval = 20;
inline constexpr int kPlayerLavaDamage = 10;
inline constexpr int kPlayerLavaFireTicks = 600;
inline constexpr int kPlayerVoidDamage = 4;
inline constexpr double kPlayerVoidY = -64.0;

// `ge.y()`'s `isEntityInsideOpaqueBlock` blow.
inline constexpr int kPlayerSuffocationDamage = 1;

// `dm.s()` -- getEyeHeight -- is **0.12**, added to `posY`, which for the
// player is already the eye. So the suffocation cell and the drowning point
// are 1.74 above the feet, not 1.62.
inline constexpr float kPlayerEyeHeightOffset = 0.12f;

// `ge.c(F)V`: `ceil(distance - 3)`, so a three-block fall is free.
inline constexpr float kPlayerSafeFall = 3.0f;

// `eu.f()` and `dm.a(Lkh;I)Z` both work in twenty-fifths.
inline constexpr int kArmourScale = 25;

// `dm.j()`: on Peaceful, one point every twenty ticks while below full health.
inline constexpr int kPeacefulRegenInterval = 20;

// `ge.y()`: twenty ticks lying there before the entity is removed.
inline constexpr int kPlayerDeathTicks = 20;

// `dm.b(Lkh;)V` -- onDeath -- shrinks the box to a fifth of a block and drops
// the eye to a tenth above the feet, which is the camera settling on the
// ground.
inline constexpr float kPlayerDeathSize = 0.2f;
inline constexpr float kPlayerDeathEyeHeight = 0.1f;

// The source of a hit -- see core/entity/damage_source.hpp -- and where it
// stands.
struct Attacker {
    DamageSource source = DamageSource::World;
    // Where it stands, for the knockback. Ignored for `World`.
    double x = 0.0;
    double z = 0.0;
};

// What `attack`, `fall` or `tick` did, for whoever plays the screen's part.
struct Harm {
    // At least one hit got past the window and the armour.
    bool landed = false;
    // Health reached zero on this call. The inventory has already been dropped
    // and the body has been laid down; the caller shows the game-over screen.
    bool died = false;
};

// Everything a hit reaches. Borrowed for the length of one call.
struct PlayerContext {
    tick::TickWorld& world;
    PlayerBody& body;
    item::Inventory& inventory;
    // Where a death puts the inventory. Null drops it nowhere, which is what a
    // test that only counts health wants.
    ItemEntitySystem* drops = nullptr;
    // `cn.l`, 0 Peaceful to 3 Hard.
    int difficulty = 2;
    // `aq`, which the death push and `attackedAtYaw` are measured from.
    float yawDegrees = 0.0f;
};

struct PlayerVitals {
    explicit PlayerVitals(i64 seed = 0) : rand(seed) {}

    i16 health = kPlayerMaxHealth;      // `E`
    i16 prevHealth = kPlayerMaxHealth;  // `F`
    i16 hurtTime = 0;                   // `G`
    i16 maxHurtTime = 0;                // `H`
    i16 hurtResistant = 0;              // `aW`
    i16 deathTime = 0;                  // `J`
    i16 attackTime = 0;                 // `K`
    i16 air = kPlayerMaxAir;            // `aX`
    i16 fire = 0;                       // `aT`

    // `dm.a` -- the twenty-fifths of damage armour absorbed but did not round
    // away. Not saved: `dm` writes no such tag.
    int armourCarry = 0;

    // `I` -- which way the last hit came from, relative to the player's
    // heading. The hurt camera tilt and the death push read it.
    float attackedAtYaw = 0.0f;

    // `aR` -- ticksExisted, which Peaceful's regeneration is timed on.
    i32 ticksExisted = 0;

    // `ge.a` -- the idle-sound clock. The player has no idle sound, but the
    // draw that decides whether to play it is made every tick all the same,
    // and it is `aQ` that makes it.
    i32 livingSoundTime = 0;

    // Ours: Creative. See the header.
    bool invulnerable = false;

    // `aQ`, and the stand-in for the three `Math.random()` calls in
    // `ge.a(Lkh;I)Z`.
    JavaRandom rand;

    bool alive() const { return health > 0; }

    // `dm.a(Lkh;I)Z` then `ge.a(Lkh;I)Z` -- the one way health goes down.
    Harm attack(PlayerContext& ctx, int amount, const Attacker& from);

    // `ge.b(I)V` -- heal. Nothing for a dead player; capped at twenty; and it
    // sets the window to half, so food eaten mid-fight buys five ticks of it.
    void heal(int amount);

    // `ge.c(F)V` -- fall damage, given `PlayerBody::landedFall`. The landing
    // thud plays only when there was damage to deal.
    Harm fall(PlayerContext& ctx, float distance);

    // One 20 Hz tick of `kh.y()`'s fire, lava and void, `ge.y()`'s suffocation,
    // drowning, counters and death, and `dm.j()`'s regeneration -- in that order.
    //
    // **Call it after `PlayerBody::updateWaterEntry` and before
    // `PlayerBody::tick`**, which is `onEntityUpdate` running before
    // `onLivingUpdate`. `inWater` is that call's answer: the water branch puts
    // the fire out before the fire counter can spend it.
    Harm tick(PlayerContext& ctx, bool inWater);

    // `dm.q()` on a freshly constructed player -- what `Minecraft.o()` builds
    // when Respawn is pressed. Everything back to the constructor's values.
    void respawn();

    // Between vitals and `level.dat`'s `Health`, `HurtTime`, `DeathTime`,
    // `AttackTime`, `Air` and `Fire`.
    void load(const world::PlayerData& data);
    void save(world::PlayerData* data) const;
};

// `eu.f()` -- how many twenty-fifths of a hit the worn armour absorbs, scaled
// down as it wears: `(points - 1) * remaining / total + 1`, or zero with none.
int armourValue(const item::Inventory& inventory);

// `eu.e(I)V` -- every worn piece takes `amount` damage, and one that runs out
// is removed.
void damageArmour(item::Inventory& inventory, int amount);

// `ev.b(I)V` -- damageItem, over one stack: a stack past its durability loses
// one item and starts again at zero damage. Returns true when the stack is now
// empty. Shared by armour and, later, tools.
bool wearStack(item::ItemStack& stack, int amount);

// `kh.I()` -- isEntityInsideOpaqueBlock, asked at `posY + 0.12`.
bool playerInsideOpaqueBlock(const tick::TickWorld& world, const PlayerBody& body);

// `kh.a(Lgb;)Z` with water -- isInsideOfMaterial, asked at `posY + 0.12` and
// against the fluid's own surface, so a head in the air gap of a flowing cell
// is not drowning.
bool playerEyeInWater(const tick::TickWorld& world, const PlayerBody& body);

}  // namespace mc::entity
