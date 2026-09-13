#include "framework.hpp"

#include "core/util/chunk_queue.hpp"

#include <set>
#include <utility>

using namespace mc;

namespace {

// Drains the whole queue into the order it came out in.
std::vector<std::pair<i32, i32>> drain(ChunkQueue& queue)
{
    std::vector<std::pair<i32, i32>> out;
    i32 x = 0;
    i32 z = 0;
    while (queue.pop(&x, &z)) {
        out.push_back({x, z});
    }
    return out;
}

}  // namespace

TEST(a_coordinate_offered_twice_is_on_the_queue_once)
{
    ChunkQueue queue;
    queue.setCapacity(16);

    // Negative on both axes, per CONTRIBUTING: the hash and the ring indexing
    // both take the coordinate as it is.
    CHECK(queue.push(-3, -7));
    CHECK(queue.size() == 1);
    CHECK(queue.contains(-3, -7));

    // The second offer of the same chunk -- what a fluid spreading in it does
    // three hundred times a tick -- is refused, and refusing is not overflow.
    CHECK(!queue.push(-3, -7));
    CHECK(!queue.push(-3, -7));
    CHECK(queue.size() == 1);
    CHECK(!queue.overflowed());

    // A different chunk is a different entry, and one coordinate's negation is
    // not the other's.
    CHECK(queue.push(-7, -3));
    CHECK(queue.size() == 2);
    CHECK(!queue.contains(3, 7));

    const auto out = drain(queue);
    CHECK(out.size() == 2);
    CHECK(out[0] == std::make_pair(-3, -7));
    CHECK(out[1] == std::make_pair(-7, -3));
    CHECK(queue.empty());

    // ...and once it is off, it can be put back on. That is what makes this a
    // queue of work rather than a set of things that have ever happened.
    CHECK(!queue.contains(-3, -7));
    CHECK(queue.push(-3, -7));
    CHECK(queue.size() == 1);
}

TEST(the_queue_is_first_in_first_out_so_a_nearest_first_producer_stays_nearest_first)
{
    ChunkQueue queue;
    queue.setCapacity(64);

    for (i32 i = 0; i < 20; ++i) {
        CHECK(queue.push(-i, i * 2));
    }
    const auto out = drain(queue);
    CHECK(out.size() == 20);
    for (i32 i = 0; i < 20; ++i) {
        CHECK(out[usize(i)] == std::make_pair(-i, i * 2));
    }
}

TEST(a_full_queue_refuses_and_says_so_once)
{
    ChunkQueue queue;
    queue.setCapacity(4);

    for (i32 i = 0; i < 4; ++i) {
        CHECK(queue.push(i, 0));
    }
    CHECK(queue.full());
    CHECK(!queue.push(99, 99));
    CHECK(queue.size() == 4);

    // **The overflow flag is the consumer's cue to recover**, and reading it
    // clears it: losing four coordinates and losing one need the same single
    // recovery.
    CHECK(queue.overflowed());
    CHECK(!queue.overflowed());

    // A push refused because the coordinate is already here is not overflow --
    // nothing was lost.
    CHECK(!queue.push(0, 0));
    CHECK(!queue.overflowed());

    // Emptying it does not clear the flag: the consumer, not the producer,
    // decides when it has recovered.
    CHECK(!queue.push(99, 99));
    queue.clear();
    CHECK(queue.empty());
    CHECK(queue.overflowed());
}

TEST(draining_and_refilling_the_queue_forever_keeps_answering_correctly)
{
    // The membership table leaves a tombstone behind every pop, so a queue used
    // the way the map uses one -- filled and drained every frame, for hours --
    // is the case that finds a table which fills up with the dead. It rehashes
    // instead; this is what says so.
    ChunkQueue queue;
    queue.setCapacity(32);

    std::set<std::pair<i32, i32>> live;
    i32 next = -5000;
    // Ten that stay on it throughout, so every round's probes walk past live
    // entries as well as dead ones.
    for (int i = 0; i < 10; ++i) {
        CHECK(queue.push(next, -next));
        live.insert({next, -next});
        ++next;
    }
    for (int round = 0; round < 400; ++round) {
        for (int i = 0; i < 10; ++i) {
            const std::pair<i32, i32> at{next, -next};
            ++next;
            CHECK(queue.push(at.first, at.second));
            live.insert(at);
        }
        // ...and the ten oldest come back off, in order.
        for (int i = 0; i < 10; ++i) {
            i32 x = 0;
            i32 z = 0;
            CHECK(queue.pop(&x, &z));
            CHECK(live.erase({x, z}) == 1);
        }
        CHECK(queue.size() == int(live.size()));
        for (const auto& at : live) {
            CHECK(queue.contains(at.first, at.second));
        }
        CHECK(!queue.contains(next, -next));
        CHECK(!queue.overflowed());
    }

    // And it still comes out in order after all that.
    const auto out = drain(queue);
    CHECK(out.size() == live.size());
    for (usize i = 1; i < out.size(); ++i) {
        CHECK(out[i].first > out[i - 1].first);
    }
}

TEST(setting_the_capacity_again_empties_the_queue)
{
    ChunkQueue queue;
    queue.setCapacity(8);
    CHECK(queue.push(1, 1));
    CHECK(!queue.push(1, 1));

    queue.setCapacity(8);
    CHECK(queue.empty());
    CHECK(!queue.contains(1, 1));
    CHECK(queue.push(1, 1));

    // A capacity below one is still a queue: the map sizes this from a window
    // and a grid, and neither is allowed to produce something that silently
    // swallows everything offered to it.
    queue.setCapacity(0);
    CHECK(queue.capacity() == 1);
    CHECK(queue.push(2, 2));
    CHECK(!queue.push(3, 3));
    CHECK(queue.overflowed());
}
