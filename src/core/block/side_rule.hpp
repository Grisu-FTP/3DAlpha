#pragma once

// **`ly.c(Lnm;IIII)Z` -- shouldSideBeRendered**, the question the mesher asks
// before it emits a face, as one function over the block table.
//
// It lives in a header of its own because two paths ask it -- the cube fast
// path in `mesh::meshSection` and `addBoundedCube` for a standard block that
// does not fill its cell -- and because it used to be one field read inline:
// `!block::def(neighbour).opaque`. That is the base class and it is right for
// sixty-five blocks in a1.1.2. It is wrong for six, and the one a player sees
// immediately is glass: a wall of it drew every internal pane, so what should
// have been a window was a stack of boxes.
//
// **The asking block is the one with the rule, not the neighbour.**
// `renderStandardBlock` calls `block.shouldSideBeRendered(access, i, j - 1, k,
// 0)` -- the block being drawn, handed the *cell beyond the face*. Getting that
// round the wrong way makes glass hide its neighbours' faces instead of its
// own.
//
// **`face` is the mesh face index**, which is the jar's own numbering: 0 is
// -Y, 1 is +Y, 2 is -Z, 3 is +Z, 4 is -X, 5 is +X. Two of the four rules read
// it and both only care whether it is the top.
//
// See `block::SideRule` for the four rules and where the column comes from.

#include "core/block/block_def.hpp"
#include "core/block/registry.hpp"
#include "core/mesh/vertex.hpp"
#include "core/util/types.hpp"

namespace mc::block {

// True when the face should be emitted. `self` is the block being drawn and
// `neighbour` is what is in the cell the face looks into.
//
// `selfDef` is passed in rather than looked up because every caller has it
// already -- this sits in the mesher's inner loop, where the row has just been
// read and a second lookup is a second cache line.
inline bool sideVisible(BlockId self, const BlockDef& selfDef, BlockId neighbour, int face)
{
    const BlockDef& other = def(neighbour);

    switch (selfDef.sideRule) {
    case SideRule::OwnKind:
        // `fc`/`hi`: `if (!this.a && world.getBlockId(...) == blockID) return
        // false;` and then the base class. Glass, ice and leaves all construct
        // with `a` false, so the test is unconditional here -- there is no
        // second state of it in this version to carry.
        if (neighbour == self) {
            return false;
        }
        break;

    case SideRule::OwnMaterial:
        // `fd`: the top first, then the material, then the base class. **The
        // top face is answered before anything else**, so a snow layer under
        // another one still draws its own top -- which is what keeps a pile
        // from looking hollow when the layer above is thinner.
        if (face == mesh::kFacePosY) {
            return true;
        }
        if (other.material == selfDef.material) {
            return false;
        }
        break;

    case SideRule::Slab:
        // `oi`: top always, then the base class, then bottom always, then the
        // id. **The order is the class file's and it is not interchangeable**:
        // the base test sits *between* the two faces that ignore it, so a slab
        // buried in stone draws its top and loses its bottom. What `l == 0`
        // buys is the underside of a slab stacked on another slab, where the
        // two boxes do not meet and the id test would otherwise hide it.
        if (face == mesh::kFacePosY) {
            return true;
        }
        if (other.opaque) {
            return false;
        }
        if (face == mesh::kFaceNegY) {
            return true;
        }
        return neighbour != self;

    case SideRule::None:
    default:
        break;
    }

    // `ly.c` itself, which every rule above falls through to.
    return !other.opaque;
}

}  // namespace mc::block
