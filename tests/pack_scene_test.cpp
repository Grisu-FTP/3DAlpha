// The Texture Pack screen's little scene: built from the block table, the
// same every time, and with hidden faces left out. See
// core/preview/pack_scene.hpp.

#include "framework.hpp"

#include "core/block/registry.hpp"
#include "core/preview/pack_scene.hpp"

#include <cstring>

using namespace mc;

TEST(pack_scene_is_the_same_scene_every_time)
{
    mesh::MeshBuilder first;
    mesh::MeshBuilder second;
    preview::buildPackScene(&first);
    preview::buildPackScene(&second);

    CHECK(first.detailVertexCount() > 0);
    CHECK_EQ(first.detailVertexCount() % 4, usize(0));
    CHECK_EQ(first.detailVertexCount(), second.detailVertexCount());
    CHECK(std::memcmp(first.detailVertices(), second.detailVertices(),
                      first.detailVertexCount() * sizeof(mesh::DetailVertex))
          == 0);
}

TEST(pack_scene_hides_faces_between_opaque_blocks)
{
    mesh::MeshBuilder builder;
    preview::buildPackScene(&builder);

    // 36 grass blocks as a floor alone would be 36 * 6 faces; the floor is a
    // single layer, so every inner side face is against grass and gone. The
    // whole scene has fewer quads than its blocks have faces by a wide margin.
    const usize quads = builder.detailQuadCount();
    CHECK(quads < usize(70 * 6));

    // No quad sits inside the floor: a grass block's bottom face at y = 0
    // faces out of the scene and is kept, but nothing is drawn at y between
    // 0 and 1 that faces sideways inside the floor's interior.
    const mesh::DetailVertex* v = builder.detailVertices();
    int innerSides = 0;
    for (usize q = 0; q < quads; ++q) {
        const mesh::DetailVertex& a = v[q * 4];
        const bool side = a.face >= mesh::kFaceNegZ;
        const bool floorBand = a.y >= 0 && a.y <= mesh::kDetailUnitsPerBlock;
        int minX = a.x;
        int minZ = a.z;
        for (int c = 1; c < 4; ++c) {
            minX = v[q * 4 + usize(c)].x < minX ? v[q * 4 + usize(c)].x : minX;
            minZ = v[q * 4 + usize(c)].z < minZ ? v[q * 4 + usize(c)].z : minZ;
        }
        const bool interior = minX > 0 && minZ > 0
                              && minX < (preview::kPackSceneEdge - 1) * mesh::kDetailUnitsPerBlock
                              && minZ < (preview::kPackSceneEdge - 1) * mesh::kDetailUnitsPerBlock;
        bool allFloor = floorBand;
        for (int c = 1; c < 4; ++c) {
            const i16 y = v[q * 4 + usize(c)].y;
            allFloor = allFloor && y >= 0 && y <= mesh::kDetailUnitsPerBlock;
        }
        if (side && allFloor && interior) {
            ++innerSides;
        }
    }
    CHECK_EQ(innerSides, 0);
}
