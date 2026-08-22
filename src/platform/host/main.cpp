// Host entry point.
//
// The host target exists so core code can be developed and tested in a desktop
// debugger under sanitizers -- see docs/architecture.md. It will grow an
// SDL2 + OpenGL mirror of the 3DS platform interfaces at M2; until then it is a
// harness whose main job is to prove that core compiles and links away from
// libctru, and to expose the world tools that do not need a console.

#include "core/io/posix_file_system.hpp"
#include "core/block/registry.hpp"
#include "core/mesh/mesher.hpp"
#include "core/mesh/visibility.hpp"
#include "core/render/chunk_renderer.hpp"
#include "core/render/vbo_pool.hpp"
#include "core/render/world_streamer.hpp"
#include "core/render/visible_set.hpp"
#include "core/world/chunk.hpp"
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
         mesh::CubeFormat cubeFormat, bool flipFormat, bool generate)
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
    if (generate) {
        streamer.setGenerateMissing(true);
        io::PosixFileSystem fs;
        mcver::Storage probe(fs);
        if (probe.open(worldDir, nowMillis()) == world::OpenResult::Ok) {
            probe.close(nowMillis());
        } else if (probe.create(worldDir, 1234567890LL, nowMillis()) != world::OpenResult::Ok) {
            std::printf("cannot create %s\n", worldDir);
            return;
        } else {
            probe.close(nowMillis());
            std::printf("created    %s (seed 1234567890)\n", worldDir);
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

    if (generate) {
        std::printf("frame  loaded  pending  drawn  queued  meshed  quads     pool MB  evict"
                    "  gen-owed  gen-queue  stale\n");
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
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
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
                std::printf("  %8d  %9d  %5d%s", streaming.pendingGeneration,
                            streaming.generationQueued, streaming.generationStale,
                            streaming.generationGated ? "  GATED" : "");
            }
            std::printf("\n");
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
    if (generate) {
        std::printf("generation      %s, %d columns still owed\n",
                    streamer.stats().workerRunning ? "on a worker thread"
                                                   : "on the main thread (no worker)",
                    streamer.stats().pendingGeneration);
        std::printf("gen queue       %d waiting, %d of them out of range, %d refused (cap), "
                    "%s\n",
                    streamer.stats().generationQueued, streamer.stats().generationStale,
                    streamer.stats().generationRefused,
                    streamer.stats().generationGated ? "GATED: grid not fully classified"
                                                     : "not gated");
    }
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
    mcver::Storage storage(fs);

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

}  // namespace

int main(int argc, char** argv)
{
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
    const char* last = argc > 2 ? argv[argc - 1] : "";
    const bool quads = std::strcmp(last, "quads") == 0;
    const bool flip = std::strcmp(last, "flip") == 0;
    const bool generate = std::strcmp(last, "gen") == 0;
    const bool trailingWord = quads || flip || generate;
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

    // The console's own loop, without the console. Everything between reading
    // the SD card and issuing a draw call runs here, so a streaming bug is a
    // sanitizer report rather than a puzzle on a 240-line screen.
    if (argc > 2 && std::strcmp(argv[1], "--fly") == 0) {
        const int distance = argc > 3 ? std::atoi(argv[3]) : 8;
        const int frames = argc > 4 ? std::atoi(argv[4]) : 200;
        // Optional, and 0 by default so every documented invocation reports the
        // same numbers it always did.
        const int switchTo = (argc > 5 && !(trailingWord && argc == 6)) ? std::atoi(argv[5]) : 0;
        fly(argv[2], distance, frames, switchTo, cubeFormat, flip, generate);
        return 0;
    }

    std::printf("3DAlpha host harness (%s).\n", mcver::kDisplay);
    std::printf("  --version                            build configuration\n");
    std::printf("  --generate [seed] [radius] [snow] [cache-columns] [raster]\n");
    std::printf("        generate a fresh world outward from one chunk and report what it\n");
    std::printf("        cost, per column and in cache high-water\n");
    std::printf("  --mesh <world-dir> [quads]           mesh a whole world, report the numbers\n");
    std::printf("  --fly <world-dir> [distance] [frames] [switch-to] [quads|flip]\n");
    std::printf("        run the console's render loop; switch-to changes the render\n");
    std::printf("        distance halfway, the way the debug settings page does\n");
    std::printf("  a trailing `quads` puts the cube range in the geometry-shader\n");
    std::printf("  format: one 8-byte vertex per quad instead of four 12-byte ones.\n");
    std::printf("  `flip` (--fly only) changes format halfway instead, which is the\n");
    std::printf("  path the settings page takes and the one worth sanitizing\n");
    std::printf("  `gen` (--fly only) generates missing chunks and writes them back,\n");
    std::printf("  creating the world if the directory has none -- the console's\n");
    std::printf("  configuration. It writes to the directory it is given.\n");
    std::printf("Run the unit tests with: make test\n");
    return 0;
}
