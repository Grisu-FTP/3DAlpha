// Farmland under a footstep -- `Block.onEntityWalking`, the half of
// `moveEntity`'s step that is paid to the block rather than to the ears.
//
// Two blocks answer it in a1.1.2 and both are checked here: farmland, which is
// trampled back to dirt on one roll in four, and redstone ore, which lights up.
// The version where trampling is a *landing* -- a fall-distance test, a field
// that survives being walked over and is ruined by a jump -- is Beta's; this
// one counts footsteps, and a footstep is one block of ground covered however
// fast it is covered.
//
// The trigger is driven the way the game drives it: the body moves against a
// const world and names the cell it trod on, and the caller that owns the tick
// hands that to `tick::entityWalkedOnBlock`.

#include "core/block/registry.hpp"
#include "core/entity/player_body.hpp"
#include "core/tick/behaviour.hpp"
#include "core/tick/tick_world.hpp"
#include "framework.hpp"
#include "scene_world.hpp"

using namespace mc;
using mc::block::BlockId;
using mc::entity::PlayerBody;
using mc::entity::PlayerInput;
using mc::test::SceneWorld;

namespace {

BlockId bid(mcver::Block b) { return BlockId(b); }

constexpr int kFloorY = 63;

// A field to cross, and a player standing at the near end of it.
struct Field {
    SceneWorld world{0, 0};
    PlayerBody body;

    explicit Field(mcver::Block floor)
    {
        for (i32 x = -8; x <= 8; ++x) {
            for (i32 z = -8; z <= 24; ++z) {
                world.place(x, kFloorY, z, bid(floor), 0);
            }
        }
        body.setFeet(0.5, double(kFloorY) + 1.0, 0.5);
    }

    // One tick of walking, plus the tail of `moveEntity` the body cannot run
    // itself. Returns whether this tick earned a footstep.
    bool step(const PlayerInput& input)
    {
        body.tick(world.w(), input);
        if (!body.steppedOn) return false;
        tick::entityWalkedOnBlock(world.w(), body.stepBlockX, body.stepBlockY,
                                  body.stepBlockZ);
        return true;
    }

    int walk(int ticks, bool sneak = false, bool jump = false)
    {
        PlayerInput input;
        input.forward = 1.0f;
        input.yawDegrees = 0.0f;
        input.sneak = sneak;
        input.jump = jump;

        int steps = 0;
        for (int i = 0; i < ticks; ++i) {
            if (step(input)) ++steps;
        }
        return steps;
    }

    // How many cells of the strip the player walks down are no longer farmland.
    int trampled() const
    {
        int count = 0;
        for (i32 z = -8; z <= 24; ++z) {
            if (world_blockAt(0, z) == bid(mcver::Block::Dirt)) ++count;
        }
        return count;
    }

    BlockId world_blockAt(i32 x, i32 z) const
    {
        return const_cast<SceneWorld&>(world).w().blockAt(x, kFloorY, z);
    }
};

}  // namespace

TEST(walking_over_a_field_tramples_it)
{
    Field f(mcver::Block::Farmland);
    const int steps = f.walk(120);

    CHECK(steps > 0);
    // One roll in four, so a walk long enough to earn a dozen footsteps ruins
    // some of what it crosses. Which cells is the world random's business.
    CHECK(f.trampled() > 0);
    CHECK(f.trampled() <= steps);
}

TEST(sneaking_over_a_field_leaves_it_alone)
{
    // A sneaking player earns no footstep at all -- the same `if` -- so there
    // is nothing to roll.
    Field f(mcver::Block::Farmland);
    CHECK_EQ(f.walk(120, /*sneak=*/true), 0);
    CHECK_EQ(f.trampled(), 0);
}

TEST(jumping_on_the_spot_never_tramples)
{
    // **The a1.1.2 reading, and the one that separates it from Beta**: there is
    // no fall-distance test in `BlockFarmland.onEntityWalking`, so what ruins a
    // field is ground covered. Jumping without going anywhere covers none.
    Field f(mcver::Block::Farmland);

    PlayerInput input;
    input.jump = true;
    for (int i = 0; i < 120; ++i) {
        CHECK(!f.step(input));
    }
    CHECK_EQ(f.trampled(), 0);
}

TEST(trampling_is_one_roll_in_four)
{
    // The callback on its own, with the field put back after every reversion,
    // so the count is of draws rather than of a strip wearing out.
    Field f(mcver::Block::Farmland);
    constexpr int kRolls = 4000;
    int reverted = 0;
    for (int i = 0; i < kRolls; ++i) {
        f.world.place(0, kFloorY, 0, bid(mcver::Block::Farmland), 0);
        tick::entityWalkedOnBlock(f.world.w(), 0, kFloorY, 0);
        if (f.world.w().blockAt(0, kFloorY, 0) == bid(mcver::Block::Dirt)) ++reverted;
    }
    // A quarter, with room for the draw: 1000 expected, and the band is wide
    // enough that a fixed seed passing is not the reason it passes.
    CHECK(reverted > kRolls / 5);
    CHECK(reverted < kRolls / 3);
}

TEST(trampling_takes_the_crop_standing_on_it)
{
    // **The crop goes in the same call, not on its next tick.**
    // `setBlockWithNotify` notifies the cell above, and a crop's
    // `onNeighborBlockChange` is `mq.h` -- checkFlowerChange -- which drops it
    // and writes air the moment `hd.b(I)Z` stops seeing farmland underneath.
    SceneWorld world{0, 0};
    world.place(0, kFloorY, 0, bid(mcver::Block::Farmland), 7);
    world.place(0, kFloorY + 1, 0, bid(mcver::Block::Wheat), 7);
    // Wheat wants light as well as ground, and a scene starts pitch dark --
    // without this the crop would be removed for the wrong reason.
    world.lightColumnsFrom(-2, 2, -2, 2, kFloorY + 1);

    // It survives every footstep the roll does not take.
    int rolls = 0;
    while (world.w().blockAt(0, kFloorY, 0) == bid(mcver::Block::Farmland)) {
        CHECK_EQ((long long) world.w().blockAt(0, kFloorY + 1, 0),
                 (long long) int(mcver::Block::Wheat));
        tick::entityWalkedOnBlock(world.w(), 0, kFloorY, 0);
        CHECK(++rolls < 200);
    }

    CHECK_EQ((long long) world.w().blockAt(0, kFloorY, 0), (long long) int(mcver::Block::Dirt));
    CHECK_EQ((long long) world.w().blockAt(0, kFloorY + 1, 0), 0LL);
}

TEST(walking_a_planted_field_leaves_no_crop_standing_on_dirt)
{
    // The whole path, in the order the frame loop runs it: nothing is left
    // floating over a furrow that has been walked back into dirt.
    Field f(mcver::Block::Farmland);
    for (i32 x = -8; x <= 8; ++x) {
        for (i32 z = -8; z <= 24; ++z) {
            f.world.place(x, kFloorY + 1, z, bid(mcver::Block::Wheat), 7);
        }
    }
    f.world.lightColumnsFrom(-8, 8, -8, 24, kFloorY + 1);

    CHECK(f.walk(200) > 0);

    int trampled = 0;
    for (i32 z = -8; z <= 24; ++z) {
        if (f.world_blockAt(0, z) != bid(mcver::Block::Dirt)) continue;
        ++trampled;
        CHECK_EQ((long long) f.world.w().blockAt(0, kFloorY + 1, z), 0LL);
    }
    CHECK(trampled > 0);
}

TEST(a_footstep_lights_redstone_ore)
{
    // `ai.a(Lcn;IIILkh;)V` -- the other override, and it needs no roll.
    Field f(mcver::Block::RedstoneOre);
    CHECK(f.walk(120) > 0);

    int lit = 0;
    for (i32 z = -8; z <= 24; ++z) {
        if (f.world_blockAt(0, z) == bid(mcver::Block::LitRedstoneOre)) ++lit;
    }
    CHECK(lit > 0);
}
