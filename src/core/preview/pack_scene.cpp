#include "core/preview/pack_scene.hpp"

#include "core/block/registry.hpp"
#include "core/mesh/box.hpp"
#include "core/util/aabb.hpp"

namespace mc::preview {

namespace {

using block::BlockId;
using mcver::Block;

constexpr int kVolume = kPackSceneEdge * kPackSceneHeight * kPackSceneEdge;

// Full light: the preview has no sky and no torches, and a pack should be seen
// as it draws, not as a cave draws it.
constexpr u8 kFullLight = 0xFF;

int indexOf(int x, int y, int z)
{
    return (y * kPackSceneEdge + z) * kPackSceneEdge + x;
}

struct Placed {
    Block block;
    u8 x, y, z;
};

// The scene, block by block, on top of a grass floor at y = 0.
constexpr Placed kPlaced[] = {
    // A sand corner and a scrap of gravel let the floor show more than grass.
    {Block::Sand, 4, 0, 4},
    {Block::Sand, 5, 0, 4},
    {Block::Sand, 4, 0, 5},
    {Block::Sand, 5, 0, 5},
    {Block::Gravel, 3, 0, 5},

    // A tree: three logs and a crown of leaves.
    {Block::Log, 1, 1, 1},
    {Block::Log, 1, 2, 1},
    {Block::Log, 1, 3, 1},
    {Block::Leaves, 0, 3, 0},
    {Block::Leaves, 1, 3, 0},
    {Block::Leaves, 2, 3, 0},
    {Block::Leaves, 0, 3, 1},
    {Block::Leaves, 2, 3, 1},
    {Block::Leaves, 0, 3, 2},
    {Block::Leaves, 1, 3, 2},
    {Block::Leaves, 2, 3, 2},
    {Block::Leaves, 1, 4, 0},
    {Block::Leaves, 0, 4, 1},
    {Block::Leaves, 1, 4, 1},
    {Block::Leaves, 2, 4, 1},
    {Block::Leaves, 1, 4, 2},

    // A workbench and a furnace side by side, and a bookshelf behind them.
    {Block::CraftingTable, 3, 1, 1},
    {Block::Furnace, 4, 1, 1},
    {Block::Bookshelf, 5, 1, 1},

    // A stone wall with ore in it.
    {Block::Cobblestone, 5, 1, 2},
    {Block::Stone, 5, 1, 3},
    {Block::IronOre, 5, 2, 2},
    {Block::Cobblestone, 5, 2, 1},

    // The rest of the palette a pack is usually looked at for.
    {Block::Planks, 3, 1, 3},
    {Block::Bricks, 2, 1, 4},
    {Block::Glass, 3, 1, 4},
    {Block::GoldOre, 0, 1, 5},
    {Block::CoalOre, 1, 1, 5},
    {Block::DiamondOre, 0, 2, 5},
    {Block::Tnt, 0, 1, 4},
};

}  // namespace

void buildPackScene(mesh::MeshBuilder* out)
{
    BlockId volume[kVolume] = {};
    for (int z = 0; z < kPackSceneEdge; ++z) {
        for (int x = 0; x < kPackSceneEdge; ++x) {
            volume[indexOf(x, 0, z)] = BlockId(Block::Grass);
        }
    }
    for (const Placed& placed : kPlaced) {
        volume[indexOf(placed.x, placed.y, placed.z)] = BlockId(placed.block);
    }

    constexpr AABB kCube{0.0, 0.0, 0.0, 1.0, 1.0, 1.0};
    for (int y = 0; y < kPackSceneHeight; ++y) {
        for (int z = 0; z < kPackSceneEdge; ++z) {
            for (int x = 0; x < kPackSceneEdge; ++x) {
                const BlockId id = volume[indexOf(x, y, z)];
                if (block::isAir(id)) {
                    continue;
                }
                int faces = 0;
                for (int face = 0; face < mesh::kFaceCount; ++face) {
                    const int nx = x + mesh::kFaceOffset[face].dx;
                    const int ny = y + mesh::kFaceOffset[face].dy;
                    const int nz = z + mesh::kFaceOffset[face].dz;
                    const bool inside = nx >= 0 && nx < kPackSceneEdge && ny >= 0
                                        && ny < kPackSceneHeight && nz >= 0
                                        && nz < kPackSceneEdge;
                    if (inside && block::isOpaque(volume[indexOf(nx, ny, nz)])) {
                        continue;
                    }
                    faces |= 1 << face;
                }
                if (faces != 0) {
                    mesh::addBox(x, y, z, kCube, block::def(id).faces, kFullLight,
                                 /*shaded=*/true, faces, *out);
                }
            }
        }
    }
}

}  // namespace mc::preview
