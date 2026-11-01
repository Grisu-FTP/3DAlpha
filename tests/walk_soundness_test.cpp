// Brute-force check that the visibility walk never drops a section a line of
// sight really reaches.
//
// A random cave-like world is built block by block, the walk's masks are
// computed from it the way the mesher does, and then thousands of straight
// lines are marched out of the eye through the blocks. Every section such a line
// passes through while the air is clear -- plus the one holding the first block
// it hits, which is what the player sees -- must be in the walk's draw list.
// Two kinds of world: caves, where the masks and the directional rule do the
// work, and rolling hills, where a walk can double back over a crest.

#include "framework.hpp"

#include "core/block/registry.hpp"
#include "core/mesh/visibility.hpp"
#include "core/render/visible_set.hpp"
#include "core/world/section.hpp"

#include <cmath>
#include <set>
#include <tuple>
#include <vector>

using namespace mc;
using render::SectionField;
using render::VisibleSet;
using world::Section;

namespace {

constexpr int kRadius = 3;                 // columns each way from the camera's
constexpr int kEdge = kRadius * 2 + 1;     // 7 x 7 columns
constexpr int kLayers = 6;                 // sections 0..5 are built; 6..7 are air
constexpr int kCameraLayer = 3;
constexpr int kSize = Section::kSize;
constexpr int kBlocksXZ = kEdge * kSize;
constexpr int kBlocksY = kLayers * kSize;
constexpr u16 kStone = u16(mcver::Block::Stone);

struct Rng {
    u64 state;
    u32 next()
    {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return u32(state >> 16);
    }
    double unit() { return double(next() & 0xFFFFFF) / double(0x1000000); }
};

struct Grid {
    std::vector<u8> solid;  // 1 = stone

    Grid() : solid(size_t(kBlocksXZ) * kBlocksXZ * kBlocksY, 0) {}

    static size_t at(int x, int y, int z)
    {
        return (size_t(x) * kBlocksXZ + size_t(z)) * kBlocksY + size_t(y);
    }
    bool isSolid(int x, int y, int z) const { return solid[at(x, y, z)] != 0; }
    bool inside(int x, int y, int z) const
    {
        return x >= 0 && z >= 0 && y >= 0 && x < kBlocksXZ && z < kBlocksXZ && y < kBlocksY;
    }
};

// Cave-like: random noise, then a few majority passes so the air forms
// tunnels and chambers rather than static, with a solid floor and a hollowed
// layer through the middle so the camera has somewhere to stand.
Grid makeWorld(u64 seed, int densityPercent)
{
    Rng rng{seed * 2654435761ull + 88172645463325252ull};
    Grid g;
    for (u8& cell : g.solid) {
        cell = rng.next() % 100 < u32(densityPercent) ? 1 : 0;
    }
    for (int pass = 0; pass < 3; ++pass) {
        Grid next = g;
        for (int x = 0; x < kBlocksXZ; ++x) {
            for (int z = 0; z < kBlocksXZ; ++z) {
                for (int y = 0; y < kBlocksY; ++y) {
                    int count = 0;
                    for (int dx = -1; dx <= 1; ++dx) {
                        for (int dy = -1; dy <= 1; ++dy) {
                            for (int dz = -1; dz <= 1; ++dz) {
                                if ((dx | dy | dz) == 0) {
                                    continue;
                                }
                                const int nx = x + dx, ny = y + dy, nz = z + dz;
                                count += !g.inside(nx, ny, nz) || g.isSolid(nx, ny, nz) ? 1 : 0;
                            }
                        }
                    }
                    next.solid[Grid::at(x, y, z)] = count >= 14 ? 1 : 0;
                }
            }
        }
        g = std::move(next);
    }
    return g;
}

// Rolling ground: a few large bumps on a flat base, solid below the surface.
Grid makeHills(u64 seed)
{
    Rng rng{seed * 40503ull + 977ull};
    struct Bump { double x, z, radius, height; };
    Bump bumps[5];
    for (Bump& b : bumps) {
        b.x = rng.unit() * kBlocksXZ;
        b.z = rng.unit() * kBlocksXZ;
        b.radius = 14.0 + rng.unit() * 22.0;
        b.height = 20.0 + rng.unit() * 40.0;
    }
    Grid g;
    for (int x = 0; x < kBlocksXZ; ++x) {
        for (int z = 0; z < kBlocksXZ; ++z) {
            double h = 20.0;
            for (const Bump& b : bumps) {
                const double dx = x - b.x, dz = z - b.z;
                h += b.height * std::exp(-(dx * dx + dz * dz) / (b.radius * b.radius));
            }
            for (int y = 0; y < kBlocksY && y < int(h); ++y) {
                g.solid[Grid::at(x, y, z)] = 1;
            }
        }
    }
    return g;
}

// The masks of every section, the way the mesher would compute them.
void publish(const Grid& g, SectionField* field)
{
    static mesh::VisibilityScratch scratch;
    field->reset(kRadius);
    field->setCentre(0, 0);
    for (int cz = -kRadius; cz <= kRadius; ++cz) {
        for (int cx = -kRadius; cx <= kRadius; ++cx) {
            mesh::SectionVisibility masks[SectionField::kSectionsY];
            for (int sy = 0; sy < SectionField::kSectionsY; ++sy) {
                if (sy >= kLayers) {
                    masks[sy] = mesh::SectionVisibility(mesh::kVisibilityAll);
                    continue;
                }
                Section section;
                for (int x = 0; x < kSize; ++x) {
                    for (int z = 0; z < kSize; ++z) {
                        for (int y = 0; y < kSize; ++y) {
                            if (g.isSolid((cx + kRadius) * kSize + x, sy * kSize + y,
                                          (cz + kRadius) * kSize + z)) {
                                section.setBlock(x, y, z, kStone);
                            }
                        }
                    }
                }
                masks[sy] = mesh::computeVisibility(section, scratch);
            }
            field->setColumn(cx, cz, masks);
            for (int sy = 0; sy < SectionField::kSectionsY; ++sy) {
                field->setMeshSlot(cx, sy, cz, u16(0));
            }
        }
    }
}

using SectionKey = std::tuple<int, int, int>;

// Marches one line out of the eye with a voxel walk and records every section it
// is inside while the air is clear, and the one that holds the block it stops at.
void march(const Grid& g, double ex, double ey, double ez, double tx, double ty, double tz,
           std::set<SectionKey>* seen)
{
    double dx = tx - ex, dy = ty - ey, dz = tz - ez;
    const double len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (len < 1e-9) {
        return;
    }
    dx /= len;
    dy /= len;
    dz /= len;

    int x = int(std::floor(ex)), y = int(std::floor(ey)), z = int(std::floor(ez));
    const int stepX = dx > 0 ? 1 : -1, stepY = dy > 0 ? 1 : -1, stepZ = dz > 0 ? 1 : -1;
    const double inf = 1e30;
    const double tDeltaX = dx != 0 ? std::fabs(1.0 / dx) : inf;
    const double tDeltaY = dy != 0 ? std::fabs(1.0 / dy) : inf;
    const double tDeltaZ = dz != 0 ? std::fabs(1.0 / dz) : inf;
    double tMaxX = dx != 0 ? ((dx > 0 ? x + 1 - ex : ex - x) * tDeltaX) : inf;
    double tMaxY = dy != 0 ? ((dy > 0 ? y + 1 - ey : ey - y) * tDeltaY) : inf;
    double tMaxZ = dz != 0 ? ((dz > 0 ? z + 1 - ez : ez - z) * tDeltaZ) : inf;

    for (;;) {
        if (!g.inside(x, y, z)) {
            return;
        }
        seen->insert({x / kSize - kRadius - (x < 0 ? 1 : 0), y / kSize, z / kSize - kRadius});
        if (g.isSolid(x, y, z)) {
            return;
        }
        double t;
        if (tMaxX <= tMaxY && tMaxX <= tMaxZ) {
            t = tMaxX;
            x += stepX;
            tMaxX += tDeltaX;
        } else if (tMaxY <= tMaxZ) {
            t = tMaxY;
            y += stepY;
            tMaxY += tDeltaY;
        } else {
            t = tMaxZ;
            z += stepZ;
            tMaxZ += tDeltaZ;
        }
        if (t > len) {
            return;
        }
    }
}

struct Outcome {
    int worlds = 0;
    int seenTotal = 0;
    int missed = 0;
    int missedWorlds = 0;
    long long drawn = 0;
};

Frustum openFrustum()
{
    Frustum f;
    f.setFromViewProjection(Mat4{}, ClipRange::NegativeOneToOne);
    return f;
}

// hills: rolling ground, not caves.
Outcome run(bool hills, int worlds)
{
    Outcome out;
    for (int w = 0; w < worlds; ++w) {
        const Grid g = hills ? makeHills(u64(w) + 1) : makeWorld(u64(w) + 1, 40 + (w % 3) * 4);
        Rng rng{u64(w) * 7919ull + 12345ull};

        // An eye in the open air of the camera's column: anywhere in the cave,
        // or standing on the ground for hills.
        int ex = -1, ey = -1, ez = -1;
        for (int tries = 0; tries < 400 && ex < 0; ++tries) {
            const int x = kRadius * kSize + int(rng.next() % kSize);
            const int z = kRadius * kSize + int(rng.next() % kSize);
            int y = kCameraLayer * kSize + int(rng.next() % kSize);
            if (hills) {
                y = 0;
                while (y < kBlocksY - 2 && g.isSolid(x, y, z)) {
                    ++y;
                }
                y += 1;
            }
            if (y < kBlocksY && !g.isSolid(x, y, z)) {
                ex = x;
                ey = y;
                ez = z;
            }
        }
        if (ex < 0) {
            continue;
        }
        const int cameraLayer = ey / kSize;
        ++out.worlds;

        SectionField field;
        publish(g, &field);

        VisibleSet set;
        render::buildVisibleSet(field, openFrustum(), 0, cameraLayer, 0, &set);

        out.drawn += (long long)set.draw.size();
        std::set<SectionKey> drawn;
        for (const render::VisibleSection& s : set.draw) {
            drawn.insert({int(s.chunkX), int(s.sectionY), int(s.chunkZ)});
        }

        std::set<SectionKey> seen;
        for (int i = 0; i < 6000; ++i) {
            const double tx = rng.unit() * kBlocksXZ;
            const double ty = rng.unit() * kBlocksY;
            const double tz = rng.unit() * kBlocksXZ;
            march(g, ex + 0.5, ey + 0.5, ez + 0.5, tx, ty, tz, &seen);
        }

        int missed = 0;
        for (const SectionKey& k : seen) {
            // The camera's own section is always kept; an all-air section the
            // mesher would skip is still in the list, as here.
            if (drawn.count(k) == 0) {
                ++missed;
            }
        }
        out.seenTotal += int(seen.size());
        out.missed += missed;
        out.missedWorlds += missed > 0 ? 1 : 0;
    }
    return out;
}

}  // namespace

TEST(the_walk_keeps_every_section_a_line_of_sight_reaches_in_caves)
{
    const Outcome walk = run(false, 4);

    CHECK(walk.worlds > 0);
    CHECK(walk.seenTotal > 0);
    CHECK_EQ(walk.missed, 0);
}

TEST(the_walk_keeps_every_section_a_line_of_sight_reaches_over_hills)
{
    const Outcome walk = run(true, 6);

    CHECK(walk.worlds > 0);
    CHECK(walk.seenTotal > 0);
    CHECK_EQ(walk.missed, 0);
}
