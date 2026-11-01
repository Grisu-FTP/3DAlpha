// MeshPool: sections meshed off the calling thread come out byte-identical to
// meshing them in place, and the slot accounting never loses or doubles one.

#include "framework.hpp"

#include "core/block/registry.hpp"
#include "core/mesh/mesh_pool.hpp"

#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

using namespace mc;
using mesh::ColumnNeighbourhood;
using mesh::MeshBuilder;
using mesh::MeshPool;
using mesh::MeshScratch;
using world::ChunkColumn;

namespace {

constexpr u16 kStone = u16(mcver::Block::Stone);

void shape(ChunkColumn& column, int salt)
{
    for (int i = 0; i < 40; ++i) {
        column.setBlock((i * 7 + salt) % 16, 3 + (i * 5) % 9, (i * 3 + salt) % 16, kStone);
    }
}

std::vector<u8> bytesOf(const MeshBuilder& builder)
{
    std::vector<u8> out(builder.byteSize());
    builder.copyTo(out.data());
    return out;
}

// Polls until `want` results are in, for at most about two seconds.
std::vector<std::vector<u8>> collect(MeshPool& pool, int want, std::vector<u32>* revisions)
{
    std::vector<std::vector<u8>> out;
    for (int spins = 0; int(out.size()) < want && spins < 2000; ++spins) {
        MeshPool::Result r;
        while (pool.poll(&r)) {
            out.push_back(bytesOf(*r.builder));
            if (revisions != nullptr) {
                revisions->push_back(r.revision);
            }
            pool.release(r.slot);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return out;
}

}  // namespace

TEST(a_pooled_mesh_matches_meshing_in_place)
{
    ChunkColumn column;
    shape(column, 1);
    const ColumnNeighbourhood hood = ColumnNeighbourhood::isolated(column);

    MeshScratch scratch;
    scratch.fill(hood, 0);
    MeshBuilder direct;
    mesh::meshSection(scratch, direct);

    MeshPool pool;
    pool.start(2, false, 64);
    CHECK(pool.submit(4, 0, -2, 7, hood));
    pool.runPending();

    MeshPool::Result r;
    CHECK(pool.poll(&r));
    CHECK_EQ(r.chunkX, 4);
    CHECK_EQ(r.chunkZ, -2);
    CHECK_EQ(r.revision, u32(7));
    CHECK(bytesOf(*r.builder) == bytesOf(direct));
    pool.release(r.slot);
    CHECK_EQ(pool.inFlight(), 0);
}

TEST(a_threaded_pool_matches_and_returns_every_job)
{
    ChunkColumn a;
    ChunkColumn b;
    shape(a, 1);
    shape(b, 5);

    MeshScratch scratch;
    MeshBuilder wantA;
    MeshBuilder wantB;
    scratch.fill(ColumnNeighbourhood::isolated(a), 0);
    mesh::meshSection(scratch, wantA);
    scratch.fill(ColumnNeighbourhood::isolated(b), 0);
    mesh::meshSection(scratch, wantB);

    MeshPool pool;
    pool.start(4, true, 64);
    CHECK(pool.threaded());
    CHECK(pool.submit(0, 0, 0, 1, ColumnNeighbourhood::isolated(a)));
    CHECK(pool.submit(1, 0, 0, 2, ColumnNeighbourhood::isolated(b)));

    std::vector<u32> revisions;
    const auto got = collect(pool, 2, &revisions);
    CHECK_EQ(got.size(), usize(2));
    if (got.size() == 2) {
        // Oldest first, so the order is the order of submission.
        CHECK_EQ(revisions[0], u32(1));
        CHECK(got[0] == bytesOf(wantA));
        CHECK(got[1] == bytesOf(wantB));
    }
    CHECK_EQ(pool.inFlight(), 0);
}

TEST(a_full_pool_refuses_rather_than_waits)
{
    ChunkColumn column;
    shape(column, 2);
    const ColumnNeighbourhood hood = ColumnNeighbourhood::isolated(column);

    MeshPool pool;
    pool.start(2, false, 64);
    CHECK(pool.submit(0, 0, 0, 1, hood));
    CHECK(pool.submit(1, 0, 0, 1, hood));
    CHECK(!pool.submit(2, 0, 0, 1, hood));
    CHECK(pool.inFlight(1, 0, 0));
    CHECK(!pool.inFlight(2, 0, 0));

    // A finished slot stays reserved until it is released.
    pool.runPending();
    MeshPool::Result r;
    CHECK(pool.poll(&r));
    CHECK(!pool.submit(2, 0, 0, 1, hood));
    pool.release(r.slot);
    CHECK(pool.submit(2, 0, 0, 1, hood));
}
