#pragma once

// What the engine knows about a block.
//
// The renderer and the physics never see a block id: they see a RenderType and
// a handful of flags. That is the whole anti-hardcoding rule for blocks -- add
// a version whose stone is id 1 and whose "stone stairs" is id 109, and nothing
// outside the generated table changes. See CONTRIBUTING.md.
//
// The table itself is generated from data/<version>/blocks.json into
// build/<version>/gen/blocks.hpp, and that JSON is in turn recovered from an
// original client jar by tools/extract_blocks.py. Nothing in this file is
// version-specific.

#include "core/util/types.hpp"

namespace mc::block {

// A numeric block id as it appears in the world arrays and on the wire.
// Deliberately a plain integer and not the generated enum: a world file or a
// server can contain an id this build does not know about, and storage has to
// round-trip it rather than reject it. 16 bits because later versions exceed
// 255 ids, and widening it after the fact would touch every array.
using BlockId = u16;

// Air is 0 in every version this project targets, and the emptiness checks that
// let the mesher skip whole sections need to know it.
inline constexpr BlockId kAir = 0;

// How a block turns into geometry. The numbering matches the original game's
// getRenderType, because that is where the values were read from and keeping
// them aligned makes the extraction re-checkable.
enum class RenderType : u8 {
    None = 0,          // drawn by something else entirely (signs)
    Cube,              // the ordinary case, and the only one greedy meshing
    Cross,             // two intersecting quads: flowers, saplings, sugar cane
    Torch,
    Fire,
    Fluid,
    RedstoneWire,
    Crops,
    Door,
    Ladder,
    Rail,
    Stairs,
    Fence,
    Lever,
    Cactus,
    Count,
};

const char* renderTypeName(RenderType type);

// What a block is shaped like when something walks into it -- collision's
// answer to RenderType, and it exists for exactly the same reason. Collision
// code references shapes and never block ids, so a version whose stairs are not
// id 53 needs no change here.
//
// **None of the three existing columns could serve.** `solid` is the original's
// Material.isSolid() and is wrong in both directions -- a stone button and a
// snow layer are solid and do not collide, glass and leaves are solid and are
// not full cubes. `fullCube` and `opaqueCube` are render properties. The note
// further down this file says as much; this is the column it was waiting for.
//
// The numbering is ours: a1.1.2 has no such concept, and the shapes below were
// recovered by asking a running jar for every block's collision boxes at every
// metadata value and grouping the answers. See tools/genref.java --collision
// and tests/collision_box_vectors.hpp, which is that measurement.
//
// A shape is a pure function of (block, metadata) -- **measured, not assumed**:
// nothing in a1.1.2 consults a neighbour, not even the top half of a door,
// which reads its own low three bits. That is what lets collisionBoxes() take
// no world at all. See core/block/collision.hpp.
enum class Shape : u8 {
    None = 0,   // no collision: air, fluids, plants, torches, rails, snow, fire
    FullCube,   // the ordinary case
    Slab,       // the bottom half only
    Stairs,     // two boxes, and the only shape that answers with more than one
    Door,       // a thin plate, which of the four sides coming from metadata
    Ladder,     // a thinner plate, likewise
    Fence,      // a full cube that is **one and a half blocks tall**, not one
    Cactus,     // inset a sixteenth all round, and a sixteenth short on top
    Count,
};

const char* shapeName(Shape shape);

// What a block *does* when the world ticks it -- the tick system's answer to
// RenderType, and it exists for the same reason. The renderer references
// render types and never block ids; the tick system references behaviours and
// never block ids, so a version whose grass is not id 2 needs no code change.
//
// The numbering is ours, not the game's: a1.1.2 dispatches on the class of the
// Block object and obfuscated class names are meaningless across versions.
// What is taken from the jar is the *grouping* -- one entry per class that
// overrides updateTick, onNeighborBlockChange or onBlockAdded, which is why
// sand and gravel share `Falling` and the four flowers share `Plant`.
enum class TickBehaviour : u8 {
    None = 0,        // the great majority: stone does nothing when ticked
    Grass,           // spreads onto dirt, dies under an opaque block
    Sapling,         // grows into a tree on the second roll
    Leaves,          // decays when no log is within four blocks
    Plant,           // flowers: stays only on a valid ground block in light
    Mushroom,        // as Plant, but wants darkness instead of light
    Crops,           // wheat, growing on the moisture of the farmland below
    Farmland,        // wets from nearby water, reverts to dirt when dry
    Reed,            // sugar cane, growing up to three tall
    Cactus,          // as Reed, and refuses a neighbour on any side
    FluidFlowing,    // water and lava spreading
    FluidStill,      // a source, which only wakes when a neighbour changes
    Falling,         // sand and gravel
    Fire,            // spread, burn-out, and what it sets alight
    Ice,             // melts to water in light
    SnowLayer,       // melts, and falls off an unsupporting block
    SnowBlock,       // melts to nothing in light
    Torch,           // drops when what it is attached to goes away
    RedstoneTorch,
    RedstoneWire,
    RedstoneOre,
    Button,
    PressurePlate,
    Lever,
    Door,
    Rail,
    Ladder,
    Sign,
    Tnt,
    Sponge,
    Stairs,          // delegates to the block it is modelled on
    Count,
};

const char* tickBehaviourName(TickBehaviour behaviour);

struct BlockDef {
    const char* name;

    float hardness;    // -1 means unbreakable
    float resistance;  // against explosions

    // Index into the terrain atlas: the value the original constructor was
    // given, and what the block shows anywhere a single tile is wanted.
    u16 texture;

    // Per-face atlas tiles, in the game's face order (`mc::mesh::Face`, which
    // this deliberately matches so the table can be read straight out of the
    // jar): grass is dirt underneath and grass on top, a log's rings face up,
    // a furnace's mouth faces one way. Always filled -- a block with one
    // texture on every side repeats it -- so the mesher indexes rather than
    // branching, and `faces[f]` is always the answer.
    //
    // Recovered by interpreting getBlockTexture out of the client jar rather
    // than by reading it, because every one of them is a branch on the face
    // index. See tools/javap.py.
    //
    // Faces whose tile depends on the surrounding world (which way a chest
    // faces, snow on grass) or on block metadata (wheat's growth stage) hold
    // the no-metadata, no-neighbours answer, and blocks.json records which
    // those are. Nothing reads metadata yet.
    u16 faces[6];

    RenderType render;

    // How the block collides. Beside `render` because the two are the same kind
    // of thing -- a dispatch column that keeps ids out of the code that uses it.
    Shape shape;

    // Ground friction for something standing on top. 0.6 for everything in
    // a1.1.2 except ice, which is 0.98 -- and the difference between those two
    // numbers is the whole of why ice is ice.
    //
    // `moveEntityWithHeading` multiplies it by 0.91 to get the per-tick factor
    // on horizontal motion, and derives the acceleration term from its cube, so
    // a higher value both keeps more speed and grants less control.
    float slipperiness;

    // Whether a ray notices this block -- `Block.canCollideCheck`, which in
    // a1.1.2 is just `isCollidable()`. False for water, lava and fire and true
    // for everything else, including blocks you cannot walk into: a torch has
    // no collision box and is still perfectly targetable, which is the whole
    // reason the selection shape is a separate table from the collision one.
    bool targetable;

    // The original's own material grouping, as a dense index -- 0 is air and
    // whatever else this version never constructs. Two questions are asked of
    // it and no others: whether two blocks share a material, which is how a
    // fluid recognises its own kind rather than by block id, and `solid`
    // below.
    //
    // Recovered from the jar like everything else here: `Block`'s constructor
    // names the Material class in its signature, and the field it lands in is
    // found by running the constructor with a marker. See
    // tools/extract_blocks.py.
    u8 material;

    u8 light;    // 0..15 emitted
    u8 opacity;  // 0..255 absorbed as light passes through

    // The face-culling test: a face touching an opaque neighbour is not
    // emitted. This is the single most consulted property in the mesher, which
    // is why it is a byte and not a call.
    bool opaque;

    // Renders as a full cube. Distinct from `opaque` -- glass and leaves are
    // full cubes that do not block light.
    bool fullCube;

    // `Block.opaqueCubeLookup[id]`, which is **not** the same question as
    // `opaque` above even though it agrees with it for all but two blocks.
    //
    // The array is filled once in the Block constructor. `opaque` is what
    // `isOpaqueCube()` answers at *runtime*, and for leaves that is
    // `!fancyGraphics` -- a video option, defaulted to fancy, applied long
    // after the array was built. So the cached answer for leaves is "solid"
    // and the live one is "see-through", and both are correct.
    //
    // Which to use is decided by what the original consults. Face culling in
    // the mesher calls the live method, so it wants `opaque`. **World
    // generation reads the array**: BlockMushroom's ground test is a plain
    // lookup into it, which is why a1.1.2 will grow a mushroom on a leaf
    // block and why that needs this column rather than the one next to it.
    //
    // Pinned against a loaded jar by tests/opaque_cube_test.cpp.
    bool opaqueCube;

    // `getRenderBlockPass() == 1`: this block's geometry belongs in the sorted,
    // blended pass rather than the opaque one.
    //
    // Three blocks in a1.1.2 -- still water, flowing water and ice -- and
    // notably **not** lava, which shares a class with water and is told apart
    // only by its material. Glass and leaves are not translucent either: they
    // are cut out by the alpha test in the opaque pass, which is why this is
    // not the same question as `opaque`.
    bool translucent;

    // `Material.isSolid()`. Distinct from **both** of the above and from the
    // render type, which is exactly why it is its own column rather than
    // something derived: glass and leaves are solid but not opaque, stairs and
    // doors are solid but not full cubes, and a stone button and a snow layer
    // are neither solid nor anything else that would give them away. Any rule
    // inferred from the other columns gets at least four of a1.1.2's blocks
    // wrong.
    //
    // What consults it: a fluid's surface height, where a solid neighbour
    // leaves the height alone and a non-solid one pulls it down.
    bool solid;

    // What the world tick does with this block, and how often.
    //
    // `tickRandomly` is `Block.tickOnLoad[id]`, the gate the random-tick loop
    // consults 80 times per chunk per tick before it dispatches anything, so
    // it is read far more than it is acted on -- hence a byte in the table
    // rather than a call. `tickRate` is `Block.tickRate()`, the delay a
    // scheduled update waits: 10 for almost everything, 5 for water, 30 for
    // lava, 3 for sand, 2 for a redstone torch.
    //
    // Both are read out of a running jar rather than out of its bytecode, and
    // that is not fussiness: three block ids get the wrong answer from the
    // bytecode because their constructors branch. See tools/extract_ticks.java.
    TickBehaviour tick;
    u8 tickRate;
    bool tickRandomly;

    // Fire, and three questions that are not the same question.
    //
    // `burnEncourage` is `BlockFire.chanceToEncourageFire[id]`: non-zero is
    // what "this can catch fire" means to a fire block beside it, and the
    // value is how strongly it invites fire into the air nearby.
    // `burnCatch` is `BlockFire.abilityToCatchFire[id]`, rolled against a
    // per-direction chance to decide whether the block itself is consumed.
    // Both are zero for all but six blocks in a1.1.2.
    //
    // `canBurn` is `Material.getCanBurn()`, and it is a **wider** set --
    // fourteen blocks, including chests, signs, doors and fences, which have a
    // burnable material and are in neither fire table. Lava's ignition search
    // reads this one and the fire block reads the other two, so conflating
    // them sets light to the wrong things.
    u8 burnEncourage;
    u8 burnCatch;
    bool canBurn;

    // False for ids this version does not define. A world or a server can name
    // a block we have never heard of, and the mesher has to survive it rather
    // than index past the end of the table. Unknown ids are given a solid
    // opaque cube on purpose: a visible wrong block is a bug report, an
    // invisible one is a mystery.
    bool known;
};

}  // namespace mc::block
