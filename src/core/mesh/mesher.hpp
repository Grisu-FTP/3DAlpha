#pragma once

// Turning a section into quads.
//
// The output is a plain vertex array in the format shaders/world.v.pica reads,
// with no index data: every chunk draws through one immutable index buffer that
// repeats 0,1,2, 0,2,3, so a mesh costs nothing but its vertices. See
// docs/3ds-performance.md section 1.
//
// This file is platform-independent on purpose. A face emitted in the wrong
// winding or a light level sampled from the wrong side is a bug you want to
// find in a host test, not by squinting at a 240-line screen.

#include "core/block/block_def.hpp"
#include "core/mesh/scratch.hpp"
#include "core/mesh/vertex.hpp"
#include "core/util/types.hpp"

#include <vector>

namespace mc::mesh {

// Which of the two DetailVertex ranges a quad belongs to. See MeshRanges: the
// third range is drawn last, sorted back to front and blended, and a quad is in
// it because a1.1.2's getRenderBlockPass says so.
enum class DetailPass : u8 {
    Opaque,
    Translucent,
};

// Three streams, because a section holds two vertex formats and three draw
// passes: cubes in the 12-byte WorldVertex, and the non-cube shapes in the
// 16-byte DetailVertex split by pass. They are kept apart here and concatenated
// at upload time, so a section is still one allocation and one pool slot -- the
// renderer draws all the cubes, then changes the shader and attribute layout
// once for the whole frame, then all the detail, then the blend state once,
// then all the translucent.
class MeshBuilder {
public:
    void clear()
    {
        cubes_.clear();
        quads_.clear();
        details_.clear();
        translucent_.clear();
        dropped_ = 0;
        discardFaces();
    }

    // Quads refused because the stream had reached kMaxQuadsPerSection. Zero in
    // every world anyone has meshed; it exists so that if a section ever does
    // reach the bound, that shows up as a number rather than as a face quietly
    // missing.
    u32 droppedQuads() const { return dropped_; }

    // Reusing one builder across sections is the point: the vectors keep their
    // capacity, so a worker thread allocates during the first few meshes and
    // never again.
    void reserveQuads(int quads)
    {
        cubes_.reserve(static_cast<usize>(quads) * 4);
        quads_.reserve(static_cast<usize>(quads));
        ensureFaceGrid();
    }

    // **Greedy meshing: equal neighbouring cube faces become one quad.** On by
    // default. "Equal" is the same face direction, the same plane, the same
    // tile and the same light byte -- everything a cube quad carries -- so a
    // merged quad draws exactly the texels and the brightness its faces would
    // have, and only the count changes. Runs stop at kCubeRepeat along either
    // edge because that is how many copies of a tile the cube atlas holds; see
    // core/mesh/cube_atlas.hpp.
    //
    // Off is the old one-quad-per-face mesh, still through the cube atlas, so
    // the renderer does not need to know which it is drawing. Like the cube
    // format, changing it only affects meshes built afterwards:
    // WorldStreamer::setGreedy is the caller that re-meshes.
    void setGreedy(bool on)
    {
        greedy_ = on;
        clear();
    }

    bool greedy() const { return greedy_; }

    // Which encoding the cube range comes out in. Both are produced by the same
    // addQuad from the same arguments, which is what lets a test mesh a section
    // twice and check the two describe the same geometry.
    //
    // Changing this mid-life is legal but every mesh already built is in the old
    // format, so the caller has to re-mesh: WorldStreamer::setCubeFormat is the
    // one that knows how.
    void setCubeFormat(CubeFormat format)
    {
        cubeFormat_ = format;
        clear();
    }

    CubeFormat cubeFormat() const { return cubeFormat_; }

    // One cube quad, written straight to the cube stream.
    //
    // (x, y, z) is the cell the quad starts at, and it covers `width` cells
    // along its face's e1 edge and `height` along e2 (kFaceBasis), each at most
    // kCubeRepeat. **The start is the cell at i = j = 0 of the run, which is not
    // always its lowest coordinate**: on -Z, e1 is -X, so a run along x starts
    // at its largest x. That is what lets both formats build the corners as
    // base + i*width*e1 + j*height*e2 with no special cases.
    void addQuad(int x, int y, int z, int face, u16 texture, u8 light, int width = 1,
                 int height = 1);

    // One visible cube face, which greedy meshing may merge with its
    // neighbours. **Nothing reaches the cube stream until flushFaces()**,
    // which meshSection calls once the section has been walked; with greedy
    // meshing off this is addQuad and the flush has nothing to do.
    void addFace(int x, int y, int z, int face, u16 texture, u8 light);
    void flushFaces();

    // A quad of arbitrary sub-block geometry. Corners are in detail units
    // (1/1024 of a block) relative to the section, wound counter-clockwise seen
    // from the side that should be visible.
    void addDetailQuad(const i16 corner[4][3], const i16 uv[4][2], u8 shade, u8 light, int face,
                       DetailPass pass);

    // Only meaningful in CubeFormat::Vertices; empty in the other.
    const WorldVertex* vertices() const { return cubes_.data(); }
    usize vertexCount() const { return cubes_.size(); }

    // Only meaningful in CubeFormat::Quads; empty in the other.
    const QuadVertex* quads() const { return quads_.data(); }

    // Cube quads, whichever format they came out in.
    usize quadCount() const
    {
        return cubeFormat_ == CubeFormat::Quads ? quads_.size() : cubes_.size() / 4;
    }

    const DetailVertex* detailVertices() const { return details_.data(); }
    usize detailVertexCount() const { return details_.size(); }
    usize detailQuadCount() const { return details_.size() / 4; }

    const DetailVertex* translucentVertices() const { return translucent_.data(); }
    usize translucentVertexCount() const { return translucent_.size(); }
    usize translucentQuadCount() const { return translucent_.size() / 4; }

    bool empty() const
    {
        return cubes_.empty() && quads_.empty() && details_.empty() && translucent_.empty();
    }

    MeshRanges ranges() const
    {
        return {quadCount() * cubeBytesPerQuad(cubeFormat_),
                details_.size() * sizeof(DetailVertex),
                translucent_.size() * sizeof(DetailVertex), cubeFormat_};
    }

    usize byteSize() const { return ranges().total(); }

    // Cubes, then opaque detail, then translucent detail. One contiguous buffer
    // per section, so the pool holds one block and the draw loop takes three
    // ranges out of it.
    void copyTo(void* destination) const;

private:
    // addQuad with the cube-atlas slot already looked up, which is what the
    // face grid stores.
    void emitQuad(int x, int y, int z, int face, int slot, u8 light, int width, int height);

    void ensureFaceGrid();
    void discardFaces();

    CubeFormat cubeFormat_ = CubeFormat::Vertices;
    bool greedy_ = true;

    // **The faces waiting to be merged**, kept apart per face direction and
    // per layer along its normal, each layer a 16x16 grid in the face's own
    // (i, j) -- the axes of kFaceBasis's e1 and e2, so a run found here is a
    // run addQuad can draw without turning it round.
    //
    //   faceKeys_   [face][layer][j][i]  (slot << 8) | light; read only where
    //                                    the row bit says a face is there, so
    //                                    it is never cleared
    //   faceRows_   [face][layer][j]     bit i set for a face at (i, j)
    //   faceLayers_ [face]               bit `layer` set for a layer with any
    //
    // 51 KB, on the heap and allocated once: the builder can live wherever its
    // owner does, and the 3DS main thread has a 32 KB stack. The merge clears
    // each row bit as it covers it, so a flushed grid is already empty and the
    // next section starts without a memset.
    std::vector<u16> faceKeys_;
    std::vector<u16> faceRows_;
    u16 faceLayers_[kFaceCount] = {};
    bool facesPending_ = false;

    // Exactly one of these is ever non-empty. Two vectors rather than a union
    // because the capacity of the unused one costs nothing on the console --
    // reserveQuads sizes both once at startup and neither ever grows again --
    // and a union would need the format switch to destroy and rebuild them.
    std::vector<WorldVertex> cubes_;
    std::vector<QuadVertex> quads_;

    u32 dropped_ = 0;

    std::vector<DetailVertex> details_;
    std::vector<DetailVertex> translucent_;
};

// Appends the section's geometry to `out`. The builder is not cleared, so a
// caller batching several sections into one buffer can, though nothing does
// yet -- per-section meshes are what the visibility graph culls.
void meshSection(const MeshScratch& scratch, MeshBuilder& out);

// Whether this build can turn a block of this render type into geometry.
//
// Not a second list: it is the same switch meshSection dispatches on, and
// tests/mesher_test.cpp meshes one block of every render type to check the two
// agree in both directions -- anything this says yes to must produce quads,
// and anything it says no to must produce none. Without that, "implemented"
// drifts from "reached", which is precisely the bug a `continue` in the
// dispatch would hide.
bool hasEmitter(block::RenderType type);

// True when the section cannot produce geometry no matter what surrounds it.
// Only uniform air qualifies: a uniform stone section buried in stone also
// emits nothing, but proving that needs its neighbours, and this test has to be
// cheap enough to run on every section every time the world moves.
bool sectionIsEmpty(const world::ChunkColumn& column, int sectionY);

}  // namespace mc::mesh
