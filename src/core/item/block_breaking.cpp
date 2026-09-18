// `nj`, the single-player controller's break loop. See block_breaking.hpp.

#include "core/item/block_breaking.hpp"

#include "core/audio/sound_engine.hpp"
#include "core/block/registry.hpp"
#include "core/entity/particle.hpp"
#include "core/entity/player_vitals.hpp"
#include "core/item/inventory.hpp"
#include "core/item/tool_rules.hpp"
#include "core/item/use.hpp"
#include "core/tick/behaviour.hpp"
#include "core/tick/drop.hpp"
#include "core/tick/tick_world.hpp"

#include <cmath>

namespace mc::item {

namespace {

// `i = 5` after a break -- five ticks before the next block takes progress.
constexpr int kBreakHitDelay = 5;

// The step sound is played when `h % 4 == 0`, and `h` counts ticks of damage.
constexpr float kDigSoundInterval = 4.0f;

}  // namespace

void blockClicked(tick::TickWorld& world, i32 x, int y, i32 z)
{
    const block::BlockId self = world.blockAt(x, y, z);
    if (self == block::kAir) {
        return;
    }
    // The four overrides, by behaviour. Kept apart from `blockActivated`'s own
    // dispatch on purpose: the chest, the workbench and the furnace answer a
    // right click and **not** a left one.
    switch (block::def(self).tick) {
    case block::TickBehaviour::Door:
    case block::TickBehaviour::Lever:
    case block::TickBehaviour::Button:
    case block::TickBehaviour::RedstoneOre:
        tick::blockActivated(world, x, y, z);
        break;
    default:
        break;
    }
}

bool extinguishFireOnFace(tick::TickWorld& world, i32 x, int y, i32 z, int face)
{
    // `cn.i(IIII)V`: the cell on the struck face, in a1.1.2's face order.
    switch (face) {
    case 0: --y; break;
    case 1: ++y; break;
    case 2: --z; break;
    case 3: ++z; break;
    case 4: --x; break;
    case 5: ++x; break;
    default: break;
    }
    const block::BlockId there = world.blockAt(x, y, z);
    if (there == block::kAir || block::def(there).tick != block::TickBehaviour::Fire) {
        return false;
    }
    JavaRandom& rand = world.random();
    const float pitch = 2.6f + (rand.nextFloat() - rand.nextFloat()) * 0.8f;
    world.playSoundAt("random.fizz", double(float(x) + 0.5f), double(float(y) + 0.5f),
                      double(float(z) + 0.5f), 0.5f, pitch);
    world.setBlockAndDataWithNotify(x, y, z, block::kAir, 0);
    return true;
}

bool harvestBlock(BreakContext& ctx, i32 x, int y, i32 z)
{
    // `nj.b(IIII)Z`.
    const block::BlockId self = ctx.world.blockAt(x, y, z);
    const u8 metadata = ctx.world.dataAt(x, y, z);
    const bool removed = destroyBlock(ctx.world, x, y, z, ctx.effects);

    // `stack.hitBlock(id, x, y, z)`, and a stack that used itself up is taken
    // out of the hand -- `destroyCurrentEquippedItem`.
    ItemStack& held = ctx.inventory.main[ctx.inventory.selected];
    if (!held.empty()) {
        const int wear = wearOnBreak(held.id);
        if (wear > 0 && entity::wearStack(held, wear)) {
            held.id = kEmptyItemId;
            held.count = 0;
            held.damage = 0;
        }
    }

    // **Asked after the wear**, of whatever is in the hand now. A pickaxe that
    // broke on this block has already gone, so the block drops nothing -- the
    // original's order, and a real cost of running a tool to its last use.
    if (removed && canHarvest(ctx.inventory.selectedItem(), self)) {
        tick::dropBlockAsItem(ctx.world, x, y, z, self, metadata);
    }
    return removed;
}

bool harvestBlockFor(tick::TickWorld& world, i32 x, int y, i32 z, ItemId held,
                     const Effects& effects)
{
    // `in.c(III)Z`, read before the removal for the reason `destroyBlock` gives:
    // the cell is about to be air and the drop is the block that was there.
    const block::BlockId self = world.blockAt(x, y, z);
    const u8 metadata = world.dataAt(x, y, z);
    const bool removed = destroyBlock(world, x, y, z, effects);
    if (removed && canHarvest(held, self)) {
        tick::dropBlockAsItem(world, x, y, z, self, metadata);
    }
    return removed;
}

bool BlockBreaker::click(BreakContext& ctx, i32 x, int y, i32 z, int face)
{
    (void)face;
    // `nj.a(IIII)V`.
    hitting_ = true;
    const block::BlockId id = ctx.world.blockAt(x, y, z);
    if (id != block::kAir && progress_ == 0.0f) {
        blockClicked(ctx.world, x, y, z);
    }
    // Asked again: a click can open a door, which changes nothing about the
    // id, but it could in principle have removed the block.
    const block::BlockId now = ctx.world.blockAt(x, y, z);
    if (now != block::kAir
        && relativeHardness(ctx.inventory.selectedItem(), now, ctx.eyeInWater, ctx.onGround)
               >= 1.0f) {
        return harvestBlock(ctx, x, y, z);
    }
    return false;
}

bool BlockBreaker::damage(BreakContext& ctx, i32 x, int y, i32 z, int face)
{
    // `nj.c(IIII)V`.
    hitting_ = true;
    bool broke = false;
    if (hitDelay_ > 0) {
        --hitDelay_;
    } else if (x == x_ && y == y_ && z == z_) {
        const block::BlockId id = ctx.world.blockAt(x, y, z);
        if (id == block::kAir) {
            return false;
        }
        progress_ +=
            relativeHardness(ctx.inventory.selectedItem(), id, ctx.eyeInWater, ctx.onGround);

        // `if (h % 4.0F == 0.0F)` -- the dig sound, on the first tick of a break
        // and every fourth after: the block's step sound at an eighth of its
        // volume plus one, and half its pitch.
        if (std::fmod(soundCounter_, kDigSoundInterval) == 0.0f && ctx.effects.sound != nullptr) {
            const block::StepSound& step = block::stepSoundOf(id);
            if (!step.silent()) {
                ctx.effects.sound->playSoundAt(step.step, double(float(x) + 0.5f),
                                               double(float(y) + 0.5f),
                                               double(float(z) + 0.5f),
                                               (step.volume + 1.0f) / 8.0f, step.pitch * 0.5f);
            }
        }
        soundCounter_ += 1.0f;

        if (progress_ >= 1.0f) {
            broke = harvestBlock(ctx, x, y, z);
            progress_ = 0.0f;
            prevProgress_ = 0.0f;
            soundCounter_ = 0.0f;
            hitDelay_ = kBreakHitDelay;
        }
    } else {
        // A different cell: forget the old one and start from nothing. **No
        // progress is added on this tick**, so every break costs one tick more
        // than its hardness alone says.
        progress_ = 0.0f;
        prevProgress_ = 0.0f;
        soundCounter_ = 0.0f;
        x_ = x;
        y_ = y;
        z_ = z;
    }

    // `Minecraft.a(IZ)V`'s other half: `effectRenderer.addBlockHitEffects` on
    // every tick the button is held on a block, whatever the controller did.
    if (ctx.effects.particles != nullptr) {
        ctx.effects.particles->addBlockHit(ctx.world, x, y, z, face);
    }
    return broke;
}

void BlockBreaker::reset()
{
    // `nj.a()V`: only if it was hitting, and only the progress and the delay.
    // The remembered cell and the sound counter survive, as they do there.
    if (!hitting_) {
        return;
    }
    hitting_ = false;
    progress_ = 0.0f;
    hitDelay_ = 0;
}

int BlockBreaker::crackStage(float partial) const
{
    // `nj.a(F)V`: nothing at or below zero, otherwise last tick's progress
    // carried `partial` of the way to this tick's.
    if (progress_ <= 0.0f) {
        return -1;
    }
    const float shown = prevProgress_ + (progress_ - prevProgress_) * partial;
    const int stage = int(shown * 10.0f);
    return stage < 0 ? -1 : (stage > 9 ? 9 : stage);
}

}  // namespace mc::item
