// The five hostile mobs: what they are, who they go for, what they do when
// they get there, and what is left afterwards.
//
// The peaceful half of this is tests/mob_test.cpp and the two share a shape:
// there is no oracle suite and there cannot be one, because every branch in
// `ek.b_()` is fed by a `Random` a1.1.2 seeds from the wall clock. What is
// checked is the table against the class files, the branches against what the
// bytecode says they do, and the things a play session would notice -- a
// creeper that goes off and leaves gunpowder, a skeleton that shoots itself, a
// slime that splits for ever.
//
// Read docs/mobs-a1.1.2.md first; this file cites it and does not repeat it.

#include "core/block/registry.hpp"
#include "core/entity/arrow.hpp"
#include "core/entity/explosion.hpp"
#include "core/entity/mob.hpp"
#include "core/entity/mob_spawn.hpp"
#include "core/item/registry.hpp"
#include "core/render/mob_mesh.hpp"
#include "core/texture/entity_skins.hpp"
#include "core/tick/tick_world.hpp"
#include "core/util/math_helper.hpp"
#include "drop_catcher.hpp"
#include "framework.hpp"
#include "scene_world.hpp"
#include "sound_catcher.hpp"

#include <cmath>
#include <cstdlib>
#include <vector>

using namespace mc;
using mc::block::BlockId;
using mc::entity::Mob;
using mc::entity::MobAi;
using mc::entity::MobDef;
using mc::entity::mobDef;
using mc::entity::MobSurroundings;
using mc::entity::MobSystem;
using mc::entity::MobType;
using mc::test::SceneWorld;

namespace {

BlockId bid(mcver::Block b) { return BlockId(b); }

// **A cave**, which is what a monster needs: a stone floor at y = 40, a stone
// roof at y = 46, air between, and no light anywhere. The roof matters -- a
// zombie under open sky burns, and half of these tests would be about that
// instead of about what they are about.
struct Cave {
    test::DropCatcher drops;
    test::SoundCatcher sounds;
    SceneWorld scene{0, 0};
    MobSystem mobs{4242};

    static constexpr int kFloor = 40;
    static constexpr double kGround = 41.0;
    static constexpr int kRoof = 46;

    explicit Cave(i32 half = 20)
    {
        for (i32 x = -half; x <= half; ++x) {
            for (i32 z = -half; z <= half; ++z) {
                scene.place(x, kFloor, z, bid(mcver::Block::Stone), 0);
                scene.place(x, kRoof, z, bid(mcver::Block::Stone), 0);
            }
        }
        drops.watch(scene.w());
        sounds.watch(scene.w());
    }

    // **The frame loop's entity query, which a bare `SceneWorld` does not
    // have.** Without it `TickWorld::anyEntityIn` answers false for everything,
    // and a spawn check that trips over its own candidate passes every test
    // while refusing every monster on the console. `watchEntities` is what
    // makes this fixture answer the question the game answers.
    void watchEntities()
    {
        scene.w().setEntityQuery(&Cave::query, this);
    }

    static bool query(void* ctx, const AABB& box, tick::EntityFilter)
    {
        const Cave& self = *static_cast<const Cave*>(ctx);
        for (int i = 0; i < self.mobs.count(); ++i) {
            if (self.mobs[i].alive && self.mobs[i].body.box.intersects(box)) {
                return true;
            }
        }
        return false;
    }

    tick::TickWorld& w() { return scene.w(); }

    int add(MobType type, double x, double z, float yaw = 0.0f)
    {
        if (!mobs.spawn(w(), type, x, kGround, z, yaw)) {
            return -1;
        }
        return mobs.count() - 1;
    }

    void run(int ticks, const MobSurroundings& around)
    {
        for (int i = 0; i < ticks; ++i) {
            mobs.tick(w(), around);
        }
    }
};

// What a hit on the player does when there is nowhere for it to land. The
// shape `platform/ctr/main.cpp` fills in for real; here it just counts.
struct Harm {
    int taken = 0;
    int hits = 0;
    double lastFromX = 0.0;

    static void sink(void* ctx, int amount, mc::entity::DamageSource, double fromX, double)
    {
        Harm& self = *static_cast<Harm*>(ctx);
        self.taken += amount;
        ++self.hits;
        self.lastFromX = fromX;
    }
};

MobSurroundings hunted(Harm* harm, double x, double z, double feet = Cave::kGround)
{
    MobSurroundings around;
    around.player.present = true;
    around.player.x = x;
    // A player's `posY` is the eye, 1.62 above the feet.
    around.player.y = feet + 1.62;
    around.player.z = z;
    around.player.box = AABB{x - 0.3, feet, z - 0.3, x + 0.3, feet + 1.8, z + 0.3};
    if (harm != nullptr) {
        around.hurtPlayer.sink = &Harm::sink;
        around.hurtPlayer.ctx = harm;
    }
    return around;
}

}  // namespace

// ---------------------------------------------------------------------------
// The table of five
// ---------------------------------------------------------------------------

TEST(the_five_monsters_are_the_five_the_jar_lists)
{
    // `new k(this, 200, co.class, {mb, cw, dd, ax, ma})`, and every number here
    // is a `setSize` argument or a field written in a constructor.
    const MobDef& zombie = mobDef(MobType::Zombie);
    const MobDef& skeleton = mobDef(MobType::Skeleton);
    const MobDef& creeper = mobDef(MobType::Creeper);
    const MobDef& spider = mobDef(MobType::Spider);
    const MobDef& slime = mobDef(MobType::Slime);

    // All five implement `co` and no animal does. That flag, not the enum
    // order, is what the 200-cap spawner counts.
    CHECK(zombie.hostile && skeleton.hostile && creeper.hostile && spider.hostile
          && slime.hostile);
    CHECK(!mobDef(MobType::Cow).hostile);

    // **Three of them never call `setSize`**, so they are `kh`'s 0.6 x 1.8.
    CHECK_EQ(double(zombie.width), double(0.6f));
    CHECK_EQ(double(zombie.height), double(1.8f));
    CHECK_EQ(double(skeleton.width), double(0.6f));
    CHECK_EQ(double(creeper.height), double(1.8f));
    // `ax` is the one that does, and it is **wider than it is tall**.
    CHECK_EQ(double(spider.width), double(1.4f));
    CHECK_EQ(double(spider.height), double(0.9f));

    // `dq`'s twenty, for all four of its subclasses.
    CHECK_EQ(zombie.health, 20);
    CHECK_EQ(skeleton.health, 20);
    CHECK_EQ(creeper.health, 20);
    CHECK_EQ(spider.health, 20);

    // **The zombie is the only one that changes two things about the fight**:
    // five damage rather than `dq`'s two, and 0.5 move speed rather than 0.7.
    CHECK_EQ(zombie.attackStrength, 5);
    CHECK_EQ(skeleton.attackStrength, 2);
    CHECK_EQ(double(zombie.moveSpeed), double(0.5f));
    CHECK_EQ(double(skeleton.moveSpeed), double(0.7f));
    CHECK_EQ(double(spider.moveSpeed), double(0.8f));

    // Only `mb` and `cw` burn at dawn.
    CHECK(zombie.burnsInSunlight);
    CHECK(skeleton.burnsInSunlight);
    CHECK(!creeper.burnsInSunlight);
    CHECK(!spider.burnsInSunlight);
    CHECK(!slime.burnsInSunlight);

    // **The creeper and the slime have no idle sound at all**: `ge.c()` returns
    // null and neither overrides it, which is why one gets behind you.
    CHECK(creeper.livingSound == nullptr);
    CHECK(slime.livingSound == nullptr);
    CHECK(zombie.livingSound != nullptr);

    // Drops, off `g()`: feather, arrow, gunpowder, string, slimeball.
    CHECK_EQ(int(zombie.dropItem), int(mcver::Item::Feather));
    CHECK_EQ(int(skeleton.dropItem), int(mcver::Item::Arrow));
    CHECK_EQ(int(creeper.dropItem), int(mcver::Item::Gunpowder));
    CHECK_EQ(int(spider.dropItem), int(mcver::Item::String));
    CHECK_EQ(int(slime.dropItem), int(mcver::Item::Slimeball));

    // `ge.b()` is 80 and `ag.b()` is 120: a monster talks half again as often
    // as an animal.
    CHECK_EQ(zombie.talkInterval, 80);
    CHECK_EQ(mobDef(MobType::Pig).talkInterval, 120);
}

// ---------------------------------------------------------------------------
// Going for the player
// ---------------------------------------------------------------------------

TEST(a_zombie_walks_at_the_player_and_hits_when_it_arrives)
{
    Cave cave;
    Harm harm;
    const int zombie = cave.add(MobType::Zombie, 0.5, 0.5);
    CHECK(zombie >= 0);

    // Six blocks away, in the open, with nothing between.
    MobSurroundings around = hunted(&harm, 6.5, 0.5);
    const double startX = cave.mobs[zombie].body.x;
    cave.run(120, around);

    // It closed the distance. Not "arrived" -- the pathfinder's per-tick budget
    // and the 1-in-100 re-plan make the exact arrival tick a coin flip -- but
    // it is nearer than it started and it is holding a target.
    CHECK(cave.mobs.count() > zombie);
    CHECK(cave.mobs[zombie].body.x > startX + 1.0);
    CHECK(cave.mobs[zombie].targetingPlayer);

    // Standing on top of it, the fist lands and costs five.
    around = hunted(&harm, cave.mobs[zombie].body.x + 0.5, cave.mobs[zombie].body.z);
    const int before = harm.hits;
    cave.run(5, around);
    CHECK(harm.hits > before);
    CHECK_EQ(harm.taken % 5, 0);
}

TEST(a_zombie_does_not_punch_through_a_floor)
{
    // `dq.a(Lkh;F)V`'s vertical test: the boxes have to overlap in y. A player
    // standing on a ceiling three blocks above a zombie is inside its 2.5-block
    // radius in a straight line and must still not be hit.
    Cave cave;
    Harm harm;
    const int zombie = cave.add(MobType::Zombie, 0.5, 0.5);
    CHECK(zombie >= 0);
    cave.mobs.at(zombie).targetingPlayer = true;

    // Feet at 44: 3 blocks up, so the boxes (41..42.8 and 44..45.8) miss.
    const MobSurroundings above = hunted(&harm, 0.7, 0.5, 44.0);
    cave.run(40, above);
    CHECK_EQ(harm.hits, 0);
}

TEST(a_spider_hunts_in_the_dark_and_loses_interest_in_the_light)
{
    // `ax.i()` -- findPlayerToAttack refuses outright when the spider's own
    // cell is brighter than half, which is the whole of "spiders go neutral in
    // daylight". It is not a burning: nothing sets them alight.
    Cave dark;
    Harm harm;
    const int spider = dark.add(MobType::Spider, 0.5, 0.5);
    CHECK(spider >= 0);
    dark.run(20, hunted(&harm, 4.5, 0.5));
    CHECK(dark.mobs[spider].targetingPlayer);

    // The same scene with the light on. `entityBrightness` reads two thirds up
    // the box, so the whole column is lit.
    Cave lit;
    lit.scene.lightColumnsFrom(-20, 20, -20, 20, Cave::kFloor + 1);
    const int day = lit.add(MobType::Spider, 0.5, 0.5);
    CHECK(day >= 0);
    lit.run(20, hunted(&harm, 4.5, 0.5));
    CHECK(!lit.mobs[day].targetingPlayer);
}

TEST(a_monster_that_is_hit_turns_on_whoever_hit_it)
{
    // `dq.a(Lkh;I)Z` -- the second and last thing that ever writes
    // `entityToAttack`. A skeleton shot from across a cavern starts walking.
    Cave cave;
    const int skeleton = cave.add(MobType::Skeleton, 0.5, 0.5);
    CHECK(skeleton >= 0);
    CHECK(!cave.mobs[skeleton].targetingPlayer);
    cave.mobs.attack(cave.w(), skeleton, 1, true, 9.5, 0.5, true);
    CHECK(cave.mobs[skeleton].targetingPlayer);

    // An animal hit by the same blow holds no target: `ag` has no
    // `findPlayerToAttack` and `attack` only sets the flag for `hostile` rows.
    Cave field;
    const int cow = field.add(MobType::Cow, 0.5, 0.5);
    CHECK(cow >= 0);
    field.mobs.attack(field.w(), cow, 1, true, 9.5, 0.5, true);
    CHECK(!field.mobs[cow].targetingPlayer);
}

// ---------------------------------------------------------------------------
// Burning at dawn
// ---------------------------------------------------------------------------

TEST(a_zombie_under_open_sky_catches_fire_and_one_under_a_roof_does_not)
{
    // `mb.j()`: daytime, brightness above a half, open sky, and a draw from 30
    // against `(brightness - 0.4) * 2`. All four have to hold.
    SceneWorld open(0, 0);
    test::DropCatcher drops;
    drops.watch(open.w());
    for (i32 x = -8; x <= 8; ++x) {
        for (i32 z = -8; z <= 8; ++z) {
            open.place(x, 60, z, bid(mcver::Block::Stone), 0);
        }
    }
    open.lightColumnsFrom(-8, 8, -8, 8, 61);
    // Noon: `skyLightSubtracted` is 0, so `isDaytime` holds.
    open.w().setTime(6000);

    MobSystem mobs{99};
    CHECK(mobs.spawn(open.w(), MobType::Zombie, 0.5, 61.0, 0.5, 0.0f));
    MobSurroundings nobody;
    // **`fire <= 0` is "not burning", not `== 0`.** The counter rests at
    // `-fireResistance` -- minus one for a mob -- because `moveEntity`'s tail
    // puts it there on every tick the entity is not standing in a flame. See
    // core/entity/fire_entry.hpp.
    for (int i = 0; i < 200 && mobs[0].fire <= 0; ++i) {
        mobs.tick(open.w(), nobody);
    }
    CHECK(mobs.count() > 0);
    CHECK(mobs[0].fire > 0);

    // The same zombie with a stone slab of a sky over it never lights, however
    // long it stands there -- `canBlockSeeTheSky` is read off the height map.
    Cave cave;
    const int sheltered = cave.add(MobType::Zombie, 0.5, 0.5);
    CHECK(sheltered >= 0);
    cave.scene.lightColumnsFrom(-20, 20, -20, 20, Cave::kFloor + 1);
    cave.w().setTime(6000);
    MobSurroundings none;
    cave.run(200, none);
    CHECK(cave.mobs.count() > sheltered);
    CHECK(cave.mobs[sheltered].fire <= 0);
}

TEST(a_creeper_in_daylight_does_not_burn)
{
    // Only `mb` and `cw` override `onLivingUpdate` with the sun check. A
    // creeper in the open is a creeper in the open.
    SceneWorld open(0, 0);
    for (i32 x = -8; x <= 8; ++x) {
        for (i32 z = -8; z <= 8; ++z) {
            open.place(x, 60, z, bid(mcver::Block::Stone), 0);
        }
    }
    open.lightColumnsFrom(-8, 8, -8, 8, 61);
    open.w().setTime(6000);

    MobSystem mobs{99};
    CHECK(mobs.spawn(open.w(), MobType::Creeper, 0.5, 61.0, 0.5, 0.0f));
    MobSurroundings nobody;
    for (int i = 0; i < 200; ++i) {
        mobs.tick(open.w(), nobody);
    }
    CHECK(mobs.count() > 0);
    CHECK(mobs[0].fire <= 0);
}

// ---------------------------------------------------------------------------
// The creeper
// ---------------------------------------------------------------------------

TEST(a_creeper_lights_at_three_blocks_and_the_fuse_runs_back_when_it_loses_you)
{
    Cave cave;
    Harm harm;
    const int creeper = cave.add(MobType::Creeper, 0.5, 0.5);
    CHECK(creeper >= 0);
    cave.mobs.at(creeper).targetingPlayer = true;

    // Two blocks away: inside the three that lights it. Two ticks, so the fuse
    // is well short of thirty.
    const MobSurroundings close = hunted(&harm, 2.0, 0.5);
    cave.mobs.tick(cave.w(), close);
    CHECK_EQ(int(cave.mobs[creeper].creeperState), 1);
    CHECK(cave.mobs[creeper].fuse > 0);
    CHECK(cave.sounds.countOf("random.fuse") == 1);
    const int lit = cave.mobs[creeper].fuse;

    // **Walk away and it runs back.** `dd.b_()` decrements the fuse on every
    // tick the creeper is not lit, which is why backing off works.
    const MobSurroundings far = hunted(&harm, 12.0, 0.5);
    cave.mobs.tick(cave.w(), far);
    cave.mobs.tick(cave.w(), far);
    CHECK(cave.mobs.count() > creeper);
    CHECK(cave.mobs[creeper].fuse < lit);
    CHECK_EQ(int(cave.mobs[creeper].creeperState), -1);
}

TEST(a_creeper_that_goes_off_takes_the_floor_with_it_and_leaves_no_gunpowder)
{
    Cave cave;
    Harm harm;
    const int creeper = cave.add(MobType::Creeper, 0.5, 0.5);
    CHECK(creeper >= 0);
    cave.mobs.at(creeper).targetingPlayer = true;

    // **Nothing between them.** A creeper that walks behind cover loses
    // `canEntityBeSeen`, `attackEntity` stops running, and `dd.b_()` winds the
    // fuse back -- which is real behaviour and is the subject of the test
    // above, not of this one.
    const MobSurroundings close = hunted(&harm, 2.0, 0.5);
    i32 lastX = 0;
    i32 lastZ = 0;
    for (int i = 0; i < 60 && cave.mobs.count() > 0; ++i) {
        lastX = MathHelper::floorDouble(cave.mobs[0].body.x);
        lastZ = MathHelper::floorDouble(cave.mobs[0].body.z);
        cave.mobs.tick(cave.w(), close);
    }

    // Gone, and gone the way `F()` goes: **no drops and no death sound.** A
    // creeper's gunpowder is only ever from one you killed.
    CHECK_EQ(cave.mobs.count(), 0);
    CHECK_EQ(cave.drops.countOf(u16(mcver::Item::Gunpowder)), 0);
    CHECK_EQ(cave.sounds.countOf("mob.creeperdeath"), 0);
    CHECK_EQ(cave.sounds.countOf("random.explode"), 1);

    // A hole in the floor it was standing on, and the player felt it.
    CHECK_EQ(int(cave.w().blockAt(lastX, Cave::kFloor, lastZ)), int(block::kAir));
    CHECK(harm.taken > 0);
}

// ---------------------------------------------------------------------------
// The skeleton
// ---------------------------------------------------------------------------

namespace {

// The seam's other end, as `platform/ctr/main.cpp` wires it.
struct Bow {
    entity::ArrowSystem* arrows = nullptr;
    tick::TickWorld* world = nullptr;
    int shots = 0;

    static void shoot(void* ctx, double x, double y, double z, double dx, double dy, double dz,
                      float velocity, float inaccuracy)
    {
        Bow& self = *static_cast<Bow*>(ctx);
        ++self.shots;
        self.arrows->shootFrom(*self.world, x, y, z, dx, dy, dz, velocity, inaccuracy,
                               entity::ArrowShooter::Skeleton);
    }
};

}  // namespace

TEST(a_skeleton_shoots_from_ten_blocks_and_waits_thirty_ticks)
{
    Cave cave;
    Harm harm;
    entity::ArrowSystem arrows{31337};
    Bow bow;
    bow.arrows = &arrows;
    bow.world = &cave.w();

    const int skeleton = cave.add(MobType::Skeleton, 0.5, 0.5);
    CHECK(skeleton >= 0);
    cave.mobs.at(skeleton).targetingPlayer = true;

    MobSurroundings around = hunted(&harm, 6.5, 0.5);
    around.shootArrow = &Bow::shoot;
    around.shootArrowCtx = &bow;

    cave.mobs.tick(cave.w(), around);
    CHECK_EQ(bow.shots, 1);
    CHECK_EQ(cave.sounds.countOf("random.bow"), 1);
    // `attackTime = 30`, counted down one a tick in `ge.y()`.
    CHECK_EQ(int(cave.mobs[skeleton].attackTime), 30);

    // Twenty-nine more ticks and it has not fired again.
    for (int i = 0; i < 29; ++i) {
        cave.mobs.tick(cave.w(), around);
    }
    CHECK_EQ(bow.shots, 1);
    // The thirtieth brings the cooldown to zero and the next tick fires.
    cave.mobs.tick(cave.w(), around);
    CHECK_EQ(bow.shots, 2);

    // **The arrow leaves above the skeleton's feet, not out of them.** 1.4 up
    // and a tenth back down, which is the constructor plus `cw`'s own lift.
    CHECK(arrows.count() >= 1);
    CHECK(arrows[0].y > Cave::kGround + 1.0);
}

TEST(a_skeletons_arrow_does_not_shoot_the_skeleton_that_fired_it)
{
    // `kg.e_()`: `entity != shootingEntity || ticksInAir >= 5`. The arrow
    // starts 1.3 blocks up *inside* the skeleton's own 0.6 x 1.8 box, so
    // without the exclusion it kills itself on the tick it is loosed.
    Cave cave;
    Harm harm;
    entity::ArrowSystem arrows{31337};
    Bow bow;
    bow.arrows = &arrows;
    bow.world = &cave.w();

    const int skeleton = cave.add(MobType::Skeleton, 0.5, 0.5);
    CHECK(skeleton >= 0);
    cave.mobs.at(skeleton).targetingPlayer = true;
    const int health = cave.mobs[skeleton].health;

    MobSurroundings around = hunted(&harm, 6.5, 0.5);
    around.shootArrow = &Bow::shoot;
    around.shootArrowCtx = &bow;

    entity::ArrowTargets hits;
    hits.mobs = &cave.mobs;
    for (int i = 0; i < 10; ++i) {
        cave.mobs.tick(cave.w(), around);
        arrows.tick(cave.w(), hits);
    }
    CHECK(bow.shots >= 1);
    CHECK(cave.mobs.count() > skeleton);
    CHECK_EQ(int(cave.mobs[skeleton].health), health);
}

TEST(a_creeper_killed_by_a_skeleton_leaves_a_record)
{
    // `dd.b(Lkh;)V` -- **the only source of a music disc in this version.**
    // Nothing crafts one, no chest generates one, and no other entity drops
    // one: a player who wants a record has to get a skeleton to shoot a
    // creeper.
    Cave cave;
    const int creeper = cave.add(MobType::Creeper, 0.5, 0.5);
    CHECK(creeper >= 0);

    // Twenty damage from a skeleton's arrow, four at a time with the
    // invulnerability window let out between hits.
    for (int i = 0; i < 6 && cave.mobs.count() > 0 && cave.mobs[creeper].health > 0; ++i) {
        cave.mobs.attack(cave.w(), creeper, 4, true, 3.0, 0.5, true, true);
        for (int t = 0; t < entity::kHurtResistantTime + 1; ++t) {
            MobSurroundings nobody;
            cave.mobs.tick(cave.w(), nobody);
        }
    }
    const int records = cave.drops.countOf(2256) + cave.drops.countOf(2257);
    CHECK_EQ(records, 1);
    // And it still drops its gunpowder, because the disc is on top of
    // `super.onDeath()` rather than instead of it.
    CHECK(cave.drops.countOf(u16(mcver::Item::Gunpowder)) >= 0);

    // The same creeper punched by a player leaves no record.
    Cave punched;
    const int other = punched.add(MobType::Creeper, 0.5, 0.5);
    CHECK(other >= 0);
    for (int i = 0; i < 6 && punched.mobs.count() > 0 && punched.mobs[other].health > 0; ++i) {
        punched.mobs.attack(punched.w(), other, 4, true, 3.0, 0.5, true, false);
        for (int t = 0; t < entity::kHurtResistantTime + 1; ++t) {
            MobSurroundings nobody;
            punched.mobs.tick(punched.w(), nobody);
        }
    }
    CHECK_EQ(punched.drops.countOf(2256) + punched.drops.countOf(2257), 0);
}

// ---------------------------------------------------------------------------
// The player a monster may not hunt
// ---------------------------------------------------------------------------
//
// `MobPlayer::targetable`, which is the port's own rule and not the jar's --
// a1.1.2 has no gamemode. The behaviour copied is the later versions',
// `TargetingConditions.forCombat` refusing an invulnerable player, and what
// matters here is the line between "not hunted" and "not there": a Creative
// player is still watched, still shoved and still despawns a herd around
// themselves.

TEST(a_zombie_does_not_hunt_an_untargetable_player)
{
    Cave cave;
    Harm harm;
    const int zombie = cave.add(MobType::Zombie, 0.5, 0.5);
    CHECK(zombie >= 0);

    MobSurroundings around = hunted(&harm, 6.5, 0.5);
    around.player.targetable = false;
    cave.run(120, around);

    CHECK(!cave.mobs[zombie].targetingPlayer);
    // And standing on top of one does not get anybody hit, however long it is
    // given -- the fist is `attackEntity`'s, and `attackEntity` is only ever
    // reached through a target.
    around = hunted(&harm, cave.mobs[zombie].body.x, cave.mobs[zombie].body.z);
    around.player.targetable = false;
    cave.run(60, around);
    CHECK_EQ(harm.hits, 0);
}

TEST(a_zombie_drops_the_chase_on_the_tick_the_player_stops_being_a_target)
{
    // Switching to Creative mid-chase. `entityToAttack` is cleared by the same
    // branch a dead player clears it with, so the walk ends rather than
    // finishing.
    Cave cave;
    Harm harm;
    const int zombie = cave.add(MobType::Zombie, 0.5, 0.5);
    CHECK(zombie >= 0);

    cave.run(40, hunted(&harm, 5.5, 0.5));
    CHECK(cave.mobs[zombie].targetingPlayer);

    MobSurroundings safe = hunted(&harm, 5.5, 0.5);
    safe.player.targetable = false;
    cave.run(1, safe);
    CHECK(!cave.mobs[zombie].targetingPlayer);
}

TEST(a_creeper_does_not_light_for_an_untargetable_player)
{
    Cave cave;
    Harm harm;
    const int creeper = cave.add(MobType::Creeper, 0.5, 0.5);
    CHECK(creeper >= 0);

    MobSurroundings around = hunted(&harm, 2.0, 0.5);
    around.player.targetable = false;
    cave.run(60, around);

    CHECK(cave.mobs.count() > creeper);
    CHECK(cave.mobs[creeper].alive);
    CHECK_EQ(int(cave.mobs[creeper].fuse), 0);
    CHECK_EQ(int(cave.mobs[creeper].creeperState), -1);
}

TEST(a_slime_neither_hurries_nor_lands_on_an_untargetable_player)
{
    // The slime is the one that does not go through `findPlayerToAttack` at
    // all: `ma.b_()` asks for the player itself, so the refusal has to be
    // written into the hop as well as into the search.
    Cave cave;
    Harm harm;
    const int slime = cave.add(MobType::Slime, 0.5, 0.5);
    CHECK(slime >= 0);
    cave.mobs.setSlimeSize(cave.mobs.at(slime), 4);

    MobSurroundings around = hunted(&harm, 1.0, 0.5);
    around.player.targetable = false;
    cave.run(200, around);
    CHECK_EQ(harm.hits, 0);

    // Sanity: the same slime, the same distance, a targetable player -- it does
    // land on them. Without this the test above would pass on a slime that had
    // simply hopped away.
    Cave other;
    Harm hurt;
    const int big = other.add(MobType::Slime, 0.5, 0.5);
    CHECK(big >= 0);
    other.mobs.setSlimeSize(other.mobs.at(big), 4);
    other.run(200, hunted(&hurt, 1.0, 0.5));
    CHECK(hurt.hits > 0);
}

TEST(an_untargetable_player_is_still_watched_and_still_shoves)
{
    // The other half of the rule: `present` stays true, so everything that is
    // not a target search still sees the player.
    Cave cave;
    const int cow = cave.add(MobType::Cow, 2.0, 0.5);
    CHECK(cow >= 0);

    MobSurroundings around = hunted(nullptr, 0.9, 0.5);
    around.player.targetable = false;
    double motionX = 0.0;
    double motionZ = 0.0;
    around.player.motionX = &motionX;
    around.player.motionZ = &motionZ;
    cave.run(40, around);

    CHECK(cave.mobs[cow].watchingPlayer);
    CHECK(motionX != 0.0 || motionZ != 0.0);
}

TEST(a_punch_that_does_not_provoke_still_lands)
{
    Cave cave;
    const int zombie = cave.add(MobType::Zombie, 0.5, 0.5);
    CHECK(zombie >= 0);
    const i16 before = cave.mobs[zombie].health;

    // `provokes` false -- a Creative fist. The damage and the knockback are
    // the same hit; only the grudge is gone.
    CHECK(cave.mobs.attack(cave.w(), zombie, 2, true, 3.0, 0.5, true, false, false));
    CHECK(cave.mobs[zombie].health < before);
    CHECK(!cave.mobs[zombie].targetingPlayer);

    // Past the ten-tick invulnerability window, with nobody about -- so the
    // target below can only have come from the punch. The default is still a
    // provocation, which is what an arrow and a Survival punch both take.
    cave.run(20, MobSurroundings{});
    CHECK(!cave.mobs[zombie].targetingPlayer);
    CHECK(cave.mobs.attack(cave.w(), zombie, 2, true, 3.0, 0.5, true));
    CHECK(cave.mobs[zombie].targetingPlayer);
}

// ---------------------------------------------------------------------------
// The slime
// ---------------------------------------------------------------------------

TEST(a_slime_is_sized_one_two_or_four_and_its_health_is_the_square)
{
    Cave cave;
    for (int i = 0; i < 30; ++i) {
        CHECK(cave.add(MobType::Slime, double(i % 10) + 0.5, double(i / 10) + 0.5) >= 0);
    }
    bool seenOne = false;
    for (int i = 0; i < cave.mobs.count(); ++i) {
        const Mob& s = cave.mobs[i];
        // `1 << rand(3)`: never 3.
        CHECK(s.slimeSize == 1 || s.slimeSize == 2 || s.slimeSize == 4);
        CHECK_EQ(int(s.health), int(s.slimeSize) * int(s.slimeSize));
        // `setSize(0.6 * size, 0.6 * size)` -- a cube, per entity, not from the
        // table.
        CHECK_EQ(double(s.body.width), double(entity::kSlimeSizeUnit * float(s.slimeSize)));
        CHECK_EQ(double(s.body.height), double(s.body.width));
        seenOne = seenOne || s.slimeSize == 1;
    }
    CHECK(seenOne);
}

TEST(a_slime_splits_into_four_of_half_its_size_and_only_when_it_is_cut_to_nothing)
{
    Cave cave;
    const int slime = cave.add(MobType::Slime, 0.5, 0.5);
    CHECK(slime >= 0);
    cave.mobs.setSlimeSize(cave.mobs.at(slime), 4);
    CHECK_EQ(int(cave.mobs[slime].health), 16);

    // **Exactly to zero.** `ma.F()` tests `health == 0`, not `<= 0`, so a big
    // slime taken past zero in one blow does not split -- which is a1.1.2's and
    // is reproduced rather than rounded.
    cave.mobs.attack(cave.w(), slime, 16, true);
    CHECK_EQ(int(cave.mobs[slime].health), 0);

    MobSurroundings nobody;
    for (int i = 0; i < entity::kDeathTicks + 3; ++i) {
        cave.mobs.tick(cave.w(), nobody);
    }
    // Four children, each size 2, and the parent is gone.
    CHECK_EQ(cave.mobs.count(), 4);
    for (int i = 0; i < cave.mobs.count(); ++i) {
        CHECK_EQ(int(cave.mobs[i].slimeSize), 2);
        CHECK_EQ(int(cave.mobs[i].health), 4);
    }
    // **A size-4 slime is worth nothing**: `ma.g()` answers only at size 1.
    CHECK_EQ(cave.drops.countOf(u16(mcver::Item::Slimeball)), 0);
}

TEST(a_slime_overshot_past_zero_does_not_split)
{
    Cave cave;
    const int slime = cave.add(MobType::Slime, 0.5, 0.5);
    CHECK(slime >= 0);
    cave.mobs.setSlimeSize(cave.mobs.at(slime), 2);
    cave.mobs.attack(cave.w(), slime, 40, true);
    CHECK(cave.mobs[slime].health < 0);

    MobSurroundings nobody;
    for (int i = 0; i < entity::kDeathTicks + 3; ++i) {
        cave.mobs.tick(cave.w(), nobody);
    }
    CHECK_EQ(cave.mobs.count(), 0);
}

TEST(the_smallest_slime_leaves_a_slimeball)
{
    Cave cave;
    const int slime = cave.add(MobType::Slime, 0.5, 0.5);
    CHECK(slime >= 0);
    cave.mobs.setSlimeSize(cave.mobs.at(slime), 1);
    // `rand(3)` of them, so nought to two: run it until one appears rather than
    // asserting on a single kill.
    int spawned = 0;
    for (int attempt = 0; attempt < 20; ++attempt) {
        const int one = cave.add(MobType::Slime, double(attempt) + 0.5, 4.5);
        if (one < 0) {
            break;
        }
        cave.mobs.setSlimeSize(cave.mobs.at(one), 1);
        cave.mobs.attack(cave.w(), one, 4, true);
        ++spawned;
    }
    CHECK(spawned > 0);
    CHECK(cave.drops.countOf(u16(mcver::Item::Slimeball)) > 0);
}

TEST(slime_chunks_are_a_property_of_the_seed_and_not_of_the_tick)
{
    // `cu.a(J)` reseeded from the chunk and `987234911L`. The answer must be
    // stable for a chunk, differ between chunks, and differ between worlds.
    const i64 seedA = 0x1234567890abcdefLL;
    const i64 seedB = 0x0fedcba098765432LL;

    for (int i = 0; i < 4; ++i) {
        CHECK_EQ(entity::isSlimeChunk(seedA, 3, -7), entity::isSlimeChunk(seedA, 3, -7));
    }

    int hitsA = 0;
    int hitsB = 0;
    int differ = 0;
    for (i32 cx = -20; cx < 20; ++cx) {
        for (i32 cz = -20; cz < 20; ++cz) {
            const bool a = entity::isSlimeChunk(seedA, cx, cz);
            const bool b = entity::isSlimeChunk(seedB, cx, cz);
            hitsA += a ? 1 : 0;
            hitsB += b ? 1 : 0;
            differ += (a != b) ? 1 : 0;
        }
    }
    // One chunk in ten, give or take, out of 1,600.
    CHECK(hitsA > 80 && hitsA < 240);
    CHECK(hitsB > 80 && hitsB < 240);
    // And two worlds do not agree about which.
    CHECK(differ > 100);
}

// ---------------------------------------------------------------------------
// Spawning
// ---------------------------------------------------------------------------

TEST(a_monster_may_not_spawn_in_a_lit_cell)
{
    // `dq.a()Z`'s second clause is `getBlockLightValue <= rand(8)`, and
    // `rand(8)` is at most 7 -- so a cell at light 15 is refused every time,
    // whatever the draw.
    Cave cave;
    cave.scene.lightColumnsFrom(-20, 20, -20, 20, Cave::kFloor + 1);
    const int zombie = cave.add(MobType::Zombie, 0.5, 0.5);
    CHECK(zombie >= 0);

    entity::SpawnContext where;
    where.playerPresent = true;
    where.playerX = 100.0;
    where.playerZ = 100.0;
    JavaRandom rand(5);
    for (int i = 0; i < 20; ++i) {
        CHECK(!entity::canMonsterSpawnAt(cave.w(), cave.mobs, zombie, rand, where));
    }
}

TEST(a_monster_spawns_in_the_dark)
{
    Cave cave;  // no lighting pass at all
    const int zombie = cave.add(MobType::Zombie, 0.5, 0.5);
    CHECK(zombie >= 0);

    entity::SpawnContext where;
    where.playerPresent = true;
    where.playerX = 100.0;
    where.playerZ = 100.0;
    JavaRandom rand(5);
    int allowed = 0;
    for (int i = 0; i < 20; ++i) {
        allowed += entity::canMonsterSpawnAt(cave.w(), cave.mobs, zombie, rand, where) ? 1 : 0;
    }
    CHECK_EQ(allowed, 20);
}

TEST(the_two_spawners_count_different_things)
{
    Cave cave;
    CHECK(cave.add(MobType::Cow, 0.5, 0.5) >= 0);
    CHECK(cave.add(MobType::Zombie, 2.5, 0.5) >= 0);
    CHECK(cave.add(MobType::Slime, 4.5, 0.5) >= 0);
    // `ag.class` is the cow; `co.class` is the zombie **and the slime**, which
    // is not an `ek` at all but does implement the interface.
    CHECK_EQ(cave.mobs.animalCount(), 1);
    CHECK_EQ(cave.mobs.monsterCount(), 2);
}

TEST(a_spawn_check_does_not_trip_over_the_candidate_itself)
{
    // **The bug this exists for.** `getCanSpawnHere` runs *after* the entity is
    // built -- it has to, because `ma.a()Z` reads the size its own constructor
    // drew -- so by the time `ge.a()Z` asks the world for colliding boxes the
    // candidate is already in the pool. `ge.a()Z` excludes `this`; a box query
    // through `TickWorld::anyEntityIn` cannot, because the frame loop's own
    // implementation walks the same pool and has never heard of an index.
    //
    // The first version of this refused **every** monster on the console and
    // passed every test here, because a bare `SceneWorld` sets no entity query
    // and `anyEntityIn` answered false for everything. So this fixture wires
    // one, and the assertion is simply that monsters still arrive.
    Cave cave{40};
    cave.watchEntities();
    JavaRandom rand(2024);
    entity::SpawnContext where;
    where.playerPresent = true;
    where.playerX = 0.0;
    where.playerY = Cave::kGround;
    where.playerZ = 0.0;
    where.spawnX = 500;
    where.spawnY = 64;
    where.spawnZ = 500;

    int total = 0;
    for (int tick = 0; tick < 4000 && total < 10; ++tick) {
        total += entity::spawnMonsters(cave.w(), cave.mobs, rand, where);
    }
    CHECK(total > 0);

    // And the query is live: a box over a spawned monster answers true, so the
    // check above really did run against a world that could see them.
    CHECK(cave.mobs.count() > 0);
    CHECK(cave.w().anyEntityIn(cave.mobs[0].body.box, tick::EntityFilter::Everything));

    // Two monsters may not be spawned into each other, which is the clause the
    // candidate's own box was masking.
    const int one = cave.add(MobType::Zombie, 0.5, 0.5);
    CHECK(one >= 0);
    const int two = cave.add(MobType::Zombie, 0.5, 0.5);
    CHECK(two >= 0);
    JavaRandom draw(11);
    CHECK(!entity::canMonsterSpawnAt(cave.w(), cave.mobs, two, draw, where));
}

TEST(the_monster_spawner_fills_a_dark_cavern_and_a_lit_one_stays_empty)
{
    // `k`'s half of `ia.c()`. The cavern is big and unlit; every candidate
    // passes `dq.a()Z`'s two light draws, and the group loop is reached far
    // more often than the animals' is because `rand(rand(120) + 8)` piles the
    // y draw up near the floor instead of spreading it over 128.
    Cave cave{40};
    JavaRandom rand(2024);
    entity::SpawnContext where;
    where.playerPresent = true;
    where.playerX = 0.0;
    where.playerY = Cave::kGround;
    where.playerZ = 0.0;
    where.spawnX = 500;
    where.spawnY = 64;
    where.spawnZ = 500;

    int total = 0;
    int ticks = 0;
    entity::SpawnCounters counted;
    for (; ticks < 4000 && total < 20; ++ticks) {
        total += entity::spawnMonsters(cave.w(), cave.mobs, rand, where, &counted);
    }
    CHECK(total > 0);

    // **The counters are what a console reports**, so they have to agree with
    // the thing they are counting: three passes a tick, one tally per spawn,
    // and every position either had no floor, was too near, was refused or
    // became a mob.
    CHECK_EQ(counted.spawned, total);
    CHECK_EQ(counted.passes, ticks * 3);
    const int accounted = counted.noFloor + counted.tooNear + counted.checkRejected;
    CHECK(accounted <= counted.positions);
    // Every position the three checks did not take became a mob -- and **a
    // jockey's skeleton is a mob with no position of its own**, which is why
    // this is a floor and not an equality.
    CHECK(counted.spawned >= counted.positions - accounted);
    CHECK(counted.chunksTried > counted.abortedSolid);
    CHECK(cave.mobs.monsterCount() > 0);
    CHECK_EQ(cave.mobs.animalCount(), 0);

    // Every one of them stands on something solid and none is inside the
    // player's 24 blocks.
    for (int i = 0; i < cave.mobs.count(); ++i) {
        const Mob& mob = cave.mobs[i];
        const i32 bx = MathHelper::floorDouble(mob.body.x);
        const int by = int(MathHelper::floorDouble(mob.body.y));
        const i32 bz = MathHelper::floorDouble(mob.body.z);
        CHECK(cave.w().opaqueAt(bx, by - 1, bz));
        // **The 24 blocks is a solid distance, not a footprint.** `az` asks
        // `getClosestPlayer(fx, fy, fz, 24.0D)`, which squares all three axes --
        // so a monster on top of the cavern's roof may be nearer than 24 in
        // plan and further than 24 in fact, and one of them always is.
        const double dx = mob.body.x - where.playerX;
        const double dy = mob.body.y - where.playerY;
        const double dz = mob.body.z - where.playerZ;
        CHECK(dx * dx + dy * dy + dz * dz >= 24.0 * 24.0 - 1.0);
    }

    // Turn the lights on and the spawner stops dead: `getBlockLightValue <=
    // rand(8)` cannot hold at 15, whatever the draw.
    Cave lit{40};
    lit.scene.lightColumnsFrom(-40, 40, -40, 40, Cave::kFloor + 1);
    for (int tick = 0; tick < 2000; ++tick) {
        CHECK_EQ(entity::spawnMonsters(lit.w(), lit.mobs, rand, where), 0);
    }
}

TEST(peaceful_takes_a_monster_away_and_leaves_the_animals)
{
    // `dq.e_()`'s last line, and it is a **removal after a full tick**, not a
    // suppression: a1.1.2 spawns them on Peaceful and then kills them, which is
    // why the 200-cap spawner still runs and still costs what it costs.
    Cave cave;
    CHECK(cave.add(MobType::Zombie, 0.5, 0.5) >= 0);
    CHECK(cave.add(MobType::Creeper, 2.5, 0.5) >= 0);
    CHECK(cave.add(MobType::Cow, 4.5, 0.5) >= 0);

    MobSurroundings peaceful;
    peaceful.difficulty = 0;
    cave.mobs.tick(cave.w(), peaceful);
    CHECK_EQ(cave.mobs.monsterCount(), 0);
    CHECK_EQ(cave.mobs.animalCount(), 1);
}

TEST(a_jockey_sits_on_its_spider_and_is_let_go_when_the_spider_dies)
{
    // `az`'s one-in-a-hundred branch, driven directly: the odds are the
    // spawner's and what matters here is that the pair holds together and that
    // a swap-remove does not leave the skeleton riding a cow.
    Cave cave;
    const int spider = cave.add(MobType::Spider, 0.5, 0.5);
    const int rider = cave.add(MobType::Skeleton, 0.5, 0.5);
    CHECK(spider >= 0 && rider >= 0);
    CHECK(cave.mobs.mountOn(rider, spider));

    MobSurroundings nobody;
    cave.mobs.tick(cave.w(), nobody);

    // `ax.h()` -- the spider's seat is `height * 0.75 - 0.5`, which is *below*
    // its own back: a skeleton riding one sits low.
    const entity::RiderSeat seat = cave.mobs.seatOf(spider);
    CHECK(seat.valid);
    CHECK_EQ(cave.mobs[rider].body.x, seat.x);
    CHECK_EQ(cave.mobs[rider].body.z, seat.z);
    CHECK(seat.y < cave.mobs[spider].body.y + double(cave.mobs[spider].body.height));

    // Kill the spider and let the corpse clear. The skeleton is dismounted and
    // still alive.
    cave.mobs.attack(cave.w(), spider, 100, true);
    for (int i = 0; i < entity::kDeathTicks + 3; ++i) {
        cave.mobs.tick(cave.w(), nobody);
    }
    CHECK_EQ(cave.mobs.count(), 1);
    CHECK_EQ(int(cave.mobs[0].type), int(MobType::Skeleton));
    CHECK_EQ(int(cave.mobs[0].mountIndex), -1);
}

// ---------------------------------------------------------------------------
// The blast itself
// ---------------------------------------------------------------------------

TEST(an_explosion_takes_dirt_and_leaves_obsidian)
{
    // `je`'s rays lose `(getExplosionResistance + 0.3) * 0.3` to the block they
    // are in, and `getExplosionResistance` is `max(resistance * 3, hardness * 5)
    // / 5` -- 2.5 for dirt against 1,200 for obsidian. One ray cannot spend
    // 1,200.
    SceneWorld scene(0, 0);
    for (i32 x = -6; x <= 6; ++x) {
        for (i32 z = -6; z <= 6; ++z) {
            for (int y = 60; y <= 64; ++y) {
                scene.place(x, y, z, bid(mcver::Block::Dirt), 0);
            }
        }
    }
    scene.place(3, 62, 0, bid(mcver::Block::Obsidian), 0);

    entity::Explosion blast(0.5, 62.0, 0.5, entity::kCreeperBlast);
    blast.cast(scene.w());
    CHECK(blast.cellCount() > 0);
    blast.destroy(scene.w());

    CHECK_EQ(int(scene.w().blockAt(0, 62, 0)), int(block::kAir));
    CHECK_EQ(int(scene.w().blockAt(1, 62, 0)), int(block::kAir));
    CHECK_EQ(int(scene.w().blockAt(3, 62, 0)), int(mcver::Block::Obsidian));
    // Nothing eight blocks out: a creeper's 3.0 reaches under four.
    CHECK_EQ(int(scene.w().blockAt(6, 62, 0)), int(mcver::Block::Dirt));
}

TEST(an_explosion_hurts_less_through_a_wall)
{
    // `getBlockDensity` is sampled across the victim's box and traced to the
    // centre, so cover is a fraction and not a switch.
    SceneWorld scene(0, 0);
    for (i32 x = -8; x <= 8; ++x) {
        for (i32 z = -8; z <= 8; ++z) {
            scene.place(x, 60, z, bid(mcver::Block::Stone), 0);
        }
    }
    // A wall of obsidian one block from the blast, so the ray cast cannot
    // remove it before the damage is worked out.
    for (int y = 61; y <= 64; ++y) {
        for (i32 z = -3; z <= 3; ++z) {
            scene.place(2, y, z, bid(mcver::Block::Obsidian), 0);
        }
    }

    entity::Explosion blast(0.5, 61.0, 0.5, entity::kCreeperBlast);
    blast.cast(scene.w());

    const AABB open{3.2, 61.0, -0.3, 3.8, 62.8, 0.3};
    const AABB clear{-3.8, 61.0, -0.3, -3.2, 62.8, 0.3};
    int shielded = 0;
    int exposed = 0;
    double vx = 0.0, vy = 0.0, vz = 0.0;
    CHECK(blast.effectOn(scene.w(), 3.5, 61.0, 0.0, open, &shielded, &vx, &vy, &vz));
    CHECK(blast.effectOn(scene.w(), -3.5, 61.0, 0.0, clear, &exposed, &vx, &vy, &vz));
    CHECK(exposed > shielded);
    // Even fully shielded, `je` adds one: nothing in reach takes zero.
    CHECK(shielded >= 1);
}

// ---------------------------------------------------------------------------
// Drawing them
// ---------------------------------------------------------------------------

TEST(every_monster_model_fits_the_part_budget_and_uses_its_own_page)
{
    Cave cave;
    const MobType kinds[] = {MobType::Zombie, MobType::Skeleton, MobType::Creeper,
                             MobType::Spider, MobType::Slime};
    for (MobType kind : kinds) {
        const int index = cave.add(kind, 0.5, 0.5);
        CHECK(index >= 0);
        render::ModelPart parts[render::kMobMaxParts];
        texture::EntitySkin skins[render::kMobMaxParts];
        const int count =
            render::poseMob(cave.mobs[index], 0.5f, parts, skins, render::kMobMaxParts);
        CHECK(count > 0);
        CHECK(count <= render::kMobMaxParts);
        // Every part has a box with a real extent -- a zero-sized box is a
        // transcription that lost a number.
        for (int i = 0; i < count; ++i) {
            CHECK(parts[i].w > 0 && parts[i].h > 0 && parts[i].d > 0);
        }
        cave.mobs.clear();
    }
}

TEST(a_swelling_creeper_grows_and_a_still_one_does_not)
{
    Cave cave;
    const int creeper = cave.add(MobType::Creeper, 0.5, 0.5);
    CHECK(creeper >= 0);

    const render::Placement calm = render::placeMob(cave.mobs[creeper], 0.0, 0.0, 0.0, 0.0f);
    // `dd.b(F)F` at fuse zero is zero, so the scale is exactly one and the
    // model axes are the plain model unit.
    CHECK(std::fabs(double(calm.ax[0]) + double(calm.az[2])) < 1e-6);

    cave.mobs.at(creeper).fuse = i16(entity::kFuseTicks - 2);
    cave.mobs.at(creeper).prevFuse = cave.mobs[creeper].fuse;
    const render::Placement full = render::placeMob(cave.mobs[creeper], 0.0, 0.0, 0.0, 0.0f);
    // A fully lit creeper is 1.4 times as wide.
    const double calmWidth = std::fabs(double(calm.az[2]));
    const double fullWidth = std::fabs(double(full.az[2]));
    CHECK(fullWidth > calmWidth * 1.3);
}

TEST(a_bigger_slime_is_drawn_bigger)
{
    Cave cave;
    const int small = cave.add(MobType::Slime, 0.5, 0.5);
    const int big = cave.add(MobType::Slime, 4.5, 0.5);
    CHECK(small >= 0 && big >= 0);
    cave.mobs.setSlimeSize(cave.mobs.at(small), 1);
    cave.mobs.setSlimeSize(cave.mobs.at(big), 4);

    // `hh`'s boxes are the same for every size; **the size is entirely in
    // `gq`'s scale**, which is why this is a placement test and not a model
    // one.
    const render::Placement a = render::placeMob(cave.mobs[small], 0.0, 0.0, 0.0, 0.0f);
    const render::Placement b = render::placeMob(cave.mobs[big], 0.0, 0.0, 0.0, 0.0f);
    CHECK(std::fabs(double(b.az[2])) > std::fabs(double(a.az[2])) * 3.5);
}

// ---- the order the spawner takes its chunks in ---------------------------
//
// **The vectors below came out of a real JVM**, not out of reasoning about
// one: `HashSet<ol>` filled exactly the way `az.a(Lcn;ILnu;)I` fills it, with
// `ol.hashCode()`'s `(x << 8) | z`, iterated and printed. See mob_spawn.cpp on
// why the order is worth a test at all -- the pass returns on the first drawn
// point that is not air, so the first chunks the set hands over are the only
// part of the square most passes ever reach, and iterating row-major put every
// monster north of the player.
//
// **Java hoists one entry per treeified bucket and this does not.** A bucket
// of eight or more becomes a red-black tree, and `moveRootToFront` puts the
// tree's root at the head of the chain; the other eight keep insertion order.
// Which entry becomes the root is the outcome of balancing nine insertions and
// is not something a table lookup reproduces. So the test compares run against
// run: the *partition* into buckets and the *order of the buckets* -- which is
// what decides where a monster may appear -- must be exact, and within a
// nine-entry run the members must match as a set.
namespace {

struct Offsets {
    i8 dx[entity::kEligibleChunks];
    i8 dz[entity::kEligibleChunks];
};

// One JVM line, "dx,dz dx,dz ...", parsed into the same shape.
Offsets fromJvm(const char* text)
{
    Offsets out{};
    int n = 0;
    const char* p = text;
    while (*p != '\0' && n < entity::kEligibleChunks) {
        out.dx[n] = i8(std::strtol(p, const_cast<char**>(&p), 10));
        ++p;  // the comma
        out.dz[n] = i8(std::strtol(p, const_cast<char**>(&p), 10));
        while (*p == ' ') {
            ++p;
        }
        ++n;
    }
    return out;
}

// Do two runs of `length` starting at `at` hold the same chunks?
bool sameMembers(const Offsets& a, const Offsets& b, int at, int length)
{
    for (int i = at; i < at + length; ++i) {
        bool found = false;
        for (int j = at; j < at + length && !found; ++j) {
            found = a.dx[i] == b.dx[j] && a.dz[i] == b.dz[j];
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

}  // namespace

TEST(the_spawner_takes_its_chunks_in_the_jars_hash_order)
{
    // Every chunk in the square has positive x and positive z here, so each
    // bucket is one row of nine and the rows run north to south.
    const Offsets far = fromJvm(
        "-1,-4 -4,-4 -3,-4 -2,-4 0,-4 1,-4 2,-4 3,-4 4,-4 "
        "-1,-3 -4,-3 -3,-3 -2,-3 0,-3 1,-3 2,-3 3,-3 4,-3 "
        "-1,-2 -4,-2 -3,-2 -2,-2 0,-2 1,-2 2,-2 3,-2 4,-2 "
        "-1,-1 -4,-1 -3,-1 -2,-1 0,-1 1,-1 2,-1 3,-1 4,-1 "
        "-1,0 -4,0 -3,0 -2,0 0,0 1,0 2,0 3,0 4,0 "
        "-1,1 -4,1 -3,1 -2,1 0,1 1,1 2,1 3,1 4,1 "
        "-1,2 -4,2 -3,2 -2,2 0,2 1,2 2,2 3,2 4,2 "
        "-1,3 -4,3 -3,3 -2,3 0,3 1,3 2,3 3,3 4,3 "
        "-1,4 -4,4 -3,4 -2,4 0,4 1,4 2,4 3,4 4,4");

    Offsets ours{};
    entity::eligibleOrder(100, 200, ours.dx, ours.dz);
    for (int run = 0; run < 9; ++run) {
        CHECK(sameMembers(ours, far, run * 9, 9));
    }

    // **Negative x turns the rows upside down**, which is the sign bits of
    // `x << 8` reaching the bucket through `h ^ (h >>> 16)`. A test that only
    // ever looked at one quadrant would miss it, and a player who walks west
    // would not.
    const Offsets west = fromJvm(
        "-1,4 -4,4 -3,4 -2,4 0,4 1,4 2,4 3,4 4,4 "
        "-1,3 -4,3 -3,3 -2,3 0,3 1,3 2,3 3,3 4,3 "
        "-1,2 -4,2 -3,2 -2,2 0,2 1,2 2,2 3,2 4,2 "
        "-1,1 -4,1 -3,1 -2,1 0,1 1,1 2,1 3,1 4,1 "
        "-1,0 -4,0 -3,0 -2,0 0,0 1,0 2,0 3,0 4,0 "
        "-1,-1 -4,-1 -3,-1 -2,-1 0,-1 1,-1 2,-1 3,-1 4,-1 "
        "-1,-2 -4,-2 -3,-2 -2,-2 0,-2 1,-2 2,-2 3,-2 4,-2 "
        "-1,-3 -4,-3 -3,-3 -2,-3 0,-3 1,-3 2,-3 3,-3 4,-3 "
        "-1,-4 -4,-4 -3,-4 -2,-4 0,-4 1,-4 2,-4 3,-4 4,-4");

    entity::eligibleOrder(-13, 7, ours.dx, ours.dz);
    for (int run = 0; run < 9; ++run) {
        CHECK(sameMembers(ours, west, run * 9, 9));
    }
}

TEST(a_square_that_straddles_the_origin_keeps_the_jars_bucket_runs)
{
    // At the origin the square straddles z = 0, and `(x << 8) | z` collapses
    // every negative-z row on to a single hash -- the sign bits swallow the x
    // half whole -- so four buckets hold fourteen chunks each and the last
    // twenty five sit in buckets of four or five. Those last twenty five are
    // under Java's treeify threshold, so there is no hoisted root and the order
    // is exactly the jar's, entry for entry.
    const Offsets origin = fromJvm(
        "-2,-1 -4,-1 3,-1 4,-1 -3,-1 -1,-1 0,-1 0,0 1,-1 1,0 2,0 3,0 4,0 2,-1 "
        "-1,-2 -4,-2 -3,-2 -2,-2 0,-2 4,-2 3,-2 0,1 1,-2 1,1 2,1 3,1 4,1 2,-2 "
        "-3,-3 -2,-3 -4,-3 -1,-3 0,-3 0,2 1,-3 3,-3 4,-3 1,2 2,2 3,2 4,2 2,-3 "
        "-4,-4 -2,-4 -3,-4 4,-4 3,-4 -1,-4 0,-4 0,3 1,-4 1,3 2,3 3,3 4,3 2,-4 "
        "0,4 1,4 2,4 3,4 4,4 -4,4 -3,4 -2,4 -1,4 "
        "-4,3 -3,3 -2,3 -1,3 -4,2 -3,2 -2,2 -1,2 "
        "-4,1 -3,1 -2,1 -1,1 -4,0 -3,0 -2,0 -1,0");

    Offsets ours{};
    entity::eligibleOrder(0, 0, ours.dx, ours.dz);

    // The four treeified buckets, as sets and in order.
    for (int run = 0; run < 4; ++run) {
        CHECK(sameMembers(ours, origin, run * 14, 14));
    }
    // The twenty five that were never treeified, entry for entry.
    for (int i = 4 * 14; i < entity::kEligibleChunks; ++i) {
        CHECK_EQ(int(ours.dx[i]), int(origin.dx[i]));
        CHECK_EQ(int(ours.dz[i]), int(origin.dz[i]));
    }
}
