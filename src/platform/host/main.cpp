// Host entry point.
//
// The host target exists so core code can be developed and tested in a desktop
// debugger under sanitizers -- see docs/architecture.md. It will grow an
// SDL2 + OpenGL mirror of the 3DS platform interfaces at M2; until then it is a
// harness whose main job is to prove that core compiles and links away from
// libctru, and to expose the world tools that do not need a console.

#include "core/io/posix_file_system.hpp"
#include "core/io/volume_info.hpp"
#include "core/block/registry.hpp"
#include "core/map/map_palette.hpp"
#include "core/map/map_render.hpp"
#include "core/map/map_sample.hpp"
#include "core/map/map_store.hpp"
#include "core/mesh/mesher.hpp"
#include "core/mesh/visibility.hpp"
#include "core/render/chunk_renderer.hpp"
#include "core/render/vbo_pool.hpp"
#include "core/render/world_streamer.hpp"
#include "core/render/visible_set.hpp"
#include "core/texture/atlas_image.hpp"
#include "core/texture/jar_import.hpp"
#include "core/texture/pack_list.hpp"
#include "core/texture/zip_archive.hpp"
#include "core/world/any_storage.hpp"
#include "core/world/chunk.hpp"
#include "core/world/chunk_cache.hpp"
#include "core/world/format/converter.hpp"
#include "core/world/world_format.hpp"
#include "core/world/world_list.hpp"
#include "impl/worldgen/alpha_nobiome/chunk_generator.hpp"
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

int meshWorld(const char* worldDir, mesh::CubeFormat cubeFormat)
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
                    // read back rather than reconstructed.
                    const mesh::QuadVertex& v = builder.quads()[q];
                    tile = int(v.tileY) * mesh::kAtlasTilesPerEdge + int(v.tileX);
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
                    tile = (w / mesh::kUvUnitsPerTile) * mesh::kAtlasTilesPerEdge
                           + (u / mesh::kUvUnitsPerTile);
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
    std::printf("  dungeon chests / spawners dropped  %u / %u\n", st.droppedChests,
                st.droppedSpawners);
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
int mapWorld(const char* worldDir, const char* packPath, bool grid)
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

    map::MapWindow window;
    window.originBlockX = minChunkX * map::kChunkPixels;
    window.originBlockZ = minChunkZ * map::kChunkPixels;
    window.width = (maxChunkX - minChunkX + 1) * map::kChunkPixels;
    window.height = (maxChunkZ - minChunkZ + 1) * map::kChunkPixels;

    // A world spread thinly over a huge area would otherwise ask for a
    // gigabyte of picture. 4,096 blocks across is more than any world this has
    // been pointed at and small enough to open.
    constexpr int kMaxEdge = 4096;
    if (window.width > kMaxEdge) {
        window.originBlockX += (window.width - kMaxEdge) / 2;
        window.width = kMaxEdge;
    }
    if (window.height > kMaxEdge) {
        window.originBlockZ += (window.height - kMaxEdge) / 2;
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
    std::printf("  picture %d x %d blocks from (%d, %d)\n", window.width, window.height,
                window.originBlockX, window.originBlockZ);
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
        constexpr int kMapHeight = 200;
        constexpr int kMapLeft = 104;
        constexpr int kMapTop = 32;

        map::MapWindow screen;
        screen.width = kMapWidth;
        screen.height = kMapHeight;
        screen.originBlockX = window.originBlockX + window.width / 2 - kMapWidth / 2;
        screen.originBlockZ = window.originBlockZ + window.height / 2 - kMapHeight / 2;

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
        const double sampleUs =
            std::chrono::duration<double, std::micro>(afterSample - beforeSample).count()
            / kRepeats;
        std::printf("  host cost: %.1f us to copy a %dx%d window\n", copyUs, kMapWidth,
                    kMapHeight);
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

    if (argc > 2 && std::strcmp(argv[1], "--mesh") == 0) {
        return meshWorld(argv[2], cubeFormat);
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

    if (argc > 2 && std::strcmp(argv[1], "--world-info") == 0) {
        return worldInfo(argv[2]);
    }

    if (argc > 2 && std::strcmp(argv[1], "--map") == 0) {
        // `grid` is this command's own trailing word rather than the shared
        // one above, which only ever means something to --fly and --mesh.
        const bool grid = argc > 3 && std::strcmp(argv[argc - 1], "grid") == 0;
        const char* pack = (argc > 3 && !(grid && argc == 4)) ? argv[3] : nullptr;
        return mapWorld(argv[2], pack, grid);
    }

    // The console's own loop, without the console. Everything between reading
    // the SD card and issuing a draw call runs here, so a streaming bug is a
    // sanitizer report rather than a puzzle on a 240-line screen.
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

    std::printf("3DAlpha host harness (%s).\n", mcver::kDisplay);
    std::printf("  --version                            build configuration\n");
    std::printf("  --generate [seed] [radius] [snow] [cache-columns] [raster]\n");
    std::printf("        generate a fresh world outward from one chunk and report what it\n");
    std::printf("        cost, per column and in cache high-water\n");
    std::printf("  --mesh <world-dir> [quads]           mesh a whole world, report the numbers\n");
    std::printf("  --pack <zip|dir|devart>              assemble a texture pack's atlas and\n");
    std::printf("        report what scaling it needed; writes atlas.pam to look at\n");
    std::printf("  --extract-jar <jar> <packs-dir>      turn a client jar into a texture pack,\n");
    std::printf("        the same code the console's Extract-from-a-jar button runs\n");
    std::printf("  --convert <world-dir> pack|unpack    move a world between the two on-disk\n");
    std::printf("        shapes, in place. Pack it, unpack it, and `diff -r` against the\n");
    std::printf("        original must be empty -- that is what \"loses no data\" means\n");
    std::printf("  --map <world-dir> [pack] [grid]      draw the bottom screen's map of a\n");
    std::printf("        whole world, one pixel per block, and report what its surface is\n");
    std::printf("        made of; writes map.pam to look at. `grid` draws the chunk and\n");
    std::printf("        128-block map-tile lines the spectator screen draws\n");
    std::printf("  --world-info <world-dir>             format, seed and what it occupies;\n");
    std::printf("        the gap between content and on-disk is cluster slack\n");
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
