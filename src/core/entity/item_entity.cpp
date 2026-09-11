// `dx` -- EntityItem -- and the throw that makes one. See item_entity.hpp for
// what is the original's and what is ours.

#include "core/entity/item_entity.hpp"

#include "core/block/fluid_flow.hpp"
#include "core/block/registry.hpp"
#include "core/entity/sweep.hpp"
#include "core/item/inventory.hpp"
#include "core/item/registry.hpp"
#include "core/tick/tick_world.hpp"
#include "core/util/math_helper.hpp"

#include <cmath>

namespace mc::entity {
namespace {

// The two materials the entity asks about by name, found the way `PlayerBody`
// finds them: the test in the class file is on the material, so the material is
// what is compared and the id is only how it is reached.
constexpr u8 kLavaMaterial = mcver::kBlocks[int(mcver::Block::Lava)].material;
constexpr u8 kWaterMaterial = mcver::kBlocks[int(mcver::Block::Water)].material;

// `cn.a(cf,gb)Z` -- **World.isMaterialInBB**, in the one shape a dropped item
// needs it: `kh.G()` shrinks the box by 0.4 top and bottom and asks for lava.
// The loop bounds are the floor of the minimum and the floor of the maximum
// plus one, which is a half-open range over every cell the box touches.
bool materialInBox(const tick::TickWorld& world, const AABB& probe, u8 material)
{
    const i32 x0 = MathHelper::floorDouble(probe.minX);
    const i32 x1 = MathHelper::floorDouble(probe.maxX + 1.0);
    const int y0 = MathHelper::floorDouble(probe.minY);
    const int y1 = MathHelper::floorDouble(probe.maxY + 1.0);
    const i32 z0 = MathHelper::floorDouble(probe.minZ);
    const i32 z1 = MathHelper::floorDouble(probe.maxZ + 1.0);

    for (i32 bx = x0; bx < x1; ++bx) {
        for (int by = y0; by < y1; ++by) {
            for (i32 bz = z0; bz < z1; ++bz) {
                if (block::def(world.blockAt(bx, by, bz)).material == material) {
                    return true;
                }
            }
        }
    }
    return false;
}

// The same `(sky << 4) | block` byte every mesh vertex carries. `getBrightness`
// asks it every frame in the original; once a tick is enough here and is what
// the particles already do.
u8 packedLightAt(const tick::TickWorld& world, double x, double y, double z)
{
    const i32 bx = MathHelper::floorDouble(x);
    const int by = int(MathHelper::floorDouble(y));
    const i32 bz = MathHelper::floorDouble(z);
    return u8((world.skyLightAt(bx, by, bz) << 4) | world.blockLightAt(bx, by, bz));
}

// `dx.g(DDD)Z` -- **pushOutOfBlocks**, which is what stops an item dropped
// against a wall from sitting inside it. Finds the nearest face of the cell
// that is *not* another solid and shoves the item that way; a cell walled in on
// all six sides leaves it where it is, which is the original's answer too.
void pushOutOfBlocks(const tick::TickWorld& world, ItemEntity& e, JavaRandom& rand)
{
    const i32 bx = MathHelper::floorDouble(e.x);
    const int by = int(MathHelper::floorDouble(e.y));
    const i32 bz = MathHelper::floorDouble(e.z);
    if (!world.opaqueAt(bx, by, bz)) {
        return;
    }

    const double fx = e.x - double(bx);
    const double fy = e.y - double(by);
    const double fz = e.z - double(bz);

    // Six candidates, each "is that side open" paired with "how far to it".
    // The order is the class file's and it matters at a tie: a strict `<`
    // means the *first* of two equal distances wins.
    const bool open[6] = {
        !world.opaqueAt(bx - 1, by, bz), !world.opaqueAt(bx + 1, by, bz),
        !world.opaqueAt(bx, by - 1, bz), !world.opaqueAt(bx, by + 1, bz),
        !world.opaqueAt(bx, by, bz - 1), !world.opaqueAt(bx, by, bz + 1),
    };
    const double distance[6] = {fx, 1.0 - fx, fy, 1.0 - fy, fz, 1.0 - fz};

    int best = -1;
    double nearest = 9999.0;
    for (int side = 0; side < 6; ++side) {
        if (open[side] && distance[side] < nearest) {
            nearest = distance[side];
            best = side;
        }
    }
    if (best < 0) {
        return;
    }

    const float push = rand.nextFloat() * 0.2f + 0.1f;
    switch (best) {
    case 0: e.motionX = -double(push); break;
    case 1: e.motionX = double(push); break;
    case 2: e.motionY = -double(push); break;
    case 3: e.motionY = double(push); break;
    case 4: e.motionZ = -double(push); break;
    default: e.motionZ = double(push); break;
    }
}

}  // namespace

ItemEntity* ItemEntitySystem::allocate(bool mayEvict)
{
    if (ItemEntity* e = items_.push()) {
        return e;
    }
    // **The heap will not hold another: the oldest makes room.** It is the one
    // nearest its own 6000-tick despawn, and the new one is what the player is
    // looking at. See the header for why this is not a refusal -- and for the
    // one caller that would rather be refused.
    if (!mayEvict || items_.empty()) {
        ++refused_;
        return nullptr;
    }
    int oldest = 0;
    for (int i = 1; i < items_.size(); ++i) {
        if (items_[i].age > items_[oldest].age) {
            oldest = i;
        }
    }
    ++evicted_;
    ItemEntity& e = items_[oldest];
    e = ItemEntity{};
    return &e;
}

void ItemEntitySystem::removeAt(int index)
{
    items_.swapRemove(index);
}

bool ItemEntitySystem::combine(ItemEntity& a, ItemEntity& b)
{
    if (&a == &b || !a.alive() || !b.alive()) {
        return false;
    }
    // `itemstack1.getItem() != itemstack.getItem()`, and the damage beside it:
    // a half-worn pick does not join a fresh one, which is the same rule
    // `Inventory::addStack` applies at the other end of the journey.
    if (a.item != b.item || a.damage != b.damage) {
        return false;
    }

    // `if (itemstack1.stackSize < itemstack.stackSize) return entityitem.combineItems(this);`
    // -- the bigger stack is the survivor, and on a tie it is `b`. Written as a
    // swap of references rather than as the original's recursive call, which is
    // the same two lines with one less stack frame.
    ItemEntity* keep = &b;
    ItemEntity* give = &a;
    if (b.count < a.count) {
        keep = &a;
        give = &b;
    }

    const int limit = int(item::def(keep->item).stack);
    if (keep->count + give->count > limit) {
        return false;
    }

    keep->count += give->count;
    // `Math.max` on the delay and `Math.min` on the age: merging must not make
    // an item pickable sooner than either half was, nor age the pair by the
    // older one's clock.
    if (give->pickupDelay > keep->pickupDelay) {
        keep->pickupDelay = give->pickupDelay;
    }
    if (give->age < keep->age) {
        keep->age = give->age;
    }
    give->count = 0;
    return true;
}

bool ItemEntitySystem::spawn(const tick::TickWorld& world, double px, double py, double pz,
                             item::ItemId id, int count, i16 damage, int pickupDelay)
{
    return place(world, px, py, pz, id, count, damage, pickupDelay, true) != nullptr;
}

ItemEntity* ItemEntitySystem::place(const tick::TickWorld& world, double px, double py,
                                    double pz, item::ItemId id, int count, i16 damage,
                                    int pickupDelay, bool mayEvict)
{
    if (id == 0 || count <= 0) {
        return nullptr;
    }
    ItemEntity* e = allocate(mayEvict);
    if (e == nullptr) {
        return nullptr;
    }

    // `dx.<init>`, in its order. The hover phase and the yaw come off
    // `Math.random()` in the original -- a time-seeded global -- so what is
    // copied is the distribution and not the sequence, exactly as the particles
    // copy theirs.
    e->item = id;
    e->count = count;
    e->damage = damage;
    e->pickupDelay = pickupDelay;
    e->age = 0;
    e->hoverPhase = float(rand_.nextDouble() * 3.141592653589793 * 2.0);
    e->yaw = float(rand_.nextDouble() * 360.0);
    e->setPosition(px, py, pz);

    e->motionX = double(float(rand_.nextDouble() * 0.20000000298023224
                              - 0.10000000149011612));
    e->motionY = 0.20000000298023224;
    e->motionZ = double(float(rand_.nextDouble() * 0.20000000298023224
                              - 0.10000000149011612));
    e->light = packedLightAt(world, px, py, pz);
    return e;
}

bool ItemEntitySystem::dropFromPlayer(const tick::TickWorld& world, double eyeX, double eyeY,
                                      double eyeZ, float yawDegrees, float pitchDegrees,
                                      item::ItemId id, int count, i16 damage)
{
    // **`posY - 0.3 + getEyeHeight()`, and `getEyeHeight` is 0.12** on
    // `EntityPlayer` -- so an item leaves the hand a fifth of a block below the
    // eye rather than at it. `posY` is already the eye here, which is what
    // `Entity` means by it.
    //
    // **A throw is refused rather than evicting** when the heap will not hold
    // another: the item is still in the player's hand, so a refusal loses
    // nothing.
    ItemEntity* thrown = place(world, eyeX, eyeY - 0.30000001192092896 + 0.12, eyeZ, id, count,
                               damage, kItemPickupDelay, false);
    if (thrown == nullptr) {
        return false;
    }
    ItemEntity& e = *thrown;

    // Two seconds before it can be walked back into, which is the whole reason
    // a drop does not immediately un-drop itself.
    e.pickupDelay = kItemDropPickupDelay;

    // The `flag == false` branch: thrown along the look vector at 0.3, lifted a
    // tenth, and then scattered by a hundredth. **`MathHelper` for the aim and
    // `std::` for the scatter**, because that is what the class file does --
    // `eo.a`/`eo.b` for the first three lines and `java.lang.Math.cos`/`sin`
    // for the two that follow. Mixing the two looks like an oversight and is
    // the original's own.
    constexpr float kPi = 3.1415927f;
    const float yaw = yawDegrees / 180.0f * kPi;
    const float pitch = pitchDegrees / 180.0f * kPi;
    constexpr float kThrow = 0.3f;

    e.motionX = double(-MathHelper::sin(yaw) * MathHelper::cos(pitch) * kThrow);
    e.motionZ = double(MathHelper::cos(yaw) * MathHelper::cos(pitch) * kThrow);
    e.motionY = double(-MathHelper::sin(pitch) * kThrow + 0.1f);

    const float angle = rand_.nextFloat() * kPi * 2.0f;
    const float scatter = 0.02f * rand_.nextFloat();
    e.motionX += std::cos(double(angle)) * double(scatter);
    e.motionY += double((rand_.nextFloat() - rand_.nextFloat()) * 0.1f);
    e.motionZ += std::sin(double(angle)) * double(scatter);
    return true;
}

void ItemEntitySystem::tick(const tick::TickWorld& world)
{
    for (int i = 0; i < items_.size(); ++i) {
        ItemEntity& e = items_[i];

        // Emptied by a merge earlier in this same pass. The sweep at the
        // bottom is what reclaims it.
        if (!e.alive()) {
            continue;
        }

        // **An item outside a loaded column does not tick.** a1.1.2 gets this
        // for free -- its entities live in the chunk and go out with it -- and
        // ours cannot, because the pool is flat and outlives the columns. So
        // the residency test is written down here instead: no physics, no
        // light resample, no merge, and above all **no ageing**, which is the
        // half that matters. Without it a drop left behind while the player
        // walks two hundred blocks away has quietly spent its five minutes by
        // the time they walk back.
        if (!world.chunkResident(MathHelper::floorDouble(e.x) >> 4,
                                 MathHelper::floorDouble(e.z) >> 4)) {
            continue;
        }

        // ---- `kh.y()` -- Entity.onEntityUpdate, which `dx.e_()` calls first --
        //
        // **Lava destroys a dropped item, and it destroys it outright.** The
        // arithmetic is not close: `attackEntityFrom(null, 10)` against
        // `dx.f`, which the constructor sets to **5**, and `dx.a(Lkh;I)Z`
        // subtracts and calls `setDead()` at or below zero. So one tick in
        // lava is the end of the stack -- the bounce below is what a *falling*
        // item does on the tick it arrives, not a way of surviving.
        //
        // This is the half of `dx.e_()` that was missing. The fizz and the hop
        // were already here and are the reason the loss looked like a bug: an
        // item thrown into lava jumped about convincingly and then lay there
        // for five minutes.
        //
        // **Fire does not do this.** Nothing in a1.1.2 sets an entity's fire
        // counter except this branch -- `og` has no `onEntityCollidedWithBlock`
        // at all and no other class writes `kh.aT` -- so a stack lying in a
        // flame is untouched. That is the version, not a gap here; see
        // docs/status.md.
        if (materialInBox(world, e.box.expand(0.0, kLavaProbeInset, 0.0), kLavaMaterial)) {
            e.count = 0;
            continue;
        }

        // `if (posY < -64.0D) kill()`. Nothing in this build can fall out of
        // the world -- the sweep stops at y = 0 -- but it is one compare and
        // it is what the method does.
        if (e.y < kVoidFloor) {
            e.count = 0;
            continue;
        }

        if (e.pickupDelay > 0) {
            --e.pickupDelay;
        }

        e.prevX = e.x;
        e.prevY = e.y;
        e.prevZ = e.z;

        e.motionY -= kItemPull;

        // **Lava throws it back out.** The cell tested is the one the *centre*
        // is in, not the box, and the sound is `random.fizz` -- which this does
        // not play, because nothing here has a sound engine. See
        // docs/audio-a1.1.2.md; the fizz is already on that list.
        const i32 bx = MathHelper::floorDouble(e.x);
        const int by = int(MathHelper::floorDouble(e.y));
        const i32 bz = MathHelper::floorDouble(e.z);
        if (block::def(world.blockAt(bx, by, bz)).material == kLavaMaterial) {
            e.motionY = 0.20000000298023224;
            e.motionX = double((rand_.nextFloat() - rand_.nextFloat()) * 0.2f);
            e.motionZ = double((rand_.nextFloat() - rand_.nextFloat()) * 0.2f);
        }

        pushOutOfBlocks(world, e, rand_);

        // `handleWaterMovement()`, which is where a river carries a dropped
        // item downstream. Its answer -- "is this in water" -- is thrown away
        // by `dx.e_()`, which calls it for the push alone.
        block::handleWaterMovement(world, e.box, kWaterMaterial, &e.motionX, &e.motionY,
                                   &e.motionZ);

        // `moveEntity`, through the sweep the player and the particles share.
        // No step up -- an item's `stepHeight` is zero -- and no fall distance.
        AABB box = e.box;
        double dx = e.motionX;
        double dy = e.motionY;
        double dz = e.motionZ;
        const double wantY = dy;

        const BlockRange range = sweepRange(box.extend(dx, dy, dz));
        dy = clipAxis(world, range, box, kAxisY, dy);
        box = box.offset(0.0, dy, 0.0);
        dx = clipAxis(world, range, box, kAxisX, dx);
        box = box.offset(dx, 0.0, 0.0);
        dz = clipAxis(world, range, box, kAxisZ, dz);
        box = box.offset(0.0, 0.0, dz);

        e.box = box;
        e.x = (box.minX + box.maxX) / 2.0;
        e.y = box.minY + kItemHalf;
        e.z = (box.minZ + box.maxZ) / 2.0;
        e.onGround = wantY != dy && wantY < 0.0;

        if (e.motionX != dx) e.motionX = 0.0;
        if (e.motionY != dy) e.motionY = 0.0;
        if (e.motionZ != dz) e.motionZ = 0.0;

        // **The horizontal drag is the floor's slipperiness**, which is why an
        // item slides a long way on ice and stops on grass. The block asked
        // about is one below the *box's* bottom, not below the centre.
        float drag = kItemAirDrag;
        if (e.onGround) {
            drag = kItemGroundDrag;
            const int floorY = int(MathHelper::floorDouble(box.minY)) - 1;
            const block::BlockId below =
                world.blockAt(MathHelper::floorDouble(e.x), floorY,
                              MathHelper::floorDouble(e.z));
            if (below != block::kAir) {
                drag = block::slipperinessOf(below) * kItemAirDrag;
            }
        }

        e.motionX *= double(drag);
        e.motionY *= kItemVerticalDrag;
        e.motionZ *= double(drag);
        if (e.onGround) {
            // A small bounce, and it is what makes a dropped item settle with a
            // hop rather than sticking where it lands.
            e.motionY *= -0.5;
        }

        e.light = packedLightAt(world, e.x, e.y, e.z);

        // **The merge scan**, on the original's own cadence and not every
        // tick. `age` here is the value *before* the increment below, so a
        // pair dropped together merges on the tick they land rather than
        // waiting a second and a quarter for the first multiple of 25.
        if (e.age % kItemMergeInterval == 0) {
            const AABB reach = e.box.expand(kItemMergeReach, 0.0, kItemMergeReach);
            for (int j = 0; j < items_.size(); ++j) {
                if (j == i || !items_[j].alive() || !items_[j].box.intersects(reach)) {
                    continue;
                }
                if (combine(e, items_[j]) && !e.alive()) {
                    // This one lost: it is the stack that was poured away.
                    break;
                }
            }
            if (!e.alive()) {
                continue;
            }
        }

        // `if (age >= 6000) setDead()`, after the increment -- five minutes.
        if (++e.age >= kItemMaxAge) {
            e.count = 0;
        }
    }

    // The sweep. Everything a merge or the age limit emptied goes here, in one
    // pass, once the walk above is finished and no reference into the pool is
    // still live.
    for (int i = 0; i < items_.size();) {
        if (items_[i].alive()) {
            ++i;
            continue;
        }
        removeAt(i);
    }
    items_.trim();
}

int ItemEntitySystem::collect(const AABB& playerBox, item::Inventory& inventory)
{
    int taken = 0;
    for (int i = 0; i < items_.size();) {
        ItemEntity& e = items_[i];
        if (e.pickupDelay != 0 || !e.box.intersects(playerBox)) {
            ++i;
            continue;
        }

        // **All of it or none of it.** `addItemStackToInventory` answers true
        // only when the stack went in whole, and only then is the entity
        // removed; a partial fill leaves the entity holding the remainder and
        // the player walks over it again next tick. The original expresses that
        // by mutating the stack's `stackSize` in place, which is the same
        // arithmetic written the other way round.
        const int leftover = inventory.addStack(e.item, e.count, e.damage);
        if (leftover == e.count) {
            ++i;
            continue;
        }
        e.count = leftover;
        if (leftover > 0) {
            ++i;
            continue;
        }
        ++taken;
        removeAt(i);
    }
    return taken;
}

}  // namespace mc::entity
