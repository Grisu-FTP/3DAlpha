#pragma once

// **What the world's drop sink was handed**, in the order it was handed it.
//
// `TickWorld::spawnItem` is the one seam every drop in the engine goes through
// -- a broken block's, a crop's seeds, a wrecked boat's planks -- and a test
// that wants to know what something left behind wires one of these to the world
// instead of building an item pool. See core/tick/drop.hpp.

#include "core/tick/tick_world.hpp"
#include "core/util/types.hpp"

#include <vector>

namespace mc::test {

struct Drop {
    double x, y, z;
    u16 item;
    int count;
};

struct DropCatcher {
    std::vector<Drop> drops;

    static void sink(void* ctx, double x, double y, double z, u16 item, int count)
    {
        static_cast<DropCatcher*>(ctx)->drops.push_back(Drop{x, y, z, item, count});
    }

    // Wire this catcher to a world. Held by pointer, so the catcher has to
    // outlive the world -- which every fixture here arranges by declaring them
    // in that order.
    void watch(tick::TickWorld& world) { world.setDropSink(&DropCatcher::sink, this); }

    int total() const
    {
        int n = 0;
        for (const Drop& d : drops) {
            n += d.count;
        }
        return n;
    }

    // How many of one item were dropped, counts included.
    int countOf(u16 item) const
    {
        int n = 0;
        for (const Drop& d : drops) {
            if (d.item == item) {
                n += d.count;
            }
        }
        return n;
    }
};

}  // namespace mc::test
