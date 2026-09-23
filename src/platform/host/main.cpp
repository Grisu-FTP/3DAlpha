// Host entry point.
//
// The host target exists so core code can be developed and tested in a desktop
// debugger under sanitizers -- see docs/architecture.md. It will grow an
// SDL2 + OpenGL mirror of the 3DS platform interfaces at M2; until then it is a
// harness whose main job is to prove that core compiles and links away from
// libctru, and to expose the world tools that do not need a console.

#include "core/audio/effect_preload.hpp"
#include "core/audio/sample.hpp"
#include "core/audio/sound_engine.hpp"
#include "core/audio/vorbis_stream.hpp"
#include "core/io/posix_file_system.hpp"
#include "core/io/volume_info.hpp"
#include "core/block/registry.hpp"
#include "core/map/map_palette.hpp"
#include "core/map/map_render.hpp"
#include "core/map/map_sample.hpp"
#include "core/map/map_store.hpp"
#include "core/mesh/cube_atlas.hpp"
#include "core/mesh/mesher.hpp"
#include "core/mesh/visibility.hpp"
#include "core/entity/mob.hpp"
#include "core/entity/mob_spawn.hpp"
#include "core/entity/mob_spawner.hpp"
#include "core/entity/player_body.hpp"
#include "core/entity/player_vitals.hpp"
#include "core/item/block_breaking.hpp"
#include "core/item/inventory.hpp"
#include "core/item/use.hpp"
#include "core/render/chunk_renderer.hpp"
#include "core/render/vbo_pool.hpp"
#include "core/render/world_streamer.hpp"
#include "core/render/visible_set.hpp"
#include "core/texture/atlas_image.hpp"
#include "core/texture/jar_import.hpp"
#include "core/texture/pack_list.hpp"
#include "core/texture/zip_archive.hpp"
#include "core/util/math.hpp"
#include "core/world/any_storage.hpp"
#include "core/world/chunk.hpp"
#include "core/world/tile_entity.hpp"
#include "core/world/chunk_cache.hpp"
#include "core/world/format/converter.hpp"
#include "core/world/world_format.hpp"
#include "core/world/world_list.hpp"
#include "platform/host/audio_wav.hpp"
#include "platform/host/join.hpp"
#include "platform/host/online.hpp"
#include "impl/worldgen/alpha_nobiome/chunk_generator.hpp"
#include "items.hpp"  // generated; see tools/configure.py
#include "version_config.hpp"
#include "version_slots.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <sys/statvfs.h>
#include <thread>
#include <tuple>
#include <vector>

namespace {

using namespace mc;

i64 nowMillis()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

struct Totals {
    int columns = 0;
    int sections = 0;
    int sectionsEmpty = 0;      // uniform air: never even meshed
    int sectionsNoGeometry = 0; // meshed, produced nothing
    usize quads = 0;
    usize meshBytes = 0;
    usize maxSectionQuads = 0;
    usize detailQuads = 0;       // opaque non-cube geometry, 16-byte vertices
    usize translucentQuads = 0;  // the same format, drawn in the blended pass
    int visibilityOpaque = 0;   // mask 0: the search stops here
    int visibilityOpen = 0;     // all 15 pairs
    long long meshMicros = 0;
    long long fillNanos = 0;   // MeshScratch::fill, the 5832 palette lookups
    long long emitNanos = 0;   // meshSection, proportional to the quads emitted

    // Per section height. Underground sections are the ones a player almost
    // never sees, and knowing what share of the geometry they hold is what
    // decides whether meshing has to be driven by visibility rather than by
    // what happens to be loaded.
    usize quadsByHeight[world::ChunkColumn::kSectionCount] = {};

    // Quads per atlas tile, read back out of the vertex UVs rather than from
    // the block table, so it measures what the buffer actually says. Which
    // tiles a real world touches is what the atlas and its mip chain get sized
    // against -- and it is the check that per-face textures reach the geometry
    // at all: grass's top tile only ever appears through `faces`.
    usize quadsByTile[mesh::kAtlasTileCount] = {};

    // Mesh size per section, keyed by (chunkX, chunkZ, sectionY), so the
    // reachability pass can price exactly the set the visibility walk reaches
    // rather than assume an average.
    //
    // Bytes rather than quads, and all three streams rather than the cube one:
    // fluid put 8 % of a real world's geometry in the 16-byte format, which is
    // no longer a rounding error against a 12 MB pool. The pool sees one
    // allocation per section holding all of it, and needs the split only to
    // know where each draw pass starts.
    std::map<std::tuple<i32, i32, int>, mesh::MeshRanges> meshBySection;

    // Every section's visibility mask, for the same reason.
    std::map<std::pair<i32, i32>, std::vector<mesh::SectionVisibility>> masks;

    // How many blocks of each render type a real world actually contains.
    // Everything but Cube is unimplemented, and this is what decides how much
    // the second vertex format they need is worth paying for.
    usize blocksByRenderType[usize(block::RenderType::Count)] = {};
};

// Collected during the directory walk so the mesh pass can hand each column its
// eight neighbours. A real world is 660 columns, which is small enough to hold
// entirely on a desktop; the console streams them instead.
struct World {
    std::map<std::pair<i32, i32>, world::ChunkColumn> columns;
};

Mat4 multiply(const Mat4& a, const Mat4& b)
{
    Mat4 r;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a.m[i][k] * b.m[k][j];
            }
            r.m[i][j] = sum;
        }
    }
    return r;
}

Mat4 perspective(float fovY, float aspect, float near, float far)
{
    const float f = 1.0f / std::tan(fovY * 0.5f);
    Mat4 r;
    r.m[0][0] = f / aspect;
    r.m[1][1] = f;
    r.m[2][2] = (far + near) / (near - far);
    r.m[2][3] = (2.0f * far * near) / (near - far);
    r.m[3][2] = -1.0f;
    return r;
}

// Look along `dir` from `eye`, world up.
Mat4 lookAlong(const Vec3& eye, const Vec3& dir)
{
    auto normalise = [](const Vec3& v) {
        const float len = std::sqrt(dot(v, v));
        return len > 0.0f ? Vec3{v.x / len, v.y / len, v.z / len} : Vec3{0, 0, -1};
    };
    auto cross = [](const Vec3& a, const Vec3& b) {
        return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    };

    const Vec3 f = normalise(dir);
    const Vec3 s = normalise(cross(f, Vec3{0.0f, 1.0f, 0.0f}));
    const Vec3 u = cross(s, f);

    Mat4 r = Mat4::identity();
    r.m[0][0] = s.x; r.m[0][1] = s.y; r.m[0][2] = s.z; r.m[0][3] = -dot(s, eye);
    r.m[1][0] = u.x; r.m[1][1] = u.y; r.m[1][2] = u.z; r.m[1][3] = -dot(u, eye);
    r.m[2][0] = -f.x; r.m[2][1] = -f.y; r.m[2][2] = -f.z; r.m[2][3] = dot(f, eye);
    return r;
}

// Two different questions, measured together because confusing them is what
// sent docs/3ds-performance.md section 4 down a blind alley:
//
//   * how much geometry the visibility walk can *reach* -- the VBO pool's worst
//     case, measured with the frustum left open because a player can spin on
//     the spot and re-meshing on every turn would be worse than holding it;
//   * how much a real frustum *draws* -- the per-frame cost, and the set that
//     actually has to be resident.
//
// The first turned out to be nearly as large as meshing everything. The second
// is what fits in the budget.
void reachability(const Totals& t, const world::LevelData& level)
{
    // A server-made world has no Player compound; fall back to the spawn point,
    // which every world has.
    const double px = level.player.present ? level.player.pos[0] : double(level.spawnX);
    const double py = level.player.present ? level.player.pos[1] : double(level.spawnY);
    const double pz = level.player.present ? level.player.pos[2] : double(level.spawnZ);

    const i32 playerChunkX = i32(std::floor(px / 16.0));
    const i32 playerChunkZ = i32(std::floor(pz / 16.0));
    const int playerSectionY = int(std::floor(py / 16.0));

    Frustum everything;
    everything.setFromViewProjection(Mat4{}, ClipRange::NegativeOneToOne);

    std::printf("\nreachable from the player at chunk (%d, %d) section %d\n",
                playerChunkX, playerChunkZ, playerSectionY);
    std::printf("  the frustum is left open: turning on the spot must not re-mesh\n");

    for (int radius : {6, 8, 10, 12}) {
        render::SectionField field;
        field.reset(radius);
        field.setCentre(playerChunkX, playerChunkZ);

        usize inRangeBytes = 0;
        int inRangeSections = 0;
        for (const auto& entry : t.masks) {
            if (!field.inRange(entry.first.first, entry.first.second)) {
                continue;
            }
            field.setColumn(entry.first.first, entry.first.second, entry.second.data());
            inRangeSections += world::ChunkColumn::kSectionCount;
            for (int sy = 0; sy < world::ChunkColumn::kSectionCount; ++sy) {
                auto it = t.meshBySection.find({entry.first.first, entry.first.second, sy});
                if (it != t.meshBySection.end()) {
                    inRangeBytes += it->second.total();
                }
            }
        }

        render::VisibleSet set;
        render::buildVisibleSet(field, everything, playerChunkX, playerSectionY,
                                playerChunkZ, &set);

        usize reachedBytes = 0;
        for (const auto& s : set.toMesh) {
            auto it = t.meshBySection.find({s.chunkX, s.chunkZ, int(s.sectionY)});
            if (it != t.meshBySection.end()) {
                reachedBytes += it->second.total();
            }
        }

        const double allMb = double(inRangeBytes) / (1024 * 1024);
        const double reachedMb = double(reachedBytes) / (1024 * 1024);

        std::printf("  distance %2d: %5d of %5d sections reached, "
                    "%6.2f MB of %6.2f MB  (%.0f %% saved)\n",
                    radius, int(set.toMesh.size()), inRangeSections,
                    reachedMb, allMb,
                    allMb > 0 ? 100.0 * (1.0 - reachedMb / allMb) : 0.0);
    }

    // The per-frame draw set, which is a different question from the mesh set:
    // what a real frustum admits from one facing. Sampled around the compass
    // because standing at one yaw would be cherry-picking.
    std::printf("\ndrawn per frame with a real 70-degree frustum, sampled every 45 degrees\n");

    for (int radius : {6, 8}) {
        render::SectionField field;
        field.reset(radius);
        field.setCentre(playerChunkX, playerChunkZ);
        for (const auto& entry : t.masks) {
            if (field.inRange(entry.first.first, entry.first.second)) {
                field.setColumn(entry.first.first, entry.first.second, entry.second.data());
                for (int sy = 0; sy < world::ChunkColumn::kSectionCount; ++sy) {
                    // Slot 0 stands in for "meshed". This pass is pricing what
                    // a frustum admits, so a section that would mesh to nothing
                    // is deliberately still counted -- it is the same set of
                    // sections either way, and the quad total comes from the
                    // map below, which already has zero for them.
                    field.setMeshSlot(entry.first.first, sy, entry.first.second, 0);
                }
            }
        }

        const Mat4 projection =
            perspective(70.0f * 3.14159265f / 180.0f, 400.0f / 240.0f, 0.1f,
                        float(radius + 1) * 16.0f);

        int minSections = 1 << 30, maxSections = 0, totalSections = 0;
        double minMb = 1e9, maxMb = 0.0, totalMb = 0.0;

        for (int step = 0; step < 8; ++step) {
            const float angle = float(step) * 3.14159265f / 4.0f;
            const Vec3 eye{float(px), float(py), float(pz)};
            const Vec3 dir{std::sin(angle), 0.0f, std::cos(angle)};

            Frustum frustum;
            frustum.setFromViewProjection(multiply(projection, lookAlong(eye, dir)),
                                          ClipRange::NegativeOneToOne);

            render::VisibleSet set;
            render::buildVisibleSet(field, frustum, playerChunkX, playerSectionY,
                                    playerChunkZ, &set);

            usize bytes = 0;
            for (const auto& s : set.draw) {
                auto it = t.meshBySection.find({s.chunkX, s.chunkZ, int(s.sectionY)});
                if (it != t.meshBySection.end()) {
                    bytes += it->second.total();
                }
            }
            const double mb = double(bytes) / (1024 * 1024);

            const int n = int(set.draw.size());
            minSections = n < minSections ? n : minSections;
            maxSections = n > maxSections ? n : maxSections;
            totalSections += n;
            minMb = mb < minMb ? mb : minMb;
            maxMb = mb > maxMb ? mb : maxMb;
            totalMb += mb;
        }

        std::printf("  distance %2d: %3d-%3d sections (mean %3d), "
                    "%.2f-%.2f MB of geometry (mean %.2f)\n",
                    radius, minSections, maxSections, totalSections / 8,
                    minMb, maxMb, totalMb / 8.0);
    }
}

// ------------------------------------------------------------ the VBO pool
//
// Two questions the pool's design rests on, both answered here against the real
// world rather than against an average column.
//
//   1. What geometric size-class ratio to use. Powers of two are the obvious
//      choice and throw away about a third of the budget; a finer ratio costs
//      only a longer table. The right ratio is whatever the actual distribution
//      of section mesh sizes says it is.
//   2. Whether the budget holds a full turn on the spot without thrashing.
//      Turning is the hard case: the draw set changes completely while the
//      *reachable* set does not move at all, so a pool that evicts what falls
//      behind the camera has to hand it all straight back a second later.

// Every non-empty section's mesh size, which is what the size classes are
// rounding up.
std::vector<usize> meshSizes(const Totals& t)
{
    std::vector<usize> sizes;
    sizes.reserve(t.meshBySection.size());
    for (const auto& entry : t.meshBySection) {
        if (entry.second.total() != 0) {
            sizes.push_back(entry.second.total());
        }
    }
    return sizes;
}

// The host stands in for VRAM with ordinary memory. Only the arithmetic is
// being measured here -- how much of each tier a real view needs -- and that is
// the same on both targets.
class HostVboAllocator : public render::VboAllocator {
public:
    void* allocate(usize bytes, render::VboTier) override { return std::malloc(bytes); }
    void release(void* pointer, usize, render::VboTier) override { std::free(pointer); }
};

void sizeClassSweep(const Totals& t)
{
    const std::vector<usize> sizes = meshSizes(t);
    usize exact = 0;
    for (usize size : sizes) {
        exact += size;
    }

    std::printf("\nsize-class waste over %zu real section meshes (%.2f MB of geometry)\n",
                sizes.size(), double(exact) / (1024.0 * 1024.0));
    std::printf("  ratio  classes   rounded up to   waste\n");

    const usize largest =
        usize(mesh::kMaxQuadsPerSection) * 4 * sizeof(mesh::WorldVertex);

    for (double ratio : {2.0, 1.5, 1.33, 1.25, 1.15, 1.1, 1.05, 1.02}) {
        render::SizeClasses classes;
        if (!classes.build(render::kSmallestSizeClass, largest, ratio)) {
            std::printf("  %.2f   %3d       (ran out of classes before the largest "
                        "section)\n",
                        ratio, classes.count());
            continue;
        }

        usize rounded = 0;
        for (usize size : sizes) {
            rounded += classes.bytes(classes.classFor(size));
        }
        std::printf("  %.2f   %3d      %8.2f MB     %5.1f %%\n", ratio, classes.count(),
                    double(rounded) / (1024.0 * 1024.0),
                    exact > 0 ? 100.0 * double(rounded - exact) / double(exact) : 0.0);
    }
    std::printf("  (the smallest class is 2 KB, so a tiny mesh rounds up to it)\n");
}

// Spins on the spot through several revolutions with a real frustum, uploading
// what comes into view and touching what stays. Reports what the pool did.
struct PoolRun {
    usize peakResident = 0;
    usize peakReserved = 0;
    int worstBacklog = 0;
    render::VboPool::Stats stats;
};

PoolRun poolUnderRotation(const Totals& t, int radius, double budgetMb, double ratio,
                          i32 playerChunkX, int playerSectionY, i32 playerChunkZ,
                          double px, double py, double pz)
{
    constexpr double kVramForVertices = 4.5;  // MB, measured -- 3ds-performance.md section 3
    const double linearMb = budgetMb > kVramForVertices ? budgetMb - kVramForVertices : 0.0;

    HostVboAllocator allocator;

    render::ChunkRendererConfig config;
    config.meshDistance = radius;
    config.budget = {usize(kVramForVertices * 1024 * 1024), usize(linearMb * 1024 * 1024)};
    config.sizeClassRatio = ratio;
    // No per-frame cap: this run is pricing the pool's ceiling, and a mesh
    // budget would stop it ever being reached.
    config.meshBudgetPerFrame = 0;

    render::ChunkRenderer renderer;
    renderer.reset(&allocator, config);
    renderer.setCentre(playerChunkX, playerChunkZ);
    for (const auto& entry : t.masks) {
        renderer.publishColumn(entry.first.first, entry.first.second, entry.second.data());
    }

    std::vector<u8> staging(usize(mesh::kMaxQuadsPerSection) * 4 * sizeof(mesh::WorldVertex));

    const Mat4 projection = perspective(70.0f * 3.14159265f / 180.0f, 400.0f / 240.0f,
                                        0.1f, float(radius + 1) * 16.0f);

    constexpr int kStepsPerTurn = 16;
    constexpr int kTurns = 3;
    usize peakResident = 0;
    usize peakReserved = 0;
    int worstBacklog = 0;

    for (int step = 0; step < kStepsPerTurn * kTurns; ++step) {
        const float angle = float(step) * 2.0f * 3.14159265f / float(kStepsPerTurn);
        Frustum frustum;
        frustum.setFromViewProjection(
            multiply(projection, lookAlong(Vec3{float(px), float(py), float(pz)},
                                           Vec3{std::sin(angle), 0.0f, std::cos(angle)})),
            ClipRange::NegativeOneToOne);

        renderer.beginFrame(u32(step + 1), frustum, playerChunkX, playerSectionY,
                            playerChunkZ);

        // The queue is nearest first, so VRAM fills with the closest geometry
        // without the pool sorting anything. The meshes are not built here --
        // their sizes came out of the mesh pass above -- but those sizes are
        // the real ones, both streams included, which is all the pool sees.
        for (const render::VisibleSection& s : renderer.meshQueue()) {
            auto it = t.meshBySection.find({s.chunkX, s.chunkZ, int(s.sectionY)});
            const mesh::MeshRanges ranges =
                it == t.meshBySection.end() ? mesh::MeshRanges{} : it->second;
            if (ranges.total() == 0) {
                renderer.noteEmptySection(s.chunkX, int(s.sectionY), s.chunkZ);
                continue;
            }
            renderer.uploadSection(s.chunkX, int(s.sectionY), s.chunkZ, staging.data(), ranges);
        }

        const render::ChunkRenderer::FrameStats& frame = renderer.frameStats();
        worstBacklog = frame.refused > worstBacklog ? frame.refused : worstBacklog;
        peakResident = renderer.pool().stats().resident > peakResident
                           ? renderer.pool().stats().resident
                           : peakResident;
        peakReserved = renderer.pool().stats().reserved > peakReserved
                           ? renderer.pool().stats().reserved
                           : peakReserved;
    }

    PoolRun run;
    run.peakResident = peakResident;
    run.peakReserved = peakReserved;
    run.worstBacklog = worstBacklog;
    run.stats = renderer.pool().stats();
    return run;
}

void reportRun(const char* label, double budgetMb, const PoolRun& run, int frames)
{
    constexpr double kVramForVertices = 4.5;
    const double linearMb = budgetMb > kVramForVertices ? budgetMb - kVramForVertices : 0.0;
    const render::VboPool::Stats& s = run.stats;
    const double mb = 1024.0 * 1024.0;

    std::printf("  %s, %4.1f MB budget: peak %5.2f MB resident, %5.2f MB held (%.0f %% "
                "size-class slack)\n",
                label, budgetMb, double(run.peakResident) / mb,
                double(run.peakReserved) / mb,
                run.peakResident > 0 ? 100.0
                                           * double(run.peakReserved - run.peakResident)
                                           / double(run.peakResident)
                                     : 0.0);
    std::printf("      %u uploads over %d frames: %u recycled a block, %u reached the "
                "allocator, %u evictions, %u refused\n",
                s.uploads, frames, s.reused, s.allocations, s.evictions, s.failures);
    std::printf("      VRAM %5.2f of %4.1f MB, linear %5.2f of %4.1f MB, worst frame "
                "refused %d\n",
                double(s.reservedByTier[int(render::VboTier::Vram)]) / mb, kVramForVertices,
                double(s.reservedByTier[int(render::VboTier::Linear)]) / mb, linearMb,
                run.worstBacklog);
}

void vboPool(const Totals& t, const world::LevelData& level)
{
    sizeClassSweep(t);

    const double px = level.player.present ? level.player.pos[0] : double(level.spawnX);
    const double py = level.player.present ? level.player.pos[1] : double(level.spawnY);
    const double pz = level.player.present ? level.player.pos[2] : double(level.spawnZ);
    const i32 playerChunkX = i32(std::floor(px / 16.0));
    const i32 playerChunkZ = i32(std::floor(pz / 16.0));
    const int playerSectionY = int(std::floor(py / 16.0));
    constexpr int kFrames = 48;

    // Waste is only half the story. Finer classes round less away but match a
    // freed block to a new mesh less often, and every miss is allocator
    // traffic -- the thing size classes exist to avoid. Both are measured on
    // the tightest configuration the game has to survive: an old 3DS at render
    // distance 8, where the pool runs at its ceiling.
    std::printf("\nsize-class ratio under load (distance 8, 12 MB, %d frames of turning)\n",
                kFrames);
    std::printf("  ratio  classes   held    real geometry   recycled   allocator   "
                "evictions\n");
    for (double ratio : {2.0, 1.5, 1.33, 1.25, 1.15, 1.1}) {
        render::SizeClasses classes;
        classes.build(render::kSmallestSizeClass,
                      usize(mesh::kMaxQuadsPerSection) * 4 * sizeof(mesh::WorldVertex),
                      ratio);
        const PoolRun run = poolUnderRotation(t, 8, 12.0, ratio, playerChunkX,
                                              playerSectionY, playerChunkZ, px, py, pz);
        std::printf("  %.2f   %3d    %5.2f MB    %5.2f MB      %5.1f %%   %9u   %9u\n",
                    ratio, classes.count(),
                    double(run.peakReserved) / (1024.0 * 1024.0),
                    double(run.peakResident) / (1024.0 * 1024.0),
                    run.stats.uploads > 0
                        ? 100.0 * double(run.stats.reused) / double(run.stats.uploads)
                        : 0.0,
                    run.stats.allocations, run.stats.evictions);
    }

    std::printf("\nVBO pool through 3 full turns on the spot, ratio %.2f\n",
                render::kSizeClassRatio);
    struct Case {
        const char* label;
        int radius;
        double budget;
    };
    for (const Case& c : {Case{"o3DS distance  6", 6, 12.0},
                          Case{"o3DS distance  8", 8, 12.0},
                          Case{"n3DS distance  8", 8, 32.0},
                          Case{"n3DS distance 10", 10, 32.0}}) {
        const PoolRun run =
            poolUnderRotation(t, c.radius, c.budget, render::kSizeClassRatio, playerChunkX,
                              playerSectionY, playerChunkZ, px, py, pz);
        reportRun(c.label, c.budget, run, kFrames);
    }
}

// ------------------------------------------------------------------ --fly
//
// The whole render pipeline, minus the GPU, over a real world: open it, stream
// columns in around a moving camera, walk, mesh, upload, evict. Everything the
// console does between reading the SD card and calling C3D_DrawElements runs
// here, under sanitizers, with the output being numbers instead of pixels.
//
// This exists because the alternative is finding a streaming bug by squinting
// at a 240-line screen. A column published before its neighbours arrive, a slot
// leaked when the grid wraps, a section that meshes to nothing and comes back
// every frame -- all of them are visible in these counters and none of them
// needs hardware.

// The 12 MB / 32 MB split docs/architecture.md budgets, with the VRAM tier
// standing in as ordinary memory -- only the arithmetic is being exercised.
render::VboPool::Budget flyBudget(int distance)
{
    return {usize(4.5 * 1024 * 1024), usize((distance >= 10 ? 27.5 : 7.5) * 1024 * 1024)};
}

// Walk a real world with a real body, under the sanitizers.
//
// **This is the only place the physics meets terrain that nobody designed.**
// tests/player_body_test.cpp proves the body agrees with the jar tick for tick,
// but it proves it on eleven hand-built scenes made of plates and walls. A
// world has overhangs, one-block gaps, sand over caves, water, and a coastline;
// what this looks for is the class of failure a fixture cannot contain --
// falling through the floor, walking into geometry and stopping dead, a
// position going NaN, or the body leaving the world entirely.
//
// It is a **check, not a measurement**: it returns non-zero when something went
// wrong, so it can sit in a script. None of the numbers it prints belong in
// docs/status.md's measured table.
//
// The walk goes straight for ten seconds at a time and then turns, rather than
// curving continuously: a constant turn rate is a circle, and a circle covers
// one hillside forever. It jumps about twice a second, not constantly -- a jump
// takes thirteen ticks to land, so jumping every twelve leaves the body
// permanently airborne and quietly stops testing the thing it is here to test.
//
// **And it turns away when it stops making progress**, which is not politeness
// to the terrain but the difference between a test and a stuck body: a first
// run fell into a ravine at tick ~300 and spent the remaining 1,700 pressed
// against the same wall, still reporting "ok" while exercising nothing.
//
// **Point it at a copy.** `open()` writes session.lock and `close()` rewrites
// level.dat, like every other mode here.
int walk(const char* worldDir, int distance, int ticks, bool generate)
{
    HostVboAllocator allocator;

    render::ChunkRendererConfig config;
    config.meshDistance = distance;
    config.budget = flyBudget(distance);
    // Meshing is not what this exercises, but it cannot be zero: the streamer
    // publishes through the renderer, and a column that never meshes never
    // reports itself resident.
    config.meshBudgetPerFrame = 4;

    render::ChunkRenderer renderer;
    renderer.reset(&allocator, config);

    render::WorldStreamer streamer;
    // Without this the walk is bounded by whatever was generated before, and
    // stepping past that edge is a fall into a world that has no floor because
    // it has no chunks -- see the absent-column note in the loop below.
    streamer.setGenerateMissing(generate);
    if (!streamer.open(worldDir, distance, nowMillis())) {
        std::printf("cannot open %s\n", worldDir);
        return 2;
    }

    // The same two meanings spawnPosition has on the console: level.dat's
    // Pos[1] is a1.1.2's posY and sits 1.62 above the feet, while spawnY is a
    // block coordinate and is the feet already. See docs/physics-a1.1.2.md.
    double spawnX = 0.0;
    double spawnY = 0.0;
    double spawnZ = 0.0;
    streamer.spawnPosition(&spawnX, &spawnY, &spawnZ);
    const double feetY = streamer.level().player.present
                             ? spawnY - double(entity::kEyeHeight)
                             : spawnY;

    entity::PlayerBody body;
    body.setFeet(spawnX, feetY, spawnZ);

    std::printf("world      %s\n", worldDir);
    std::printf("spawn      feet %.3f %.3f %.3f, chunk (%d, %d)\n", body.x, body.y, body.z,
                int(std::floor(body.x / 16.0)), int(std::floor(body.z / 16.0)));
    std::printf("distance   %d chunks, %d ticks\n", distance, ticks);
    std::printf("\n");

    // The streamer's per-frame budget, not the pool's -- two different things
    // with the same name. Two columns a frame is the console's rate.
    render::WorldStreamer::Budget budget;
    budget.columnsPerFrame = 2;

    // Let the streamer settle before the first step, or the body spends its
    // opening ticks falling through chunks that have not arrived. The console
    // covers this with a loading screen; here it is just a wait.
    for (int i = 0; i < 400; ++i) {
        Frustum frustum;
        frustum.setOrigin(body.chunkX(), body.chunkZ());
        renderer.beginFrame(u32(i), frustum, body.chunkX(), int(std::floor(body.y / 16.0)),
                            body.chunkZ());
        streamer.update(renderer, body.chunkX(), body.chunkZ(), budget);
    }

    tick::TickWorld* world = streamer.worldTick();
    if (world == nullptr) {
        std::printf("no tick world\n");
        return 2;
    }

    // A world with no saved player hands back `spawnY`, a block coordinate that
    // can leave the body buried -- and a buried body cannot walk in any
    // direction, which looks exactly like broken physics.
    const int lifted = body.liftOutOfGround(*world);
    if (lifted > 0) {
        std::printf("spawn      was inside the ground; lifted %d block%s to feet %.3f\n",
                    lifted, lifted == 1 ? "" : "s", body.y);
    }

    const double startX = body.x;
    const double startZ = body.z;
    double lowest = body.y;
    double highestFall = 0.0;
    int grounded = 0;
    int airborne = 0;
    int stuck = 0;
    int failures = 0;
    double travelled = 0.0;
    int stalledRun = 0;
    int escapes = 0;

    for (int t = 0; t < ticks; ++t) {
        // Straight for 200 ticks, then a turn that is not a fraction of a
        // circle, so the path wanders instead of closing on itself. `escapes`
        // adds to it whenever the body has been getting nowhere.
        const float yaw = float(t / 200) * 37.0f + float(escapes) * 53.0f;

        entity::PlayerInput input;
        input.strafe = 0.0f;
        input.forward = 1.0f;
        input.yawDegrees = yaw;
        // Often enough to keep exercising the launch and the landing, rarely
        // enough that most ticks are still a walk.
        input.jump = (t % 40) == 0;
        input.sneak = false;

        const double beforeX = body.x;
        const double beforeZ = body.z;

        body.tick(*world, input);

        Frustum frustum;
        frustum.setOrigin(body.chunkX(), body.chunkZ());
        renderer.beginFrame(u32(t + 1000), frustum, body.chunkX(),
                            int(std::floor(body.y / 16.0)), body.chunkZ());
        streamer.update(renderer, body.chunkX(), body.chunkZ(), budget);

        if (body.onGround) {
            ++grounded;
        } else {
            ++airborne;
        }
        if (body.y < lowest) {
            lowest = body.y;
        }
        if (double(body.fallDistance) > highestFall) {
            highestFall = double(body.fallDistance);
        }

        const double moved = (body.x - beforeX) * (body.x - beforeX)
                           + (body.z - beforeZ) * (body.z - beforeZ);
        travelled += std::sqrt(moved);
        if (moved < 1e-9) {
            ++stuck;
            ++stalledRun;
            // Twenty ticks of no progress is a wall, not a pause. Turn.
            if (stalledRun >= 20) {
                ++escapes;
                stalledRun = 0;
            }
        } else {
            stalledRun = 0;
        }

        // The three things a fixture cannot catch.
        if (!(body.x == body.x) || !(body.y == body.y) || !(body.z == body.z)) {
            std::printf("FAIL tick %d: position is NaN\n", t);
            ++failures;
            break;
        }
        if (body.y < 0.0) {
            // **Two very different things look the same from here.** Falling
            // through a floor that exists is a physics bug. Falling because the
            // column underneath was never loaded is the original's own
            // behaviour, faithfully reproduced -- getCollidingBoundingBoxes
            // contributes nothing for an absent chunk -- and it means the walk
            // outran the world rather than that the body is wrong. Only the
            // first is a failure.
            const bool loaded = world->chunkResident(body.chunkX(), body.chunkZ());
            if (loaded) {
                std::printf("FAIL tick %d: fell through loaded ground at %.3f %.3f %.3f\n",
                            t, body.x, body.y, body.z);
                ++failures;
            } else {
                std::printf("stopped tick %d: walked off the edge of the generated world "
                            "at %.3f %.3f -- pass `gen` to keep going\n",
                            t, body.x, body.z);
            }
            break;
        }
        if (body.y > 256.0) {
            std::printf("FAIL tick %d: left the world upwards at %.3f\n", t, body.y);
            ++failures;
            break;
        }
    }

    const double dx = body.x - startX;
    const double dz = body.z - startZ;
    std::printf("end        feet %.3f %.3f %.3f\n", body.x, body.y, body.z);
    std::printf("walked     %.1f blocks of path, %.1f from where it started\n",
                travelled, std::sqrt(dx * dx + dz * dz));
    std::printf("lowest y   %.3f\n", lowest);
    std::printf("longest fall %.2f blocks\n", highestFall);
    std::printf("on ground  %d ticks, airborne %d\n", grounded, airborne);
    std::printf("stalled    %d ticks, turned away %d times\n", stuck, escapes);

    streamer.close(nowMillis());

    // Never leaving the ground over a long walk means the body never fell, which
    // on real terrain means it is not being asked anything. Never touching it
    // means it never landed.
    const bool ranToCompletion = grounded + airborne == ticks;
    if (ranToCompletion && (grounded == 0 || airborne == 0)) {
        std::printf("FAIL: the walk never %s\n", grounded == 0 ? "landed" : "left the ground");
        ++failures;
    }
    // Mostly airborne means the body is falling somewhere, not walking, and
    // every ground-contact path -- friction, the step up, the landing -- has
    // stopped being exercised without anything having failed.
    if (ranToCompletion && airborne > grounded) {
        std::printf("FAIL: airborne for %d of %d ticks; this is a fall, not a walk\n",
                    airborne, ticks);
        ++failures;
    }
    if (ranToCompletion && stuck == ticks) {
        std::printf("FAIL: the body never moved at all\n");
        ++failures;
    }

    std::printf("\n%s\n", failures == 0 ? "walk ok" : "walk FAILED");
    return failures == 0 ? 0 : 1;
}

// **Survival, on a real world rather than on a fixture.** `--walk` proves the
// body moves; this proves the three rules laid on top of it hurt, resist and
// drop the way a1.1.2's do, against terrain nobody built for the test.
//
// Three things it refuses to let pass, which is the whole reason it exists:
//
//   * a fall of more than three blocks that costs no health (`ge.c(F)V`)
//   * water the player can stand in for ever (`ge.y()`'s air counter)
//   * stone broken with a bare hand that leaves cobblestone on the ground
//     (`dm.b(Lly;)Z` -- canHarvestBlock, and a pickaxe is the only way)
//
// It runs on a **copy** of a world, like every other harness mode here, and it
// does write blocks: it needs a cell of water and a cell of stone at a known
// place and a real world does not promise either within reach. Both are put
// back after the check, so what is on the card at the end is what came off it
// -- but the copy is still what it is run on.
int survive(const char* worldDir, int ticks, bool generate)
{
    HostVboAllocator allocator;

    render::ChunkRendererConfig config;
    config.meshDistance = 4;
    config.budget = flyBudget(4);
    config.meshBudgetPerFrame = 4;

    render::ChunkRenderer renderer;
    renderer.reset(&allocator, config);

    render::WorldStreamer streamer;
    streamer.setGenerateMissing(generate);
    if (!streamer.open(worldDir, 4, nowMillis())) {
        std::printf("cannot open %s\n", worldDir);
        return 2;
    }

    double spawnX = 0.0;
    double spawnY = 0.0;
    double spawnZ = 0.0;
    streamer.spawnPosition(&spawnX, &spawnY, &spawnZ);
    const double feetY = streamer.level().player.present
                             ? spawnY - double(entity::kEyeHeight)
                             : spawnY;

    entity::PlayerBody body;
    body.setFeet(spawnX, feetY, spawnZ);

    render::WorldStreamer::Budget budget;
    budget.columnsPerFrame = 2;
    for (int i = 0; i < 400; ++i) {
        Frustum frustum;
        frustum.setOrigin(body.chunkX(), body.chunkZ());
        renderer.beginFrame(u32(i), frustum, body.chunkX(), int(std::floor(body.y / 16.0)),
                            body.chunkZ());
        streamer.update(renderer, body.chunkX(), body.chunkZ(), budget);
    }

    tick::TickWorld* world = streamer.worldTick();
    if (world == nullptr) {
        std::printf("no tick world\n");
        return 2;
    }
    body.liftOutOfGround(*world);

    std::printf("world      %s\n", worldDir);
    std::printf("spawn      feet %.3f %.3f %.3f\n", body.x, body.y, body.z);
    std::printf("\n");

    // What a break leaves on the ground. Counted rather than spawned: the
    // entity pool is the console's, and what is being checked is whether the
    // drop happened at all.
    static int droppedItem = 0;
    static int droppedCount = 0;
    droppedItem = 0;
    droppedCount = 0;
    world->setDropSink(
        [](void*, double, double, double, u16 id, int count) {
            droppedItem = int(id);
            droppedCount += count;
        },
        nullptr);

    item::Inventory inventory;
    entity::PlayerVitals vitals(1234);
    entity::PlayerContext ctx{*world, body, inventory, nullptr, 2, 0.0f};
    int failures = 0;

    // ---- The fall. **Ten blocks of air made rather than found**: a real
    // world's spawn can be under a tree or in a cave, and a drop that lands on
    // a branch two blocks down tests nothing. The column over the highest solid
    // block at the body's own (x, z) is emptied, the body dropped down it, and
    // every block put back exactly as it was.
    const i32 fx = i32(std::floor(body.x));
    const i32 fz = i32(std::floor(body.z));
    constexpr int kDropHeight = 12;
    int topY = int(std::floor(body.y)) - 1;
    for (int y = 120; y > 0; --y) {
        if (world->opaqueAt(fx, y, fz)) {
            topY = y;
            break;
        }
    }
    block::BlockId wasColumn[kDropHeight];
    for (int i = 0; i < kDropHeight; ++i) {
        wasColumn[i] = world->blockAt(fx, topY + 1 + i, fz);
        world->setBlockWithNotify(fx, topY + 1 + i, fz, block::kAir);
    }

    body.setFeet(double(fx) + 0.5, double(topY + kDropHeight), double(fz) + 0.5);
    body.motionY = 0.0;
    body.fallDistance = 0.0f;
    body.landedFall = 0.0f;
    int fell = 0;
    for (int t = 0; t < 200 && body.landedFall == 0.0f; ++t) {
        entity::PlayerInput input;
        input.yawDegrees = 0.0f;
        body.tick(*world, input);
        ++fell;
    }
    const float fallDistance = body.landedFall;
    const entity::Harm fallHarm = vitals.fall(ctx, fallDistance);
    body.landedFall = 0.0f;
    for (int i = kDropHeight - 1; i >= 0; --i) {
        world->setBlockWithNotify(fx, topY + 1 + i, fz, wasColumn[i]);
    }
    std::printf("fall       %.2f blocks over %d ticks, health %d\n", double(fallDistance), fell,
                vitals.health);
    if (fallDistance <= entity::kPlayerSafeFall) {
        std::printf("FAIL: a %d-block drop banked only %.2f blocks of fall\n", kDropHeight,
                    double(fallDistance));
        ++failures;
    } else if (!fallHarm.landed || vitals.health >= entity::kPlayerMaxHealth) {
        std::printf("FAIL: a %.2f-block fall cost no health\n", double(fallDistance));
        ++failures;
    }
    vitals.respawn();

    // ---- Drowning. A cell of water over the body's head, held until the air
    // runs out. `ge.y()` drains one a tick from 300 and deals two at -20, so
    // the whole of it is 320 ticks and the budget has to clear that.
    const i32 wx = i32(std::floor(body.x));
    const i32 wz = i32(std::floor(body.z));
    const int wy = int(std::floor(body.y)) + 1;
    const block::BlockId wasHead = world->blockAt(wx, wy, wz);
    const block::BlockId wasAbove = world->blockAt(wx, wy + 1, wz);
    world->setBlockWithNotify(wx, wy, wz, block::BlockId(mcver::Block::Water));
    world->setBlockWithNotify(wx, wy + 1, wz, block::BlockId(mcver::Block::Water));
    const bool eyeWet = entity::playerEyeInWater(*world, body);
    int airTicks = 0;
    const int airBudget = ticks > 400 ? ticks : 400;
    while (airTicks < airBudget && vitals.health >= entity::kPlayerMaxHealth) {
        vitals.tick(ctx, true);
        ++airTicks;
    }
    std::printf("drowning   eye in water %s, air %d after %d ticks, health %d\n",
                eyeWet ? "yes" : "no", vitals.air, airTicks, vitals.health);
    if (!eyeWet) {
        std::printf("FAIL: two cells of water over the feet and the eye is dry\n");
        ++failures;
    } else if (vitals.health >= entity::kPlayerMaxHealth) {
        std::printf("FAIL: %d ticks under water and nothing drowned\n", airTicks);
        ++failures;
    }
    world->setBlockWithNotify(wx, wy + 1, wz, wasAbove);
    world->setBlockWithNotify(wx, wy, wz, wasHead);
    vitals.respawn();

    // ---- Stone, by hand and then with a pickaxe. The cell is two blocks in
    // front of the feet at eye level, which is inside reach and outside the
    // body.
    const i32 sx = wx + 2;
    const int sy = wy;
    const block::BlockId wasStone = world->blockAt(sx, sy, wz);
    const item::Effects effects;

    struct Attempt {
        const char* what;
        item::ItemId held;
        int ticksTaken;
        int drop;
        int count;
        bool broke;
    };
    Attempt attempts[2] = {{"hand", item::ItemId(0), 0, 0, 0, false},
                           {"stone pickaxe", item::ItemId(mcver::Item::StonePickaxe), 0, 0, 0,
                            false}};

    for (Attempt& attempt : attempts) {
        world->setBlockWithNotify(sx, sy, wz, block::BlockId(mcver::Block::Stone));
        inventory.clear();
        if (attempt.held != 0) {
            inventory.set(0, attempt.held, 1);
        }
        inventory.selected = 0;
        droppedItem = 0;
        droppedCount = 0;

        item::BlockBreaker breaker;
        item::BreakContext breakCtx{*world, inventory, effects, false, true};
        // `nj.a(IIII)V` first -- a block soft enough goes on the click -- then
        // one `c(IIII)V` a tick until it gives.
        attempt.broke = breaker.click(breakCtx, sx, sy, wz, 1);
        while (!attempt.broke && attempt.ticksTaken < 400) {
            breaker.update();
            attempt.broke = breaker.damage(breakCtx, sx, sy, wz, 1);
            ++attempt.ticksTaken;
        }
        attempt.drop = droppedItem;
        attempt.count = droppedCount;
        std::printf("stone      %-13s %3d ticks, %s, dropped %d x %d, wear %d\n", attempt.what,
                    attempt.ticksTaken, attempt.broke ? "broke" : "DID NOT BREAK", attempt.count,
                    attempt.drop, int(inventory.at(0).damage));
    }
    world->setBlockWithNotify(sx, sy, wz, wasStone);

    if (!attempts[0].broke || !attempts[1].broke) {
        std::printf("FAIL: stone did not break within 400 ticks\n");
        ++failures;
    }
    if (attempts[0].ticksTaken <= 1) {
        std::printf("FAIL: stone broke instantly; there is no break progress\n");
        ++failures;
    }
    if (attempts[0].count != 0) {
        std::printf("FAIL: stone broken by hand dropped %d x %d\n", attempts[0].count,
                    attempts[0].drop);
        ++failures;
    }
    if (attempts[1].drop != int(mcver::Block::Cobblestone) || attempts[1].count == 0) {
        std::printf("FAIL: stone broken with a pickaxe dropped %d x %d, not cobblestone\n",
                    attempts[1].count, attempts[1].drop);
        ++failures;
    }
    if (attempts[1].ticksTaken >= attempts[0].ticksTaken) {
        std::printf("FAIL: the pickaxe was no faster than the hand (%d vs %d ticks)\n",
                    attempts[1].ticksTaken, attempts[0].ticksTaken);
        ++failures;
    }
    if (inventory.at(0).damage <= 0) {
        std::printf("FAIL: the pickaxe broke a block and took no damage\n");
        ++failures;
    }

    // The sink is a local lambda and the streamer outlives this scope only as
    // far as close(), but a dangling function pointer is not something to leave
    // lying about.
    world->setDropSink(nullptr, nullptr);
    streamer.close(nowMillis());

    std::printf("\n%s\n", failures == 0 ? "survive ok" : "survive FAILED");
    return failures == 0 ? 0 : 1;
}

void fly(const char* worldDir, int distance, int frames, int switchTo,
         mesh::CubeFormat cubeFormat, bool flipFormat, bool generate, bool cacheThreaded,
         int prefetchRings, world::WorldFormat createAs)
{
    HostVboAllocator allocator;

    render::ChunkRendererConfig config;
    config.meshDistance = distance;
    config.budget = flyBudget(distance);
    config.meshBudgetPerFrame = 4;

    render::ChunkRenderer renderer;
    renderer.reset(&allocator, config);

    render::WorldStreamer streamer;

    // `gen` makes this the console's configuration rather than the harness's:
    // a missing chunk is generated and written back instead of counted absent.
    // Off by default, because every documented --fly invocation measures a
    // fixed world and must keep reporting the numbers it always did -- and
    // because a streamer that generates writes chunk files into whatever
    // directory it was pointed at.
    //
    // The world is created if there is none, so `--fly <empty-dir> 8 400 gen`
    // is the whole recipe for exercising generation, streaming, meshing and
    // saving end to end under sanitizers.
    // **The console's chunk cache, on the harness too.** `--fly` is the one
    // place the streaming path runs under sanitizers, so the threaded cache and
    // the read-ahead band have to run here or they are never checked; and
    // ThreadSanitizer has already earned its keep on this class once.
    //
    // The cap is the console's New 3DS number, so a host run fills and evicts
    // at the same point a console does rather than never evicting at all.
    world::ChunkCache::Config cacheConfig;
    cacheConfig.cleanCapBytes = 8u << 20;
    cacheConfig.threaded = cacheThreaded;

    // **`MC_IO_LATENCY_US` is how a console's card is put on a dev host.**
    //
    // A chunk read here comes out of the page cache in tens of microseconds; on
    // the console it is four to six IPC round trips to the FS sysmodule. Every
    // scheduling question in ChunkCache -- which queue starves which, when the
    // dirty backstop bites, how long classification takes to catch up -- is
    // decided by that ratio and by nothing else, so without this knob they are
    // only answerable on hardware. 4000 is about what a card measures per
    // operation; 0, the default, keeps every documented number where it was.
    if (const char* latency = std::getenv("MC_IO_LATENCY_US")) {
        cacheConfig.opLatencyMicros = u32(std::atoi(latency));
    }
    streamer.setCacheConfig(cacheConfig);
    streamer.setPrefetchRings(prefetchRings);
    // Seconds, and deliberately short by default: a --fly run is a handful of
    // wall-clock seconds, and the console's interval would never fire once.
    //
    // `MC_FLY_AUTOSAVE` overrides it, and the case it is there for is the
    // opposite one: setting it past the length of the run measures what a
    // console holds *between* autosaves, which is where the memory backstop on
    // dirty columns either works or does not.
    const char* autosave = std::getenv("MC_FLY_AUTOSAVE");
    streamer.setAutosaveSeconds(autosave != nullptr ? std::atoi(autosave) : 1);

    // **How long a frame is allowed to take, which is what turns a frame count
    // into wall time.** The default of 1 ms keeps every documented `--fly`
    // number where it was; `MC_FLY_FRAME_MS=33` is the console's refresh rate,
    // and it is the setting that makes the per-frame budgets mean what they
    // mean on hardware -- two columns adopted per frame is 60 a second at 30 Hz
    // and 2,000 a second here. Paired with `MC_IO_LATENCY_US` it is the whole
    // of the console's pacing: a slow card and a slow frame.
    const char* framePace = std::getenv("MC_FLY_FRAME_MS");
    const int frameMillis = framePace != nullptr && std::atoi(framePace) > 0
                                ? std::atoi(framePace)
                                : 1;

    if (generate) {
        streamer.setGenerateMissing(true);
        io::PosixFileSystem fs;
        // AnyStorage rather than the slot directly: an existing world opens in
        // whichever shape it is already in, and only a world being *made* here
        // needs a format named.
        //
        // **Folder unless asked otherwise**, deliberately -- the console makes
        // packed worlds, but every documented --fly number in
        // docs/3ds-performance.md was taken on a folder world, and a default
        // that quietly changed the layout under them would move the table.
        world::AnyStorage probe(fs);
        if (probe.open(worldDir, nowMillis()) == world::OpenResult::Ok) {
            probe.close(nowMillis());
        } else if (probe.create(worldDir, 1234567890LL, nowMillis(), createAs)
                   != world::OpenResult::Ok) {
            std::printf("cannot create %s\n", worldDir);
            return;
        } else {
            probe.close(nowMillis());
            std::printf("created    %s (seed 1234567890, %s)\n", worldDir,
                        world::formatName(createAs));
        }
    }

    if (!streamer.open(worldDir, distance, nowMillis())) {
        std::printf("cannot open %s\n", worldDir);
        return;
    }
    // Before anything is published, so this is the format everything is meshed
    // in rather than a switch partway through -- the switch path is what
    // setCubeFormat is for, and it is not what this run is measuring.
    streamer.setCubeFormat(cubeFormat, renderer);

    // Double throughout, like the camera on hardware and like level.dat. This
    // harness flies near spawn where it would make no difference, but it is
    // also the only place the relative-origin path gets exercised under
    // sanitizers, so it uses the same types the console does.
    double px, py, pz;
    streamer.spawnPosition(&px, &py, &pz);
    py += 1.62;

    std::printf("world      %s\n", worldDir);
    std::printf("spawn      %.1f %.1f %.1f, chunk (%d, %d)\n", px, py, pz,
                int(std::floor(px / 16.0)), int(std::floor(pz / 16.0)));
    std::printf("distance   %d chunks, %d frames\n", distance, frames);
    std::printf("cubes      %s\n", cubeFormat == mesh::CubeFormat::Quads
                                       ? "geoshader, 8 bytes a quad"
                                       : "4 x 12-byte vertices a quad");
    if (switchTo > 0) {
        std::printf("switch     to distance %d at frame %d\n", switchTo, frames / 2);
    }
    std::printf("\n");

    Mat4 projection = perspective(70.0f * 3.14159265f / 180.0f, 400.0f / 240.0f, 0.2f,
                                  float(distance + 1) * 16.0f);

    render::WorldStreamer::Budget budget;
    budget.columnsPerFrame = 2;

    // Three phases, because each one breaks something different:
    //
    //   settle  standing still while the world loads in -- how long the player
    //           waits, and whether the streamer ever catches up at all
    //   turn    spinning on the spot -- the case the VBO pool was sized for,
    //           where the draw set changes completely and the reachable set
    //           does not move at all
    //   walk    moving -- the case that moves the field's centre, so columns
    //           fall out of range and their cells are reused by columns a whole
    //           render distance away. Every slot-leak and stale-cell bug lives
    //           in this phase.
    const int settlePhase = frames / 3;
    const int turnPhase = (frames * 2) / 3;

    usize peakResident = 0;
    int settledFrame = -1;
    usize totalMeshed = 0;
    int worstRefused = 0;

    // **The card on the render thread, per frame rather than per session.**
    //
    // `ChunkCache::Stats::stats` counts the one main-thread storage call left in
    // the design -- the existence check a cell falls back to when its group has
    // not been listed yet -- and the session total hides the shape that matters.
    // A hardware report of the game stopping for a second or two while moving is
    // a *frame* that took dozens of them, so the worst frame is what this
    // measures. See classifyCell.
    u32 lastStats = 0;
    u32 worstStatsFrame = 0;
    int worstStatsAt = 0;

    // **How much finished world is waiting for the card**, at its worst over
    // the run. A dirty column cannot be evicted -- it is the only copy of that
    // part of the world -- so this is the number that answers "what would be
    // full" when a console runs out of heap between autosaves.
    usize peakDirty = 0;
    u32 peakDirtyColumns = 0;

    // **What the card did in this frame, split three ways.** With
    // `MC_IO_LATENCY_US` set these are the only thing spending wall time on the
    // I/O thread, so the shape of a stall reads straight off them: reads
    // outrank listings in `takeJobLocked`, and a row of listings that never
    // gets a turn is classification standing still, which is generation
    // standing still.
    u32 lastReads = 0;
    u32 lastListings = 0;
    u32 lastWrites = 0;

    if (generate) {
        std::printf("frame  loaded  pending  drawn  queued  meshed  quads     pool MB  evict"
                    "  gen-owed  unclassified   rd  ls  wr\n");
    } else {
        std::printf("frame  loaded  pending  drawn  queued  meshed  quads     pool MB  evict\n");
    }

    for (int frame = 1; frame <= frames; ++frame) {
        // **Generation runs on its own thread, so the harness has to give it
        // wall time.** Without this, a host with nothing to render finishes its
        // frame budget in milliseconds and reports a world that has barely
        // started -- which measures how fast an empty frame is and nothing
        // else. A millisecond a frame is not the console's pacing, but it is
        // enough for the run to mean something.
        if (generate) {
            std::this_thread::sleep_for(std::chrono::milliseconds(frameMillis));

            // **The console's save path, exercised under sanitizers.** The
            // camera has no body here, but the position and the world clock go
            // into level.dat exactly as they do on hardware, and the autosave
            // interval is short so a run of a few hundred frames trips it
            // several times rather than never. This is the only place the
            // housekeeping job, the write-back flush and the player round trip
            // run outside a console.
            streamer.setPlayerState(px, py, pz, 0.0f, 0.0f, i64(frame));
            streamer.tickSaves(nowMillis());
        }
        // Changing the render distance mid-flight, which is what the debug
        // settings page does. The order is load-bearing and is the reason this
        // is exercised here rather than only on hardware: the renderer's field
        // has to be rebuilt first, because the streamer republishes every
        // column it kept into whatever field it finds.
        if (switchTo > 0 && frame == frames / 2) {
            config.meshDistance = switchTo;
            config.budget = flyBudget(switchTo);
            renderer.shutdown();
            renderer.reset(&allocator, config);
            streamer.setMeshDistance(switchTo, renderer);

            projection = perspective(70.0f * 3.14159265f / 180.0f, 400.0f / 240.0f, 0.2f,
                                     float(switchTo + 1) * 16.0f);
            std::printf("---- distance now %d, %d columns kept\n", switchTo,
                        streamer.stats().columnsResident);
        }

        // The same shape of change for the cube format, and here for the same
        // reason: this is the path the settings page takes, every mesh in the
        // pool is in the wrong encoding at the moment it runs, and the only
        // thing that catches a mistake in it is a sanitizer over a real world.
        // Nothing is re-read from the SD card -- the columns stay, the geometry
        // is rebuilt.
        if (flipFormat && frame == frames / 2) {
            const mesh::CubeFormat next = cubeFormat == mesh::CubeFormat::Quads
                                              ? mesh::CubeFormat::Vertices
                                              : mesh::CubeFormat::Quads;
            renderer.shutdown();
            renderer.reset(&allocator, config);
            streamer.setCubeFormat(next, renderer);
            cubeFormat = next;
            std::printf("---- cube format now %s, %d columns kept\n",
                        next == mesh::CubeFormat::Quads ? "geoshader" : "4-vertex",
                        streamer.stats().columnsResident);
        }

        // The heading stops rotating once the walk starts, or the player would
        // circle back to where they began and the field's centre would never
        // move -- which is exactly the thing this phase exists to move.
        const int turnFrames = (frame < settlePhase ? 0
                                : frame > turnPhase ? turnPhase - settlePhase
                                                    : frame - settlePhase);
        const float turn = float(turnFrames) * 2.0f * 3.14159265f / 32.0f;

        // 0.4 blocks a frame is 24 a second: the original's sprint, near enough,
        // and fast enough to outrun the streamer if the streamer can be outrun.
        if (frame > turnPhase) {
            px += double(std::sin(turn)) * 0.4;
            pz += double(std::cos(turn)) * 0.4;
        }

        const i32 cameraChunkX = i32(std::floor(px / 16.0));
        const i32 cameraChunkZ = i32(std::floor(pz / 16.0));
        const int cameraSectionY = int(std::floor(py / 16.0));

        // Camera-chunk-relative, the same as the console renderer -- the eye
        // is offset by the origin and the frustum is told to subtract it back
        // out. See Frustum::setOrigin.
        const float eyeX = float(px - double(cameraChunkX) * 16.0);
        const float eyeZ = float(pz - double(cameraChunkZ) * 16.0);

        Frustum frustum;
        frustum.setFromViewProjection(
            multiply(projection, lookAlong(Vec3{eyeX, float(py), eyeZ},
                                           Vec3{std::sin(turn), 0.0f, std::cos(turn)})),
            ClipRange::NegativeOneToOne);
        frustum.setOrigin(cameraChunkX, cameraChunkZ);

        renderer.beginFrame(u32(frame), frustum, cameraChunkX, cameraSectionY, cameraChunkZ);
        streamer.update(renderer, cameraChunkX, cameraChunkZ, budget);

        // **The world tick, under sanitizers, over a real world.** The harness
        // has no clock worth pacing to -- `MC_FLY_FRAME_MS` is a millisecond by
        // default -- so it runs one tick per frame rather than reading a timer.
        // That is not the console's rate and is not meant to be: what this
        // exercises is the tick reaching real columns, changing real blocks and
        // reporting them to the mesher and the saver, which is the part that
        // cannot be tested without a world.
        streamer.stepTicks(renderer, 1);

        const render::ChunkRenderer::FrameStats& stats = renderer.frameStats();
        const render::WorldStreamer::Stats& streaming = streamer.stats();

        usize quads = 0;
        for (const render::VisibleSection& section : renderer.drawList()) {
            quads += renderer.pool().size(section.slot) / (4 * sizeof(mesh::WorldVertex));
        }

        peakResident = renderer.pool().stats().resident > peakResident
                           ? renderer.pool().stats().resident
                           : peakResident;
        totalMeshed += usize(streaming.meshedThisFrame);
        worstRefused = stats.refused > worstRefused ? stats.refused : worstRefused;

        if (streaming.io.dirtyBytes > peakDirty) {
            peakDirty = streaming.io.dirtyBytes;
            peakDirtyColumns = streaming.io.dirtyColumns;
        }

        const u32 statsThisFrame = streaming.io.stats - lastStats;
        lastStats = streaming.io.stats;
        if (statsThisFrame > worstStatsFrame) {
            worstStatsFrame = statsThisFrame;
            worstStatsAt = frame;
        }

        // The world has settled when nothing is left to load and nothing is
        // left to mesh. How long that takes is the loading screen's length.
        if (settledFrame < 0 && streaming.pendingColumns == 0 && stats.queued == 0
            && streaming.pendingGeneration == 0 && streamer.generationIdle()) {
            settledFrame = frame;
        }

        if (frame <= 5 || frame % 20 == 0 || frame == frames) {
            std::printf("%5d  %6d  %7d  %5d  %6d  %6d  %7zu  %6.2f  %5u", frame,
                        streaming.columnsResident, streaming.pendingColumns, stats.drawn,
                        stats.queued, streaming.meshedThisFrame, quads,
                        double(renderer.pool().stats().resident) / (1024.0 * 1024.0),
                        renderer.pool().stats().evictions);
            if (generate) {
                std::printf("  %8d  %12d  %3u %3u %3u%s", streaming.pendingGeneration,
                            streaming.unclassified, streaming.io.reads - lastReads,
                            streaming.io.listings - lastListings,
                            streaming.io.writes - lastWrites,
                            streaming.generationGated ? "  GATED" : "");
            }
            std::printf("\n");
            lastReads = streaming.io.reads;
            lastListings = streaming.io.listings;
            lastWrites = streaming.io.writes;
        }
    }

    const render::VboPool::Stats& pool = renderer.pool().stats();
    std::printf("\npeak resident   %.2f MB of %.1f MB budget\n",
                double(peakResident) / (1024.0 * 1024.0),
                double(config.budget.vram + config.budget.linear) / (1024.0 * 1024.0));
    std::printf("uploads         %u (%u recycled, %u reached the allocator)\n", pool.uploads,
                pool.reused, pool.allocations);
    std::printf("evictions       %u, refused %u (worst frame %d)\n", pool.evictions, pool.failures,
                worstRefused);
    std::printf("ended at        %.1f %.1f, chunk (%d, %d)\n", px, pz,
                int(std::floor(px / 16.0)), int(std::floor(pz / 16.0)));
    std::printf("meshed          %zu sections over %d frames\n", totalMeshed, frames);
    std::printf("card            %u reads, %u listings, %u writes",
                streamer.stats().io.reads, streamer.stats().io.listings,
                streamer.stats().io.writes);
    if (cacheConfig.opLatencyMicros != 0) {
        std::printf(", modelled at %u us each", cacheConfig.opLatencyMicros);
    }
    std::printf("\n");
    if (generate) {
        std::printf("generation      %s, %d columns still owed\n",
                    streamer.stats().workerRunning ? "on a worker thread"
                                                   : "on the main thread (no worker)",
                    streamer.stats().pendingGeneration);
        if (streamer.stats().generationFailures != 0) {
            // Loud, because it is the one way generation stops for good: the
            // nearest owed column is retried every frame and fails every frame.
            // **Named by cause**, because when this last fired on hardware it
            // was reported as the cache and was not the cache.
            std::printf("sweeps failed   %u -- %u unlightable, %u could not be swept\n",
                        streamer.stats().generationFailures,
                        streamer.stats().generationUnlightable,
                        streamer.stats().generationIncomplete);
        }
        // **The generator cache's high-water mark, next to its size.** A live
        // column is one still being written into by its neighbours' passes; it
        // cannot be evicted without losing that work, so peak live approaching
        // the capacity is the run about to start losing blocks -- and
        // `evictedLive` is it having lost them. Both are session totals from
        // the worker, and both belong in the same report as the failed sweeps,
        // because peak live is the *cause* the failures are the symptom of.
        std::printf("generator cache %u columns, peak live %u, retiredLive %u, evictedLive %u\n",
                    u32(worldgen::ChunkGenerator::cacheColumnsFor(config.meshDistance + 1)),
                    streamer.stats().generatorPeakLive, streamer.stats().generatorRetiredLive,
                    streamer.stats().generatorEvictedLive);
        std::printf("classification  %d cells still to ask about, %s\n",
                    streamer.stats().unclassified,
                    streamer.stats().generationGated
                        ? "GATED: the nearest owed columns are waiting on a listing"
                        : "not gated");
    }
    // Microseconds here are the host's, and a host `stat` hits the page cache;
    // the console pays an IPC round trip to the FS sysmodule for each one and
    // may queue behind the I/O thread's current file. So the count is the
    // number to read, and the worst frame is the one that shows up as a stall.
    std::printf("owed to the card %.2f MB at its peak (%u columns), cap %.2f MB\n",
                double(peakDirty) / (1024.0 * 1024.0), peakDirtyColumns,
                double(streamer.stats().io.dirtyCapBytes) / (1024.0 * 1024.0));
    std::printf("main-thread SD  %u checks, %lld us total, worst frame %u at frame %d\n",
                streamer.stats().io.stats,
                static_cast<long long>(streamer.stats().io.mainThreadMicros), worstStatsFrame,
                worstStatsAt);
    std::printf("columns         %d resident, %d absent, %.2f MB of block data\n",
                streamer.stats().columnsResident, streamer.stats().columnsMissing,
                double(streamer.stats().blockBytes) / (1024.0 * 1024.0));
    if (settledFrame > 0) {
        std::printf("settled         frame %d: nothing left to load or mesh\n", settledFrame);
    } else {
        std::printf("settled         never within %d frames -- %d columns and %d sections "
                    "still pending\n",
                    frames, streamer.stats().pendingColumns,
                    renderer.frameStats().queued);
    }

    streamer.close(nowMillis());
}

bool collect(void* context, i32 x, i32 z)
{
    auto* found = static_cast<std::vector<std::pair<i32, i32>>*>(context);
    found->emplace_back(x, z);
    return true;
}

// **Load every chunk and write it straight back**, which is the whole of the
// round-trip claim: a world this build has opened and saved must be the same
// world. Nothing is changed on purpose, so every difference `tools/nbtdiff.py
// difftree` reports afterwards is a bug in the codec.
//
// It exists because the modelled tags are no longer only the ones nothing
// cares about. `TileEntities` used to go out as the bytes it came in as and
// could not be wrong; it is decoded and re-encoded now, and a chest whose
// contents did not survive would be invisible until somebody opened the world
// in a real client.
//
// Point it at a **copy**. Opening a world writes `session.lock` and closing it
// rewrites `level.dat`, and this rewrites every chunk in it besides.
//
// `reconcile` additionally runs the heal-and-drop pass over every column before
// writing it, which is what the game does at a save. It is off by default
// because it is a *change*: the pure round trip is the one whose diff must be
// empty. Over a real world the pass should find nothing to do, and saying so
// takes measuring it.
int rewriteWorld(const char* worldDir, bool reconcile)
{
    io::PosixFileSystem fs;
    world::AnyStorage storage(fs);

    const world::OpenResult opened = storage.open(worldDir, nowMillis());
    if (opened != world::OpenResult::Ok) {
        std::printf("cannot open %s: %s\n", worldDir, world::describeOpenResult(opened));
        return 1;
    }

    std::vector<std::pair<i32, i32>> coords;
    if (!storage.forEachChunk(&coords, collect)) {
        std::printf("scan failed\n");
        return 1;
    }

    int read = 0;
    int written = 0;
    int failed = 0;
    int tiles = 0;
    int byKind[5] = {};
    int stacks = 0;
    int healed = 0;

    for (const auto& c : coords) {
        world::ChunkColumn column;
        if (!storage.loadChunk(c.first, c.second, &column)) {
            ++failed;
            continue;
        }
        ++read;
        for (const world::TileEntity& tile : column.tileEntities) {
            ++tiles;
            ++byKind[int(tile.kind)];
            stacks += int(tile.items.size());
        }
        if (reconcile) {
            healed += world::reconcileTileEntities(column);
        }
        if (storage.saveChunk(column)) {
            ++written;
        } else {
            ++failed;
        }
    }
    storage.commit();
    storage.close(nowMillis());

    static const char* kKindNames[5] = {"Furnace", "Chest", "Sign", "MobSpawner", "unknown"};
    std::printf("rewrite    %s\n", worldDir);
    std::printf("  chunks        %6d read, %d written, %d failed\n", read, written, failed);
    std::printf("  tile entities %6d\n", tiles);
    for (int i = 0; i < 5; ++i) {
        if (byKind[i] > 0) {
            std::printf("    %-12s%6d\n", kKindNames[i], byKind[i]);
        }
    }
    std::printf("  item stacks   %6d\n", stacks);
    if (reconcile) {
        // Entries added plus dropped. **Must be zero on a world a real client
        // wrote**: anything else means the pass disagrees with the original
        // about which blocks carry a tile entity.
        std::printf("  reconciled    %6d\n", healed);
    }
    return failed == 0 ? 0 : 1;
}

int meshWorld(const char* worldDir, mesh::CubeFormat cubeFormat, bool greedy)
{
    io::PosixFileSystem fs;
    // Whichever shape the folder is in, so a packed world can be measured
    // against the folder world it came from without converting it back first.
    world::AnyStorage storage(fs);

    const world::OpenResult opened = storage.open(worldDir, nowMillis());
    if (opened != world::OpenResult::Ok) {
        std::printf("cannot open %s: %s\n", worldDir, world::describeOpenResult(opened));
        return 1;
    }

    std::vector<std::pair<i32, i32>> coords;
    if (!storage.forEachChunk(&coords, collect)) {
        std::printf("scan failed\n");
        return 1;
    }

    World w;
    for (const auto& c : coords) {
        world::ChunkColumn column;
        if (storage.loadChunk(c.first, c.second, &column)) {
            w.columns.emplace(c, std::move(column));
        }
    }

    Totals t;
    mesh::MeshScratch scratch;
    mesh::VisibilityScratch visScratch;
    mesh::MeshBuilder builder;
    builder.setCubeFormat(cubeFormat);
    builder.setGreedy(greedy);
    builder.reserveQuads(4096);

    for (const auto& entry : w.columns) {
        const i32 cx = entry.first.first;
        const i32 cz = entry.first.second;
        ++t.columns;

        mesh::ColumnNeighbourhood n;
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dx = -1; dx <= 1; ++dx) {
                auto it = w.columns.find({cx + dx, cz + dz});
                n.at(dx, dz) = it == w.columns.end() ? nullptr : &it->second;
            }
        }

        for (int sy = 0; sy < world::ChunkColumn::kSectionCount; ++sy) {
            ++t.sections;

            // Counted from the block data rather than from the mesh, because
            // the whole point is the geometry that is *not* being emitted yet.
            const world::Section& section = entry.second.section(sy);
            if (!section.isUniformAir()) {
                for (int i = 0; i < world::Section::kVolume; ++i) {
                    const world::BlockId id = section.block(i);
                    if (id != world::kAirBlock) {
                        ++t.blocksByRenderType[usize(block::def(id).render)];
                    }
                }
            }

            const mesh::SectionVisibility vis =
                mesh::computeVisibility(entry.second.section(sy), visScratch);
            t.masks[{cx, cz}].resize(world::ChunkColumn::kSectionCount);
            t.masks[{cx, cz}][sy] = vis;
            if (vis.mask() == 0) {
                ++t.visibilityOpaque;
            } else if (vis.mask() == mesh::kVisibilityAll) {
                ++t.visibilityOpen;
            }

            // The cheap rejection the render loop leans on. Counting it here is
            // the point: if it does not fire often, it is not worth having.
            if (mesh::sectionIsEmpty(entry.second, sy)) {
                ++t.sectionsEmpty;
                continue;
            }

            builder.clear();
            // Timed apart, because they are two different jobs with two
            // different fixes: the fill is 5,832 palette lookups whatever the
            // section holds, and the mesh is proportional to the geometry that
            // comes out. Which one dominates decides whether bulk-copying the
            // interior is worth doing.
            const auto start = std::chrono::steady_clock::now();
            scratch.fill(n, sy);
            const auto filled = std::chrono::steady_clock::now();
            mesh::meshSection(scratch, builder);
            const auto meshed = std::chrono::steady_clock::now();

            t.fillNanos +=
                std::chrono::duration_cast<std::chrono::nanoseconds>(filled - start).count();
            t.emitNanos +=
                std::chrono::duration_cast<std::chrono::nanoseconds>(meshed - filled).count();
            t.meshMicros += std::chrono::duration_cast<std::chrono::microseconds>(meshed - start)
                                .count();

            if (builder.empty()) {
                ++t.sectionsNoGeometry;
            }
            t.quads += builder.quadCount();
            t.detailQuads += builder.detailQuadCount();
            t.translucentQuads += builder.translucentQuadCount();
            t.quadsByHeight[sy] += builder.quadCount();
            for (usize q = 0; q < builder.quadCount(); ++q) {
                int tile;
                if (cubeFormat == mesh::CubeFormat::Quads) {
                    // The quad format never encoded the UVs, so the tile is
                    // read back from its cube-atlas slot.
                    const mesh::QuadVertex& v = builder.quads()[q];
                    tile = mesh::kCubeAtlas.tileOfSlot[int(v.slotY) * mesh::kCubeSlotsPerEdge
                                                       + int(v.slotX)];
                } else {
                    // Corners sit on both edges of the tile and which corner
                    // comes first depends on the winding, so the tile is the
                    // corner nearest the atlas origin, not corner zero.
                    const mesh::WorldVertex* v = builder.vertices() + q * 4;
                    i16 u = v[0].u;
                    i16 w = v[0].v;
                    for (int c = 1; c < 4; ++c) {
                        u = v[c].u < u ? v[c].u : u;
                        w = v[c].v < w ? v[c].v : w;
                    }
                    tile = mesh::kCubeAtlas.tileOfSlot[(w / mesh::kCubeUvPerSlot)
                                                           * mesh::kCubeSlotsPerEdge
                                                       + (u / mesh::kCubeUvPerSlot)];
                }
                if (tile >= 0 && tile < mesh::kAtlasTileCount) {
                    ++t.quadsByTile[tile];
                }
            }
            t.meshBySection[{cx, cz, sy}] = builder.ranges();
            t.meshBytes += builder.byteSize();
            if (builder.quadCount() > t.maxSectionQuads) {
                t.maxSectionQuads = builder.quadCount();
            }
        }
    }

    const world::LevelData level = std::move(storage.level());
    storage.close(nowMillis());

    const int meshed = t.sections - t.sectionsEmpty;
    std::printf("world           %s\n", worldDir);
    std::printf("cube format     %s\n",
                cubeFormat == mesh::CubeFormat::Quads
                    ? "geoshader, one 8-byte vertex per quad"
                    : "4 x 12-byte vertices per quad, shared index buffer");
    std::printf("greedy meshing  %s\n",
                greedy ? "on, runs of up to 3x3 equal faces per quad" : "off, one quad per face");
    std::printf("columns         %d\n", t.columns);
    std::printf("sections        %d  (%d uniform air, %d meshed)\n",
                t.sections, t.sectionsEmpty, meshed);
    std::printf("  produced no geometry  %d of the %d meshed\n", t.sectionsNoGeometry, meshed);
    std::printf("quads           %zu  (%.0f per meshed section, worst %zu)\n",
                t.quads, meshed ? double(t.quads) / meshed : 0.0, t.maxSectionQuads);
    std::printf("  worst against the %d the index buffer allows\n", mesh::kMaxQuadsPerSection);
    const usize allQuads = t.quads + t.detailQuads + t.translucentQuads;
    std::printf("detail quads    %zu opaque + %zu translucent in the 16-byte format "
                "(%.2f %% of all quads)\n",
                t.detailQuads, t.translucentQuads,
                allQuads ? 100.0 * double(t.detailQuads + t.translucentQuads) / double(allQuads)
                         : 0.0);
    std::printf("mesh bytes      %.2f MB total, %.1f KB per column\n",
                double(t.meshBytes) / (1024.0 * 1024.0),
                t.columns ? double(t.meshBytes) / t.columns / 1024.0 : 0.0);
    std::printf("visibility      %d sections fully opaque, %d fully open, %d partial\n",
                t.visibilityOpaque, t.visibilityOpen,
                t.sections - t.visibilityOpaque - t.visibilityOpen);
    std::printf("mesh time       %lld ms on this host for %d sections (%.1f us each)\n",
                t.meshMicros / 1000, meshed, meshed ? double(t.meshMicros) / meshed : 0.0);
    {
        const long long total = t.fillNanos + t.emitNanos;
        std::printf("  scratch fill  %6.1f us each  %4.1f %%   (5832 lookups whatever is in it)\n",
                    meshed ? double(t.fillNanos) / meshed / 1000.0 : 0.0,
                    total ? 100.0 * double(t.fillNanos) / double(total) : 0.0);
        std::printf("  face emit     %6.1f us each  %4.1f %%   (proportional to the quads)\n",
                    meshed ? double(t.emitNanos) / meshed / 1000.0 : 0.0,
                    total ? 100.0 * double(t.emitNanos) / double(total) : 0.0);
    }

    std::printf("quads by height (y range: share of all quads)\n");
    for (int sy = 0; sy < world::ChunkColumn::kSectionCount; ++sy) {
        const double share = t.quads ? 100.0 * double(t.quadsByHeight[sy]) / t.quads : 0.0;
        std::printf("  y %3d-%3d  %9zu  %5.1f %%  %.*s\n",
                    sy * 16, sy * 16 + 15, t.quadsByHeight[sy], share,
                    int(share), "########################################");
    }

    int tilesUsed = 0;
    for (usize count : t.quadsByTile) {
        tilesUsed += count != 0;
    }
    std::printf("atlas           %d of %d tiles appear in the geometry\n",
                tilesUsed, mesh::kAtlasTileCount);
    std::vector<std::pair<usize, int>> byTile;
    for (int tile = 0; tile < mesh::kAtlasTileCount; ++tile) {
        if (t.quadsByTile[tile] != 0) {
            byTile.push_back({t.quadsByTile[tile], tile});
        }
    }
    std::sort(byTile.rbegin(), byTile.rend());
    for (usize i = 0; i < byTile.size() && i < 8; ++i) {
        std::printf("  tile %3d  %9zu quads  %5.1f %%\n", byTile[i].second,
                    byTile[i].first,
                    t.quads ? 100.0 * double(byTile[i].first) / t.quads : 0.0);
    }

    // What the mesher is drawing and what it still is not. Every non-cube type
    // needs sub-block geometry the 12-byte vertex cannot express -- its
    // position is one byte per axis, one unit per block -- so this is the size
    // of the second vertex format's job.
    //
    // Which types have an emitter is asked of the mesher rather than written
    // down here, because a hand-maintained list is wrong from the moment the
    // next emitter lands and this report is exactly where that would mislead.
    std::printf("\nblocks by render type\n");
    {
        usize nonCube = 0;
        usize cube = 0;
        std::vector<std::pair<usize, int>> byType;
        for (int i = 0; i < int(block::RenderType::Count); ++i) {
            if (block::RenderType(i) == block::RenderType::Cube) {
                cube = t.blocksByRenderType[i];
            } else {
                nonCube += t.blocksByRenderType[i];
            }
            if (t.blocksByRenderType[i] != 0) {
                byType.push_back({t.blocksByRenderType[i], i});
            }
        }
        std::sort(byType.rbegin(), byType.rend());
        for (const auto& entry : byType) {
            const block::RenderType type = block::RenderType(entry.second);
            std::printf("  %-14s %9zu  %6.3f %%%s\n", block::renderTypeName(type), entry.first,
                        cube + nonCube ? 100.0 * double(entry.first) / double(cube + nonCube)
                                       : 0.0,
                        mesh::hasEmitter(type) ? "" : "   no emitter yet");
        }
        std::printf("  non-cube total %9zu  %6.3f %% of all solid blocks\n", nonCube,
                    cube + nonCube ? 100.0 * double(nonCube) / double(cube + nonCube) : 0.0);

        // The types this build can draw at all, whether or not this world
        // happens to contain one. A torch is 0.000 % of a natural world and
        // 100 % of what a player notices missing.
        std::printf("  emitters       ");
        for (int i = 0; i < int(block::RenderType::Count); ++i) {
            const block::RenderType type = block::RenderType(i);
            if (type != block::RenderType::None && mesh::hasEmitter(type)) {
                std::printf(" %s", block::renderTypeName(type));
            }
        }
        std::printf("\n");
    }

    reachability(t, level);
    vboPool(t, level);
    return 0;
}

// --generate: what a fresh world costs, per column and in total.
//
// The chunk generator is the only thing in this project whose per-unit cost has
// never been measured, and it is about to sit on the chunk worker next to the
// mesher. The numbers that matter are the time for one delivered column and the
// cache high-water mark, because the second is what decides how much of the
// heap the generator takes.
//
// The walk is a spiral out from the origin chunk, which is what the streamer
// asks for, so the cache reuse this reports is the reuse the console will get.
// Nothing is written to a disk; this measures generation, not storage.
int generateWorld(i64 seed, int radius, bool snow, int cacheColumns, bool rowMajor)
{
    struct Offset {
        int dx, dz;
    };
    std::vector<Offset> spiral;
    for (int dx = -radius; dx <= radius; ++dx) {
        for (int dz = -radius; dz <= radius; ++dz) {
            spiral.push_back({dx, dz});
        }
    }
    // Nearest-first is what the streamer asks for -- the player wants the
    // ground under their feet before the horizon. Row-major is the alternative
    // worth measuring, because how much the generator has to hold at once is a
    // property of the request order and nothing else: a spiral leaves an
    // unfinished band all the way round the delivered disc, a raster leaves one
    // strip.
    if (!rowMajor) {
        std::sort(spiral.begin(), spiral.end(), [](const Offset& a, const Offset& b) {
            const int da = a.dx * a.dx + a.dz * a.dz;
            const int db = b.dx * b.dx + b.dz * b.dz;
            return std::tie(da, a.dx, a.dz) < std::tie(db, b.dx, b.dz);
        });
    }

    // **Standing in for the save.** The generator evicts columns it has handed
    // out and reaches them again through Existing::load, so a caller that does
    // not persist what it gets would see them come back as bare terrain with
    // every neighbour's population missing. On the console that is the storage
    // slot; here it is a map, which is enough to exercise the contract and
    // keeps this measuring generation rather than gzip.
    struct Store {
        std::map<std::pair<i32, i32>, world::ChunkColumn> columns;
        usize blockBytes = 0;
        int delivered = 0;

        static void deliver(void* context, world::ChunkColumn& column)
        {
            Store& self = *static_cast<Store*>(context);
            const i32 x = column.x;
            const i32 z = column.z;
            world::ChunkColumn kept(std::move(column));
            kept.compact();
            self.blockBytes += kept.memoryUsage();
            ++self.delivered;
            self.columns.erase({x, z});
            self.columns.emplace(std::make_pair(x, z), std::move(kept));
        }

        static const world::ChunkColumn* load(void* context, i32 x, i32 z,
                                              world::ChunkColumn* scratch)
        {
            (void) scratch;
            Store& self = *static_cast<Store*>(context);
            auto it = self.columns.find({x, z});
            return it == self.columns.end() ? nullptr : &it->second;
        }
    };
    Store store;

    worldgen::GeneratorOptions options;
    options.snowCovered = snow;
    // Far from the origin: the chunk provider is the same everywhere, but a
    // world's own spawn area is the least representative place to measure.
    const i32 baseX = 1000;
    const i32 baseZ = -1000;

    worldgen::ChunkGenerator::Store seam;
    seam.context = &store;
    seam.load = &Store::load;
    seam.deliver = &Store::deliver;

    auto generator = std::make_unique<worldgen::ChunkGenerator>(seed, options, seam,
                                                                cacheColumns);
    auto column = std::make_unique<world::ChunkColumn>();

    std::printf("Generating %d columns, seed %lld, snow %s, %s order\n", int(spiral.size()),
                (long long) seed, snow ? "on" : "off", rowMajor ? "raster" : "nearest-first");

    int delivered = 0;
    const auto start = std::chrono::steady_clock::now();
    double firstMicros = 0.0;
    for (usize i = 0; i < spiral.size(); ++i) {
        const auto one = std::chrono::steady_clock::now();
        if (!generator->provide(baseX + spiral[i].dx, baseZ + spiral[i].dz, column.get())) {
            const worldgen::ChunkGenerator::Stats& bad = generator->stats();
            std::printf("  provide failed at %d, %d after %d columns\n", baseX + spiral[i].dx,
                        baseZ + spiral[i].dz, delivered);
            std::printf("  evictions %u, evictedLive %u, peak live %u\n", bad.evicted,
                        bad.evictedLive, bad.peakLive);
            return 1;
        }
        ++delivered;
        Store::deliver(&store, *column);
        if (i == 0) {
            firstMicros =
                std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - one)
                    .count();
        }
    }
    const double totalMillis =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

    const worldgen::ChunkGenerator::Stats& st = generator->stats();
    std::printf("\n");
    std::printf("  delivered columns        %d\n", delivered);
    std::printf("  total                    %.1f ms\n", totalMillis);
    std::printf("  per requested column     %.0f us  (the latency a streamer sees)\n",
                totalMillis * 1000.0 / double(delivered));
    std::printf("  per finished column      %.0f us  (%d finished for %d asked for -- the\n",
                totalMillis * 1000.0 / double(store.delivered), store.delivered, delivered);
    std::printf("                                    sweep finishes neighbours on the way)\n");
    std::printf("  the first column alone   %.0f us  (its whole 6x6 sweep is cold)\n",
                firstMicros);
    std::printf("\n");
    std::printf("  terrain columns generated %u  (%.2f per delivered column)\n", st.generated,
                double(st.generated) / double(delivered));
    std::printf("  population passes run     %u  (%.2f per delivered column)\n", st.populated,
                double(st.populated) / double(delivered));
    std::printf("  per generated terrain     %.0f us\n",
                totalMillis * 1000.0 / double(st.generated ? st.generated : 1));
    std::printf("  cache hits / misses       %u / %u\n", st.cacheHits, st.cacheMisses);
    std::printf("  evictions                 %u\n", st.evicted);
    std::printf("  peak live (undelivered)   %u columns = %.2f MB of raw block arrays\n",
                st.peakLive, double(st.peakLive) * 32768.0 / (1024.0 * 1024.0));
    std::printf("  block data delivered      %.2f MB over %d columns (%.1f KB each)\n",
                double(store.blockBytes) / (1024.0 * 1024.0), store.delivered,
                double(store.blockBytes) / double(store.delivered) / 1024.0);
    std::printf("\n");
    std::printf("  scratch window columns    %u generated, %u reused\n", st.scratchColumns,
                st.scratchHits);
    std::printf("  must all be zero: refused %u, escapes %u, evictedLive %u\n",
                st.refusedOutOfWindow, st.populationEscapes, st.evictedLive);
    return 0;
}


// ---------------------------------------------------------------------------
// Texture packs
// ---------------------------------------------------------------------------
//
// The console cannot be debugged and cannot be sanitised, and the importer is
// the one part of this feature that touches a file the player did not make.
// Both halves therefore run here first, over the real jar on the dev machine,
// under ASan/UBSan -- which is the whole reason core/texture/ has no libctru in
// it.

// A netpbm P7 so the atlas can be looked at without a decoder. RGBA rather than
// PPM's RGB, because the alpha is exactly what a cutout tile has to be judged
// on and a PPM would drop it.
bool writeAtlasPam(const char* path, const std::vector<u8>& rgba)
{
    std::FILE* file = std::fopen(path, "wb");
    if (file == nullptr) {
        return false;
    }
    std::fprintf(file,
                 "P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n",
                 texture::kAtlasEdge, texture::kAtlasEdge);
    const usize written = std::fwrite(rgba.data(), 1, rgba.size(), file);
    std::fclose(file);
    return written == rgba.size();
}

int inspectPack(const char* path)
{
    io::PosixFileSystem fs;

    const bool devArt = std::strcmp(path, "devart") == 0;
    const char* label = devArt ? "Dev Art" : path;

    // What the pack holds, before anything is decoded. Straight off the central
    // directory, which is the same thing the pack screen shows.
    if (!devArt && !fs.isDirectory(path)) {
        std::vector<u8> bytes;
        if (fs.readFile(path, &bytes, 64u << 20)) {
            texture::ZipArchive archive;
            const texture::ZipError error = archive.open(bytes);
            if (error != texture::ZipError::Ok) {
                std::printf("%s: %s\n", label, texture::zipErrorText(error));
                return 1;
            }
            int pngs = 0;
            int stored = 0;
            for (const texture::ZipEntry& entry : archive.entries()) {
                const usize dot = entry.name.rfind(".png");
                if (dot != std::string::npos && dot + 4 == entry.name.size()) {
                    ++pngs;
                }
                if (entry.method == 0) {
                    ++stored;
                }
            }
            std::printf("%s\n  %zu entries, %d png, %d stored / %zu deflated\n", label,
                        archive.entries().size(), pngs, stored,
                        archive.entries().size() - usize(stored));
        }
    }

    texture::AtlasImage atlas;
    const texture::PackError error =
        texture::buildAtlas(fs, devArt ? "" : path, &atlas);
    if (error != texture::PackError::Ok) {
        std::printf("%s: %s\n", label, texture::packErrorText(error));
        return 1;
    }

    if (atlas.sourceEdge == 0) {
        std::printf("  generated -> atlas %dx%d\n", texture::kAtlasEdge, texture::kAtlasEdge);
    } else {
        const char* scaling = atlas.sourceEdge > texture::kAtlasEdge ? "box-filtered down"
                              : atlas.sourceEdge < texture::kAtlasEdge ? "replicated up"
                                                                       : "as-is";
        std::printf("  terrain.png %dx%d -> atlas %dx%d (%s)\n", atlas.sourceEdge,
                    atlas.sourceEdge, texture::kAtlasEdge, texture::kAtlasEdge, scaling);
    }

    // How much of the atlas is not fully opaque, which is the one number that
    // says at a glance whether cutout tiles survived the scaling.
    usize transparent = 0;
    for (usize i = 3; i < atlas.rgba.size(); i += 4) {
        if (atlas.rgba[i] != 255) {
            ++transparent;
        }
    }
    std::printf("  %zu of %d texels are not fully opaque\n", transparent,
                texture::kAtlasEdge * texture::kAtlasEdge);

    const char* out = devArt ? "atlas-devart.pam" : "atlas.pam";
    if (!writeAtlasPam(out, atlas.rgba)) {
        std::printf("  could not write %s\n", out);
        return 1;
    }
    std::printf("  wrote %s\n", out);
    return 0;
}

// The bottom screen's map, drawn on the host so it can be looked at.
//
// **This is the only way to see what the spectator screen draws without a
// console**, and it runs the same three pieces the console runs -- the chunk
// sampler, the store and the window renderer -- over a whole real world instead
// of a 192-pixel window. What comes out is a picture of the save, one pixel per
// block, which makes a wrong scan or a wrong shade obvious in a way that a
// number never would.
//
// It also prints what the world is *made of* at the surface, which is the check
// that matters when the colours come from a texture pack rather than a table: a
// world whose top block is 60 % grass and whose map is not mostly green has a
// palette bug, not an artistic difference.
//
// **It writes to the directory it is given**, like every other harness here:
// opening a world writes session.lock. Point it at a copy.
int mapWorld(const char* worldDir, const char* packPath, bool grid, int zoom)
{
    io::PosixFileSystem fs;
    world::AnyStorage storage(fs);

    const world::OpenResult opened = storage.open(worldDir, nowMillis());
    if (opened != world::OpenResult::Ok) {
        std::printf("cannot open %s: %s\n", worldDir, world::describeOpenResult(opened));
        return 1;
    }

    // "" is Dev Art, which is what the console falls back to and what this
    // defaults to so the harness needs no pack on disk to run.
    texture::AtlasImage atlas;
    const texture::PackError packError =
        texture::buildAtlas(fs, packPath == nullptr ? "" : packPath, &atlas);
    if (packError != texture::PackError::Ok) {
        std::printf("pack %s: %s\n", packPath, texture::packErrorText(packError));
        return 1;
    }
    map::MapPalette palette;
    map::buildMapPalette(atlas, &palette);

    std::vector<std::pair<i32, i32>> coords;
    if (!storage.forEachChunk(&coords, collect)) {
        std::printf("scan failed\n");
        return 1;
    }
    if (coords.empty()) {
        std::printf("%s has no chunks\n", worldDir);
        return 1;
    }

    map::MapStore store;
    store.setCapacity(int(coords.size()));

    i32 minChunkX = coords[0].first;
    i32 maxChunkX = coords[0].first;
    i32 minChunkZ = coords[0].second;
    i32 maxChunkZ = coords[0].second;

    std::map<block::BlockId, int> surfaceCounts;
    int sampled = 0;
    map::MapChunkSample sample;
    for (const auto& c : coords) {
        world::ChunkColumn column;
        if (!storage.loadChunk(c.first, c.second, &column)) {
            continue;
        }
        map::sampleChunk(column, &sample);
        store.store(c.first, c.second, sample);
        ++sampled;

        minChunkX = c.first < minChunkX ? c.first : minChunkX;
        maxChunkX = c.first > maxChunkX ? c.first : maxChunkX;
        minChunkZ = c.second < minChunkZ ? c.second : minChunkZ;
        maxChunkZ = c.second > maxChunkZ ? c.second : maxChunkZ;

        for (int i = 0; i < map::kChunkSamples; ++i) {
            ++surfaceCounts[sample.surface[i]];
        }
    }

    // The map page's own zoom, so the scaled blit can be looked at as a picture
    // rather than only as a test's assertion. Clamped to what the console
    // offers rather than accepted as given: a level outside the range is one
    // the console can never be in, so a harness that drew it would be answering
    // a question nobody can ask.
    if (zoom < map::kZoomMin) {
        zoom = map::kZoomMin;
    }
    if (zoom > map::kZoomMax) {
        zoom = map::kZoomMax;
    }
    const int pixelsPerBlock = map::mapPixelsPerBlock(zoom);
    const int blocksPerPixel = map::mapBlocksPerPixel(zoom);

    map::MapWindow window;
    window.zoom = zoom;
    window.originBlockX = minChunkX * map::kChunkPixels;
    window.originBlockZ = minChunkZ * map::kChunkPixels;
    window.width =
        (maxChunkX - minChunkX + 1) * map::kChunkPixels * pixelsPerBlock / blocksPerPixel;
    window.height =
        (maxChunkZ - minChunkZ + 1) * map::kChunkPixels * pixelsPerBlock / blocksPerPixel;

    // A world spread thinly over a huge area would otherwise ask for a
    // gigabyte of picture. 4,096 pixels across is more than any world this has
    // been pointed at and small enough to open. Trimmed from the middle, and
    // the trim is rounded to whole sampling steps so the lattice the shrunk
    // window samples on does not shift with the world's extent.
    constexpr int kMaxEdge = 4096;
    if (window.width > kMaxEdge) {
        const int trim = map::mapWindowBlocks(window.width - kMaxEdge, zoom) / 2;
        window.originBlockX += trim - (trim % blocksPerPixel);
        window.width = kMaxEdge;
    }
    if (window.height > kMaxEdge) {
        const int trim = map::mapWindowBlocks(window.height - kMaxEdge, zoom) / 2;
        window.originBlockZ += trim - (trim % blocksPerPixel);
        window.height = kMaxEdge;
    }

    std::vector<map::MapPixel> pixels(usize(window.width) * usize(window.height), 0);
    map::MapStyle style;
    style.unexplored = map::rgb565(20, 22, 34);
    style.chunkGrid = grid;
    style.tileGrid = grid;
    style.tileGridColour = map::rgb565(150, 60, 60);

    map::MapSurface surface;
    surface.pixels = pixels.data();
    surface.strideX = 1;
    surface.strideZ = window.width;
    // Draw every chunk's patch, then copy them onto the picture -- the same two
    // steps the console takes, in the same order.
    map::refreshMapWindow(store, palette, window, style, 1);
    map::renderMapWindow(store, window, style, surface);

    // **The player marker, where the player actually is.** Not decoration: it
    // is the same call the console makes, on the same surface, and it is the
    // only place the marker's shape and its facing can be looked at without a
    // console. A world nobody has stood in has no Player compound, and the
    // marker is left off rather than pinned to the origin.
    const world::PlayerData& player = storage.level().player;
    if (player.present) {
        map::drawMarker(surface, window, player.pos[0], player.pos[2], player.rotation[0], 6.5f,
                        map::rgb565(255, 255, 255), map::rgb565(0, 0, 0));
    }

    usize unexplored = 0;
    for (const map::MapPixel pixel : pixels) {
        if (pixel == style.unexplored) {
            ++unexplored;
        }
    }

    std::printf("%s\n", worldDir);
    std::printf("  %d of %zu chunks sampled\n", sampled, coords.size());
    std::printf("  chunks x %d..%d  z %d..%d\n", minChunkX, maxChunkX, minChunkZ, maxChunkZ);
    std::printf("  picture %d x %d pixels from (%d, %d), zoom %d (%d px/block, %d block/px)\n",
                window.width, window.height, window.originBlockX, window.originBlockZ, zoom,
                pixelsPerBlock, blocksPerPixel);
    if (player.present) {
        std::printf("  player at (%.1f, %.1f) facing %s (yaw %.1f)\n", player.pos[0],
                    player.pos[2], map::kCompass[map::facingFromYaw(player.rotation[0])],
                    player.rotation[0]);
    }
    std::printf("  %zu of %zu pixels unexplored\n", unexplored, pixels.size());

    // **What the console actually pays, measured rather than argued about.**
    //
    // Three costs, and they are separate because they happen at different
    // rates. A chunk is *sampled* once ever. A chunk's patch is *drawn* when it
    // is sampled, when its northern neighbour arrives, or when the palette or
    // the grid changes. The window is *copied* on every block the player
    // crosses, which is the one that has to be cheap -- and the one a New 3DS
    // measured at 5,000 us before the patches existed.
    //
    // **Timed through the console's own strides**, not a row-major buffer: the
    // copy takes its `memcpy` path only when the z stride is -1, so timing it
    // any other way would measure a path the console never runs.
    {
        constexpr int kRepeats = 200;
        constexpr int kScreenWidth = 320;
        constexpr int kScreenHeight = 240;
        // The console's own rectangle, so the number below is the one it pays:
        // 208 by 200 at (104, 32), with the tab strip above it and the
        // coordinate panel beside it. See platform/ctr/map_screen.hpp.
        constexpr int kMapWidth = 208;
        // The console's window, which lost 32 pixels to the hotbar band and 16
        // to the focus banner; see platform/ctr/map_screen.hpp. Kept in step so
        // the cost this prints is the cost the console pays.
        constexpr int kMapHeight = 158;
        constexpr int kMapLeft = 104;
        constexpr int kMapTop = 32;

        map::MapWindow screen;
        screen.width = kMapWidth;
        screen.height = kMapHeight;
        // **At the zoom the run asked for**, because the copy is the one cost
        // zoom changes: the 1:1 blit is a `memcpy` per chunk column and nothing
        // else is. Centred on the same ground whatever the level, and the
        // origin snapped to the sampling step exactly as MapScreen snaps it.
        screen.zoom = zoom;
        const i32 centreBlockX =
            window.originBlockX + map::mapWindowBlocks(window.width, zoom) / 2;
        const i32 centreBlockZ =
            window.originBlockZ + map::mapWindowBlocks(window.height, zoom) / 2;
        screen.originBlockX =
            floorDiv(centreBlockX - map::mapWindowBlocks(kMapWidth, zoom) / 2, blocksPerPixel)
            * blocksPerPixel;
        screen.originBlockZ =
            floorDiv(centreBlockZ - map::mapWindowBlocks(kMapHeight, zoom) / 2, blocksPerPixel)
            * blocksPerPixel;

        std::vector<map::MapPixel> framebuffer(usize(kScreenWidth) * usize(kScreenHeight), 0);
        map::MapSurface screenSurface;
        screenSurface.pixels =
            framebuffer.data() + kMapLeft * kScreenHeight + (kScreenHeight - 1 - kMapTop);
        screenSurface.strideX = kScreenHeight;
        screenSurface.strideZ = -1;

        map::refreshMapWindow(store, palette, screen, style, 1);

        const auto beforeCopy = std::chrono::steady_clock::now();
        for (int i = 0; i < kRepeats; ++i) {
            map::renderMapWindow(store, screen, style, screenSurface);
        }
        const auto afterCopy = std::chrono::steady_clock::now();

        // **The scan on its own, with nothing stale**, which is what a redraw
        // pays when the player has only turned. Same stamp every time, so every
        // patch is current and the whole call is `patchStale` over the window's
        // chunks -- one hash lookup each. It is the number that says whether
        // skipping the scan is worth the state it costs to know it can be
        // skipped. See MapScreen::drawPixels.
        const auto beforeScan = std::chrono::steady_clock::now();
        for (int i = 0; i < kRepeats; ++i) {
            map::refreshMapWindow(store, palette, screen, style, 1);
        }
        const auto afterScan = std::chrono::steady_clock::now();

        // Every patch in the window, drawn again from scratch: a fresh stamp
        // each time is what a texture-pack change costs.
        const auto beforeDraw = std::chrono::steady_clock::now();
        int patches = 0;
        for (int i = 0; i < kRepeats; ++i) {
            patches = map::refreshMapWindow(store, palette, screen, style, u32(2 + i));
        }
        const auto afterDraw = std::chrono::steady_clock::now();

        world::ChunkColumn probe;
        const bool haveProbe = storage.loadChunk(coords[coords.size() / 2].first,
                                                 coords[coords.size() / 2].second, &probe);
        const auto beforeSample = std::chrono::steady_clock::now();
        if (haveProbe) {
            for (int i = 0; i < kRepeats; ++i) {
                map::sampleChunk(probe, &sample);
            }
        }
        const auto afterSample = std::chrono::steady_clock::now();

        const double copyUs =
            std::chrono::duration<double, std::micro>(afterCopy - beforeCopy).count() / kRepeats;
        const double drawUs =
            std::chrono::duration<double, std::micro>(afterDraw - beforeDraw).count() / kRepeats;
        const double scanUs =
            std::chrono::duration<double, std::micro>(afterScan - beforeScan).count() / kRepeats;
        const double sampleUs =
            std::chrono::duration<double, std::micro>(afterSample - beforeSample).count()
            / kRepeats;
        std::printf("  host cost: %.1f us to copy a %dx%d window\n", copyUs, kMapWidth,
                    kMapHeight);
        std::printf("             %.1f us to scan it with nothing stale\n", scanUs);
        std::printf("             %.1f us to draw its %d patches (%.1f us each)\n", drawUs,
                    patches, patches > 0 ? drawUs / patches : 0.0);
        std::printf("             %.1f us to sample a chunk\n", haveProbe ? sampleUs : 0.0);
    }

    // The surface census. Sorted by how much of the world it is, which is the
    // order that makes a wrong colour findable.
    std::vector<std::pair<int, block::BlockId>> ranked;
    for (const auto& entry : surfaceCounts) {
        ranked.emplace_back(entry.second, entry.first);
    }
    std::sort(ranked.begin(), ranked.end(),
              [](const std::pair<int, block::BlockId>& a, const std::pair<int, block::BlockId>& b) {
                  return a.first > b.first;
              });
    const int total = sampled * map::kChunkSamples;
    std::printf("  surface blocks:\n");
    for (usize i = 0; i < ranked.size() && i < 8; ++i) {
        const block::BlockId id = ranked[i].second;
        const u32 rgb = palette.base[id];
        std::printf("    %-16s %5.1f %%  #%02X%02X%02X%s\n", block::def(id).name,
                    total > 0 ? 100.0 * double(ranked[i].first) / double(total) : 0.0,
                    (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF,
                    palette.known[id] ? "" : "  (no colour: never drawn on a map)");
    }

    // Expanded to eight bits a channel on the way out. RGB565 is what the
    // console's screen holds and what the renderer therefore produces; a viewer
    // wants the bits replicated rather than shifted, so white stays white.
    std::vector<u8> rgb(pixels.size() * 3);
    for (usize i = 0; i < pixels.size(); ++i) {
        const u32 value = pixels[i];
        const u32 r = (value >> 11) & 0x1F;
        const u32 g = (value >> 5) & 0x3F;
        const u32 b = value & 0x1F;
        rgb[i * 3 + 0] = u8((r << 3) | (r >> 2));
        rgb[i * 3 + 1] = u8((g << 2) | (g >> 4));
        rgb[i * 3 + 2] = u8((b << 3) | (b >> 2));
    }

    std::FILE* file = std::fopen("map.pam", "wb");
    if (file == nullptr) {
        std::printf("  could not write map.pam\n");
        return 1;
    }
    std::fprintf(file, "P7\nWIDTH %d\nHEIGHT %d\nDEPTH 3\nMAXVAL 255\nTUPLTYPE RGB\nENDHDR\n",
                 window.width, window.height);
    const usize written = std::fwrite(rgb.data(), 1, rgb.size(), file);
    std::fclose(file);
    if (written != rgb.size()) {
        std::printf("  could not write map.pam\n");
        return 1;
    }
    std::printf("  wrote map.pam\n");
    return 0;
}

int extractJar(const char* jarPath, const char* outDir)
{
    io::PosixFileSystem fs;

    const texture::ImportResult result = texture::importJar(fs, jarPath, outDir);
    std::printf("%s\n", jarPath);
    std::printf("  %d png copied, %d entries skipped, %d of %d known a1.1.2 names\n",
                result.copied, result.skipped, result.known, texture::kA112FileCount);
    if (!result.ok()) {
        std::printf("  \x1b[31m%s\x1b[0m\n", texture::packErrorText(result.error));
        return 1;
    }
    // importJar has already re-opened this file and built the atlas from it;
    // saying so is the difference between "written" and "verified", and only
    // the second one justifies offering to delete the source.
    std::printf("  wrote and verified %s\n", result.outPath.c_str());
    return 0;
}

// Converting a world between the two on-disk shapes, which on the console is a
// button and here is the thing that proves it loses nothing:
//
//   ./3dalpha --convert <copy> pack
//   ./3dalpha --convert <copy> unpack
//   diff -r <original> <copy>          # must be empty
//
// **On a copy.** Every path here writes to the directory it is given.
int convertWorldCommand(const char* worldDir, const char* which)
{
    io::PosixFileSystem fs;

    world::WorldFormat target = world::WorldFormat::Unknown;
    if (std::strcmp(which, "pack") == 0) {
        target = world::WorldFormat::Packed;
    } else if (std::strcmp(which, "unpack") == 0) {
        target = world::WorldFormat::Folder;
    } else {
        std::printf("--convert wants `pack` or `unpack`, not `%s`\n", which);
        return 1;
    }

    world::format::ConvertEstimate estimate;
    const world::format::ConvertResult sized =
        world::format::estimateConversion(fs, worldDir, target, &estimate);
    if (sized != world::format::ConvertResult::Ok) {
        std::printf("%s\n  \x1b[31m%s\x1b[0m\n", worldDir,
                    world::format::describeConvertResult(sized));
        return 1;
    }

    std::printf("%s -> %s\n", worldDir, world::formatName(target));
    std::printf("  %u chunks, %u files\n", unsigned(estimate.chunks), unsigned(estimate.files));
    std::printf("  %llu bytes on disk now, about %llu after (cluster %llu)\n",
                (unsigned long long)estimate.sourceOnDisk,
                (unsigned long long)estimate.targetOnDisk,
                (unsigned long long)estimate.clusterSize);

    // Every stage the console draws, printed instead. Same callback, same
    // cancellation contract -- this one simply never cancels.
    struct Progress {
        const char* last = nullptr;

        static bool observe(void* context, const world::format::ConvertProgress& p)
        {
            auto* self = static_cast<Progress*>(context);
            if (self->last != p.stage) {
                self->last = p.stage;
                std::printf("  %s\n", p.stage);
            }
            return true;
        }
    };

    Progress progress;
    world::format::ConvertOptions options;
    options.context = &progress;
    options.observe = &Progress::observe;

    const world::format::ConvertResult result =
        world::format::convertWorld(fs, worldDir, target, options);
    if (result != world::format::ConvertResult::Ok) {
        std::printf("  \x1b[31m%s\x1b[0m\n", world::format::describeConvertResult(result));
        return 1;
    }
    std::printf("  \x1b[32mdone\x1b[0m -- now %s\n",
                world::formatName(world::detectFormat(fs, worldDir)));
    return 0;
}

// What a world is and what it costs, without opening it. The number worth
// looking at is the gap between the two sizes: that gap is cluster slack, and
// it is the whole argument for the packed format.
int worldInfo(const char* worldDir)
{
    io::PosixFileSystem fs;

    const world::WorldFormat format = world::detectFormat(fs, worldDir);
    if (format == world::WorldFormat::Unknown) {
        std::printf("%s\n  not a world\n", worldDir);
        return 1;
    }

    std::printf("%s\n  format     %s\n", worldDir, world::formatName(format));

    world::AnyStorage storage(fs);
    world::LevelData level;
    if (storage.peekLevel(worldDir, &level)) {
        std::printf("  seed       %lld\n", (long long)level.randomSeed);
        std::printf("  last played %lld\n", (long long)level.lastPlayed);
    }

    world::WorldSize size;
    if (!world::worldSize(fs, worldDir, &size)) {
        std::printf("  could not measure it\n");
        return 1;
    }
    std::printf("  content    %llu bytes\n", (unsigned long long)size.contentBytes);
    std::printf("  on disk    %llu bytes in %u files and %u directories\n",
                (unsigned long long)size.onDiskBytes, unsigned(size.fileCount),
                unsigned(size.directoryCount));
    if (size.contentBytes > 0) {
        std::printf("  slack      %.2fx\n",
                    double(size.onDiskBytes) / double(size.contentBytes));
    }
    return 0;
}

// The host's answer to the volume-info seam, so the size and free-space paths
// the console takes are the ones the harness and the tests exercise too --
// otherwise a conversion's refusal check would be dead code off-console.
bool queryVolume(const char* path, io::VolumeInfo* out)
{
    struct statvfs info;
    if (::statvfs(path, &info) != 0) {
        return false;
    }
    // f_frsize is the allocation unit; f_bsize is a preferred I/O size and is
    // not what a file's footprint rounds up to.
    const u64 unit = info.f_frsize != 0 ? u64(info.f_frsize) : u64(info.f_bsize);
    out->clusterSize = unit;
    out->freeBytes = u64(info.f_bavail) * unit;
    out->totalBytes = u64(info.f_blocks) * unit;
    return true;
}

int audioList(const char* resources)
{
    io::PosixFileSystem fs;
    audio::ResourceIndex index;
    if (!indexResources(fs, resources, &index)) {
        std::printf("no resources folder at %s\n", resources);
        std::printf("a1.1.2 downloaded these at runtime from a server that no longer\n");
        std::printf("exists; copy a resources/ folder from any alpha- or beta-era\n");
        std::printf("install. See docs/assets.md.\n");
        return 1;
    }

    std::printf("%s\n", resources);
    std::printf("  music      %4zu  (music/ and newmusic/ -- what the ticker draws from)\n",
                index.music.size());
    std::printf("  sounds     %4zu  (sound/ and newsound/)\n", index.sounds.size());
    std::printf("  streaming  %4zu  (records, keyed by name -- a jukebox asks for one\n",
                index.streaming.size());
    std::printf("                    of these by the disc's own track name)\n");

    // **The records, with the key a jukebox asks for**, which for the streaming
    // pool is the file's own name with the extension off -- the digits stay, so
    // `13.mus` really is addressable as "13". A `.mus` is an Ogg Vorbis file
    // behind a one-byte cipher keyed on its file name; see
    // core/audio/vorbis_stream.hpp.
    if (!index.streaming.empty()) {
        std::printf("\nstreaming pool, as registered and keyed:\n");
        for (const audio::SoundEntry& entry : index.streaming.entries()) {
            std::printf("  %-28s -> %s\n", entry.name.c_str(),
                        audio::poolKey(entry.name, false).c_str());
        }
    }

    // The keys, because the digit strip and the category strip are both easy
    // to get wrong and this is the cheapest way to look at what they did. The
    // name is what `installResource` registers -- the path *after* the category
    // -- so `music/calm1.ogg` is `calm1.ogg` here and keys as `calm`.
    std::printf("\nmusic pool, as registered and keyed:\n");
    for (const audio::SoundEntry& entry : index.music.entries()) {
        std::printf("  %-28s -> %s\n", entry.name.c_str(),
                    audio::poolKey(entry.name, true).c_str());
    }

    // The interface sound, decoded here exactly as the console decodes it at
    // boot. This is what says whether a player's resources folder will make the
    // menus click before they carry it to a console -- the decode is the part
    // that can fail, and a card with no `random/click.ogg` in `sound/` or
    // `newsound/` on it is a
    // silent menu with nothing on screen to explain why.
    host::WavBackend sink("");
    audio::SoundEngine engine(fs, sink, 0);
    engine.loadResources(resources);
    const usize clicks = engine.preloadSound("random.click");

    std::printf("\ninterface sound (what the menus click with):\n");
    if (clicks == 0) {
        std::printf("  random.click   not loadable -- %s\n",
                    audio::vorbisAvailable()
                        ? "no random/click.ogg in sound/ or newsound/, or it would not decode"
                        : "no Vorbis decoder in this build");
        std::printf("  the menus would be silent; everything else still works.\n");
        return 0;
    }

    std::printf("  random.click   %zu file(s) decoded\n", clicks);
    for (const audio::Sample& sample : sink.samples()) {
        std::printf("                 %zu frames, %d ch, %d Hz (%.0f ms)\n", sample.frames(),
                    sample.channels, sample.sampleRate,
                    1000.0 * double(sample.frames()) / double(sample.sampleRate));
    }

    // `of.a`'s arithmetic at the two settings the menus use, so the numbers can
    // be read rather than trusted. See docs/audio-a1.1.2.md.
    std::printf("  gain at sound volume 100%%: choose %.3f (1.0/1.0), "
                "move %.3f (0.3/0.5)\n",
                double(audio::interfaceGain(1.0f, 1.0f)),
                double(audio::interfaceGain(0.3f, 1.0f)));

    // **The whole boot set, decoded, so the console's sample cap is a
    // measurement and not a guess.**
    //
    // `ctr::kMaxSamples` has to hold every entry this prints, because loading
    // is all-or-nothing per key: `playSoundFX` draws its variant before it
    // knows whether that file is resident, so a key with four of its eight
    // variants loaded is a footstep that is silent half the time. The cap is
    // therefore a property of the *player's* folder, and this is the only
    // place it can be read off one. A modern resources tree carries more
    // variants per step than the a1.1.2-era one did, so the honest number is
    // the bigger of the two.
    //
    // The click above is already decoded and `preloadEffects` names it again;
    // `preloadSound` is idempotent per path, so the total below is the set and
    // not a sum with a double-count in it.
    audio::preloadEffects(engine);

    usize bytes = 0;
    usize frames = 0;
    for (const audio::Sample& sample : sink.samples()) {
        frames += sample.frames();
        bytes += sample.frames() * usize(sample.channels) * sizeof(i16);
    }
    std::printf("\nboot effect set (what preloadEffects decodes):\n");
    std::printf("  %zu samples, %zu frames, %.1f KB of PCM\n", sink.samples().size(), frames,
                double(bytes) / 1024.0);
    std::printf("  ctr::kMaxSamples must be at least %zu for this folder\n",
                sink.samples().size());

    // **The console's boot path since the menu stopped waiting for it**: the
    // click first, then the rest on a worker that is pumped rather than joined.
    // It must end with the same set as the synchronous decode above.
    host::WavBackend background("");
    audio::SoundEngine staged(fs, background, 0);
    staged.loadResources(resources);
    staged.preloadSound("random.click");
    const auto start = std::chrono::steady_clock::now();
    if (staged.startPreload(&audio::preloadEffects)) {
        while (staged.preloading()) {
            staged.pumpPreload();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - start)
                          .count();
    std::printf("  background preload: %zu samples in %.1f ms on this machine%s\n",
                background.samples().size(), ms,
                background.samples().size() == sink.samples().size() ? "" : " -- MISMATCH");
    return background.samples().size() == sink.samples().size() ? 0 : 1;
}

// The feature, without a console and without a decoder: run the ticker for a
// stretch of simulated time and print when a1.1.2 would have started a track.
int musicSchedule(const char* resources, i64 seed, int hours)
{
    io::PosixFileSystem fs;
    audio::NullBackend silent;
    audio::SoundEngine engine(fs, silent, seed);
    const usize found = engine.loadResources(resources);

    // The NullBackend reports unavailable, which is the one thing that would
    // stop the ticker. So the schedule is run against the ticker directly.
    audio::MusicTicker ticker(seed);
    audio::MusicState state;
    state.available = true;
    state.musicVolume = 1.0f;

    audio::ResourceIndex index;
    indexResources(fs, resources, &index);

    std::printf("seed %lld, %d hours of simulated play, %zu resources (%zu music)\n",
                (long long)seed, hours, found, index.music.size());
    std::printf("first gap is nextInt(12000) ticks; every later gap is\n");
    std::printf("nextInt(24000)+24000 -- and the counter does not run while a track\n");
    std::printf("plays, so the spacing below is the gap plus the track's own length.\n\n");

    // How long each track runs, in ticks. Taken from the Ogg page headers via
    // ov_pcm_total, which costs a seek to the end of the file and no decoding
    // at all -- and it has to be known, because a1.1.2's counter is frozen for
    // exactly this long after every track starts. Without it the schedule below
    // would be roughly a track-length too dense, which is the whole subtlety of
    // `of.c()` and would make this mode confirm the wrong thing.
    std::vector<i32> durations(index.music.size(), 0);
    bool haveDurations = audio::vorbisAvailable();
    if (haveDurations) {
        for (usize i = 0; i < index.music.entries().size(); ++i) {
            const audio::SoundEntry& entry = index.music.entries()[i];
            std::unique_ptr<audio::VorbisStream> stream =
                audio::VorbisStream::create(fs, entry.path);
            if (stream && stream->prepare() && stream->sampleRate() > 0) {
                durations[i] = i32(stream->totalFrames() * 20 / u64(stream->sampleRate()));
            }
        }
    } else {
        std::printf("(no Vorbis decoder in this build -- track lengths unknown, so the\n");
        std::printf(" spacing shown is the counter gap alone)\n\n");
    }

    const i32 ticks = i32(hours) * 20 * 60 * 60;
    i32 previous = -1;
    i32 playingUntil = -1;
    int played = 0;
    for (i32 tick = 0; tick < ticks; ++tick) {
        state.musicPlaying = tick < playingUntil;

        const audio::SoundEntry* entry = ticker.tick(index.music, state);
        if (entry == nullptr) {
            continue;
        }

        // Which entry it was, so its length can be looked up. The pool hands
        // back a pointer into its own storage, so this is pointer arithmetic
        // rather than a search.
        const usize which = usize(entry - index.music.entries().data());
        const i32 length = which < durations.size() ? durations[which] : 0;
        playingUntil = tick + length;

        const int minutes = int(tick / (20 * 60));
        const int seconds = int((tick / 20) % 60);
        std::printf("  %3d:%02d  %-28s", minutes, seconds, entry->name.c_str());
        if (length > 0) {
            std::printf(" %3ds", int(length / 20));
        } else {
            std::printf("     ");
        }
        if (previous >= 0) {
            std::printf("  (+%d min)", int((tick - previous) / (20 * 60)));
        }
        std::printf("\n");
        previous = tick;
        ++played;
    }
    std::printf("\n%d tracks in %d hours\n", played, hours);
    return 0;
}

// What the console would actually have played, as a listenable file. Silence
// between tracks is written too, so the gaps in the .wav are the game's gaps.
int audioDump(const char* resources, const char* outPath, i64 seed, int minutes)
{
    io::PosixFileSystem fs;
    host::WavBackend wav(outPath);
    audio::SoundEngine engine(fs, wav, seed);
    const usize found = engine.loadResources(resources);
    if (found == 0) {
        std::printf("no resources at %s -- nothing to render\n", resources);
        return 1;
    }
    if (!audio::vorbisAvailable()) {
        std::printf("this build has no Vorbis decoder; install libvorbisfile and\n");
        std::printf("reconfigure. --music-schedule works without one.\n");
        return 1;
    }

    const int ticks = minutes * 20 * 60;
    for (int tick = 0; tick < ticks; ++tick) {
        engine.tick(1);
        wav.advance(1);
    }
    wav.finish();

    std::printf("%s: %d tracks, %.1f s of audio from %zu resources\n", outPath,
                wav.tracksPlayed(), double(wav.framesWritten()) / 44100.0, found);
    return 0;
}


// `--record-dump`: **one disc, decoded and written out**, which is the only way
// to say the `.mus` cipher is right rather than merely plausible. The path is
// the console's exactly -- the same pool lookup by track name, the same
// `VorbisStream`, the same positional gain -- with a .wav where ndsp is.
//
// A record is not on the music voice by accident here: `playRecord` shares it,
// because a1.1.2 stops `BgMusic` the moment a disc starts. See
// core/audio/sound_engine.hpp.
int recordDump(const char* resources, const char* track, const char* outPath, int seconds)
{
    io::PosixFileSystem fs;
    host::WavBackend wav(outPath);
    audio::SoundEngine engine(fs, wav, 0);
    const usize found = engine.loadResources(resources);
    if (found == 0) {
        std::printf("no resources at %s -- nothing to play\n", resources);
        return 1;
    }
    if (!audio::vorbisAvailable()) {
        std::printf("this build has no Vorbis decoder; install libvorbisfile and\n");
        std::printf("reconfigure.\n");
        return 1;
    }

    // The jukebox is at the origin and so are the ears, which is the gain a
    // player standing on the block would hear: 0.5 * soundVolume, undimmed.
    engine.setListener(0.0, 0.0, 0.0);
    engine.playRecord(track, 0.0, 0.0, 0.0);
    if (!engine.recordPlaying()) {
        std::printf("no record called \"%s\" in %s -- try --audio-list\n", track, resources);
        return 1;
    }

    const int ticks = seconds * 20;
    for (int tick = 0; tick < ticks && engine.recordPlaying(); ++tick) {
        engine.tick(1);
        wav.advance(1);
    }
    wav.finish();

    std::printf("%s: %.1f s of \"%s\"\n", outPath, double(wav.framesWritten()) / 44100.0,
                track);
    // A cipher that is wrong does not decode at all: Vorbis refuses the header
    // and the stream ends with nothing written. One second is enough to tell.
    if (wav.framesWritten() < 44100) {
        std::printf("that is less than a second -- the file did not decode\n");
        return 1;
    }
    return 0;
}

// `--spawns`: where the monsters go, in a real world, on the host.
//
// **This exists because "nothing is spawning" cannot be answered from a unit
// test.** A fixture world is a floor and some air: every drawn point that is
// not the floor is air, so the spawner never takes `az`'s early return and a
// probe over one reports a spawn rate no real world has. A generated world is
// mostly stone, most of `k`'s y draws land inside it, and the pass ends on the
// first chunk it tries -- which is the original's shape and the thing a
// synthetic scene cannot show.
//
// It runs the console's own loop for the mobs: the same streamer, the same
// `stepTicks`, the same `spawnMonsters`/`spawnAnimals` pair out of one random,
// and the same entity query wired over the mob pool -- which is the seam that
// hid a bug once already. What it adds is `SpawnCounters` and a y histogram, so
// "no monsters" and "monsters, 90 blocks below you" stop looking alike.
//
// **Point it at a copy.** `open()` writes session.lock and `close()` rewrites
// level.dat, like every other mode here.
struct SpawnScene {
    const entity::MobSystem* mobs = nullptr;
    AABB player;
    bool playerPresent = false;

    static bool query(void* ctx, const AABB& box, tick::EntityFilter filter)
    {
        const SpawnScene* self = static_cast<const SpawnScene*>(ctx);
        if (self->playerPresent && self->player.intersects(box)) {
            return true;
        }
        if (filter == tick::EntityFilter::Players || self->mobs == nullptr) {
            return false;
        }
        for (int i = 0; i < self->mobs->count(); ++i) {
            const entity::Mob& mob = (*self->mobs)[i];
            if (mob.alive && mob.body.box.intersects(box)) {
                return true;
            }
        }
        return false;
    }
};

int spawnProbe(const char* worldDir, int distance, int ticks, i64 timeOfDay, bool generate,
               const double* standAt)
{
    HostVboAllocator allocator;

    render::ChunkRendererConfig config;
    config.meshDistance = distance;
    config.budget = flyBudget(distance);
    config.meshBudgetPerFrame = 4;

    render::ChunkRenderer renderer;
    renderer.reset(&allocator, config);

    render::WorldStreamer streamer;
    streamer.setGenerateMissing(generate);
    if (generate) {
        // The same recipe --fly uses: an empty directory becomes a world, an
        // existing one opens as it is.
        io::PosixFileSystem fs;
        world::AnyStorage probe(fs);
        if (probe.open(worldDir, nowMillis()) == world::OpenResult::Ok) {
            probe.close(nowMillis());
        } else if (probe.create(worldDir, 1234567890LL, nowMillis(),
                                world::WorldFormat::Folder)
                   != world::OpenResult::Ok) {
            std::printf("cannot create %s\n", worldDir);
            return 2;
        } else {
            probe.close(nowMillis());
            std::printf("created    %s (seed 1234567890)\n", worldDir);
        }
    }
    // **The block spawners, wired the way the console wires them** -- before
    // the settling loop below, so the columns it pulls in hand their tile
    // entities over as they arrive. See core/entity/mob_spawner.hpp.
    entity::MobSpawnerStore spawners;
    streamer.setColumnSinks(
        [](void* ctx, const world::ChunkColumn& column) {
            entity::readMobSpawners(column.tileEntities,
                                    *static_cast<entity::MobSpawnerStore*>(ctx));
        },
        [](void* ctx, i32 chunkX, i32 chunkZ) {
            static_cast<entity::MobSpawnerStore*>(ctx)->eraseColumn(chunkX, chunkZ);
        },
        [](void* ctx, world::ChunkColumn& column) {
            world::reconcileTileEntities(column);
            entity::writeMobSpawners(*static_cast<entity::MobSpawnerStore*>(ctx), column);
        },
        &spawners);

    if (!streamer.open(worldDir, distance, nowMillis())) {
        std::printf("cannot open %s\n", worldDir);
        return 2;
    }

    double px = 0.0, py = 0.0, pz = 0.0;
    streamer.spawnPosition(&px, &py, &pz);
    // **Somewhere other than the spawn point**, which is what `at=` is for: a
    // dungeon's spawner does nothing until a player is within sixteen blocks
    // of it, and a world's spawn point is on the surface. `spawner blocks`
    // below prints where the nearest ones are, so the second run of this
    // command can stand on one.
    if (standAt != nullptr) {
        px = standAt[0];
        py = standAt[1];
        pz = standAt[2];
    }

    render::WorldStreamer::Budget budget;
    budget.columnsPerFrame = 2;

    const int chunkX = int(std::floor(px / 16.0));
    const int chunkZ = int(std::floor(pz / 16.0));
    for (int i = 0; i < 600; ++i) {
        Frustum frustum;
        frustum.setOrigin(chunkX, chunkZ);
        renderer.beginFrame(u32(i), frustum, chunkX, int(std::floor(py / 16.0)), chunkZ);
        streamer.update(renderer, chunkX, chunkZ, budget);
    }

    tick::TickWorld* world = streamer.worldTick();
    if (world == nullptr) {
        std::printf("no tick world\n");
        return 2;
    }
    world->setTime(timeOfDay);

    entity::MobSystem mobs(4242LL);
    SpawnScene scene;
    scene.mobs = &mobs;
    scene.playerPresent = true;
    scene.player = AABB{px - 0.3, py, pz - 0.3, px + 0.3, py + 1.8, pz + 0.3};
    world->setEntityQuery(&SpawnScene::query, &scene);

    entity::SpawnContext context;
    context.playerPresent = true;
    context.playerX = px;
    context.playerY = py;
    context.playerZ = pz;
    context.spawnX = streamer.level().spawnX;
    context.spawnY = streamer.level().spawnY;
    context.spawnZ = streamer.level().spawnZ;
    context.difficulty = 2;
    context.worldSeed = streamer.level().randomSeed;

    entity::MobSurroundings around;
    around.player.present = true;
    around.player.x = px;
    around.player.y = py;
    around.player.z = pz;
    around.difficulty = 2;

    JavaRandom rand(i64(nowMillis()) ^ 0x5a2d);
    entity::SpawnCounters monsterCount;
    entity::SpawnCounters animalCount;
    entity::MobSpawnerCounters blockCount;

    // Sixteen bands of eight, which is the resolution that separates "in the
    // caves" from "on the ground" without a page of output.
    int band[16] = {0};
    int byType[entity::kMobTypeCount] = {0};
    int offset[9][9] = {{0}};
    int resident = 0;

    // How many of the 9x9 the spawner asks for are actually loaded. A monster
    // that cannot spawn because its column is absent is a streaming answer, not
    // a spawning one.
    for (i32 dz = -4; dz <= 4; ++dz) {
        for (i32 dx = -4; dx <= 4; ++dx) {
            if (world->chunkResident(chunkX + dx, chunkZ + dz)) {
                ++resident;
            }
        }
    }

    std::printf("world      %s\n", worldDir);
    std::printf("player     %.1f %.1f %.1f, chunk (%d, %d)\n", px, py, pz, chunkX, chunkZ);
    std::printf("spawn      %d %d %d, seed %lld\n", int(context.spawnX), context.spawnY,
                int(context.spawnZ), (long long)context.worldSeed);
    std::printf("chunks     %d of %d in the spawner's square are resident after settling\n",
                resident, entity::kEligibleChunks);
    std::printf("time       %lld, sky light subtracted %d\n", (long long)timeOfDay,
                world->skyDarken());
    std::printf("ticks      %d\n\n", ticks);

    for (int t = 0; t < ticks; ++t) {
        // **Held, not advanced.** A 6,000-tick run covers a quarter of a day
        // and would average night and morning together; the question here is
        // what one time of day does, so every tick starts at the same one.
        world->setTime(timeOfDay);
        streamer.stepTicks(renderer, 1);
        mobs.tick(*world, around);

        const int before = mobs.count();
        entity::spawnMonsters(*world, mobs, rand, context, &monsterCount);
        for (int i = before; i < mobs.count(); ++i) {
            const entity::Mob& mob = mobs[i];
            const int y = int(mob.body.y);
            band[y < 0 ? 0 : (y > 127 ? 15 : y / 8)] += 1;
            byType[int(mob.type)] += 1;
            const int ox = int(std::floor(mob.body.x / 16.0)) - chunkX + 4;
            const int oz = int(std::floor(mob.body.z / 16.0)) - chunkZ + 4;
            if (ox >= 0 && ox < 9 && oz >= 0 && oz < 9) {
                offset[oz][ox] += 1;
            }
        }
        entity::spawnAnimals(*world, mobs, rand, context, &animalCount);

        // The tile-entity tick list, which is one member long in this version.
        // Out of the world's random, like the two above are out of `rand` --
        // and note that the mobs it makes are **not** counted in the bands: a
        // dungeon spawner's zombies are not `k`'s and mixing them would make
        // the histogram lie.
        entity::tickMobSpawners(spawners, *world, mobs, world->random(), context,
                                &blockCount);

        Frustum frustum;
        frustum.setOrigin(chunkX, chunkZ);
        renderer.beginFrame(u32(t + 1000), frustum, chunkX, int(std::floor(py / 16.0)), chunkZ);
        streamer.update(renderer, chunkX, chunkZ, budget);
    }

    // The world's own time moved while it ticked; report where it ended so a
    // run long enough to reach dawn says so rather than quietly measuring day.
    std::printf("time       ended at %lld, sky light subtracted %d\n\n",
                (long long)world->time(), world->skyDarken());

    const auto report = [](const char* what, const entity::SpawnCounters& c) {
        std::printf("%s\n", what);
        std::printf("  passes          %d\n", c.passes);
        std::printf("  chunks tried    %d\n", c.chunksTried);
        std::printf("  ended in solid  %d\n", c.abortedSolid);
        std::printf("  positions       %d\n", c.positions);
        std::printf("    no floor      %d\n", c.noFloor);
        std::printf("    too near      %d\n", c.tooNear);
        std::printf("    refused       %d\n", c.checkRejected);
        std::printf("    spawned       %d\n", c.spawned);
    };
    report("monsters", monsterCount);
    std::printf("\n");
    report("animals", animalCount);

    // **The block spawners**, which are a different question from the two
    // above: `az` and `k` sweep the world, and these sit in dungeons and wait.
    // What matters here is whether the world *has* any -- a run reporting no
    // spawners at all in a world with dungeons means the `TileEntities` read
    // is not finding them, and that is invisible from the mob counts.
    std::printf("\nspawner blocks\n");
    std::printf("  resident        %d\n", spawners.count());
    {
        int byMob[entity::kMobTypeCount] = {0};
        int unknownMobs = 0;
        for (int i = 0; i < spawners.count(); ++i) {
            if (spawners[i].known) {
                byMob[int(spawners[i].mob)] += 1;
            } else {
                ++unknownMobs;
            }
        }
        for (int i = 0; i < entity::kMobTypeCount; ++i) {
            if (byMob[i] != 0) {
                std::printf("    %-10s %d\n", entity::mobDef(entity::MobType(i)).saveId,
                            byMob[i]);
            }
        }
        if (unknownMobs != 0) {
            std::printf("    unknown    %d\n", unknownMobs);
        }
    }
    double lastSpawnerDistSq = -1.0;
    for (int shown = 0, best = -1; shown < 3; ++shown, best = -1) {
        // The nearest three, each printed once: a selection sort over a list
        // that is thirteen long on the world this was measured against.
        double bestSq = 1e18;
        for (int i = 0; i < spawners.count(); ++i) {
            const double dx = double(spawners[i].x) + 0.5 - px;
            const double dy = double(spawners[i].y) + 0.5 - py;
            const double dz = double(spawners[i].z) + 0.5 - pz;
            const double d = dx * dx + dy * dy + dz * dz;
            if (d < bestSq && d > (shown == 0 ? -1.0 : lastSpawnerDistSq)) {
                bestSq = d;
                best = i;
            }
        }
        if (best < 0) {
            break;
        }
        lastSpawnerDistSq = bestSq;
        std::printf("    nearest    %s at %d %d %d, %.1f blocks away\n",
                    spawners[best].entityId, int(spawners[best].x), spawners[best].y,
                    int(spawners[best].z), std::sqrt(bestSq));
    }
    std::printf("  visited         %d\n", blockCount.visited);
    std::printf("    in range      %d\n", blockCount.inRange);
    std::printf("    fired         %d\n", blockCount.fired);
    std::printf("      crowded     %d\n", blockCount.crowded);
    std::printf("      refused     %d\n", blockCount.refused);
    std::printf("      spawned     %d\n", blockCount.spawned);

    std::printf("\nmonsters by kind\n");
    for (int i = entity::kAnimalTypeCount; i < entity::kMobTypeCount; ++i) {
        std::printf("  %-10s %d\n", entity::mobDef(entity::MobType(i)).saveId, byType[i]);
    }

    std::printf("\nwhere they appeared\n");
    for (int i = 0; i < 16; ++i) {
        if (band[i] == 0) {
            continue;
        }
        std::printf("  y %3d-%3d  %d\n", i * 8, i * 8 + 7, band[i]);
    }

    // **Which chunk of the square they came out of**, which is the question
    // `az`'s early return makes worth asking: a pass that ends on the first
    // solid draw only ever reaches the chunks its iteration order puts first.
    std::printf("\nby chunk, player at the centre (rows north to south)\n");
    for (int oz = 0; oz < 9; ++oz) {
        std::printf("  ");
        for (int ox = 0; ox < 9; ++ox) {
            std::printf("%4d", offset[oz][ox]);
        }
        std::printf("\n");
    }

    int liveMonsters = 0;
    int highest = -1;
    double nearestSq = 1e18;
    for (int i = 0; i < mobs.count(); ++i) {
        const entity::Mob& mob = mobs[i];
        if (!mob.alive || int(mob.type) < entity::kAnimalTypeCount) {
            continue;
        }
        ++liveMonsters;
        if (int(mob.body.y) > highest) {
            highest = int(mob.body.y);
        }
        const double dx = mob.body.x - px;
        const double dy = mob.body.y - py;
        const double dz = mob.body.z - pz;
        const double d = dx * dx + dy * dy + dz * dz;
        if (d < nearestSq) {
            nearestSq = d;
        }
    }
    std::printf("\nalive      %d monsters, %d animals\n", liveMonsters,
                mobs.count() - liveMonsters);
    if (liveMonsters > 0) {
        std::printf("highest    y %d\n", highest);
        std::printf("nearest    %.1f blocks away\n", std::sqrt(nearestSq));
    }

    streamer.close(nowMillis());
    return monsterCount.spawned > 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv)
{
    mc::io::setVolumeInfoQuery(&queryVolume);

    if (argc > 1 && std::strcmp(argv[1], "--version") == 0) {
        std::printf("3DAlpha host build\n");
        std::printf("  Minecraft version  %s\n", mcver::kDisplay);
        std::printf("  protocol           %d\n", mcver::kProtocol);
        std::printf("  world height       %d\n", mcver::kWorldHeight);
        return 0;
    }

    // Meshing every section of a real world is how the M2 numbers in
    // docs/3ds-performance.md get measured rather than guessed, and it is the
    // harness the geometry-shader experiment will be compared against.
    // A trailing word rather than another optional number, because --fly
    // already has three of those and a fourth would be unreadable. `quads`
    // meshes in the geometry-shader format throughout; `flip` starts in the
    // 12-byte one and changes over halfway, which is the settings page's path
    // and the one worth having a sanitizer walk through.
    //
    // `gen` is the console's configuration: generation on, and the chunk cache
    // threaded with a read-ahead band. `gensync` is the same world made the way
    // it was made before any of that existed -- every read and write on the
    // calling thread, nothing read ahead -- and it is there for exactly one
    // reason: **generating a seed both ways and diffing the two trees is the
    // test that the cache changed what chunk I/O costs and not what it says.**
    //
    // Everything else leaves the cache unthreaded, because every documented
    // --fly invocation measures a fixed world and has to keep reporting the
    // numbers it always did; a read posted to a thread is a column that is
    // pending this frame rather than loaded, and the table would move.
    const char* last = argc > 2 ? argv[argc - 1] : "";
    const bool quads = std::strcmp(last, "quads") == 0;
    const bool flip = std::strcmp(last, "flip") == 0;
    const bool generateSync = std::strcmp(last, "gensync") == 0;
    const bool generate = std::strcmp(last, "gen") == 0 || generateSync;
    // `packed` makes a *created* world packed instead of folder-shaped. It has
    // to be opt-in: every documented --fly number was taken on a folder world,
    // and changing the layout under them would move the table.
    const bool packed = std::strcmp(last, "packed") == 0;
    const bool trailingWord = quads || flip || generate || packed;
    const mesh::CubeFormat cubeFormat =
        quads ? mesh::CubeFormat::Quads : mesh::CubeFormat::Vertices;

    // What a world costs to make. See generateWorld.
    if (argc > 1 && std::strcmp(argv[1], "--generate") == 0) {
        const i64 seed = argc > 2 ? i64(std::atoll(argv[2])) : i64(1234567890);
        const int radius = argc > 3 ? std::atoi(argv[3]) : 8;
        const bool snow = argc > 4 && std::strcmp(argv[4], "snow") == 0;
        const int cache = argc > 5 ? std::atoi(argv[5])
                                   : worldgen::ChunkGenerator::cacheColumnsFor(radius);
        const bool rowMajor = argc > 6 && std::strcmp(argv[6], "raster") == 0;
        return generateWorld(seed, radius, snow, cache, rowMajor);
    }

    // `--mesh <world> [quads] [flat]`, in either order: `flat` turns greedy
    // meshing off, so the two meshes of one world can be set side by side.
    if (argc > 2 && std::strcmp(argv[1], "--mesh") == 0) {
        bool meshQuads = false;
        bool flat = false;
        for (int i = 3; i < argc; ++i) {
            meshQuads = meshQuads || std::strcmp(argv[i], "quads") == 0;
            flat = flat || std::strcmp(argv[i], "flat") == 0;
        }
        return meshWorld(argv[2],
                         meshQuads ? mesh::CubeFormat::Quads : mesh::CubeFormat::Vertices, !flat);
    }

    // The two halves of the texture-pack feature that do not need a console.
    // `devart` in place of a path assembles the built-in pack instead.
    if (argc > 2 && std::strcmp(argv[1], "--pack") == 0) {
        return inspectPack(argv[2]);
    }

    if (argc > 3 && std::strcmp(argv[1], "--extract-jar") == 0) {
        return extractJar(argv[2], argv[3]);
    }

    if (argc > 3 && std::strcmp(argv[1], "--convert") == 0) {
        return convertWorldCommand(argv[2], argv[3]);
    }

    if (argc > 2 && std::strcmp(argv[1], "--audio-list") == 0) {
        return audioList(argv[2]);
    }

    if (argc > 2 && std::strcmp(argv[1], "--music-schedule") == 0) {
        const i64 seed = argc > 3 ? i64(std::atoll(argv[3])) : 0;
        const int hours = argc > 4 ? std::atoi(argv[4]) : 8;
        return musicSchedule(argv[2], seed, hours);
    }

    if (argc > 3 && std::strcmp(argv[1], "--audio-dump") == 0) {
        const i64 seed = argc > 4 ? i64(std::atoll(argv[4])) : 0;
        const int minutes = argc > 5 ? std::atoi(argv[5]) : 15;
        return audioDump(argv[2], argv[3], seed, minutes);
    }

    if (argc > 4 && std::strcmp(argv[1], "--record-dump") == 0) {
        const int seconds = argc > 5 ? std::atoi(argv[5]) : 30;
        return recordDump(argv[2], argv[3], argv[4], seconds > 0 ? seconds : 30);
    }

    if (argc > 2 && std::strcmp(argv[1], "--rewrite") == 0) {
        const bool reconcile = argc > 3 && std::strcmp(argv[3], "reconcile") == 0;
        return rewriteWorld(argv[2], reconcile);
    }

    if (argc > 2 && std::strcmp(argv[1], "--world-info") == 0) {
        return worldInfo(argv[2]);
    }

    if (argc > 2 && std::strcmp(argv[1], "--map") == 0) {
        // `grid` and `zoom=<n>` are this command's own words rather than the
        // shared ones above, which only ever mean something to --fly and
        // --mesh. Both are recognised by name wherever they appear after the
        // world, which is what lets the pack -- the one argument that cannot be
        // recognised by name -- be whatever is left over.
        bool grid = false;
        int zoom = 0;
        const char* pack = nullptr;
        for (int i = 3; i < argc; ++i) {
            if (std::strcmp(argv[i], "grid") == 0) {
                grid = true;
            } else if (std::strncmp(argv[i], "zoom=", 5) == 0) {
                zoom = std::atoi(argv[i] + 5);
            } else if (pack == nullptr) {
                pack = argv[i];
            }
        }
        return mapWorld(argv[2], pack, grid, zoom);
    }

    // The console's own loop, without the console. Everything between reading
    // the SD card and issuing a draw call runs here, so a streaming bug is a
    // sanitizer report rather than a puzzle on a 240-line screen.
    if (argc > 2 && std::strcmp(argv[1], "--walk") == 0) {
        const int distance = argc > 3 ? std::atoi(argv[3]) : 8;
        const int ticks = (argc > 4 && !(trailingWord && argc == 5)) ? std::atoi(argv[4]) : 2000;
        return walk(argv[2], distance, ticks, generate);
    }

    // Falling, drowning and breaking stone, on a real world. See survive.
    if (argc > 2 && std::strcmp(argv[1], "--survive") == 0) {
        const int ticks = (argc > 3 && !(trailingWord && argc == 4)) ? std::atoi(argv[3]) : 400;
        return survive(argv[2], ticks, generate);
    }

    // Where the monsters go, in a real world. See spawnProbe.
    if (argc > 2 && std::strcmp(argv[1], "--spawns") == 0) {
        // The two positional arguments, skipping any `at=` and any trailing
        // word: a keyword argument must not be read as a tick count.
        const char* positional[2] = {nullptr, nullptr};
        int positionalCount = 0;
        for (int i = 3; i < argc && positionalCount < 2; ++i) {
            if (std::strncmp(argv[i], "at=", 3) == 0
                || (trailingWord && i == argc - 1)) {
                continue;
            }
            positional[positionalCount++] = argv[i];
        }
        const int ticks = positional[0] != nullptr ? std::atoi(positional[0]) : 6000;
        // Midnight, because that is the only time of day the surface can be
        // asked about: `dq.a()Z` reads the *stored* sky light for its first
        // clause and the day-subtracted one for its second, so the time changes
        // the second and nothing else.
        const i64 when = positional[1] != nullptr ? i64(std::atoll(positional[1])) : i64(18000);
        // `at=x,y,z` -- where the player stands. Anywhere in the argument
        // list, like `zoom=` on --map, because the two positional arguments
        // before it both have defaults worth keeping.
        double at[3] = {0.0, 0.0, 0.0};
        bool haveAt = false;
        for (int i = 3; i < argc; ++i) {
            if (std::strncmp(argv[i], "at=", 3) == 0) {
                haveAt = std::sscanf(argv[i] + 3, "%lf,%lf,%lf", &at[0], &at[1], &at[2]) == 3;
            }
        }
        return spawnProbe(argv[2], 8, ticks, when, generate, haveAt ? at : nullptr);
    }

    if (argc > 2 && std::strcmp(argv[1], "--fly") == 0) {
        const int distance = argc > 3 ? std::atoi(argv[3]) : 8;
        const int frames = argc > 4 ? std::atoi(argv[4]) : 200;
        // Optional, and 0 by default so every documented invocation reports the
        // same numbers it always did.
        const int switchTo = (argc > 5 && !(trailingWord && argc == 6)) ? std::atoi(argv[5]) : 0;
        // `packed` implies generation: there is no other way for the harness
        // to be the thing that creates a world, and a trailing word that
        // silently did nothing would be worse than one that is refused.
        const bool wantsGeneration = generate || packed;
        const bool threadedCache = wantsGeneration && !generateSync;
        fly(argv[2], distance, frames, switchTo, cubeFormat, flip, wantsGeneration,
            threadedCache, threadedCache ? 2 : 0,
            packed ? world::WorldFormat::Packed : world::WorldFormat::Folder);
        return 0;
    }

    if (argc > 1 && std::strcmp(argv[1], "--online") == 0) {
        return runOnline(argc, argv);
    }
    if (argc > 1 && std::strcmp(argv[1], "--join") == 0) {
        return runJoin(argc, argv);
    }

    std::printf("3DAlpha host harness (%s).\n", mcver::kDisplay);
    std::printf("  --version                            build configuration\n");
    std::printf("  --join host[:port] [name] [seconds] [compare=<world-copy>]\n");
    std::printf("  --online host[:port] [principal] [seconds]\n");
    std::printf("        play a scripted session on a real protocol-2 server: log in, keep\n");
    std::printf("        every column, chat, dig and place, and check the server echoed it.\n");
    std::printf("        compare= diffs the columns against a copy of the server's world\n");
    std::printf("  --generate [seed] [radius] [snow] [cache-columns] [raster]\n");
    std::printf("        generate a fresh world outward from one chunk and report what it\n");
    std::printf("        cost, per column and in cache high-water\n");
    std::printf("  --mesh <world-dir> [quads]           mesh a whole world, report the numbers\n");
    std::printf("  --walk <world-dir> [distance] [ticks] [gen]\n");
    std::printf("        walk a real world with the player body under the sanitizers and\n");
    std::printf("        report what it hit. A check, not a measurement: non-zero exit when\n");
    std::printf("        the body falls through the world, goes NaN or never moves. Point it\n");
    std::printf("        at a copy -- opening a world writes to it. `gen` generates the\n");
    std::printf("        chunks it walks into, so the walk is not bounded by what exists\n");
    std::printf("  --survive <world-dir> [ticks] [gen]\n");
    std::printf("        the Survival rules on a real world: fall, drown, break stone. A\n");
    std::printf("        check, not a measurement: non-zero exit when a fall of more than\n");
    std::printf("        three blocks costs no health, when water never drowns, or when\n");
    std::printf("        stone broken by hand drops cobblestone. It writes blocks and puts\n");
    std::printf("        them back, so point it at a copy like everything else here\n");
    std::printf("  --pack <zip|dir|devart>              assemble a texture pack's atlas and\n");
    std::printf("        report what scaling it needed; writes atlas.pam to look at\n");
    std::printf("  --extract-jar <jar> <packs-dir>      turn a client jar into a texture pack,\n");
    std::printf("        the same code the console's Extract-from-a-jar button runs\n");
    std::printf("  --convert <world-dir> pack|unpack    move a world between the two on-disk\n");
    std::printf("        shapes, in place. Pack it, unpack it, and `diff -r` against the\n");
    std::printf("        original must be empty -- that is what \"loses no data\" means\n");
    std::printf("  --map <world-dir> [pack] [grid] [zoom=<n>]\n");
    std::printf("                                       draw the bottom screen's map of a\n");
    std::printf("        whole world and report what its surface is made of; writes map.pam\n");
    std::printf("        to look at. `grid` draws the chunk and 128-block map-tile lines the\n");
    std::printf("        map page draws. `zoom=` is that page's own zoom, -1 to 2, where 0 is\n");
    std::printf("        one pixel per block\n");
    std::printf("  --audio-list <resources-dir>         what the sound pools ended up holding,\n");
    std::printf("        and the key a1.1.2 would have filed each music file under\n");
    std::printf("  --music-schedule <resources-dir> [seed] [hours]\n");
    std::printf("        when a1.1.2 would start background music over a stretch of play.\n");
    std::printf("        Needs no decoder and no console -- this is the feature itself\n");
    std::printf("  --record-dump <resources-dir> <track> <out.wav> [seconds]\n");
    std::printf("        decode one music disc -- `13`, `cat` -- and write it out. This is\n");
    std::printf("        the path a jukebox takes, cipher included: a `.mus` is an Ogg\n");
    std::printf("        Vorbis file behind a byte cipher keyed on its own file name\n");
    std::printf("  --audio-dump <resources-dir> <out.wav> [seed] [minutes]\n");
    std::printf("        render that schedule to a .wav, silence between tracks included,\n");
    std::printf("        so the music can be listened to without a 3DS\n");
    std::printf("  --world-info <world-dir>             format, seed and what it occupies;\n");
    std::printf("  --rewrite <world-dir> [reconcile]    load every chunk and write it back,\n");
    std::printf("        unchanged, then `tools/nbtdiff.py difftree` it against the copy\n");
    std::printf("        it was made from. `reconcile` also runs the heal-and-drop pass,\n");
    std::printf("        which must find nothing on a real world. Point it at a copy\n");
    std::printf("        the gap between content and on-disk is cluster slack\n");
    std::printf("  --spawns <world-dir> [ticks] [time] [at=x,y,z] [gen]\n");
    std::printf("        run the monster and animal spawners over a real world and report\n");
    std::printf("        where the attempts went -- passes, chunks, the positions each\n");
    std::printf("        check refused, and the height band every monster appeared in.\n");
    std::printf("        Default 6000 ticks at time 18000, which is midnight. Non-zero\n");
    std::printf("        exit when nothing spawned at all. Point it at a copy.\n");
    std::printf("        It also reports the world's mob spawner *blocks* and where the\n");
    std::printf("        nearest three are; `at=x,y,z` stands the player somewhere other\n");
    std::printf("        than the spawn point, which is how one of those is reached\n");
    std::printf("  --fly <world-dir> [distance] [frames] [switch-to] [quads|flip]\n");
    std::printf("        run the console's render loop; switch-to changes the render\n");
    std::printf("        distance halfway, the way the debug settings page does\n");
    std::printf("  a trailing `quads` puts the cube range in the geometry-shader\n");
    std::printf("  format: one 8-byte vertex per quad instead of four 12-byte ones.\n");
    std::printf("  `flip` (--fly only) changes format halfway instead, which is the\n");
    std::printf("  path the settings page takes and the one worth sanitizing\n");
    std::printf("  `gen` (--fly only) generates missing chunks and writes them back,\n");
    std::printf("  creating the world if the directory has none -- the console's\n");
    std::printf("  configuration, chunk cache and read-ahead included. It writes to\n");
    std::printf("  the directory it is given.\n");
    std::printf("  `gensync` is the same, with every read and write on the calling\n");
    std::printf("  thread and nothing read ahead. Generate a seed both ways and diff\n");
    std::printf("  the trees: they must be identical.\n");
    std::printf("  `packed` (--fly only) generates like `gen`, but a world created by\n");
    std::printf("  the run is packed rather than folder-shaped -- the console's\n");
    std::printf("  default. Existing worlds open in whichever shape they are in.\n");
    std::printf("Run the unit tests with: make test\n");
    return 0;
}
