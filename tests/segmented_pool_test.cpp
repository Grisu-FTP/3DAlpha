// The growable pool every entity lives in, and the nearest-first cutoff that
// decides which of them are drawn when there are more than a buffer holds.
//
// The pool's promises are the ones the entity code leans on without saying so:
// a reference survives a push, a refusal is a null rather than a crash, and
// memory comes back when a spike is over.

#include "core/render/draw_budget.hpp"
#include "core/util/memory.hpp"
#include "core/util/segmented_pool.hpp"
#include "framework.hpp"
#include "low_heap.hpp"

using namespace mc;

namespace {

struct Thing {
    int value = -1;
    double weight = 0.0;
};

using Pool = SegmentedPool<Thing, 8>;

usize gFakeFree = 0;
usize fakeFree() { return gFakeFree; }

struct FakeHeap {
    explicit FakeHeap(usize free)
    {
        gFakeFree = free;
        setHeapFreeQuery(&fakeFree);
    }
    ~FakeHeap() { setHeapFreeQuery(nullptr); }
};

}  // namespace

TEST(a_pool_grows_past_its_first_segment_without_moving_anything)
{
    Pool pool;
    CHECK_EQ(pool.capacity(), 8);
    Thing* first = pool.push();
    CHECK(first != nullptr);
    first->value = 100;
    for (int i = 1; i < 50; ++i) {
        Thing* t = pool.push();
        CHECK(t != nullptr);
        t->value = 100 + i;
    }
    CHECK_EQ(pool.size(), 50);
    CHECK(pool.capacity() >= 50);
    // **The reference taken before six growths is still the element.** A
    // vector would have moved it on the first.
    CHECK(first == &pool[0]);
    for (int i = 0; i < 50; ++i) {
        CHECK_EQ(pool[i].value, 100 + i);
    }
}

TEST(a_pushed_slot_is_fresh_even_when_it_was_used_before)
{
    Pool pool;
    pool.push()->value = 7;
    pool.swapRemove(0);
    const Thing* again = pool.push();
    CHECK_EQ(again->value, -1);
}

TEST(swap_remove_moves_the_last_into_the_hole)
{
    Pool pool;
    for (int i = 0; i < 20; ++i) {
        pool.push()->value = i;
    }
    pool.swapRemove(3);
    CHECK_EQ(pool.size(), 19);
    CHECK_EQ(pool[3].value, 19);
    pool.swapRemove(pool.size() - 1);
    CHECK_EQ(pool.size(), 18);
    CHECK_EQ(pool[17].value, 17);
}

TEST(trim_hands_back_segments_with_one_to_spare)
{
    Pool pool;
    for (int i = 0; i < 40; ++i) {
        pool.push();
    }
    CHECK_EQ(pool.segments(), 5);

    // Removal alone frees nothing, so a tick that removes while it walks never
    // has a segment go under a reference it holds.
    pool.truncate(9);
    CHECK_EQ(pool.segments(), 5);

    // Nine need two segments; one more is kept so a pool on a boundary does
    // not allocate and free on alternate ticks.
    pool.trim();
    CHECK_EQ(pool.segments(), 3);

    pool.clear();
    CHECK_EQ(pool.size(), 0);
    CHECK_EQ(pool.segments(), 1);
}

TEST(pool_bytes_are_counted_and_given_back)
{
    const usize before = poolBytes();
    {
        Pool pool;
        CHECK_EQ(poolBytes(), before + Pool::kSegmentBytes);
        for (int i = 0; i < 17; ++i) {
            pool.push();
        }
        CHECK_EQ(poolBytes(), before + 3 * Pool::kSegmentBytes);
    }
    CHECK_EQ(poolBytes(), before);
}

TEST(a_heap_that_says_stop_gets_a_null_rather_than_an_allocation)
{
    Pool pool;
    test::LowHeap low;
    // The first segment was taken at construction and is not asked about.
    for (int i = 0; i < 8; ++i) {
        CHECK(pool.push() != nullptr);
    }
    CHECK(pool.push() == nullptr);
    CHECK_EQ(pool.size(), 8);
}

TEST(growth_keeps_a_reserve_and_room_for_three_save_copies)
{
    // The rule in core/util/memory.hpp, at its edge: growth is allowed while
    // the heap left after it still holds the reserve plus three more copies of
    // every pool byte.
    const usize seg = Pool::kSegmentBytes;
    const usize pools = poolBytes();
    const usize exact = seg + kPoolHeapReserve + kPoolSaveCopies * (pools + seg);
    {
        FakeHeap heap(exact);
        CHECK(poolGrowthAllowed(seg));
    }
    {
        FakeHeap heap(exact - 1);
        CHECK(!poolGrowthAllowed(seg));
    }
    {
        FakeHeap heap(kPoolHeapReserve);
        CHECK(!poolGrowthAllowed(seg));
    }
    // No answer from the platform: a host, where only malloc can refuse.
    CHECK(poolGrowthAllowed(seg));
}

TEST(a_cutoff_that_fits_draws_everything)
{
    render::DrawCutoff cut;
    const double at[3] = {1.0, 5.0, 9.0};
    cut.compute(3, 100, [&](int i, double* x, double* y, double* z) {
        *x = at[i];
        *y = 0.0;
        *z = 0.0;
        return true;
    }, [](int) { return 10; });
    CHECK(cut.drawsEverything());
    CHECK(cut.admit(30.0, 0.0, 0.0, 10));
}

TEST(a_cutoff_draws_the_nearest_rings_whole_and_the_edge_to_the_budget)
{
    // Three near, four at the edge, two far; a budget of five.
    const double at[9] = {20.0, 1.0, 20.0, 9.0, 1.0, 9.0, 1.0, 9.0, 9.0};
    const auto place = [&](int i, double* x, double* y, double* z) {
        *x = at[i];
        *y = 0.0;
        *z = 0.0;
        return true;
    };
    render::DrawCutoff cut;
    cut.compute(9, 5, place, [](int) { return 1; });
    CHECK(!cut.drawsEverything());

    int drawn = 0;
    int near = 0;
    int far = 0;
    for (int i = 0; i < 9; ++i) {
        if (cut.admit(at[i], 0.0, 0.0, 1)) {
            ++drawn;
            near += at[i] == 1.0 ? 1 : 0;
            far += at[i] == 20.0 ? 1 : 0;
        }
    }
    CHECK_EQ(drawn, 5);
    CHECK_EQ(near, 3);
    CHECK_EQ(far, 0);

    // A rewound cutoff admits the same ones again, which is what the sign
    // text relies on to follow the boards.
    cut.rewind();
    int again = 0;
    for (int i = 0; i < 9; ++i) {
        again += cut.admit(at[i], 0.0, 0.0, 1) ? 1 : 0;
    }
    CHECK_EQ(again, 5);
}

TEST(a_pile_bigger_than_the_budget_still_draws_what_fits)
{
    // Every entity in one ring: the edge is the nearest ring, and it gets the
    // whole budget rather than nothing.
    render::DrawCutoff cut;
    cut.compute(100, 30, [](int, double* x, double* y, double* z) {
        *x = 2.0;
        *y = 0.0;
        *z = 0.0;
        return true;
    }, [](int) { return 4; });
    int drawn = 0;
    for (int i = 0; i < 100; ++i) {
        drawn += cut.admit(2.0, 0.0, 0.0, 4) ? 1 : 0;
    }
    CHECK_EQ(drawn, 7);
}
