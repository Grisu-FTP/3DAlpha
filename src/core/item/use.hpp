#pragma once

// **What one right-click does**, in one place and on the host side of the
// split.
//
// This used to live in `platform/ctr/main.cpp`, inline in the button handler,
// and that was fine for as long as placing a block was "offset by the struck
// face and write one cell". It stopped being fine twice over:
//
//   * A door is **two blocks**, oriented from the player's heading, mirrored
//     against whatever is already beside it. Written in the ctr layer it could
//     not be tested at all -- and a door placed as one block deletes itself the
//     moment a neighbour changes, so the bug it caused looked like a
//     disappearing door rather than like a half-written placement.
//   * A right-click is not only a placement. `PlayerController.onPlayerRightClick`
//     asks the *block* first, and a door, a lever or a button answers it and
//     eats the click. Without that there was no way to open anything.
//
// So the decision lives here, in core, where the test suite can drive it, and
// the platform layer is left holding the button and the renderer.
//
// The three methods this is: `hq.a(Ldm;Lcn;Lev;IIII)Z` -- onPlayerRightClick,
// `av.a(Lev;Ldm;Lcn;IIII)Z` -- ItemBlock.onItemUse, and `ec.a(...)` --
// ItemDoor.onItemUse. One thing they do that this does not: **spend the
// stack** -- there is no depletion on the placement path anywhere, which is
// Survival's subtraction to make. See docs/todo-m3.md.

#include "core/entity/ray_trace.hpp"
#include "core/item/item_def.hpp"
#include "core/util/aabb.hpp"
#include "core/util/types.hpp"

namespace mc::tick {
class TickWorld;
}
namespace mc::entity {
class ParticleSystem;
class PaintingSystem;
class ArrowSystem;
class BoatSystem;
class MinecartSystem;
class MobSystem;
}
namespace mc::world {
class SignStore;
}
namespace mc::net {
class RemoteEntities;
}
namespace mc::audio {
class SoundEngine;
}

namespace mc::item {

// **Everything a click sets off that is not the world itself.**
//
// Both are optional and both are borrowed. A caller with neither gets a world
// that changes silently and invisibly, which is exactly what the tests want and
// what the headless harness has; the console passes both. That is also why they
// are pointers rather than references -- "no sound here" is a real answer, not
// a null to be defended against.
//
// They travel together because they are set off together and in an order that
// matters: `onPlayerDestroyBlock` spawns its particles from the block *before*
// it removes it, and plays the sound *after*.
// **Where an entity goes when an item spawns one.** A null pool means there is
// nowhere to put that kind of entity, and a click that would have spawned one
// does nothing -- which is exactly the state this build was in for every one of
// the items in `ItemDef::spawns`, and is what "paintings don't work", "the bow
// doesn't work" and "boats and minecarts are not even placeable" all were.
//
// Grouped rather than spread across `Effects` because they are one idea and
// because a caller that has none of them should be able to say so in one word.
struct EntityPools {
    entity::PaintingSystem* paintings = nullptr;
    entity::ArrowSystem* arrows = nullptr;
    entity::BoatSystem* boats = nullptr;
    entity::MinecartSystem* minecarts = nullptr;

    // **The animals.** The one pool here whose entities are alive: a click on
    // one is a hit, a milking or a saddling, and which of those it is depends
    // on what is in the hand -- see `interactWithEntity`.
    entity::MobSystem* mobs = nullptr;

    // **The other people in the session.** Null in single player, which is
    // every caller that has no link. Unlike every other pool here these
    // entities are not this console's to change: what a click on one does is
    // decided wherever that player is actually running, so the pool is here to
    // be *aimed at* and nothing more. See `EntityTarget::remote`.
    net::RemoteEntities* players = nullptr;

    // **Not an entity**, but it lives here for the same reason: a sign's text
    // is a side effect of a click that most callers have nowhere to put. See
    // core/world/sign_store.hpp.
    world::SignStore* signs = nullptr;
};

// **Which pool turned a click down, so the player can be told.**
//
// The pools have no cap (core/util/segmented_pool.hpp); what refuses is the
// heap, and each pool counts its refusals in `refused()`. A click that spawned
// nothing looks the same from outside whether the item had nowhere to go or
// the heap said stop, so the caller marks the counts before the click and asks
// afterwards which of them rose -- nothing in the click's signature changes.
enum class LimitedEntity : u8 { None, Painting, Arrow, Boat, Minecart, Sign, DroppedItem };

struct RefusalMark {
    u32 paintings = 0;
    u32 arrows = 0;
    u32 boats = 0;
    u32 minecarts = 0;
    u32 signs = 0;
};

RefusalMark markRefusals(const EntityPools& pools);
LimitedEntity refusedSince(const EntityPools& pools, const RefusalMark& mark);

// **Legacy Console Edition's sentence**, which is the one players know for
// this: "The maximum number of Minecarts in a world has been reached." --
// with Boats and Paintings in the same mould, and Signs, Arrows and Dropped
// Items made to match, since LCE had fixed limits of its own on none of them
// that it told anyone about. Null for `None`.
const char* limitMessage(LimitedEntity which);

struct Effects {
    entity::ParticleSystem* particles = nullptr;
    audio::SoundEngine* sound = nullptr;

    // The pools an item's click may add to. See `EntityPools`; it sits here
    // rather than in the signature for the same reason the particle system
    // does -- it is a side effect a click may or may not have, and most callers
    // have none.
    EntityPools entities;
};

// One right-click on what the crosshair found. `playerBox` is the body the
// placement must not land inside -- `World.checkIfAABBIsClear`, which fails on
// any entity that prevents spawning, and the player is one. `yawDegrees` is the
// heading, in degrees as the original stores it, and only a door reads it.
//
// Returns whether the world changed, which is also whether the click was
// spent: a lever flicked, a door opened, a block placed.
//
// `itemTook`, when given, says **which of the two took it** -- true when the
// held item did (a block placed, a painting hung, a field tilled, a seed
// planted) and false when the block answered first (a lever, a door, a
// button). Survival spends the stack on the first and never on the second; see
// item::spendOnUse.
//
// **Five items are not `ItemBlock`** and are routed here by the behaviour of
// the block they would place rather than by their id: the sign, the two doors,
// flint and steel, and the seed -- which is the only one that can overwrite a
// block that is already there. See the branches in `rightClick` and, for what
// the seed does to bedrock, `useSeeds`.
bool rightClick(tick::TickWorld& world, ItemId held, const entity::RayHit& hit,
                const AABB& playerBox, float yawDegrees, const Effects& effects = {},
                bool* itemTook = nullptr);

// **The other half of a right-click**, and the reason it is a second function:
// `Minecraft.clickMouse` runs `onPlayerRightClick` on the block the crosshair
// found, and *then* runs `Item.onItemRightClick` on the stack -- two entry
// points, tried in that order, and the second one gets **no hit at all**. It
// does its own ray trace, with its own reach and its own rules about what the
// ray may stop on, which is exactly why a bucket cannot be expressed as a
// placement: it needs to see water, and the crosshair's ray is not allowed to.
//
// It takes an eye and a direction rather than a `RayHit` for the same reason.
// `partialTicks` is 1.0F in the original's call, so the interpolated position
// it computes is just the current one and the caller passes that.
//
// **It can change what is in the hand**, which nothing else in this file does.
// A bucket is the whole of that: emptied it becomes full, full it becomes
// empty, and a build with no depletion still has to model it or water can be
// poured and never picked up. `becomes` is the item the stack turns into and
// is `held` itself when nothing about it changed, so a caller that ignores the
// field is correct rather than merely lucky.
struct ItemUse {
    bool changed = false;
    ItemId becomes = 0;
};

// `random.bow` at `jg`'s pitch, which `useItem` plays for a shot it fires.
// Public for the two shots it does not fire: a guest's, which the host fires
// and the guest still hears at its own ear, and the host firing for a guest.
void playBowSound(const Effects& effects, double x, double y, double z);

ItemUse useItem(tick::TickWorld& world, ItemId held, double eyeX, double eyeY, double eyeZ,
                double dirX, double dirY, double dirZ, const Effects& effects = {});

// `hq.b(IIII)Z` -- **PlayerController.onPlayerDestroyBlock**, and its three
// steps are in an order that cannot be shuffled:
//
// ```
// effectRenderer.addBlockDestroyEffects(i, j, k);      // the block is still there
// Block block = Block.blocksList[world.getBlockId(i, j, k)];
// boolean removed = world.setBlockWithNotify(i, j, k, 0);
// if (block != null && removed) {
//     sndManager.playSound(block.stepSound.getBreakSound(), i + 0.5F, j + 0.5F, k + 0.5F,
//                          (volume + 1) / 2, pitch * 0.8F);
//     block.onBlockDestroyedByPlayer(world, i, j, k, metadata);
// }
// ```
//
// The particles come first because they need the block's texture and the cell
// is about to be air; the sound comes last because it is only played if the
// removal actually happened. Returns whether the world changed.
bool destroyBlock(tick::TickWorld& world, i32 x, int y, i32 z, const Effects& effects = {});

// **The entity under the crosshair**, if any -- the second half of
// `EntityRenderer.getMouseOver` (see core/entity/ray_trace.hpp for the
// transcription). One found here *is* `objectMouseOver`: the block behind it is
// neither outlined, broken nor clicked.
//
// `blockHit` is the crosshair's block ray from the same eye, and it is what
// clips the search: an entity has to be within `kEntityReach` **and nearer
// than the block**. Every painting, boat and cart is a candidate -- each
// class's `canBeCollidedWith` is true while it is alive -- and the nearest
// intercept against its box grown by `kEntityPickBorder` wins, with the
// original's `d < best || best == 0` rule. The ridden vehicle is not
// excluded, as it is not in the original.
struct EntityTarget {
    enum class Kind : u8 { None, Painting, Boat, Minecart, Mob, Player };
    Kind kind = Kind::None;
    int index = -1;
    // From the eye to where the ray meets the grown box.
    double distance = 0.0;

    bool found() const { return kind != Kind::None; }

    // **The one kind this console must not resolve itself.** Another player
    // belongs to whichever console is running them; a hit on one is sent, and
    // what it costs them is decided there. `attackEntity` refuses it for that
    // reason rather than by omission. See `packet::UseEntity`.
    bool remote() const { return kind == Kind::Player; }
};

EntityTarget pickEntity(const EntityPools& pools, double eyeX, double eyeY, double eyeZ,
                        double dirX, double dirY, double dirZ, const entity::RayHit& blockHit);

// **The other half of a left click**, and it was missing entirely:
// `Minecraft.clickMouse` asks `objectMouseOver` for what is under the
// crosshair, and **an entity there is hit instead of the block behind it** --
// `playerController.attackEntity(player, entity)`, which is
// `EntityPlayer.attackTargetEntityWithCurrentItem`:
//
// ```
// int i = inventory.getDamageVsEntity(entity);
// if (i > 0) entity.attackEntityFrom(this, i);
// ```
//
// Without it there was no way to break a boat, a minecart or a painting at all,
// so none of the three could leave anything on the ground however hard it was
// hit. Returns true when something was hit.
//
// `held` is what is in the hand, and it is the whole of how hard the hit lands:
// `InventoryPlayer.getDamageVsEntity` forwards to the stack's own item, which
// is 1 for an empty hand and for everything that is neither a tool nor a sword,
// and the constructed number for the two classes that are -- a wooden sword 4,
// a diamond one 10, a diamond axe 6. It is a generated column
// (`ItemDef::damageVsEntity`) rather than a constant here, and the unknown
// row's 1 is the empty hand's answer, so `held == 0` needs no special case.
//
// One thing about it is stated rather than derived: a hit that does not break
// the thing still counts as a hit, exactly as `attackEntityFrom` returning true
// does.
//
// **Durability is spent by the caller, not here.** The jar follows the hit
// with `stack.hitEntity(living)`, which is `damageItem(1)` on a sword and
// `(2)` on a tool -- `item::wearOnHit` in core/item/tool_rules.hpp, applied by
// the Survival branch of the edit path. Creative wears nothing.
// **Where the hit came from**, which only a living target reads: `ge`'s
// knockback pushes the mob away from its attacker and needs a position to push
// away from. A caller that leaves it out lands the damage and no shove, which
// is what `attackEntityFrom(null, n)` does in the jar -- suffocation, drowning
// and the void all come in that way.
struct Attacker {
    bool present = false;
    double x = 0.0;
    double z = 0.0;

    // **False is a Creative fist**: the hit lands in full -- the sheep is still
    // shorn, the cow still dies, the knockback is still applied -- and the
    // monster is left with nobody to chase. It is the swinging half of
    // `entity::MobPlayer::targetable`, which is where the whole argument for a
    // player a mob cannot hunt is written down.
    bool provokes = true;
};

bool attackEntity(tick::TickWorld& world, const EntityTarget& target, ItemId held,
                  const Effects& effects, const Attacker& attacker = {});

// `bi.a_(Lkh;)V` -- **useCurrentItemOnEntity**, the right click's entity branch,
// and it is two steps rather than one:
//
// ```
// if (entity.interact(player)) return;              // the entity's own answer
// ItemStack stack = getCurrentEquippedItem();
// if (stack != null && entity instanceof EntityLiving) stack.useItemOnEntity(living);
// ```
//
// A boat or a plain cart takes the player aboard; a **chest cart opens its 27
// slots and a furnace cart takes a piece of coal and is aimed away from the
// player**, both of which answer the first step and so never reach the second.
// A vehicle already ridden and a painting refuse. An animal answers the first step
// with a bucket (a cow gives milk) or a saddle already on (a pig carries you),
// and the second step is where `ItemSaddle` puts a saddle on a pig -- which is
// why a saddle held out to a *saddled* pig mounts it instead of being eaten.
//
// `held` is what is in the hand and `becomes` is what it turns into, the same
// shape `useItem` takes: a bucket becomes a milk bucket and a spent saddle
// becomes nothing (item 0). `becomes` is `held` when nothing about the stack
// changed, so a caller that ignores it is right rather than lucky.
struct EntityInteraction {
    bool taken = false;
    ItemId becomes = 0;

    // **A chest minecart opens a screen and has no block to open it on**, so
    // it cannot go through `TickWorld::openContainer` like a chest does: that
    // seam names a cell. The cart's own id comes back here instead, and the
    // caller opens the screen on it. Zero when nothing was opened.
    u32 opensMinecartChest = 0;

    // A furnace cart swallowed the piece of coal in the hand, so Survival
    // takes one off the stack -- the same cue `itemTook` is on the block path.
    bool spentFuel = false;
};

// `playerX`/`playerZ` are where the click came from, which only the furnace
// cart reads -- it is aimed away from whoever pushed it.
EntityInteraction interactWithEntity(tick::TickWorld& world, const EntityTarget& target,
                                     const EntityPools& pools, ItemId held,
                                     double playerX = 0.0, double playerZ = 0.0);

}  // namespace mc::item
