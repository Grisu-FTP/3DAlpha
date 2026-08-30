#include "framework.hpp"

#include "core/block/registry.hpp"
#include "core/world/chunk.hpp"
#include "core/world/light_update.hpp"
#include "core/world/lighting.hpp"

#include <memory>
#include <string>
#include <vector>

// **The oracle.** `LightUpdater` repairs a few cells after a block change;
// `LightEngine` solves a whole column from nothing. lighting.hpp argues they
// must agree -- a1.1.2's rule is a monotone fixed point with a strictly
// positive decrement, so there is exactly one answer and any algorithm that
// finds it finds the same numbers -- and this is where that argument is
// checked rather than believed.
//
// It needs no JVM of its own. `LightEngine::computeCentre` is already compared
// nibble by nibble against a real a1.1.2 `World` by tests/light_test.cpp, so
// pinning the incremental engine to it pins it to the jar transitively.
//
// The shape of every case is the same: build a 5x5 of chunks, light the inner
// 3x3 with the engine, edit one block in the centre, let the updater settle,
// then re-light the centre from scratch and compare all 32,768 cells of each
// plane. A 3x3 is a sufficient window (lighting.hpp), and light reaches at most
// 15 blocks, so an edit in the centre chunk cannot depend on anything the inner
// 3x3 does not hold -- which is what makes the two computations comparable.

using namespace mc;
using world::ChunkColumn;
using world::LightAccess;
using world::LightEngine;
using world::LightUpdater;
using world::kColumnBlocks;

namespace {

u8 bid(mcver::Block b) { return u8(b); }

// The chunk file's own order, which is what LightEngine reads and writes.
int columnIndex(int x, int y, int z) { return (x << 11) | (z << 7) | y; }

u8 nibbleAt(const u8* packed, int index)
{
    const u8 byte = packed[usize(index >> 1)];
    return (index & 1) == 0 ? u8(byte & 0x0F) : u8(byte >> 4);
}

constexpr int kGrid = 5;      // chunks per side of the scratch world
constexpr int kInner = 1;     // the ring around the centre that gets lit

int gridIndex(int gx, int gz) { return gz * kGrid + gx; }

// A 5x5 of flat block arrays plus ChunkColumns for the inner 3x3, which is
// everything the updater is allowed to see.
struct Scratch {
    std::vector<std::vector<u8>> blocks;                  // kGrid * kGrid flat arrays
    std::vector<std::unique_ptr<ChunkColumn>> columns;    // null outside the inner 3x3

    Scratch()
    {
        blocks.resize(usize(kGrid * kGrid));
        for (auto& b : blocks) {
            b.assign(usize(kColumnBlocks), bid(mcver::Block::Air));
        }
        columns.resize(usize(kGrid * kGrid));
    }

    // Centre chunk is (2, 2) in grid coordinates and (0, 0) in world chunk
    // coordinates, so a world block x maps to grid column 2 + (x >> 4).
    static int gridOfBlock(i32 v) { return 2 + int(v >> 4); }

    u8* flatAt(i32 x, i32 z)
    {
        const int gx = gridOfBlock(x);
        const int gz = gridOfBlock(z);
        if (gx < 0 || gx >= kGrid || gz < 0 || gz >= kGrid) return nullptr;
        return blocks[usize(gridIndex(gx, gz))].data();
    }

    void setBlockFlat(i32 x, int y, i32 z, u8 id)
    {
        u8* flat = flatAt(x, z);
        if (flat == nullptr) return;
        flat[usize(columnIndex(int(x & 15), y, int(z & 15)))] = id;
    }

    u8 blockFlat(i32 x, int y, i32 z) const
    {
        const int gx = gridOfBlock(x);
        const int gz = gridOfBlock(z);
        return blocks[usize(gridIndex(gx, gz))][usize(columnIndex(int(x & 15), y, int(z & 15)))];
    }

    // Ground at `surface` and below, air above, across the whole 5x5.
    void floorOf(u8 id, int surface)
    {
        for (auto& b : blocks) {
            for (int x = 0; x < 16; ++x) {
                for (int z = 0; z < 16; ++z) {
                    for (int y = 0; y <= surface; ++y) {
                        b[usize(columnIndex(x, y, z))] = id;
                    }
                }
            }
        }
    }

    // The nine block arrays LightEngine wants for chunk (cx, cz), in its
    // row-major x-slowest order.
    void windowFor(i32 cx, i32 cz, const u8* out[9])
    {
        for (int dx = -1; dx <= 1; ++dx) {
            for (int dz = -1; dz <= 1; ++dz) {
                const int gx = 2 + int(cx) + dx;
                const int gz = 2 + int(cz) + dz;
                out[(dx + 1) * 3 + (dz + 1)] =
                    (gx >= 0 && gx < kGrid && gz >= 0 && gz < kGrid)
                        ? blocks[usize(gridIndex(gx, gz))].data()
                        : nullptr;
            }
        }
    }

    // Lights one chunk from scratch and returns the packed planes.
    void lightChunk(LightEngine& engine, i32 cx, i32 cz, std::vector<u8>* sky,
                    std::vector<u8>* blockLight, std::vector<u8>* heightMap)
    {
        const u8* window[9] = {};
        windowFor(cx, cz, window);
        sky->assign(usize(kColumnBlocks / 2), 0);
        blockLight->assign(usize(kColumnBlocks / 2), 0);
        heightMap->assign(256, 0);
        engine.computeCentre(window, sky->data(), blockLight->data(), heightMap->data());
    }

    // Builds the inner 3x3 as ChunkColumns, lit by the engine.
    void buildInner(LightEngine& engine)
    {
        std::vector<u8> sky;
        std::vector<u8> blockLight;
        std::vector<u8> heightMap;

        for (int dz = -kInner; dz <= kInner; ++dz) {
            for (int dx = -kInner; dx <= kInner; ++dx) {
                lightChunk(engine, dx, dz, &sky, &blockLight, &heightMap);

                auto column = std::make_unique<ChunkColumn>(i32(dx), i32(dz));
                const u8* flat = blocks[usize(gridIndex(2 + dx, 2 + dz))].data();
                for (int lx = 0; lx < 16; ++lx) {
                    for (int lz = 0; lz < 16; ++lz) {
                        for (int y = 0; y < 128; ++y) {
                            const int i = columnIndex(lx, y, lz);
                            column->setBlock(lx, y, lz, flat[usize(i)]);
                            column->setSkyLight(lx, y, lz, nibbleAt(sky.data(), i));
                            column->setBlockLight(lx, y, lz, nibbleAt(blockLight.data(), i));
                        }
                    }
                }
                for (usize i = 0; i < 256; ++i) {
                    column->heightMap[i] = heightMap[i];
                }
                columns[usize(gridIndex(2 + dx, 2 + dz))] = std::move(column);
            }
        }
    }

    ChunkColumn* columnFor(i32 chunkX, i32 chunkZ)
    {
        const int gx = 2 + int(chunkX);
        const int gz = 2 + int(chunkZ);
        if (gx < 0 || gx >= kGrid || gz < 0 || gz >= kGrid) return nullptr;
        return columns[usize(gridIndex(gx, gz))].get();
    }
};

ChunkColumn* columnHook(void* ctx, i32 chunkX, i32 chunkZ)
{
    return static_cast<Scratch*>(ctx)->columnFor(chunkX, chunkZ);
}

int gLitSections = 0;
void litHook(void*, i32, int, i32) { ++gLitSections; }

// One mismatch in 32,768 is unreadable as a bare failure, so say where.
struct Mismatch {
    bool bad = false;
    int x = 0, y = 0, z = 0;
    int got = 0, want = 0, blockId = 0;
};

Mismatch comparePlane(const ChunkColumn& column, const std::vector<u8>& packed, bool sky)
{
    Mismatch m;
    for (int lx = 0; lx < 16; ++lx) {
        for (int lz = 0; lz < 16; ++lz) {
            for (int y = 0; y < 128; ++y) {
                const u8 got = sky ? column.skyLight(lx, y, lz) : column.blockLight(lx, y, lz);
                const u8 want = nibbleAt(packed.data(), columnIndex(lx, y, lz));
                if (got != want) {
                    m.bad = true;
                    m.x = lx;
                    m.y = y;
                    m.z = lz;
                    m.got = got;
                    m.want = want;
                    m.blockId = column.block(lx, y, lz);
                    return m;
                }
            }
        }
    }
    return m;
}

void report(const Mismatch& m, const char* plane)
{
    if (!m.bad) return;
    ::mc::test::reportFailure(__FILE__, __LINE__,
                              std::string(plane) + " light at (" + ::mc::test::describe(m.x) + ","
                                  + ::mc::test::describe(m.y) + "," + ::mc::test::describe(m.z)
                                  + ") is " + ::mc::test::describe(m.got) + ", the engine says "
                                  + ::mc::test::describe(m.want) + " (block "
                                  + ::mc::test::describe(m.blockId) + ")");
}

// Apply an edit the way the game does: write the block, move the height map,
// tell the updater, then let it settle. The height map is recomputed with the
// engine's own rule rather than a copy of TickWorld::refreshHeight, because a
// disagreement there would show up as a lighting failure and be blamed on the
// wrong file.
void applyEdit(Scratch& scratch, LightUpdater& updater, i32 x, int y, i32 z, u8 id)
{
    scratch.setBlockFlat(x, y, z, id);

    ChunkColumn* column = scratch.columnFor(x >> 4, z >> 4);
    CHECK(column != nullptr);
    column->setBlock(int(x & 15), y, int(z & 15), id);
    world::computeHeightMap(scratch.flatAt(x, z), column->heightMap);

    updater.blockChanged(x, y, z);

    // Settle completely. The budget exists so a frame can stop early; a test
    // wants the fixed point.
    for (int guard = 0; guard < 4096 && !updater.idle(); ++guard) {
        updater.drain(4096);
    }
    CHECK(updater.idle());
}

// The whole assertion: the centre column, as the updater left it, equals the
// centre column lit from scratch.
void checkCentreMatches(Scratch& scratch, LightEngine& engine)
{
    std::vector<u8> sky;
    std::vector<u8> blockLight;
    std::vector<u8> heightMap;
    scratch.lightChunk(engine, 0, 0, &sky, &blockLight, &heightMap);

    const ChunkColumn* centre = scratch.columnFor(0, 0);
    CHECK(centre != nullptr);

    report(comparePlane(*centre, sky, true), "sky");
    report(comparePlane(*centre, blockLight, false), "block");
}

}  // namespace

// A torch appearing in open air is the plain addition case, and the one lava
// flowing is: a light source arrives where there was none.
TEST(a_light_source_appearing_lights_what_the_engine_would)
{
    Scratch scratch;
    scratch.floorOf(bid(mcver::Block::Stone), 40);

    LightEngine engine;
    scratch.buildInner(engine);

    LightUpdater updater(LightAccess{&scratch, columnHook, litHook});
    gLitSections = 0;
    applyEdit(scratch, updater, 8, 41, 8, bid(mcver::Block::Torch));

    checkCentreMatches(scratch, engine);
    CHECK(gLitSections > 0);
    CHECK_EQ(updater.stats().dropped, u64(0));
}

// Lava is the reported symptom: it emits 15, it spreads, and none of it used to
// reach the stored light at all.
TEST(lava_arriving_relights_the_world_around_it)
{
    Scratch scratch;
    scratch.floorOf(bid(mcver::Block::Stone), 40);

    // A pocket, so the light has somewhere to travel and something to stop it.
    for (int y = 41; y <= 45; ++y) {
        for (int x = -6; x <= 6; ++x) {
            for (int z = -6; z <= 6; ++z) {
                scratch.setBlockFlat(x, y, z, bid(mcver::Block::Air));
            }
        }
    }

    LightEngine engine;
    scratch.buildInner(engine);

    LightUpdater updater(LightAccess{&scratch, columnHook, litHook});

    // A short flow, one block at a time, exactly as fluidFlowingTick writes it.
    for (int step = 0; step < 5; ++step) {
        applyEdit(scratch, updater, i32(step), 41, 0, bid(mcver::Block::Lava));
        checkCentreMatches(scratch, engine);
    }
    CHECK_EQ(updater.stats().dropped, u64(0));
}

// Removal is the harder half: the light that was flowing through the removed
// cell has to be taken back before anything refills it.
TEST(a_light_source_going_out_darkens_what_the_engine_would)
{
    Scratch scratch;
    scratch.floorOf(bid(mcver::Block::Stone), 40);
    scratch.setBlockFlat(8, 41, 8, bid(mcver::Block::Torch));

    LightEngine engine;
    scratch.buildInner(engine);

    LightUpdater updater(LightAccess{&scratch, columnHook, litHook});
    applyEdit(scratch, updater, 8, 41, 8, bid(mcver::Block::Air));

    checkCentreMatches(scratch, engine);
    CHECK_EQ(updater.stats().dropped, u64(0));
}

// Two sources side by side: removing one must not zero what the other is
// holding up, which is the case a naive removal gets wrong.
TEST(removing_one_of_two_sources_leaves_the_other_standing)
{
    Scratch scratch;
    scratch.floorOf(bid(mcver::Block::Stone), 40);
    scratch.setBlockFlat(4, 41, 8, bid(mcver::Block::Torch));
    scratch.setBlockFlat(9, 41, 8, bid(mcver::Block::Torch));

    LightEngine engine;
    scratch.buildInner(engine);

    LightUpdater updater(LightAccess{&scratch, columnHook, litHook});
    applyEdit(scratch, updater, 4, 41, 8, bid(mcver::Block::Air));

    checkCentreMatches(scratch, engine);
}

// Sky light, the other plane. Roofing a cell darkens the column under it, and
// the height map moves by one.
TEST(a_block_placed_under_open_sky_darkens_the_column)
{
    Scratch scratch;
    scratch.floorOf(bid(mcver::Block::Stone), 40);

    LightEngine engine;
    scratch.buildInner(engine);

    LightUpdater updater(LightAccess{&scratch, columnHook, litHook});
    applyEdit(scratch, updater, 8, 45, 8, bid(mcver::Block::Stone));

    checkCentreMatches(scratch, engine);
}

// And the reverse, where the height map moves by many cells at once: digging
// out a shaft lets the sky all the way down.
TEST(digging_a_shaft_lets_the_sky_down_it)
{
    Scratch scratch;
    scratch.floorOf(bid(mcver::Block::Stone), 40);

    LightEngine engine;
    scratch.buildInner(engine);

    LightUpdater updater(LightAccess{&scratch, columnHook, litHook});
    for (int y = 40; y >= 30; --y) {
        applyEdit(scratch, updater, 8, y, 8, bid(mcver::Block::Air));
    }

    checkCentreMatches(scratch, engine);
    CHECK_EQ(updater.stats().dropped, u64(0));
}

// A roof broken from underneath, which moves the height map up by a lot in one
// edit rather than a cell at a time.
TEST(breaking_a_roof_relights_everything_under_it)
{
    Scratch scratch;
    scratch.floorOf(bid(mcver::Block::Stone), 40);

    // A slab of stone at y = 60 over the whole world, with a hollow beneath it.
    for (int x = -40; x <= 40; ++x) {
        for (int z = -40; z <= 40; ++z) {
            scratch.setBlockFlat(x, 60, z, bid(mcver::Block::Stone));
        }
    }

    LightEngine engine;
    scratch.buildInner(engine);

    LightUpdater updater(LightAccess{&scratch, columnHook, litHook});
    applyEdit(scratch, updater, 8, 60, 8, bid(mcver::Block::Air));

    checkCentreMatches(scratch, engine);
}

// Deep underground, where nothing changes: the updater must settle quickly and
// leave the planes exactly as they were.
TEST(an_edit_in_the_dark_settles_without_touching_anything)
{
    Scratch scratch;
    scratch.floorOf(bid(mcver::Block::Stone), 40);

    LightEngine engine;
    scratch.buildInner(engine);

    LightUpdater updater(LightAccess{&scratch, columnHook, litHook});
    applyEdit(scratch, updater, 8, 10, 8, bid(mcver::Block::Dirt));

    checkCentreMatches(scratch, engine);
    CHECK_EQ(updater.stats().cellsSettled, u64(0));
}

// **A relight that changes nothing must report nothing.**
//
// This is the property that decides whether an active fluid makes the renderer
// churn. Light is baked into vertices, so every section the relighter reports
// is a remesh -- and a fluid rewrites the same few blocks over and over as it
// flips between its flowing and still forms. If a change that leaves every
// stored value where it was still reported a section, the chunks around a lava
// pool would remesh for ever and visibly refresh in a loop.
TEST(a_relight_that_settles_to_the_same_values_reports_no_section)
{
    Scratch scratch;
    scratch.floorOf(bid(mcver::Block::Stone), 40);
    for (int y = 41; y <= 45; ++y) {
        for (int x = -6; x <= 6; ++x) {
            for (int z = -6; z <= 6; ++z) {
                scratch.setBlockFlat(x, y, z, bid(mcver::Block::Air));
            }
        }
    }
    scratch.setBlockFlat(2, 41, 2, bid(mcver::Block::Lava));

    LightEngine engine;
    scratch.buildInner(engine);

    LightUpdater updater(LightAccess{&scratch, columnHook, litHook});

    // Settled already: the engine lit the world with this lava in it. Telling
    // the updater about it again must move nothing.
    gLitSections = 0;
    applyEdit(scratch, updater, 2, 41, 2, bid(mcver::Block::Lava));
    CHECK_EQ(gLitSections, 0);
    CHECK_EQ(updater.stats().cellsSettled, u64(0));

    // And again, repeatedly -- the shape a fluid tick produces.
    for (int i = 0; i < 8; ++i) {
        applyEdit(scratch, updater, 2, 41, 2, bid(mcver::Block::Lava));
    }
    CHECK_EQ(gLitSections, 0);
    CHECK_EQ(updater.stats().cellsSettled, u64(0));

    checkCentreMatches(scratch, engine);
}

// The same for the flowing/still flip a fluid actually performs: two block ids
// that light identically, written alternately, must not keep waking the mesher.
TEST(flipping_a_fluid_between_its_two_forms_reports_no_section)
{
    Scratch scratch;
    scratch.floorOf(bid(mcver::Block::Stone), 40);
    for (int y = 41; y <= 45; ++y) {
        for (int x = -6; x <= 6; ++x) {
            for (int z = -6; z <= 6; ++z) {
                scratch.setBlockFlat(x, y, z, bid(mcver::Block::Air));
            }
        }
    }
    scratch.setBlockFlat(0, 41, 0, bid(mcver::Block::Lava));

    LightEngine engine;
    scratch.buildInner(engine);

    LightUpdater updater(LightAccess{&scratch, columnHook, litHook});

    // One settling pass for the first flip, then nothing for ever after: both
    // forms emit 15 and both are opaque to light, so the stored planes are the
    // same either way.
    applyEdit(scratch, updater, 0, 41, 0, bid(mcver::Block::FlowingLava));
    gLitSections = 0;
    const u64 settledBefore = updater.stats().cellsSettled;

    for (int i = 0; i < 10; ++i) {
        applyEdit(scratch, updater, 0, 41, 0,
                  i % 2 == 0 ? bid(mcver::Block::Lava) : bid(mcver::Block::FlowingLava));
    }
    CHECK_EQ(updater.stats().cellsSettled, settledBefore);
    CHECK_EQ(gLitSections, 0);
}

// The queues are bounded and the budget is real: a drain that is cut short must
// leave the work queued rather than lose it.
TEST(a_budgeted_drain_finishes_the_job_across_calls)
{
    Scratch scratch;
    scratch.floorOf(bid(mcver::Block::Stone), 40);

    LightEngine engine;
    scratch.buildInner(engine);

    LightUpdater updater(LightAccess{&scratch, columnHook, litHook});

    scratch.setBlockFlat(8, 41, 8, bid(mcver::Block::Torch));
    ChunkColumn* centre = scratch.columnFor(0, 0);
    CHECK(centre != nullptr);
    centre->setBlock(8, 41, 8, bid(mcver::Block::Torch));
    world::computeHeightMap(scratch.flatAt(8, 8), centre->heightMap);
    updater.blockChanged(8, 41, 8);

    // Four cells at a time, which is far below what this edit needs.
    int calls = 0;
    while (!updater.idle() && calls < 100000) {
        updater.drain(4);
        ++calls;
    }
    CHECK(updater.idle());
    CHECK(calls > 1);   // it really was cut short

    checkCentreMatches(scratch, engine);
}
