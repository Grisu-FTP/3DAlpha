#pragma once

// World-level state: the contents of level.dat, minus its encoding.
//
// The storage slot decides where this lives -- Alpha and Beta keep the player
// inside level.dat, later versions move it to its own file -- so the fields sit
// here and the NBT layout sits in the slot, exactly as with chunks.

#include "core/item/item_stack.hpp"
#include "core/nbt/preserved.hpp"
#include "core/util/types.hpp"

#include <memory>
#include <vector>

namespace mc::entity { struct PersistentEntities; }

namespace mc::world {

struct PlayerData {
    // A server-created world has no Player compound at all, and inventing one
    // would move the player to 0,0,0 the next time the world is opened
    // elsewhere. Absent has to stay absent.
    bool present = false;

    // a1.1.2 does not write Dimension -- it arrived with the Nether in a1.2.0,
    // and a real a1.1.2 level.dat has thirteen Player tags with no Dimension
    // among them. Writing one anyway would add a tag to every world we touch,
    // so it goes back only if it was there. The field stays because later
    // versions need it.
    bool hasDimension = false;
    i32 dimension = 0;
    double pos[3] = {0.0, 0.0, 0.0};
    float rotation[2] = {0.0f, 0.0f};  // yaw, pitch
    double motion[3] = {0.0, 0.0, 0.0};
    bool onGround = false;
    float fallDistance = 0.0f;

    i16 health = 20;
    i16 attackTime = 0;
    i16 hurtTime = 0;
    i16 deathTime = 0;
    i16 air = 300;
    i16 fire = -20;
    i32 score = 0;

    std::vector<item::ItemStack> inventory;

    nbt::PreservedTags preserved;
};

struct LevelData {
    i64 lastPlayed = 0;
    // The original recomputes this on save and nothing reads it, so it is
    // round-tripped rather than maintained.
    i64 sizeOnDisk = 0;
    i64 randomSeed = 0;

    i32 spawnX = 0;
    i32 spawnY = 64;
    i32 spawnZ = 0;

    // Ticks. 24000 per day; the sky light multiplier is derived from it, which
    // after M0b costs a 256-texel lightmap rewrite and nothing else.
    i64 time = 0;

    // level.dat's `SnowCovered`. Modelled rather than preserved because the
    // **terrain generator reads it**: it is the one thing a1.1.2's
    // ChunkProviderGenerate asks the World for, and it puts ice at sea level - 1
    // across every ocean in the world. a1.1.2 rolls it once when the world is
    // created, with a one-in-four chance, and never changes it again.
    bool snowCovered = false;

    PlayerData player;

    // Null means this world has never had a port-owned entity snapshot.
    std::shared_ptr<const entity::PersistentEntities> entities;

    nbt::PreservedTags preserved;      // unmodelled tags inside Data
    nbt::PreservedTags preservedRoot;  // unmodelled siblings of Data
};

}  // namespace mc::world
