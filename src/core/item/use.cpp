// The right-click path. See use.hpp for why it is in core.

#include "core/item/use.hpp"

#include "core/block/collision.hpp"
#include "core/block/registry.hpp"
#include "core/audio/block_sound.hpp"
#include "core/audio/sound_engine.hpp"
#include "core/entity/arrow.hpp"
#include "core/entity/boat.hpp"
#include "core/entity/minecart.hpp"
#include "core/entity/mob.hpp"
#include "core/entity/player_body.hpp"
#include "core/net/entities.hpp"
#include "core/world/sign_store.hpp"
#include "core/entity/painting.hpp"
#include "core/entity/particle.hpp"
#include "core/entity/ray_trace.hpp"
#include "core/item/registry.hpp"
#include "core/tick/behaviour.hpp"
#include "core/tick/drop.hpp"
#include "core/tick/tick_world.hpp"
#include "core/util/java_random.hpp"

#include <cmath>
#include "core/util/math_helper.hpp"

namespace mc::item {
namespace {

using block::BlockId;
using block::TickBehaviour;

// `cn.a(IIIIZ)Z` -- **World.canBlockBePlacedAt**, which is not the same method
// as `Block.canPlaceBlockAt` and is the one a placement actually asks:
//
// ```
// Block existing = Block.blocksList[getBlockId(i, j, k)];
// AxisAlignedBB box = flag ? null : Block.blocksList[id].getCollisionBoundingBoxFromPool(...);
// if (box != null && !checkIfAABBIsClear(box)) return false;
// if (existing == water || lava || fire || snow) return true;
// return id > 0 && existing == null && Block.blocksList[id].canPlaceBlockAt(...);
// ```
//
// **The early return is the part worth having.** A cell holding water, lava,
// fire or a snow layer takes the block *whatever the block's own rule says* --
// the override is never reached. So a1.1.2 lets you put a sapling into water
// and a torch into fire, and then the tick takes them away a moment later,
// which is a sequence this port had quietly tightened into a refusal.
bool canBePlacedAt(const tick::TickWorld& world, BlockId placed, u8 metadata, i32 x, int y,
                   i32 z, const AABB& playerBox)
{
    if (y < 0 || y >= mcver::kWorldHeight) {
        return false;
    }

    // `checkIfAABBIsClear`: every entity in the box that prevents spawning
    // refuses it, and the player is the only one there is here. Without this a
    // block placed at the feet pushes the body out of the world, and in
    // Creative -- where nothing stops you looking straight down -- that is one
    // press away at all times.
    //
    // **The metadata is the one the block is about to land with, not zero.**
    // `getCollisionBoundingBoxFromPool` reads the *world's* metadata, which at
    // an empty cell is 0 -- and a ladder at metadata 0 is outside the 2..5 the
    // game writes, where a1.1.2 answers with whatever the shared Block
    // singleton was last left holding and this port answers with the
    // constructor's full cube (core/block/collision.cpp says why). A full cube
    // in the cell in front of you overlaps the body, so **a ladder could not be
    // hung on the wall you were standing against** -- which is exactly where a
    // ladder goes. Asking with the metadata `onBlockPlaced` is about to write
    // is deterministic, needs no leftover state, and agrees with the original
    // everywhere the original is not reading its own litter.
    AABB boxes[block::kMaxCollisionBoxes];
    const int count = block::collisionBoxes(placed, metadata, boxes, block::kMaxCollisionBoxes);
    for (int i = 0; i < count; ++i) {
        if (boxes[i].offset(double(x), double(y), double(z)).intersects(playerBox)) {
            return false;
        }
    }

    switch (block::def(world.blockAt(x, y, z)).tick) {
    case TickBehaviour::FluidFlowing:
    case TickBehaviour::FluidStill:
    case TickBehaviour::Fire:
    case TickBehaviour::SnowLayer:
        return true;
    default:
        break;
    }

    return world.blockAt(x, y, z) == block::kAir
           && tick::canPlaceAt(world, placed, x, y, z);
}

// The one sound line all three paths share: a block's own cue, played where
// the block is -- `i + 0.5F` on every axis, which is its centre.
void playAt(const Effects& effects, const audio::SoundCue& cue, i32 x, int y, i32 z)
{
    if (effects.sound == nullptr || !cue.playable()) {
        return;
    }
    effects.sound->playSoundAt(cue.key, double(x) + 0.5, double(y) + 0.5, double(z) + 0.5,
                               cue.volume, cue.pitch);
}

// The two blocks a sign item can place, found by their tick behaviour rather
// than by their ids -- the same rule the door and the fire paths above follow.
// A version with neither leaves them air and `useSign` refuses.
BlockId signPostBlock()
{
    for (int id = 0; id < mcver::kBlockTableSize; ++id) {
        if (mcver::kBlocks[id].known && mcver::kBlocks[id].tick == TickBehaviour::SignPost) {
            return BlockId(id);
        }
    }
    return block::kAir;
}

BlockId wallSignBlock()
{
    for (int id = 0; id < mcver::kBlockTableSize; ++id) {
        if (mcver::kBlocks[id].known && mcver::kBlocks[id].tick == TickBehaviour::SignWall) {
            return BlockId(id);
        }
    }
    return block::kAir;
}

// `random.bow`, at the pitch `jg` gives it: `1.0F / (rand.nextFloat() * 0.4F + 0.8F)`.
//
// **Its own generator, seeded from the shot.** Drawing the pitch from the
// arrow pool's random would shift the launch scatter of the very arrow being
// fired, which is the kind of coupling that makes a vector test disagree with
// itself. A pitch nobody can hear the seed of is the right place to not care.
void playBowShot(const Effects& effects, double x, double y, double z)
{
    if (effects.sound == nullptr) {
        return;
    }
    static JavaRandom pitchRand(0x626f77LL);  // "bow"
    const float pitch = 1.0f / (pitchRand.nextFloat() * 0.4f + 0.8f);
    effects.sound->playSoundAt("random.bow", x, y, z, 1.0f, pitch);
}

// `md.a(Lev;Ldm;Lcn;IIII)Z` -- **ItemSign.onItemUse**, which is not an
// `ItemBlock` and does four things an ordinary placement does not.
//
//   * It **refuses the underside** outright: face 0 returns false, so a sign
//     cannot hang from a ceiling in this version.
//   * It tests the **struck block's material**, not the target cell's -- a sign
//     needs something solid to stand on or hang off, and the solid thing is the
//     block that was clicked.
//   * It chooses **which of two blocks** to place from the face: the top gives
//     a sign post whose metadata is the player's heading rounded to a
//     sixteenth, and the four sides give a wall sign whose metadata is the face
//     itself. Nothing else in a1.1.2 places two different blocks from one item
//     except the door, and the door places the same block twice.
//   * It **opens the editor**, which is the half that was reported: a sign
//     placed with no keyboard is a sign that can never say anything.
//
// The heading is `floor_double((yaw + 180) * 16 / 360 + 0.5) & 15`, which is
// the original's own rounding -- a half added before the floor, and the mask
// rather than a modulo so a negative yaw still lands in range.
bool useSign(tick::TickWorld& world, const entity::RayHit& hit, float yawDegrees,
             const Effects& effects)
{
    const int face = int(hit.face);
    if (face == 0) {
        return false;
    }
    // `world.getBlockMaterial(x, y, z).isSolid()` on the *struck* block.
    if (!block::def(world.blockAt(hit.x, hit.y, hit.z)).solid) {
        return false;
    }

    const i32 x = hit.placeX();
    const int y = hit.placeY();
    const i32 z = hit.placeZ();

    // The cell has to be free. `Block.canPlaceBlockAt` is "air or something
    // that gets replaced", which is what `canBePlacedAt` already answers.
    BlockId placed = block::kAir;
    u8 metadata = 0;
    if (face == 1) {
        placed = signPostBlock();
        const double turns =
            (double(yawDegrees) + 180.0) * 16.0 / 360.0 + 0.5;
        metadata = u8(MathHelper::floorDouble(turns) & 15);
    } else {
        placed = wallSignBlock();
        metadata = u8(face);
    }
    if (placed == block::kAir) {
        return false;
    }
    if (!canBePlacedAt(world, placed, metadata, x, y, z, AABB{})) {
        return false;
    }

    // **The tile entity, which is the whole of a sign -- and it goes first.**
    // Without it the block is render type -1 and draws nothing at all (see
    // core/world/sign_store.hpp), so a store that would not hold another sign
    // has to refuse the click before the block is written, not after: the
    // other order left an invisible block that took a text nobody could see.
    if (effects.entities.signs != nullptr
        && effects.entities.signs->put(x, y, z, face != 1, metadata) < 0) {
        return false;
    }

    world.setBlockAndDataWithNotify(x, y, z, placed, metadata);
    playAt(effects, audio::placeCue(placed), x, y, z);
    return true;
}

// `av.a(Lev;Ldm;Lcn;IIII)Z` -- ItemBlock.onItemUse, which every ordinary item
// in the hand goes through.
bool useBlock(tick::TickWorld& world, BlockId placed, const entity::RayHit& hit,
              const AABB& playerBox, const Effects& effects)
{
    i32 x = hit.x;
    int y = hit.y;
    i32 z = hit.z;
    int face = int(hit.face);

    // **A snow layer is replaced, not built on**, and it is the first line of
    // the method: the block goes into the snow's own cell and the struck face
    // is rewritten to 0 rather than offset. Without it, clicking the top of a
    // snow layer left the new block hovering a cell up with two inches of snow
    // under it.
    if (block::def(world.blockAt(x, y, z)).tick == TickBehaviour::SnowLayer) {
        face = 0;
    } else {
        x = hit.placeX();
        y = hit.placeY();
        z = hit.placeZ();
    }

    // `onBlockPlaced`, as a table: the struck face is what a torch, a ladder,
    // a lever and a button orient from. See the note on
    // block::placementMetadata. It is worked out before the clearance test
    // because the clearance test asks about the shape this metadata gives.
    //
    // It lands *before* the block's onBlockAdded rather than after, which is
    // the one liberty taken with the order: a1.1.2 writes metadata 0, runs
    // onBlockAdded, then onBlockPlaced. The blocks that orient in onBlockAdded
    // -- the furnace, the staircase -- have a 0 here, so they start from the
    // same 0. tests/placement_test.cpp runs this order for every block and
    // face in the jar's sweep and gets the jar's metadata back.
    //
    // **The table is measured against a stone cube, and the struck block is
    // not always one.** A torch clicked onto the top of another torch beside a
    // wall hangs on the wall in a1.1.2, not on the torch -- `onBlockPlaced`
    // only overrides `onBlockAdded` when the struck face holds the block up.
    // See tick::attachedPlacementMetadata.
    const u8 metadata = tick::attachedPlacementMetadata(world, placed, x, y, z,
                                                        block::placementMetadata(placed, face));

    if (!canBePlacedAt(world, placed, metadata, x, y, z, playerBox)) {
        return false;
    }

    world.setBlockAndDataWithNotify(x, y, z, placed, metadata);

    // **A block is placed with a footstep**, at the break volume and pitch --
    // `stepSound.getStepSound()` against `onPlayerDestroyBlock`'s
    // `getBreakSound()`. See core/audio/block_sound.hpp for the table of the
    // three.
    playAt(effects, audio::placeCue(placed), x, y, z);
    return true;
}

// `ec.a(Lev;Ldm;Lcn;IIII)Z` -- **ItemDoor.onItemUse**, and a door is the one
// thing in a1.1.2 that an item places as two blocks.
//
// Three things in it are easy to miss and all three are visible to a player:
//
//   * **Only the top face places a door.** Clicking a wall does nothing at all;
//     the method's first line returns false on any other side.
//   * The facing comes from the player's heading, floored through a float
//     expression that has to be kept in float -- `(yaw + 180) * 4 / 360` -- and
//     then masked to two bits.
//   * **The hinge mirrors**, and the rule is neighbour-sensitive: a door
//     already standing on one side, or more solid wall on one side than the
//     other, flips the hinge by turning the facing back one quarter and setting
//     bit 2. It is what makes a pair of doors meet in the middle.
//
// There is no `checkIfAABBIsClear` here, which is not an omission: ItemDoor
// asks `Block.canPlaceBlockAt` directly and never the world's placement test,
// so a1.1.2 lets you close a door on yourself.
bool useDoor(tick::TickWorld& world, BlockId door, const entity::RayHit& hit,
             float yawDegrees, const Effects& effects)
{
    if (hit.face != mesh::kFacePosY) {
        return false;
    }

    const i32 x = hit.x;
    const int y = hit.y + 1;
    const i32 z = hit.z;

    // `fw.a(Lcn;III)Z`: a solid floor and room for the upper half. The height
    // check is inside it.
    if (!tick::canPlaceAt(world, door, x, y, z)) {
        return false;
    }

    // Kept in float exactly as the class file has it -- the multiply and the
    // divide are `fmul`/`fdiv` and only the `- 0.5` is a double.
    const float turn = (yawDegrees + 180.0f) * 4.0f / 360.0f;
    const int facing = int(MathHelper::floorDouble(double(turn) - 0.5)) & 3;

    // Which way "along the door" is, from the facing.
    int dx = 0;
    int dz = 0;
    if (facing == 0) dz = 1;
    if (facing == 1) dx = -1;
    if (facing == 2) dz = -1;
    if (facing == 3) dx = 1;

    const int solidBehind = (world.opaqueAt(x - dx, y, z - dz) ? 1 : 0)
                            + (world.opaqueAt(x - dx, y + 1, z - dz) ? 1 : 0);
    const int solidAhead = (world.opaqueAt(x + dx, y, z + dz) ? 1 : 0)
                           + (world.opaqueAt(x + dx, y + 1, z + dz) ? 1 : 0);
    const bool doorBehind = world.blockAt(x - dx, y, z - dz) == door
                            || world.blockAt(x - dx, y + 1, z - dz) == door;
    const bool doorAhead = world.blockAt(x + dx, y, z + dz) == door
                           || world.blockAt(x + dx, y + 1, z + dz) == door;

    int metadata = facing;
    if ((doorBehind && !doorAhead) || solidAhead > solidBehind) {
        metadata = ((facing - 1) & 3) + 4;
    }

    // Bit 3 is "this is the upper half", and every other bit is shared. The
    // original writes the block and then the metadata, separately and in that
    // order; writing both at once changes only what a neighbour woken in
    // between would have seen, and the only cell woken in between is the one
    // the other half is about to fill.
    world.setBlockAndDataWithNotify(x, y, z, door, u8(metadata));
    world.setBlockAndDataWithNotify(x, y + 1, z, door, u8(metadata + 8));

    // **No sound.** `ItemDoor.onItemUse` plays none -- the place sound belongs
    // to `ItemBlock`, and a door is not one. It is a real difference and this
    // is where it would otherwise be papered over.
    (void) effects;
    return true;
}

// ------------------------------------------------------------------- the hoe

// The block a hoe makes, found by its tick behaviour rather than by its id --
// the same rule `signPostBlock` and the door and fire branches follow. A
// version without one leaves it air and `useHoe` refuses.
BlockId farmlandBlock()
{
    for (int id = 0; id < mcver::kBlockTableSize; ++id) {
        if (mcver::kBlocks[id].known && mcver::kBlocks[id].tick == TickBehaviour::Farmland) {
            return BlockId(id);
        }
    }
    return block::kAir;
}

// `fu.a(Lev;Ldm;Lcn;IIII)Z` -- **ItemHoe.onItemUse**, the whole of what a hoe
// is. There is no other method on the class.
//
// ```
// int i1 = world.getBlockId(i, j, k);
// Material above = world.getBlockMaterial(i, j + 1, k);
// if ((above.isSolid() || i1 != Block.grass.blockID) && i1 != Block.dirt.blockID) return false;
// Block tilled = Block.tilledField;
// world.playSoundEffect(i + 0.5, j + 0.5, k + 0.5, tilled.stepSound.getStepSound(),
//                       (tilled.stepSound.getVolume() + 1.0F) / 2.0F,
//                       tilled.stepSound.getPitch() * 0.8F);
// world.setBlockWithNotify(i, j, k, tilled.blockID);
// itemstack.damageItem(1, entityplayer);
// if (world.rand.nextInt(8) != 0) return true;
// if (i1 != Block.grass.blockID) return true;
// int count = 1;
// for (int j1 = 0; j1 < count; j1++) {
//     float f = 0.7F;
//     float f1 = world.rand.nextFloat() * f + (1.0F - f) * 0.5F;
//     float f2 = 1.2F;
//     float f3 = world.rand.nextFloat() * f + (1.0F - f) * 0.5F;
//     EntityItem e = new EntityItem(world, i + f1, j + f2, k + f3, new ItemStack(Item.seeds));
//     e.delayBeforeCanPickup = 10;
//     world.entityJoinedWorld(e);
// }
// return true;
// ```
//
// Five things in it are not what a reader would guess, and four of them are
// visible in play:
//
//   * **The struck cell is the one that changes**, not the cell the face points
//     into. That is why `ItemDef::places` measures 0 for all five hoes and why
//     `tills` had to be a column of its own -- see item_def.hpp.
//   * **The face is never read.** `l` is a parameter and the method does not
//     mention it, so a hoe tills from underneath and from the side, and hoeing
//     the side of a dirt cliff turns that cell into farmland the crop on top
//     of it cannot use.
//   * **`isSolid` guards grass and not dirt.** Only the grass branch asks what
//     is above, so *dirt under a stone slab still tills*. Grass under anything
//     solid does not, which is the same material test that decides whether
//     grass dies -- but a torch, a flower or snow overhead is not solid and
//     does not stop it.
//   * **The place cue, exactly.** `stepSound.getStepSound()` at
//     `(volume + 1) / 2` and `pitch * 0.8` is `ItemBlock.onItemUse`'s row of
//     the table in core/audio/block_sound.hpp, arithmetic and all, so this is
//     that cue and not a second transcription of it.
//   * **The seed roll happens on dirt too.** `nextInt(8)` is drawn before the
//     block is compared against grass, so tilling dirt costs the world's random
//     a draw and yields nothing. Skipping it would be a different world
//     downstream, which is the reason `dropBlockAsItem` writes its own dead
//     draw out rather than folding it away.
//
// `count` is a literal 1: the loop is the shape the later versions grew a
// fortune roll into, and here it runs once. The offsets are float the whole
// way down -- the block coordinate is widened to float and the sum taken there
// -- exactly as the crop's seeds in core/tick/drop.cpp are, and `1.2F` is a
// constant rather than a third draw, so a successful roll costs two.
//
// **Durability is not spent.** `damageItem` has nowhere to go in this build --
// see `ItemDef::durability` -- and that is the one line of the method this does
// not perform.
bool useHoe(tick::TickWorld& world, const entity::RayHit& hit, const Effects& effects)
{
    const BlockId farmland = farmlandBlock();
    if (farmland == block::kAir) {
        return false;
    }

    const i32 x = hit.x;
    const int y = hit.y;
    const i32 z = hit.z;

    const BlockId struck = world.blockAt(x, y, z);
    const bool grass = struck == BlockId(mcver::Block::Grass);
    const bool dirt = struck == BlockId(mcver::Block::Dirt);
    // `getBlockMaterial(i, j + 1, k).isSolid()`, which above the top of the
    // world is air's and so is false.
    const bool coveredBySolid =
        y + 1 < mcver::kWorldHeight && block::def(world.blockAt(x, y + 1, z)).solid;
    if ((coveredBySolid || !grass) && !dirt) {
        return false;
    }

    playAt(effects, audio::placeCue(farmland), x, y, z);
    world.setBlockWithNotify(x, y, z, farmland);

    // One in eight, drawn whether or not there is any grass to pay for it.
    JavaRandom& rand = world.random();
    if (rand.nextInt(8) != 0 || !grass) {
        return true;
    }

    constexpr float kSpread = 0.7f;
    constexpr float kEdge = (1.0f - kSpread) * 0.5f;
    constexpr float kRise = 1.2f;
    const float ox = rand.nextFloat() * kSpread + kEdge;
    const float oz = rand.nextFloat() * kSpread + kEdge;
    world.spawnItem(double(float(x) + ox), double(float(y) + kRise), double(float(z) + oz),
                    u16(mcver::Item::Seeds), 1);
    return true;
}

// ------------------------------------------------------------------ the seeds

// `jn.a(Lev;Ldm;Lcn;IIII)Z` -- **ItemSeeds.onItemUse**, which is seven lines
// and one of them is missing a test:
//
// ```
// if (l != 1) return false;
// int i1 = world.getBlockId(i, j, k);
// if (i1 == Block.tilledField.blockID) {
//     world.setBlockWithNotify(i, j + 1, k, blockType);
//     itemstack.stackSize--;
//     return true;
// }
// return false;
// ```
//
// **It writes the crop over whatever is above the farmland.** There is no
// `isAirBlock`, no `canBlockBePlacedAt` and no `canPlaceBlockAt` anywhere in
// it: the only questions asked are "was the top face clicked" and "is the
// struck block farmland". Every other placement in the game goes through
// `ItemBlock.onItemUse` and is tested three ways; this one is not, and the
// later versions fixed it by adding the air test this transcribes without.
//
// So **a seed breaks bedrock**, and it is a legitimate thing to do in a1.1.2:
// farmland is fifteen sixteenths tall, so the sixteenth of a block above it
// stays clickable with a solid block sitting on top, and a crop written into
// that cell replaces it. Anything replaceable this way is replaced -- bedrock
// is the one worth naming, because nothing else in the game removes it.
//
// The seed is spent on the write either way, exactly as `stackSize--` is
// reached whether or not `setBlockWithNotify` did anything; a write above the
// top of the world is refused by the world and the seed is still gone.
//
// **No sound**, which is the original's: `ItemBlock`'s place cue is in
// `ItemBlock`, and a crop is not placed by one.
bool useSeeds(tick::TickWorld& world, BlockId crop, const entity::RayHit& hit)
{
    if (int(hit.face) != 1) {
        return false;
    }
    if (block::def(world.blockAt(hit.x, hit.y, hit.z)).tick != TickBehaviour::Farmland) {
        return false;
    }
    world.setBlockWithNotify(hit.x, hit.y + 1, hit.z, crop);
    return true;
}

// `lg.a(Lev;Ldm;Lcn;IIII)Z` -- **ItemRecord.onItemUse**, which is seven lines
// and is the only way a disc gets into a jukebox:
//
// ```
// if (world.getBlockId(i, j, k) == Block.jukebox.blockID
//     && world.getBlockMetadata(i, j, k) == 0) {
//     world.setBlockMetadata(i, j, k, this.shiftedIndex - Item.record13.shiftedIndex + 1);
//     world.playRecord(this.recordName, i, j, k);
//     itemstack.stackSize--;
//     return true;
// }
// return false;
// ```
//
// **The struck cell, not the face's offset.** A disc is not placed past the
// block it is clicked on; it goes *into* it, which is why this cannot be an
// `ItemBlock` and why `places` measures 0 for both discs.
//
// **The metadata test is the whole of "one at a time"**: a jukebox that is
// already playing has non-zero metadata and refuses, and the block's own
// `blockActivated` has already taken that click to eject instead -- so the two
// halves never both run. See `tick::blockActivated`.
bool useRecord(tick::TickWorld& world, ItemId held, const char* track,
               const entity::RayHit& hit)
{
    if (block::def(world.blockAt(hit.x, hit.y, hit.z)).tick != TickBehaviour::Jukebox) {
        return false;
    }
    if (world.dataAt(hit.x, hit.y, hit.z) != 0) {
        return false;
    }
    world.setDataRaw(hit.x, hit.y, hit.z, recordMetadata(held));
    world.playRecord(track, hit.x, hit.y, hit.z);
    return true;
}

// `di.b` -- **`Item.itemRand`**, the static Random every item's own use method
// draws from. It is deliberately *not* the world's tick random: pitching a
// sound must not perturb the stream that decides where a tree grows, and in the
// original the two are different objects for exactly that reason.
JavaRandom itemRand{0};

// `nx.a(Lev;Ldm;Lcn;IIII)Z` -- **ItemFlintAndSteel.onItemUse**, and it is not
// an `ItemBlock` at all. The whole method is:
//
// ```
// if (l == 0) j--;  ... the six faces, as an offset
// if (world.getBlockId(i, j, k) == 0) {
//     world.playSoundEffect(i + 0.5, j + 0.5, k + 0.5, "fire.ignite", 1.0F,
//                           itemRand.nextFloat() * 0.4F + 0.8F);
//     world.setBlockWithNotify(i, j, k, Block.fire.blockID);
// }
// itemstack.damageItem(1, entityplayer);
// return true;
// ```
//
// Three ways it differs from the placement path it used to be routed through,
// and all three are audible or visible:
//
//   * **No `canBlockBePlacedAt`.** No clearance test against the player and no
//     `canPlaceBlockAt`, so a fire is lit at your own feet and in a cell whose
//     support is about to refuse it -- which `BlockFire.onBlockAdded` then
//     removes on the same call. That sequence is the original's and it is what
//     "the flame went out immediately" is *supposed* to look like on stone.
//   * **Air only.** `ItemBlock` treats water, lava, fire and snow as free
//     space; flint and steel tests `getBlockId == 0` and nothing else, so it
//     will not light a puddle or replace a snow layer.
//   * **`fire.ignite`, not the block's place cue.** Volume 1, and a pitch drawn
//     per use. A block placed by `ItemBlock` makes a footstep; this makes the
//     striking noise, and it is the only feedback the player gets when the fire
//     it lit cannot stay.
//
// **It returns true either way**, because the original spends the durability
// whether or not anything caught -- so a click on a solid wall is still a click
// that was used up.
bool useIgnite(tick::TickWorld& world, BlockId fire, const entity::RayHit& hit,
               const Effects& effects)
{
    const i32 x = hit.placeX();
    const int y = hit.placeY();
    const i32 z = hit.placeZ();

    if (y < 0 || y >= mcver::kWorldHeight || world.blockAt(x, y, z) != block::kAir) {
        return true;
    }

    if (effects.sound != nullptr) {
        effects.sound->playSoundAt("fire.ignite", double(x) + 0.5, double(y) + 0.5,
                                   double(z) + 0.5, 1.0f,
                                   itemRand.nextFloat() * 0.4f + 0.8f);
    }
    world.setBlockWithNotify(x, y, z, fire);
    return true;
}


// ---------------------------------------------------------------- the bucket

// **Five, and the crosshair's is four.** `ItemBucket.onItemRightClick` builds
// its own ray and the literal in it is `5.0D` -- it does not ask the controller
// for a reach at all, so a bucket genuinely fills and pours a block further
// than a block can be broken. See core/entity/ray_trace.hpp for why 4.0 is the
// number everywhere else.
constexpr double kBucketReach = 5.0;

// The single item in the table that answers `bucket == 0`, found rather than
// written down: no item id may appear in this file (CONTRIBUTING.md), and the
// column that says which item is an empty bucket is generated from the jar.
// 512 entries scanned on a button press is nothing, and hoisting it into a
// static would cost the constness for no measurable gain.
ItemId emptyBucketItem()
{
    for (ItemId id = 0; id < ItemId(mcver::kItemTableSize); ++id) {
        const ItemDef& d = def(id);
        if (d.known && d.bucket == 0) {
            return id;
        }
    }
    return 0;
}

// Which full bucket a source block of `struck` fills, or 0 for a block that no
// bucket holds.
//
// **Matched on material, which is what the original matches on.** The method
// tests `getBlockMaterial(i, j, k) == Material.water`, so it fills from either
// of the two water blocks -- the still one and the flowing one -- while the
// bucket's own column names only the flowing id. Comparing block ids directly
// would fill from block 8 and refuse block 9, which is most of the water in a
// world.
ItemId filledBucketFor(BlockId struck)
{
    const u8 material = block::def(struck).material;
    if (material == 0) {
        return 0;
    }
    for (ItemId id = 0; id < ItemId(mcver::kItemTableSize); ++id) {
        const ItemDef& d = def(id);
        if (!d.known || d.bucket <= 0) {
            continue;
        }
        if (block::def(BlockId(d.bucket)).material == material) {
            return id;
        }
    }
    return 0;
}

// `ac.a(Lev;Lcn;Ldm;)Lev;` -- **ItemBucket.onItemRightClick**, whose three
// branches are the three things the `bucket` column can say.
//
// Two details are worth having in front of you, because both are visible:
//
//   * **The ray is told to notice liquids exactly when the bucket is empty.**
//     `rayTraceBlocks_do(from, to, this.isFull == 0)` -- so an empty bucket
//     stops on a water source and a full one passes straight through the
//     surface and pours against the floor underneath. That single flag is what
//     makes both halves of the item work from the same aim.
//   * **A full bucket pours the *flowing* block**, id 8 or 10 rather than 9 or
//     11, which is why poured water spreads out instead of standing in the one
//     cell. The column carries the field verbatim; see ItemDef::bucket.
//
// Milk is the third branch and does nothing at all here: `EntityPlayer` has no
// effects to clear in a1.1.2, so the whole of it is the bucket coming back
// empty -- which is still worth doing, because a milk bucket that stayed full
// would be the one item in the game that cannot be spent.
ItemUse useBucket(tick::TickWorld& world, ItemId held, const ItemDef& heldDef, double eyeX,
                  double eyeY, double eyeZ, double dirX, double dirY, double dirZ)
{
    const i16 fill = heldDef.bucket;
    const entity::RayHit hit = entity::rayTrace(world, eyeX, eyeY, eyeZ, dirX, dirY, dirZ,
                                                kBucketReach, fill == 0);

    // Milk is answered before the ray matters, exactly as the original does --
    // its branch is inside the block-hit test but reads nothing from the hit.
    if (fill < 0) {
        return ItemUse{false, emptyBucketItem()};
    }
    if (!hit.hit) {
        return ItemUse{false, held};
    }

    if (fill == 0) {
        // **Filling, and it is the struck cell rather than the one in front of
        // it.** There is no face offset on this branch: the ray stopped *on*
        // the water, and the water is what is being taken away.
        const BlockId struck = world.blockAt(hit.x, hit.y, hit.z);
        if (world.dataAt(hit.x, hit.y, hit.z) != 0) {
            return ItemUse{false, held};
        }
        const ItemId filled = filledBucketFor(struck);
        if (filled == 0) {
            return ItemUse{false, held};
        }
        world.setBlockWithNotify(hit.x, hit.y, hit.z, block::kAir);
        return ItemUse{true, filled};
    }

    // Pouring, into the cell on the struck face.
    const i32 x = hit.placeX();
    const int y = hit.placeY();
    const i32 z = hit.placeZ();
    if (y < 0 || y >= mcver::kWorldHeight) {
        return ItemUse{false, held};
    }

    // `getBlockId == 0 || !getBlockMaterial().isSolid()` -- **not the placement
    // test**. There is no clearance check against the player, so a bucket can
    // be emptied over your own feet, and anything non-solid is written straight
    // over: a torch, a flower, snow, or another fluid.
    const BlockId target = world.blockAt(x, y, z);
    if (target != block::kAir && block::def(target).solid) {
        return ItemUse{false, held};
    }

    world.setBlockAndDataWithNotify(x, y, z, BlockId(fill), 0);
    return ItemUse{true, emptyBucketItem()};
}

}  // namespace

bool rightClick(tick::TickWorld& world, ItemId held, const entity::RayHit& hit,
                const AABB& playerBox, float yawDegrees, const Effects& effects,
                bool* itemTook)
{
    if (itemTook != nullptr) {
        *itemTook = false;
    }
    // Every item branch below reports through this; the block's own answer
    // does not, which is the whole of the distinction Survival needs.
    const auto byItem = [itemTook](bool taken) {
        if (taken && itemTook != nullptr) {
            *itemTook = true;
        }
        return taken;
    };

    if (!hit.hit) {
        return false;
    }

    // **Minecarts are handled before block activation.** Rails normally answer
    // false to `onBlockActivated`, but the cart item is an `onItemUse` path and
    // must be allowed to consume the click on the struck rail directly. This
    // also keeps a future rail interaction from swallowing cart placement.
    if (held != 0 && def(held).spawns == SpawnsEntity::Minecart) {
        if (effects.entities.minecarts == nullptr) {
            return false;
        }
        return byItem(effects.entities.minecarts->place(
            world, hit.x, hit.y, hit.z, entity::MinecartType(def(held).spawnVariant)));
    }

    // **The block is asked first** for every other item, and an empty hand still
    // asks it.
    if (tick::blockActivated(world, hit.x, hit.y, hit.z)) {
        return true;
    }

    // **An item that spawns an entity rather than placing a block**, which is
    // six items in a1.1.2 and is checked here because it is the *item* that
    // decides, not the block -- there is no block. `places` is 0 for all six,
    // so without this column they fall through to "does nothing", which is what
    // they did. Minecarts were consumed above because their entry point is
    // specifically the block-use route rather than the general activation one.
    //
    // A painting is the only one wired up so far. The other five need pools
    // this build does not have yet; they fall through as before and are named
    // in docs/todo-m3.md rather than silently ignored.
    if (held != 0 && def(held).spawns == SpawnsEntity::Painting) {
        if (effects.entities.paintings == nullptr) {
            return false;
        }
        return byItem(effects.entities.paintings->place(world, hit.x, hit.y, hit.z, hit.face));
    }

    // **A music disc, which also changes the cell it struck** -- it goes into
    // the jukebox rather than past it -- and so cannot be routed by the block
    // it places either: it places none. The `record` column is what says so.
    if (held != 0) {
        if (const char* track = recordTrack(held)) {
            return byItem(useRecord(world, held, track, hit));
        }
    }

    // **A hoe, which changes the cell it struck rather than the one past it**
    // and so cannot be routed by the block it places -- it places none. Its
    // column says so; see `ItemDef::tills` and `useHoe`.
    if (held != 0 && def(held).tills) {
        return byItem(useHoe(world, hit, effects));
    }

    // **The item decides the block.** The hand holds an *item*; what goes into
    // the world is whatever that item places, which the generated table
    // answers -- item 324 puts down block 64 and item 338 puts down block 83,
    // and nothing here has to know that either pair exists. An item that places
    // nothing -- a sword, a bucket -- does nothing, which is also what an empty
    // slot does.
    const BlockId placed = BlockId(placesBlock(held));
    if (held == 0 || placed == block::kAir) {
        return false;
    }

    // **A sign, which places one of two blocks and opens a keyboard.** Routed
    // by the behaviour of the block it would place, which is the same rule the
    // door and fire branches below use -- and it catches the sign item without
    // naming it, because item 323 is the only thing in the table whose `places`
    // column is a sign post.
    if (block::def(placed).tick == TickBehaviour::SignPost
        || block::def(placed).tick == TickBehaviour::SignWall) {
        return byItem(useSign(world, hit, yawDegrees, effects));
    }

    // **Dispatched on the block being placed, not on the item.** a1.1.2 makes
    // this distinction in the item's class -- ItemDoor against ItemBlock -- and
    // this port has no item-class column, but it does have the behaviour column
    // that says a door is a door. The two agree for every item in the table:
    // the only items that place a door block are the two door items.
    if (block::def(placed).tick == TickBehaviour::Door) {
        return byItem(useDoor(world, placed, hit, yawDegrees, effects));
    }
    // **And the same argument for fire.** Flint and steel is `nx`, not `av`,
    // and the two behave differently enough to be seen -- see useIgnite. The
    // only item in the table that puts fire down is item 259; block 51's own
    // ItemBlock form exists in the table, places fire too, and is not offered
    // by the palette or obtainable in a world, so routing it here as well is a
    // difference nothing can observe.
    if (block::def(placed).tick == TickBehaviour::Fire) {
        return byItem(useIgnite(world, placed, hit, effects));
    }
    // **And the same argument once more for the seeds.** `jn` is not `av`
    // either: it plants on the block it struck rather than past it, asks none
    // of the three questions a placement asks, and is the one item in a1.1.2
    // that can overwrite a block that is already there. The only item in the
    // table that puts a crop down is item 295. See useSeeds.
    if (block::def(placed).tick == TickBehaviour::Crops) {
        return byItem(useSeeds(world, placed, hit));
    }
    return byItem(useBlock(world, placed, hit, playerBox, effects));
}

ItemUse useItem(tick::TickWorld& world, ItemId held, double eyeX, double eyeY, double eyeZ,
                double dirX, double dirY, double dirZ, const Effects& effects)
{
    const ItemDef& heldDef = def(held);
    if (held == 0 || !heldDef.known) {
        return ItemUse{false, held};
    }
    if (heldDef.bucket != ItemDef::kNotABucket) {
        // `ItemBucket` is silent on both branches, which is the original.
        return useBucket(world, held, heldDef, eyeX, eyeY, eyeZ, dirX, dirY, dirZ);
    }

    // **The bow**, which is the second item in a1.1.2 with an
    // `onItemRightClick` this build can perform.
    //
    // `jg.a(...)`'s three steps in its order: take an arrow, play `random.bow`,
    // spawn one `kg`. **The first is not done here**: Survival takes the arrow
    // before it calls this (`main.cpp`, beside the food), and Creative's hand
    // is a catalogue rather than a stock. The player gets it back by walking
    // over it once it has stuck -- `ArrowSystem::collect`. The pitch is the
    // original's, `1 / (rand * 0.4 + 0.8)`.
    if (heldDef.spawns == SpawnsEntity::Arrow) {
        if (effects.entities.arrows == nullptr) {
            return ItemUse{false, held};
        }
        // The heading is recovered from the aim vector the caller already
        // built, rather than taken as two more arguments: `useItem`'s contract
        // is an eye and a direction, and an arrow's launch angles are that
        // direction expressed the other way round.
        const double horizontal = std::sqrt(dirX * dirX + dirZ * dirZ);
        const float yaw = float(std::atan2(-dirX, dirZ) * 180.0 / 3.1415927410125732);
        const float pitch = float(-std::atan2(dirY, horizontal) * 180.0 / 3.1415927410125732);
        if (!effects.entities.arrows->shoot(world, eyeX, eyeY, eyeZ, yaw, pitch)) {
            return ItemUse{false, held};
        }
        playBowShot(effects, eyeX, eyeY, eyeZ);
        return ItemUse{true, held};
    }

    // **The boat**, and it is here rather than in the block path for a reason
    // that is easy to miss: `me.a(...)` is an `onItemRightClick`, not an
    // `onItemUse`. It casts its own ray at reach **5.0 with liquids on** --
    // exactly the bucket's shape -- because the crosshair's ray is not allowed
    // to see water at all, and water is the only place a boat is any use. A
    // build that routed it through `rightClick` would find a boat placeable
    // only on dry land, which is the opposite of what it is for.
    if (heldDef.spawns == SpawnsEntity::Boat) {
        if (effects.entities.boats == nullptr) {
            return ItemUse{false, held};
        }
        const entity::RayHit hit = entity::rayTrace(world, eyeX, eyeY, eyeZ, dirX, dirY,
                                                    dirZ, entity::kBoatReach, true);
        if (!hit.hit) {
            return ItemUse{false, held};
        }
        // `if (movingobjectposition.typeOfHit != EnumMovingObjectType.TILE) return`.
        // Ours only ever reports a tile, so there is nothing to test.
        if (!effects.entities.boats->place(world, hit.x, hit.y, hit.z)) {
            return ItemUse{false, held};
        }
        return ItemUse{true, held};
    }
    return ItemUse{false, held};
}

EntityTarget pickEntity(const EntityPools& pools, double eyeX, double eyeY, double eyeZ,
                        double dirX, double dirY, double dirZ, const entity::RayHit& blockHit)
{
    const double reach = entity::entityPickReach(eyeX, eyeY, eyeZ, blockHit);
    const double endX = eyeX + dirX * reach;
    const double endY = eyeY + dirY * reach;
    const double endZ = eyeZ + dirZ * reach;
    constexpr double kBorder = entity::kEntityPickBorder;

    EntityTarget best;
    const auto consider = [&](EntityTarget::Kind kind, int index, const AABB& box) {
        double distance = 0.0;
        if (!entity::interceptDistance(box.expand(kBorder, kBorder, kBorder), eyeX, eyeY, eyeZ,
                                       endX, endY, endZ, &distance)) {
            return;
        }
        // `d < best || best == 0.0D`, with `best` starting at zero -- so a hit
        // at exactly the eye is replaced by the next one, as it is there.
        if (distance < best.distance || best.distance == 0.0) {
            best = EntityTarget{kind, index, distance};
        }
    };

    // The original first narrows the candidates to those touching the player's
    // box swept along the segment (`getEntitiesWithinAABBExcludingEntity`).
    // That is not modelled: from the body's own eye the segment runs at least
    // 0.18 inside the swept box, farther than the 0.1 border reaches, so the
    // filter removes nothing the intercept below would find.
    if (pools.paintings != nullptr) {
        for (int i = 0; i < pools.paintings->count(); ++i) {
            if ((*pools.paintings)[i].alive) {
                consider(EntityTarget::Kind::Painting, i, (*pools.paintings)[i].box);
            }
        }
    }
    if (pools.boats != nullptr) {
        for (int i = 0; i < pools.boats->count(); ++i) {
            if ((*pools.boats)[i].alive) {
                consider(EntityTarget::Kind::Boat, i, (*pools.boats)[i].box);
            }
        }
    }
    if (pools.minecarts != nullptr) {
        for (int i = 0; i < pools.minecarts->count(); ++i) {
            if ((*pools.minecarts)[i].alive) {
                consider(EntityTarget::Kind::Minecart, i, (*pools.minecarts)[i].box);
            }
        }
    }
    // **An animal is collidable while it is alive and while it is dying**:
    // `ge.c_()` answers `!isDead`, and a mob that has run out of health is not
    // dead until its twenty death ticks are up -- so a corpse can still be hit,
    // exactly as it can in the original.
    if (pools.mobs != nullptr) {
        for (int i = 0; i < pools.mobs->count(); ++i) {
            if ((*pools.mobs)[i].alive) {
                consider(EntityTarget::Kind::Mob, i, (*pools.mobs)[i].body.box);
            }
        }
    }
    // **The other players, last, so a tie goes to something local.** A body is
    // the player box -- 0.6 across and 1.8 tall, `dm`'s own -- built around
    // where the session last said they were, because `RemotePlayer` keeps a
    // position and not a box: it is drawn from the packets rather than moved
    // by physics, so there is no box for it to have.
    if (pools.players != nullptr) {
        for (int i = 0; i < pools.players->playerCount(); ++i) {
            const net::RemotePlayer& other = pools.players->player(i);
            if (!other.used) {
                continue;
            }
            // `dm`'s own size, and `RemotePlayer::y` is the feet.
            const double half = double(entity::kPlayerWidth / 2.0f);
            AABB box;
            box.minX = other.x - half;
            box.maxX = other.x + half;
            box.minY = other.y;
            box.maxY = other.y + double(entity::kPlayerHeight);
            box.minZ = other.z - half;
            box.maxZ = other.z + half;
            consider(EntityTarget::Kind::Player, i, box);
        }
    }
    return best;
}

bool attackEntity(tick::TickWorld& world, const EntityTarget& target, ItemId held,
                  const Effects& effects, const Attacker& attacker)
{
    // `int i = inventory.getDamageVsEntity(entity)`, which is the held item's
    // own answer -- and the unknown row carries `InventoryPlayer`'s own
    // empty-slot 1, so the bare hand falls out of the same lookup rather than
    // out of a branch. See `ItemDef::damageVsEntity`.
    const int damage = int(def(held).damageVsEntity);
    // `if (i > 0)`. Nothing in a1.1.2's table answers zero, but the guard is
    // the jar's and a version whose table does would otherwise land a free hit.
    if (damage <= 0) {
        return false;
    }

    const EntityPools& pools = effects.entities;
    // **Somebody else's player is not hit here.** The hit crosses the link and
    // lands on the console that is running them; doing it here as well would
    // be this world arguing with theirs about their health. See
    // `EntityTarget::remote` and `NetPlay::attackEntity`.
    if (target.remote()) {
        return false;
    }
    switch (target.kind) {
    case EntityTarget::Kind::Painting:
        return pools.paintings != nullptr && pools.paintings->attack(world, target.index);
    case EntityTarget::Kind::Boat:
        return pools.boats != nullptr && pools.boats->attack(world, target.index, damage);
    case EntityTarget::Kind::Minecart:
        return pools.minecarts != nullptr
               && pools.minecarts->attack(world, target.index, damage);
    case EntityTarget::Kind::Mob:
        // **The hit is `fromPlayer`**, which is what shears a sheep: `bo`'s
        // `attackEntityFrom` drops wool only when an `EntityLiving` did the
        // hitting, and the player is the only one in this build.
        return pools.mobs != nullptr
               && pools.mobs->attack(world, target.index, damage, true, attacker.x,
                                     attacker.z, attacker.present, false,
                                     attacker.provokes);
    case EntityTarget::Kind::Player:
    case EntityTarget::Kind::None:
        break;
    }
    return false;
}

EntityInteraction interactWithEntity(tick::TickWorld& world, const EntityTarget& target,
                                     const EntityPools& pools, ItemId held, double playerX,
                                     double playerZ)
{
    EntityInteraction result;
    result.becomes = held;
    switch (target.kind) {
    case EntityTarget::Kind::Boat:
        result.taken = pools.boats != nullptr && pools.boats->mount(target.index);
        break;
    case EntityTarget::Kind::Minecart: {
        if (pools.minecarts == nullptr) {
            break;
        }
        // `oc.a(Ldm;)Z`, and it is one method with three bodies: a plain cart
        // is mounted, a chest cart opens its slots and a furnace cart takes the
        // coal and is pointed away from the player.
        const entity::MinecartSystem::Interaction answer =
            pools.minecarts->interact(target.index, held, playerX, playerZ);
        result.taken = answer.taken();
        result.spentFuel = answer.spentFuel;
        if (answer.kind == entity::MinecartSystem::Interaction::Kind::Chest) {
            result.opensMinecartChest = answer.cart;
        }
        break;
    }
    case EntityTarget::Kind::Mob: {
        if (pools.mobs == nullptr) {
            break;
        }
        const entity::MobSystem::Interaction answer =
            pools.mobs->interact(world, target.index, held);
        result.taken = answer.taken;
        result.becomes = answer.becomes;
        break;
    }
    case EntityTarget::Kind::Painting:
    // **A right click on another player does nothing**, here and in a1.1.2:
    // `EntityPlayer` has no `interact` of its own, so the click falls through
    // and places whatever is in the hand against the block behind them.
    case EntityTarget::Kind::Player:
    case EntityTarget::Kind::None:
        break;
    }
    return result;
}

bool destroyBlock(tick::TickWorld& world, i32 x, int y, i32 z, const Effects& effects)
{
    // **Before the removal**, because the particles are cut out of the block's
    // own texture and the cell is about to be air.
    if (effects.particles != nullptr) {
        effects.particles->addBlockDestroy(world, x, y, z);
    }

    // `block != null && flag`, and both halves matter. `flag` is
    // `Chunk.setBlockID`'s answer, which is **false when nothing changed** --
    // so breaking air is not a break: no sound, and nothing to report to the
    // caller. Our own writer answers true for a write that changes nothing, so
    // the "was there anything there" half is tested here rather than inferred.
    const BlockId broken = world.blockAt(x, y, z);
    // **Read before the write**, which is the original's order and not a
    // convenience: `hq.b` takes the metadata into a local and only then calls
    // `setBlockWithNotify(0)`, so what `onBlockDestroyedByPlayer` sees is what
    // the block had rather than the zero left behind.
    const u8 metadata = world.dataAt(x, y, z);
    const bool removed = broken != block::kAir
                         && world.setBlockAndDataWithNotify(x, y, z, block::kAir, 0);
    if (removed) {
        playAt(effects, audio::breakCue(broken), x, y, z);
        // **The last of `hq.b`'s three steps, and it was missing.** Every block
        // in a1.1.2 but three does nothing here; the one that shows is the crop,
        // which leaves its seeds through this and not through the drop table.
        // See core/tick/drop.hpp.
        tick::blockDestroyedByPlayer(world, x, y, z, broken, metadata);
    }
    return removed;
}

RefusalMark markRefusals(const EntityPools& pools)
{
    RefusalMark mark;
    mark.paintings = pools.paintings != nullptr ? pools.paintings->refused() : 0;
    mark.arrows = pools.arrows != nullptr ? pools.arrows->refused() : 0;
    mark.boats = pools.boats != nullptr ? pools.boats->refused() : 0;
    mark.minecarts = pools.minecarts != nullptr ? pools.minecarts->refused() : 0;
    mark.signs = pools.signs != nullptr ? pools.signs->refused() : 0;
    return mark;
}

LimitedEntity refusedSince(const EntityPools& pools, const RefusalMark& mark)
{
    const RefusalMark now = markRefusals(pools);
    if (now.minecarts != mark.minecarts) {
        return LimitedEntity::Minecart;
    }
    if (now.boats != mark.boats) {
        return LimitedEntity::Boat;
    }
    if (now.paintings != mark.paintings) {
        return LimitedEntity::Painting;
    }
    if (now.signs != mark.signs) {
        return LimitedEntity::Sign;
    }
    if (now.arrows != mark.arrows) {
        return LimitedEntity::Arrow;
    }
    return LimitedEntity::None;
}

const char* limitMessage(LimitedEntity which)
{
    switch (which) {
    case LimitedEntity::Painting:
        return "The maximum number of Paintings in a world has been reached.";
    case LimitedEntity::Arrow:
        return "The maximum number of Arrows in a world has been reached.";
    case LimitedEntity::Boat:
        return "The maximum number of Boats in a world has been reached.";
    case LimitedEntity::Minecart:
        return "The maximum number of Minecarts in a world has been reached.";
    case LimitedEntity::Sign:
        return "The maximum number of Signs in a world has been reached.";
    case LimitedEntity::DroppedItem:
        return "The maximum number of Dropped Items in a world has been reached.";
    case LimitedEntity::None:
        break;
    }
    return nullptr;
}

void playBowSound(const Effects& effects, double x, double y, double z)
{
    playBowShot(effects, x, y, z);
}

}  // namespace mc::item
