// The player's health and what spends it. See player_vitals.hpp for the
// argument; this file is the transcription, in the class files' own order.

#include "core/entity/player_vitals.hpp"

#include "core/block/block_def.hpp"
#include "core/block/fluid_flow.hpp"
#include "core/block/registry.hpp"
#include "core/entity/item_entity.hpp"
#include "core/entity/particle.hpp"
#include "core/entity/player_body.hpp"
#include "core/item/registry.hpp"
#include "core/tick/tick_world.hpp"
#include "core/util/math_helper.hpp"
#include "core/world/level_data.hpp"

#include "blocks.hpp"   // generated; see tools/configure.py
#include "harvest.hpp"  // generated; see tools/configure.py

#include <cmath>

namespace mc::entity {

namespace {

constexpr u8 kWaterMaterial = mcver::kBlocks[int(mcver::Block::Water)].material;
constexpr u8 kLavaMaterial = mcver::kBlocks[int(mcver::Block::Lava)].material;

constexpr float kPi = 3.1415927f;

// `ge.b()` -- getTalkInterval. The player does not override it, and has no
// idle sound to time with it, but the clock is reset to it all the same.
constexpr int kLivingSoundInterval = 80;

// `ItemArmor.aY`, or -1 for an item that is not armour -- which is how
// `eu.f()`'s `instanceof mr` reads here.
int armourPointsOf(item::ItemId id)
{
    for (int i = 0; i < mcver::kArmourRuleCount; ++i) {
        if (mcver::kArmourRules[i].item == id) {
            return mcver::kArmourRules[i].points;
        }
    }
    return -1;
}

void clearStack(item::ItemStack& stack)
{
    stack.id = item::kEmptyItemId;
    stack.count = 0;
    stack.damage = 0;
}

void merge(Harm& into, const Harm& from)
{
    into.landed = into.landed || from.landed;
    into.died = into.died || from.died;
}

// `ge.a(Lkh;IDD)V` -- knockBack. `dx`/`dz` run from the player *to* the
// attacker and are subtracted, which is the way round `attackEntityFrom`
// passes them.
void knockBack(PlayerBody& body, double dx, double dz)
{
    const float length = MathHelper::sqrtDouble(dx * dx + dz * dz);
    constexpr float kPush = 0.4f;
    body.motionX /= 2.0;
    body.motionY /= 2.0;
    body.motionZ /= 2.0;
    body.motionX -= dx / double(length) * double(kPush);
    body.motionY += 0.4000000059604645;
    body.motionZ -= dz / double(length) * double(kPush);
    if (body.motionY > 0.4000000059604645) {
        body.motionY = 0.4000000059604645;
    }
}

// `eu.g()` -- dropAllItems: the thirty-six, then the four, each through
// `dm.a(Lev;Z)V` with the scatter flag set. **The slot is emptied whether or
// not the drop found room**, as the original nulls it unconditionally; the pool
// evicts rather than refuses for exactly that reason.
void dropInventory(PlayerContext& ctx, JavaRandom& rand)
{
    auto drop = [&](item::ItemStack& stack) {
        if (stack.empty()) {
            return;
        }
        if (ctx.drops != nullptr) {
            ctx.drops->dropOnDeath(ctx.world, ctx.body.x, ctx.body.posY, ctx.body.z, stack.id,
                                   stack.count, stack.damage, rand);
        }
        clearStack(stack);
    };
    for (item::ItemStack& stack : ctx.inventory.main) {
        drop(stack);
    }
    for (item::ItemStack& stack : ctx.inventory.armour) {
        drop(stack);
    }
}

}  // namespace

bool wearStack(item::ItemStack& stack, int amount)
{
    // `ev.b(I)V`: `d += n; if (d > getMaxDamage()) { a--; if (a < 0) a = 0; d = 0; }`
    const int maxDamage = item::def(stack.id).durability;
    int damage = int(stack.damage) + amount;
    if (damage > maxDamage) {
        int count = int(stack.count) - 1;
        if (count < 0) {
            count = 0;
        }
        stack.count = i8(count);
        damage = 0;
    }
    stack.damage = i16(damage);
    return stack.count == 0;
}

int armourValue(const item::Inventory& inventory)
{
    // `eu.f()`, over `armorInventory` in its own order.
    int points = 0;
    int remaining = 0;
    int total = 0;
    for (const item::ItemStack& stack : inventory.armour) {
        if (stack.empty()) {
            continue;
        }
        const int piece = armourPointsOf(stack.id);
        if (piece < 0) {
            continue;
        }
        const int maxDamage = item::def(stack.id).durability;
        remaining += maxDamage - int(stack.damage);
        total += maxDamage;
        points += piece;
    }
    if (total == 0) {
        return 0;
    }
    return (points - 1) * remaining / total + 1;
}

void damageArmour(item::Inventory& inventory, int amount)
{
    // `eu.e(I)V`: every piece wears, and one worn through is removed.
    for (item::ItemStack& stack : inventory.armour) {
        if (stack.empty() || armourPointsOf(stack.id) < 0) {
            continue;
        }
        if (wearStack(stack, amount)) {
            clearStack(stack);
        }
    }
}

bool playerInsideOpaqueBlock(const tick::TickWorld& world, const PlayerBody& body)
{
    // `kh.I()`: `world.isBlockNormalCube(floor(posX), floor(posY + s()), floor(posZ))`.
    const i32 bx = MathHelper::floorDouble(body.x);
    const int by =
        int(MathHelper::floorDouble(body.posY + double(kPlayerEyeHeightOffset)));
    const i32 bz = MathHelper::floorDouble(body.z);
    return world.opaqueAt(bx, by, bz);
}

namespace {

// `kh.a(Lgb;)Z`. The cell is the eye point's, and the answer is whether the
// point is below the fluid's surface in that cell --
// `(y + 1) - (getPercentAir(meta) - 1/9)` -- not merely inside it.
bool eyeInFluid(const tick::TickWorld& world, const PlayerBody& body, u8 material)
{
    const double eye = body.posY + double(kPlayerEyeHeightOffset);
    const i32 bx = MathHelper::floorDouble(body.x);
    const int by = int(MathHelper::floorDouble(eye));
    const i32 bz = MathHelper::floorDouble(body.z);
    const block::BlockId id = world.blockAt(bx, by, bz);
    if (id == block::kAir || block::def(id).material != material) {
        return false;
    }
    const float surface =
        block::fluidPercentAir(int(world.dataAt(bx, by, bz))) - 0.11111111f;
    const float top = float(by + 1) - surface;
    return eye < double(top);
}

}  // namespace

bool playerEyeInWater(const tick::TickWorld& world, const PlayerBody& body)
{
    return eyeInFluid(world, body, kWaterMaterial);
}

bool playerEyeInLava(const tick::TickWorld& world, const PlayerBody& body)
{
    return eyeInFluid(world, body, kLavaMaterial);
}

Harm PlayerVitals::attack(PlayerContext& ctx, int amount, const Attacker& from)
{
    Harm out;
    if (invulnerable) {
        return out;
    }

    // ---- `dm.a(Lkh;I)Z` ------------------------------------------------
    if (health <= 0) {
        return out;
    }
    // **The window, tested before `ge` ever sees the hit.** `aW > j / 2` in
    // float, and a hit inside it is refused outright -- no "bigger hit lands".
    if (float(hurtResistant) > float(kPlayerHurtResistantTime) / 2.0f) {
        return out;
    }
    if (from.source == DamageSource::Monster || from.source == DamageSource::Arrow) {
        if (ctx.difficulty == 0) {
            amount = 0;
        }
        if (ctx.difficulty == 1) {
            amount = amount / 3 + 1;
        }
        if (ctx.difficulty == 3) {
            amount = amount * 3 / 2;
        }
    }
    const int absorbing = kArmourScale - armourValue(ctx.inventory);
    const int scaled = amount * absorbing + armourCarry;
    damageArmour(ctx.inventory, amount);
    amount = scaled / kArmourScale;
    armourCarry = scaled % kArmourScale;
    if (amount == 0) {
        return out;
    }

    // ---- `ge.a(Lkh;I)Z` ------------------------------------------------
    //
    // `ge`'s own window test sits here too and cannot pass: `dm` returned
    // above on the same condition.
    prevHealth = health;
    hurtResistant = i16(kPlayerHurtResistantTime);
    health = i16(health - amount);
    hurtTime = i16(kPlayerHurtTime);
    maxHurtTime = i16(kPlayerHurtTime);
    attackedAtYaw = 0.0f;
    out.landed = true;

    PlayerBody& body = ctx.body;
    if (from.source != DamageSource::World) {
        double dx = from.x - body.x;
        double dz = from.z - body.z;
        // `Math.random()` in the original, twice a pass, until the two are not
        // standing in the same place.
        while (dx * dx + dz * dz < 1.0E-4) {
            dx = (rand.nextDouble() - rand.nextDouble()) * 0.01;
            dz = (rand.nextDouble() - rand.nextDouble()) * 0.01;
        }
        attackedAtYaw =
            float(std::atan2(dz, dx) * 180.0 / 3.1415927410125732) - ctx.yawDegrees;
        knockBack(body, dx, dz);
    } else {
        attackedAtYaw = float(int(rand.nextDouble() * 2.0) * 180);
    }

    // `random.hurt` for both the hurt and the death sound -- `dm` overrides
    // neither `d()` nor `e()` -- at `playSoundAtEntity`'s feet.
    const float pitch = (rand.nextFloat() - rand.nextFloat()) * 0.2f + 1.0f;
    ctx.world.playSoundAt("random.hurt", body.x, body.posY - double(body.yOffset), body.z, 1.0f,
                          pitch);

    if (health <= 0) {
        // ---- `dm.b(Lkh;)V` -- onDeath ----------------------------------
        //
        // `setSize` changes only the two dimensions; `setPosition` straight
        // afterwards rebuilds the box around the same `posY`, which is what
        // `setFeet` at the box's own floor does.
        body.width = kPlayerDeathSize;
        body.height = kPlayerDeathSize;
        body.setFeet(body.x, body.posY - double(body.yOffset) + double(body.ySize), body.z);
        body.motionY = 0.10000000149011612;
        // (`if (username.equals("Notch"))` drops an apple here. Nobody on this
        // console is called that.)
        dropInventory(ctx, rand);
        if (from.source != DamageSource::World) {
            const float angle = (attackedAtYaw + ctx.yawDegrees) * kPi / 180.0f;
            body.motionX = double(-MathHelper::cos(angle) * 0.1f);
            body.motionZ = double(-MathHelper::sin(angle) * 0.1f);
        } else {
            body.motionX = 0.0;
            body.motionZ = 0.0;
        }
        body.yOffset = kPlayerDeathEyeHeight;
        out.died = true;
    }
    return out;
}

void PlayerVitals::heal(int amount)
{
    // `ge.b(I)V`.
    if (health <= 0) {
        return;
    }
    int next = int(health) + amount;
    if (next > kPlayerMaxHealth) {
        next = kPlayerMaxHealth;
    }
    health = i16(next);
    hurtResistant = i16(kPlayerHurtResistantTime / 2);
}

Harm PlayerVitals::fall(PlayerContext& ctx, float distance)
{
    // `ge.c(F)V`: `(int) Math.ceil(distance - 3.0F)` -- the subtraction in float.
    Harm out;
    const int damage = int(std::ceil(double(distance - kPlayerSafeFall)));
    if (damage <= 0) {
        return out;
    }
    out = attack(ctx, damage, Attacker{});

    // The thud, off the block under the feet. **Read after the hit**, as the
    // original reads `yOffset` after `attackEntityFrom` has run -- so a fall
    // that killed looks a tenth of a block under the corpse's eye instead.
    const PlayerBody& body = ctx.body;
    const i32 bx = MathHelper::floorDouble(body.x);
    const int by = int(MathHelper::floorDouble(body.posY - 0.20000000298023224
                                               - double(body.yOffset)));
    const i32 bz = MathHelper::floorDouble(body.z);
    const block::BlockId under = ctx.world.blockAt(bx, by, bz);
    if (under != block::kAir) {
        const block::StepSound& step = block::stepSoundOf(under);
        if (!step.silent()) {
            ctx.world.playSoundAt(step.step, body.x, body.posY - double(body.yOffset), body.z,
                                  step.volume * 0.5f, step.pitch * 0.75f);
        }
    }
    return out;
}

Harm PlayerVitals::tick(PlayerContext& ctx, bool inWater)
{
    Harm out;
    PlayerBody& body = ctx.body;
    tick::TickWorld& world = ctx.world;

    // ---- `kh.y()` ------------------------------------------------------
    ++ticksExisted;
    if (inWater) {
        fire = 0;
    }
    if (fire > 0) {
        if (fire % kPlayerFireDamageInterval == 0) {
            merge(out, attack(ctx, 1, Attacker{}));
        }
        --fire;
    }
    if (body.inLava(world)) {
        merge(out, attack(ctx, kPlayerLavaDamage, Attacker{}));
        fire = i16(kPlayerLavaFireTicks);
    }
    if (body.posY < kPlayerVoidY) {
        merge(out, attack(ctx, kPlayerVoidDamage, Attacker{}));
    }

    // ---- `ge.y()` ------------------------------------------------------
    //
    // The idle-sound draw, made and discarded: the player has no idle sound.
    {
        const int draw = rand.nextInt(1000);
        const int before = livingSoundTime++;
        if (draw < before) {
            livingSoundTime = -kLivingSoundInterval;
        }
    }
    if (alive() && playerInsideOpaqueBlock(world, body)) {
        merge(out, attack(ctx, kPlayerSuffocationDamage, Attacker{}));
    }
    if (alive() && playerEyeInWater(world, body)) {
        --air;
        if (air == kPlayerDrownAt) {
            air = 0;
            for (int n = 0; n < 8; ++n) {
                const float dx = rand.nextFloat() - rand.nextFloat();
                const float dy = rand.nextFloat() - rand.nextFloat();
                const float dz = rand.nextFloat() - rand.nextFloat();
                world.spawnParticle(int(ParticleKind::Bubble), body.x + double(dx),
                                    body.posY + double(dy), body.z + double(dz), body.motionX,
                                    body.motionY, body.motionZ);
            }
            merge(out, attack(ctx, kPlayerDrownDamage, Attacker{}));
        }
        // **Every tick the head is under**, not only on the blow.
        fire = 0;
    } else {
        air = i16(kPlayerMaxAir);
    }
    if (attackTime > 0) {
        --attackTime;
    }
    if (hurtTime > 0) {
        --hurtTime;
    }
    if (hurtResistant > 0) {
        --hurtResistant;
    }
    if (health <= 0) {
        ++deathTime;
        if (deathTime == kPlayerDeathTicks + 1) {
            // `D()` does nothing, `F()` removes the entity, and twenty `explode`
            // puffs go up where it lay: three Gaussian drifts drawn first, then
            // the position inside the corpse's box.
            for (int n = 0; n < 20; ++n) {
                const double mx = rand.nextGaussian() * 0.02;
                const double my = rand.nextGaussian() * 0.02;
                const double mz = rand.nextGaussian() * 0.02;
                const double w = double(body.width);
                const double px = body.x + double(rand.nextFloat() * body.width * 2.0f) - w;
                const double py = body.posY + double(rand.nextFloat() * body.height);
                const double pz = body.z + double(rand.nextFloat() * body.width * 2.0f) - w;
                world.spawnParticle(int(ParticleKind::Explode), px, py, pz, mx, my, mz);
            }
        }
    }

    // ---- `dm.j()` ------------------------------------------------------
    //
    // `if (difficulty == 0 && health < 20 && ticksExisted % 20 * 4 == 0) heal(1);`
    if (ctx.difficulty == 0 && health < kPlayerMaxHealth
        && (ticksExisted % kPeacefulRegenInterval) * 4 == 0) {
        heal(1);
    }
    return out;
}

void PlayerVitals::respawn()
{
    // A new `bi` through `dm.q()`: the constructor's values, twenty health and
    // no death time. `rand` keeps running -- a new entity's generator is a
    // fresh `new Random()`, and nothing depends on which one it is.
    health = i16(kPlayerMaxHealth);
    prevHealth = i16(kPlayerMaxHealth);
    hurtTime = 0;
    maxHurtTime = 0;
    hurtResistant = 0;
    deathTime = 0;
    attackTime = 0;
    air = i16(kPlayerMaxAir);
    fire = 0;
    armourCarry = 0;
    attackedAtYaw = 0.0f;
    ticksExisted = 0;
    livingSoundTime = 0;
}

void PlayerVitals::load(const world::PlayerData& data)
{
    // `ge.b(Lhm;)V` and `kh`'s own half. `HurtTime` is `G`, the animation,
    // not the window -- the window is not saved, so a world reopened mid-fight
    // opens with none.
    health = data.health;
    prevHealth = data.health;
    hurtTime = data.hurtTime;
    deathTime = data.deathTime;
    attackTime = data.attackTime;
    air = data.air;
    fire = data.fire;
}

void PlayerVitals::save(world::PlayerData* data) const
{
    if (data == nullptr) {
        return;
    }
    data->health = health;
    data->hurtTime = hurtTime;
    data->deathTime = deathTime;
    data->attackTime = attackTime;
    data->air = air;
    data->fire = fire;
}

}  // namespace mc::entity
