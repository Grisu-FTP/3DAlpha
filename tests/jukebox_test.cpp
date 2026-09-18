// The jukebox, which is the one block in a1.1.2 that keeps an item without a
// tile entity: `cv` (BlockJukeBox) stores the disc as `1 + (item - record13)`
// in the cell's four metadata bits, so a jukebox round-trips through the save
// format with nothing but the block it is.
//
// The three halves of it, and they are in three different files because the
// original puts them there:
//
//   * **putting one in** is `lg.a(...)` -- the *item's* onItemUse, in
//     core/item/use.cpp -- and it only works on a jukebox whose metadata is 0;
//   * **taking one out by clicking** is `cv.a(Lcn;IIILdm;)Z` in
//     core/tick/behaviour.cpp, which answers false for an empty jukebox so that
//     the click falls through to the item above;
//   * **taking one out by breaking it** is `cv.a(Lcn;IIIIF)V`, an override of
//     `dropBlockAsItemWithChance`, so the eject happens in core/tick/drop.cpp
//     *before* the block's own drop and takes its three random draws first.

#include "core/block/registry.hpp"
#include "core/entity/ray_trace.hpp"
#include "core/item/registry.hpp"
#include "core/item/use.hpp"
#include "core/tick/behaviour.hpp"
#include "core/tick/drop.hpp"
#include "core/tick/tick_world.hpp"
#include "drop_catcher.hpp"
#include "framework.hpp"
#include "scene_world.hpp"

#include <string>
#include <vector>

using namespace mc;
using mc::block::BlockId;
using mc::entity::RayHit;
using mc::test::SceneWorld;
using mc::tick::TickWorld;

namespace {

constexpr item::ItemId kRecord13 = 2256;
constexpr item::ItemId kRecordCat = 2257;
constexpr item::ItemId kStoneItem = 1;

const AABB kNoPlayer{1000.0, 1000.0, 1000.0, 1000.6, 1001.8, 1000.6};

RayHit hitOn(i32 x, int y, i32 z, mesh::Face face)
{
    RayHit hit;
    hit.hit = true;
    hit.x = x;
    hit.y = y;
    hit.z = z;
    hit.face = face;
    return hit;
}

// Every call the world made to `playRecord`, in order. A null track is the
// stop, and it is recorded as one rather than dropped: the order of "stop" and
// "start" is half of what this seam is for.
struct RecordLog {
    struct Call {
        std::string track;
        bool stop = false;
        i32 x = 0;
        int y = 0;
        i32 z = 0;
    };
    std::vector<Call> calls;

    static void sink(void* ctx, const char* track, i32 x, int y, i32 z)
    {
        RecordLog* self = static_cast<RecordLog*>(ctx);
        self->calls.push_back(Call{track != nullptr ? std::string(track) : std::string(),
                                   track == nullptr, x, y, z});
    }

    void watch(TickWorld& world) { world.setRecordSink(&RecordLog::sink, this); }
};

struct Jukebox {
    SceneWorld scene{0, 0};
    mc::test::DropCatcher drops;
    RecordLog records;

    Jukebox()
    {
        drops.watch(scene.w());
        records.watch(scene.w());
        scene.place(0, 63, 0, BlockId(mcver::Block::Jukebox), 0);
    }

    TickWorld& w() { return scene.w(); }
    int md() { return int(scene.w().dataAt(0, 63, 0)); }

    bool click(item::ItemId held)
    {
        return item::rightClick(scene.w(), held, hitOn(0, 63, 0, mesh::kFacePosY), kNoPlayer,
                                0.0f);
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// The disc goes in
// ---------------------------------------------------------------------------

TEST(a_disc_used_on_an_empty_jukebox_goes_in_and_plays)
{
    Jukebox fixture;
    CHECK_EQ(fixture.md(), 0);

    CHECK(fixture.click(kRecord13));

    // `this.shiftedIndex - Item.record13.shiftedIndex + 1` -- record 13 is 1.
    CHECK_EQ(fixture.md(), 1);
    CHECK_EQ(fixture.records.calls.size(), usize(1));
    CHECK(!fixture.records.calls[0].stop);
    CHECK_EQ(fixture.records.calls[0].track, std::string("13"));
    CHECK_EQ(fixture.records.calls[0].y, 63);
    // The block is still a jukebox and nothing was dropped.
    CHECK_EQ(int(fixture.w().blockAt(0, 63, 0)), int(mcver::Block::Jukebox));
    CHECK_EQ(fixture.drops.total(), 0);
}

// The second disc is metadata 2, which is the whole of the arithmetic and the
// reason the table carries `kFirstRecordItem` rather than the two ids.
TEST(the_second_disc_is_the_second_metadata)
{
    Jukebox fixture;
    CHECK(fixture.click(kRecordCat));
    CHECK_EQ(fixture.md(), 2);
    CHECK_EQ(fixture.records.calls[0].track, std::string("cat"));

    CHECK_EQ(int(item::recordMetadata(kRecord13)), 1);
    CHECK_EQ(int(item::recordMetadata(kRecordCat)), 2);
    CHECK_EQ(item::recordItemFor(1), kRecord13);
    CHECK_EQ(item::recordItemFor(2), kRecordCat);
    // Not a disc, so not a metadata -- and metadata 0 is an empty jukebox.
    CHECK_EQ(int(item::recordMetadata(kStoneItem)), 0);
    CHECK_EQ(item::recordItemFor(0), item::ItemId(0));
}

// `lg.a`'s metadata test: a jukebox that is already playing refuses the disc,
// and the block's own activation has taken the click to eject instead -- so the
// two never both run.
TEST(a_second_disc_cannot_be_forced_into_a_playing_jukebox)
{
    Jukebox fixture;
    CHECK(fixture.click(kRecord13));
    fixture.records.calls.clear();

    // Clicking again with the other disc ejects the first rather than swapping.
    CHECK(fixture.click(kRecordCat));
    CHECK_EQ(fixture.md(), 0);
    CHECK_EQ(fixture.drops.countOf(u16(kRecord13)), 1);
    CHECK_EQ(fixture.drops.countOf(u16(kRecordCat)), 0);
}

// A disc used on anything else does nothing at all, and does not eat the click.
TEST(a_disc_used_on_anything_else_does_nothing)
{
    Jukebox fixture;
    fixture.scene.place(2, 63, 0, BlockId(mcver::Block::Stone), 0);
    CHECK(!item::rightClick(fixture.w(), kRecord13, hitOn(2, 63, 0, mesh::kFacePosY),
                            kNoPlayer, 0.0f));
    CHECK_EQ(fixture.records.calls.size(), usize(0));
}

// ---------------------------------------------------------------------------
// The disc comes out
// ---------------------------------------------------------------------------

TEST(clicking_a_playing_jukebox_gives_the_disc_back_and_stops_it)
{
    Jukebox fixture;
    CHECK(fixture.click(kRecord13));
    fixture.records.calls.clear();

    // An empty hand is enough: it is the *block* that answers.
    CHECK(fixture.click(0));
    CHECK_EQ(fixture.md(), 0);
    CHECK_EQ(int(fixture.w().blockAt(0, 63, 0)), int(mcver::Block::Jukebox));

    CHECK_EQ(fixture.records.calls.size(), usize(1));
    CHECK(fixture.records.calls[0].stop);

    CHECK_EQ(fixture.drops.drops.size(), usize(1));
    CHECK_EQ(int(fixture.drops.drops[0].item), int(kRecord13));
    CHECK_EQ(fixture.drops.drops[0].count, 1);
    // **Out of the top of the block, not its middle**: `dy` is the one of the
    // three offsets that is not `nextFloat() * 0.7 + 0.15`.
    CHECK(fixture.drops.drops[0].y >= 63.6);
    CHECK(fixture.drops.drops[0].y < 64.4);
    CHECK(fixture.drops.drops[0].x >= 63.0 - 63.0);
    CHECK(fixture.drops.drops[0].x < 1.0);
}

// An empty jukebox answers false, which is what lets the hand act on it. It
// must not eat the click and it must not stop anything.
TEST(clicking_an_empty_jukebox_does_not_take_the_click)
{
    Jukebox fixture;
    CHECK(!fixture.click(0));
    CHECK_EQ(fixture.records.calls.size(), usize(0));
    CHECK_EQ(fixture.drops.total(), 0);
}

// `cv.a(Lcn;IIIIF)V` -- breaking a jukebox that is playing hands back **both**
// the block and the disc, and the disc comes first.
TEST(breaking_a_playing_jukebox_drops_the_disc_and_the_block)
{
    Jukebox fixture;
    CHECK(fixture.click(kRecord13));
    fixture.records.calls.clear();

    const u8 metadata = u8(fixture.md());
    fixture.w().setBlockWithNotify(0, 63, 0, block::kAir);
    tick::dropBlockAsItem(fixture.w(), 0, 63, 0, BlockId(mcver::Block::Jukebox), metadata);

    CHECK_EQ(fixture.records.calls.size(), usize(1));
    CHECK(fixture.records.calls[0].stop);
    CHECK_EQ(fixture.drops.countOf(u16(kRecord13)), 1);
    CHECK_EQ(fixture.drops.countOf(u16(mcver::Block::Jukebox)), 1);
    // The disc is handed over before the block, which is the order `cv.a` runs
    // its own body and then `super`'s.
    CHECK_EQ(int(fixture.drops.drops[0].item), int(kRecord13));
}

// **The Creative break, which drops nothing at all** -- `item::destroyBlock` on
// its own, with no `dropBlockAsItem` after it. a1.1.2 has no such path and `cv`
// therefore puts its eject in the drop; this port has one, and a jukebox broken
// that way kept playing and swallowed the disc. Reported from play.
TEST(a_break_that_drops_nothing_still_gives_the_disc_back_and_stops_it)
{
    Jukebox fixture;
    CHECK(fixture.click(kRecord13));
    fixture.records.calls.clear();

    // No harvest, no drop table -- just the block going, which is what the
    // Creative left click is.
    CHECK(item::destroyBlock(fixture.w(), 0, 63, 0));
    CHECK_EQ(int(fixture.w().blockAt(0, 63, 0)), 0);

    CHECK_EQ(fixture.records.calls.size(), usize(1));
    CHECK(fixture.records.calls[0].stop);
    CHECK_EQ(fixture.drops.countOf(u16(kRecord13)), 1);
    // The block itself is not dropped, which is the whole point of that path.
    CHECK_EQ(fixture.drops.countOf(u16(mcver::Block::Jukebox)), 0);
}

// **And exactly one disc however the block goes.** The eject lives in two
// places now -- `blockRemoved` and `dropBlockAsItem` -- and both read the
// metadata out of the world rather than trusting a caller's saved copy, so
// whichever runs first clears the cell and the second finds nothing.
TEST(a_jukebox_gives_back_one_disc_and_not_two)
{
    // The Survival order: the block goes, and then its drop table runs with
    // the metadata the breaker saved before the write.
    Jukebox fixture;
    CHECK(fixture.click(kRecord13));
    const u8 saved = u8(fixture.md());
    CHECK(item::destroyBlock(fixture.w(), 0, 63, 0));
    tick::dropBlockAsItem(fixture.w(), 0, 63, 0, BlockId(mcver::Block::Jukebox), saved);
    CHECK_EQ(fixture.drops.countOf(u16(kRecord13)), 1);
    CHECK_EQ(fixture.drops.countOf(u16(mcver::Block::Jukebox)), 1);

    // The explosion's order: the drop table runs while the jukebox is still
    // standing, and the block goes afterwards.
    Jukebox blast;
    CHECK(blast.click(kRecordCat));
    tick::dropBlockAsItem(blast.w(), 0, 63, 0, BlockId(mcver::Block::Jukebox),
                          u8(blast.md()));
    blast.w().setBlockWithNotify(0, 63, 0, block::kAir);
    CHECK_EQ(blast.drops.countOf(u16(kRecordCat)), 1);
    CHECK_EQ(blast.drops.countOf(u16(mcver::Block::Jukebox)), 1);
}

// An empty one drops only itself, and takes no extra draws doing it.
TEST(breaking_an_empty_jukebox_drops_only_the_block)
{
    Jukebox fixture;
    fixture.w().setBlockWithNotify(0, 63, 0, block::kAir);
    tick::dropBlockAsItem(fixture.w(), 0, 63, 0, BlockId(mcver::Block::Jukebox), 0);

    CHECK_EQ(fixture.records.calls.size(), usize(0));
    CHECK_EQ(fixture.drops.countOf(u16(mcver::Block::Jukebox)), 1);
    CHECK_EQ(fixture.drops.total(), 1);
}

// The record table is the jar's: two discs, and the track name is what the
// streaming pool files `streaming/13.mus` under.
TEST(the_two_discs_carry_the_track_they_play)
{
    CHECK(item::recordTrack(kRecord13) != nullptr);
    CHECK_EQ(std::string(item::recordTrack(kRecord13)), std::string("13"));
    CHECK_EQ(std::string(item::recordTrack(kRecordCat)), std::string("cat"));
    CHECK(item::recordTrack(kStoneItem) == nullptr);
    CHECK(item::recordTrack(0) == nullptr);
}
