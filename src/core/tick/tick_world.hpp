#pragma once

// The world as a block tick sees it: read a block, write a block, wake the
// neighbours, come back later.
//
// This is a1.1.2's `cn` (World) reduced to the twenty-odd methods its block
// behaviours actually call, and it is the seam that makes the tick system
// testable on a host with no renderer, no streamer and no card. It reaches the
// loaded columns through a pair of function pointers rather than by owning
// them, the same shape `mc::WorkerSpawn` uses and for the same reason: the
// thing that owns chunks (`WorldStreamer`) is renderer-side and core's tick
// code has no business depending on it.
//
// **Coordinates are the original's.** x and z are world block coordinates and
// may be negative; y is 0..kWorldHeight-1. Reads outside that return air, and
// writes outside it are dropped -- both are `cn.a(III)I`'s own behaviour, and
// blocks rely on it: a torch at y=0 asks what is below it.
//
// **A tick never generates.** `cn` would have loaded or generated a missing
// chunk on the spot. We return "absent" instead, and every behaviour that
// crosses a chunk boundary is written to survive it, because generation order
// *is* the world here (see docs/status.md 0g) and a random tick must not be
// allowed to reorder it. The practical effect is that a fluid stops at the
// edge of what is loaded and resumes when the ground arrives, which is also
// what the original does when a chunk is genuinely not there.

#include "core/block/block_def.hpp"
#include "core/block/registry.hpp"
#include "core/tick/tick_scheduler.hpp"
#include "core/util/aabb.hpp"
#include "core/util/java_random.hpp"
#include "core/util/types.hpp"
#include "core/world/chunk.hpp"

#include <vector>

namespace mc::tick {

// Where the tick reaches chunks, and what it tells the rest of the game when
// one changes. Plain function pointers and a context, because the alternative
// is a virtual interface and CONTRIBUTING keeps those for the platform and
// storage seams.
struct TickAccess {
    void* ctx = nullptr;

    // The resident column, or null. Never generates, never blocks, never
    // touches the card.
    world::ChunkColumn* (*column)(void* ctx, i32 chunkX, i32 chunkZ) = nullptr;

    // A block was written. The renderer remeshes the section and its
    // neighbours; the streamer marks the column dirty so the autosave writes
    // it. Called once per changed block, after the write.
    void (*changed)(void* ctx, i32 x, int y, i32 z) = nullptr;
};

// The six faces, in the order `cn.g(IIII)V` notifies them. The order is
// observable -- a redstone update that arrives from -x before +x can settle
// differently -- so it is written down rather than left to a loop.
enum class Side : u8 { NegX = 0, PosX, NegY, PosY, NegZ, PosZ };

// `js` -- **EnumMobType**, which is the only thing in a1.1.2 that asks the
// world about entities *by kind*. A pressure plate holds one of these and its
// three values are the three list queries `al` can make: everything in the box
// (`cn.b(kh,cf)` with a null exclusion), every `EntityLiving` in it, or every
// `EntityPlayer`. The ordinals are the enum's own, so a table indexed by one
// stays in step with the jar.
enum class EntityFilter : u8 { Everything = 0, Mobs = 1, Players = 2 };

class TickWorld {
public:
    static constexpr int kHeight = world::ChunkColumn::kHeight;

    // `seed` seeds both random sources. a1.1.2 seeds them from
    // `new Random()` -- wall-clock -- so its random ticks are not reproducible
    // between two runs of the same world, and nothing in the game depends on
    // them being. Seeding from the world seed instead costs no fidelity and
    // buys a test that can assert on an exact outcome.
    TickWorld(TickAccess access, i64 seed, usize schedulerCapacity = 4096);

    // ---- reading -------------------------------------------------------

    block::BlockId blockAt(i32 x, int y, i32 z) const;
    u8 dataAt(i32 x, int y, i32 z) const;

    // `cn.a(by,III)I` -- the stored value, before the day's subtraction.
    u8 skyLightAt(i32 x, int y, i32 z) const;
    u8 blockLightAt(i32 x, int y, i32 z) const;

    // `cn.j(III)I` -- getBlockLightValue: sky light less the day's
    // subtraction, or block light, whichever is brighter. This is what grass,
    // mushrooms and crops read, so the time of day is an input to block
    // behaviour and not only to the lightmap.
    //
    // **Ice and snow are not among them** -- they read `blockLightAt` above,
    // and the comment here used to say otherwise. See behaviour.cpp's note
    // above `iceTick` for what that cost.
    int lightValue(i32 x, int y, i32 z, bool checkNeighbours = true) const;

    // `cn.i(III)Z` -- World.canBlockSeeTheSky, answered from the chunk's own
    // height map rather than by tracing upwards. A plant reads it to decide
    // whether it may stay somewhere dark.
    bool canSeeSky(i32 x, int y, i32 z) const;

    // `cn.g(III)Z` -- `Block.isOpaqueCube()`, the live method. What a torch
    // asks of the block it hangs on.
    bool opaqueAt(i32 x, int y, i32 z) const { return block::def(blockAt(x, y, z)).opaque; }

    // `cn.d(II)I` -- the y above the highest solid-or-liquid block, or -1.
    int precipitationHeight(i32 x, i32 z) const;

    // `cn.a(IIIIII)Z` -- are all the chunks this box touches in memory?
    bool chunksExist(i32 x1, int y1, i32 z1, i32 x2, int y2, i32 z2) const;

    bool chunkResident(i32 chunkX, i32 chunkZ) const;

    // ---- entities ------------------------------------------------------
    //
    // **The one question block behaviour asks that is not about blocks.**
    // `cn.b(kh,cf)Ljava/util/List;` and `cn.a(Ljava/lang/Class;cf)` build a
    // list; the only caller in this version is `al.h`, which does nothing with
    // it but ask whether it is empty -- so the seam is a predicate and not a
    // list, and nothing has to allocate to answer it.
    //
    // **Not on `TickAccess`, deliberately.** `TickAccess` is how the tick
    // reaches *chunks*, and it is built by `WorldStreamer`, which owns them.
    // Nothing owns the player and the dropped items together except the thing
    // running the frame, so that is what sets this, once, after both exist.
    //
    // Unset means **there are no entities**, which is the honest answer for
    // every headless tool in this project: `--fly` moves a body through a
    // world with no plates to press and the test suite builds worlds with
    // nobody in them. A plate in such a world simply never arms, which is what
    // it did before this existed.
    using EntityQuery = bool (*)(void* ctx, const AABB& box, EntityFilter filter);
    void setEntityQuery(EntityQuery query, void* ctx)
    {
        entityQuery_ = query;
        entityQueryCtx_ = ctx;
    }
    bool anyEntityIn(const AABB& box, EntityFilter filter) const
    {
        return entityQuery_ != nullptr && entityQuery_(entityQueryCtx_, box, filter);
    }

    // ---- the entities that are solid -----------------------------------
    //
    // **`kh.f_()` -- getBoundingBox -- and the half of
    // `cn.a(Lkh;Lcf;)Ljava/util/List;` this port did not have.**
    // `getCollidingBoundingBoxes` is not only a block loop: after it has
    // gathered the blocks it asks every entity within a quarter of a block of
    // the swept volume for `getBoundingBox()`, and adds whatever is not null.
    // Every `moveEntity` in the game clips against that same list, so an
    // entity with a box there is something to walk into and stand on.
    //
    // **Exactly two classes in this jar answer with a box**, and it was worth
    // disassembling all 402 of them to be sure: `dc` (EntityBoat) and `oc`
    // (EntityMinecart) both return their own `boundingBox`, and `kh`'s own
    // method -- which every mob, item, arrow and particle inherits -- returns
    // null. That is why a cow can be walked through and a minecart cannot, and
    // why "you can stand on top of a minecart" is a statement about two
    // classes rather than about entities in general.
    //
    // A **fold rather than a list**, for the reason `anyEntityIn` is a
    // predicate: the sweep wants one number per axis and the pools belong to
    // the frame loop, so nothing has to allocate or cap to answer. The sink is
    // handed every solid box that intersects `swept` -- the original's final
    // `intersectsWith` gate, which is what makes the 0.25 expansion on its
    // candidate query invisible from here.
    //
    // `self` is `getCollidingBoundingBoxes`'s first argument: **the entity
    // doing the moving, which is left out of its own collision list.** A cart
    // whose own box came back would find it overlapping every swept volume it
    // ever builds and would refuse to move at all. Anything that is not solid
    // passes null, because it cannot be in the list in the first place.
    //
    // **The same list has a second entry per neighbour**, and it is the
    // mover's own answer rather than the neighbour's:
    //
    //     AABB c = entity.getCollisionBox(e);   // kh.b_(kh), on the MOVER
    //     if (c != null && c.intersectsWith(box)) list.add(c);
    //
    // `kh.b_` is null, so for a player, a mob, an item or a falling block this
    // branch adds nothing at all. `dc.b_(kh)` and `oc.b_(kh)` are both
    // `return e.boundingBox` -- unconditional, with no liveness or
    // can-be-collided-with test in front of them -- so **a moving boat or
    // minecart is stopped by every entity near it**, not only by the other
    // boats and carts. That is `moverCollidesWithEntities`, and it is a
    // property of the thing moving, which is why it travels with `self`
    // instead of being asked of each candidate.
    //
    // **Unset means nothing is solid**, which is the honest answer for the
    // headless tools: `--fly` moves a body through a world with no vehicles in
    // it, and the block half of the sweep is unchanged.
    using SolidBoxSink = void (*)(void* sinkCtx, const AABB& box);
    using SolidBoxQuery = void (*)(void* ctx, const AABB& swept, const void* self,
                                   bool moverCollidesWithEntities, SolidBoxSink sink,
                                   void* sinkCtx);
    void setSolidBoxQuery(SolidBoxQuery query, void* ctx)
    {
        solidBoxQuery_ = query;
        solidBoxQueryCtx_ = ctx;
    }
    void forEachSolidBox(const AABB& swept, const void* self, bool moverCollidesWithEntities,
                         SolidBoxSink sink, void* sinkCtx) const
    {
        if (solidBoxQuery_ != nullptr) {
            solidBoxQuery_(solidBoxQueryCtx_, swept, self, moverCollidesWithEntities, sink,
                           sinkCtx);
        }
    }

    // `getCollidingBoundingBoxes(...).isEmpty()` asked of the entity half
    // alone -- what the sneak walk-back and `isOffsetPositionInLiquid` want on
    // top of their block loop.
    bool anySolidBoxIn(const AABB& box, const void* self = nullptr,
                       bool moverCollidesWithEntities = false) const
    {
        bool hit = false;
        forEachSolidBox(box, self, moverCollidesWithEntities,
                        [](void* ctx, const AABB&) { *static_cast<bool*>(ctx) = true; },
                        &hit);
        return hit;
    }

    // `cn.a(Lkh;)Z` -- spawnEntityInWorld, narrowed to the one entity a block
    // behaviour ever asks for: a dropped item, at a position, of an id, one
    // deep. Set beside `setEntityQuery` and for the same reason -- the pool
    // belongs to the frame loop, not to the chunks `TickAccess` reaches.
    //
    // **Unset means the drop is thrown away, not that it is skipped.**
    // `core/tick/drop.cpp` still makes every draw it would have made, so a
    // world ticked by a headless tool takes the same random path as one ticked
    // with a pool behind it. That is the difference between "no entities here"
    // and "a different world".
    // `dh.h(Lcn;III)V`'s other half -- **`new ff(...)` and
    // `World.spawnEntityInWorld`**, for the one block behaviour that spawns a
    // moving copy of itself.
    //
    // **Unset is not "do nothing"; it is `BlockSand.fallInstantly`.** That flag
    // is a real static on the class file, true while a chunk is populated, and
    // with it set `tryToFall` runs the entity's own tick to a standstill rather
    // than letting it live. Here the flag is "is there a pool to put it in":
    // world generation and every headless tool have none, and
    // `tick::fallingTick` takes the instant path for them -- which lands the
    // block in the same cell, because that path *is* the entity's landing test
    // run to completion. Returns whether the entity was taken.
    using FallingBlockSink = bool (*)(void* ctx, i32 x, int y, i32 z, u16 block);
    void setFallingBlockSink(FallingBlockSink sink, void* ctx)
    {
        fallingSink_ = sink;
        fallingSinkCtx_ = ctx;
    }
    bool spawnFallingBlock(i32 x, int y, i32 z, block::BlockId id) const
    {
        return fallingSink_ != nullptr && fallingSink_(fallingSinkCtx_, x, y, z, u16(id));
    }

    // `q`'s three `new jd(...)` sites -- **BlockTNT priming itself**, and the
    // second block behaviour in this version that spawns an entity. The cell
    // is already air by the time this is called, exactly as it is in the jar:
    // `hq.b(IIII)Z` writes the air and then calls `onBlockDestroyedByPlayer`,
    // and `je`'s phase three does the same before
    // `onBlockDestroyedByExplosion`.
    //
    // **The fuse is the caller's** and not this seam's. Two of the three sites
    // want the constructor's 80; the third, `q.c(Lcn;III)V`, re-rolls it as
    // `world.rand.nextInt(20) + 10` -- and that draw has to stay on the
    // world's generator, in the order the jar makes it, or every random after
    // it in the tick moves.
    //
    // **Unset throws the entity away**, which is the bargain `spawnItem` takes
    // and for the same reason: the pool belongs to the frame loop. A headless
    // tool breaking TNT therefore gets a cell of air and no blast, and makes
    // every draw it would have made either way. Returns whether it was taken.
    using PrimedTntSink = bool (*)(void* ctx, i32 x, int y, i32 z, int fuse);
    void setPrimedTntSink(PrimedTntSink sink, void* ctx)
    {
        tntSink_ = sink;
        tntSinkCtx_ = ctx;
    }
    bool spawnPrimedTnt(i32 x, int y, i32 z, int fuse) const
    {
        return tntSink_ != nullptr && tntSink_(tntSinkCtx_, x, y, z, fuse);
    }

    // `cn.a(DDDLjava/lang/String;FF)V` -- **World.playSoundEffect**, the one
    // thing a block behaviour does that is neither a block nor an entity.
    //
    // It is a seam for the same reason the other two are: `core/tick/` has no
    // sound engine and must not grow one -- the tick runs on a worker, the
    // mixer does not, and a `SoundEngine&` reaching into here would tie them
    // together. Unset is silence, which is what every headless tool wants.
    //
    // **This is what a pressure plate was missing**, and it was reported as the
    // plate feeling unresponsive rather than as a missing sound: a plate is
    // flush with the floor and its state is two pixels of metadata, so the
    // click *is* the feedback. The lever, the button and the door were in the
    // same position.
    //
    // `key` is a pool name and outlives the call -- every caller passes a
    // string literal.
    using SoundSink = void (*)(void* ctx, const char* key, double x, double y, double z,
                               float volume, float pitch);
    void setSoundSink(SoundSink sink, void* ctx)
    {
        soundSink_ = sink;
        soundSinkCtx_ = ctx;
    }
    void playSoundAt(const char* key, double x, double y, double z, float volume,
                     float pitch) const
    {
        if (soundSink_ != nullptr) {
            soundSink_(soundSinkCtx_, key, x, y, z, volume, pitch);
        }
    }

    // **`cn.a(String, DDDDDD)` -- World.spawnParticle**, and it is a seam for
    // exactly the reason the sound is: a block behaviour and an entity tick
    // both ask the world for a puff of smoke, and the pool that holds the
    // particles belongs to the frame loop.
    //
    // `kind` is `entity::ParticleKind` as an integer, so this header does not
    // have to include the particle pool to declare the seam -- `core/entity`
    // already depends on `core/tick` and the reverse has never been true. The
    // one caller that installs the sink converts it back.
    using ParticleSink = void (*)(void* ctx, int kind, double x, double y, double z,
                                  double motionX, double motionY, double motionZ);
    void setParticleSink(ParticleSink sink, void* ctx)
    {
        particleSink_ = sink;
        particleSinkCtx_ = ctx;
    }
    bool hasParticleSink() const { return particleSink_ != nullptr; }
    void spawnParticle(int kind, double x, double y, double z, double motionX = 0.0,
                       double motionY = 0.0, double motionZ = 0.0) const
    {
        if (particleSink_ != nullptr) {
            particleSink_(particleSinkCtx_, kind, x, y, z, motionX, motionY, motionZ);
        }
    }

    using DropSink = void (*)(void* ctx, double x, double y, double z, u16 item, int count);
    void setDropSink(DropSink sink, void* ctx)
    {
        dropSink_ = sink;
        dropSinkCtx_ = ctx;
    }
    void spawnItem(double x, double y, double z, u16 item, int count) const
    {
        if (dropSink_ != nullptr) {
            dropSink_(dropSinkCtx_, x, y, z, item, count);
        }
    }

    // `cn.l(III)V` -- **World.removeBlockTileEntity**, which `jt.b` --
    // BlockContainer.onBlockRemoval -- calls on every removal of a block that
    // has one, whoever removed it. The tile entities are held by the frame loop
    // (core/world/sign_store.hpp), so this is a seam like the three above it.
    //
    // **This is why a sign stayed up when its wall went.** The player's break
    // was the only thing that forgot the text; a sign dropped by
    // `signNeighbourChanged` became air in the world and stayed in the store,
    // and a sign is drawn from the store and not from the chunk.
    using TileEntitySink = void (*)(void* ctx, i32 x, int y, i32 z);
    void setTileEntityRemovedSink(TileEntitySink sink, void* ctx)
    {
        tileEntitySink_ = sink;
        tileEntitySinkCtx_ = ctx;
    }
    void removeTileEntity(i32 x, int y, i32 z) const
    {
        if (tileEntitySink_ != nullptr) {
            tileEntitySink_(tileEntitySinkCtx_, x, y, z);
        }
    }

    // `cn.a(IIILic;)V` -- **World.setBlockTileEntity**, the other half of the
    // pair. `jt.e` -- BlockContainer.onBlockAdded -- calls it with whatever
    // `getBlockEntity()` builds, on *every* way the block can appear rather
    // than on a click, which is why this hangs off the block-added dispatch
    // and not off `core/item/use.cpp`.
    //
    // It carries no contents: all four of this version's tile entities start
    // from their own constructor's defaults, and the only one with anything
    // interesting in it -- `bd`'s `"Pig"` -- is the store's business. A world
    // file's saved contents arrive by a different road; see
    // core/entity/mob_spawner.hpp.
    void setTileEntityAddedSink(TileEntitySink sink, void* ctx)
    {
        tileEntityAddedSink_ = sink;
        tileEntityAddedSinkCtx_ = ctx;
    }
    void addTileEntity(i32 x, int y, i32 z) const
    {
        if (tileEntityAddedSink_ != nullptr) {
            tileEntityAddedSink_(tileEntityAddedSinkCtx_, x, y, z);
        }
    }

    // **The furnace's and the chest's contents**, which live in the column's
    // own list rather than in a store -- see core/world/tile_entity.hpp. Null
    // for a column that is not resident.
    std::vector<world::TileEntity>* tileEntitiesAt(i32 x, i32 z) const
    {
        world::ChunkColumn* column = columnAtBlock(x, z);
        return column != nullptr ? &column->tileEntities : nullptr;
    }

    // `ic.j_()` -- **onInventoryChanged**, which marks the chunk modified so
    // the next save writes what is in a furnace or a chest. Not a block write:
    // nothing is remeshed.
    using ColumnModifiedSink = void (*)(void* ctx, i32 x, i32 z);
    void setColumnModifiedSink(ColumnModifiedSink sink, void* ctx)
    {
        columnModifiedSink_ = sink;
        columnModifiedSinkCtx_ = ctx;
    }
    void markTileEntityChanged(i32 x, i32 z) const
    {
        if (columnModifiedSink_ != nullptr) {
            columnModifiedSink_(columnModifiedSinkCtx_, x, z);
        }
    }

    // **A screen a block opens** -- `dm.a(Lgh;)V` for a chest, `dm.l()` for the
    // workbench, `dm.a(Lke;)V` for a furnace. The screens are the frame loop's;
    // this says which and where, and the block has already taken the click.
    enum class ContainerKind : u8 { Workbench, Furnace, Chest };
    using ContainerSink = void (*)(void* ctx, ContainerKind kind, i32 x, int y, i32 z);
    void setContainerSink(ContainerSink sink, void* ctx)
    {
        containerSink_ = sink;
        containerSinkCtx_ = ctx;
    }
    void openContainer(ContainerKind kind, i32 x, int y, i32 z) const
    {
        if (containerSink_ != nullptr) {
            containerSink_(containerSinkCtx_, kind, x, y, z);
        }
    }

    // **A whole stack thrown with its own motion** -- `new EntityItem(...)` with
    // the stack's damage and a velocity the caller chose, which is how a broken
    // chest spills. `spawnItem` above is the block-drop constructor and cannot
    // carry either.
    using StackSink = void (*)(void* ctx, double x, double y, double z, u16 item, int count,
                               i16 damage, double motionX, double motionY, double motionZ);
    void setStackSink(StackSink sink, void* ctx)
    {
        stackSink_ = sink;
        stackSinkCtx_ = ctx;
    }
    void spawnItemStack(double x, double y, double z, u16 item, int count, i16 damage,
                        double motionX, double motionY, double motionZ) const
    {
        if (stackSink_ != nullptr) {
            stackSink_(stackSinkCtx_, x, y, z, item, count, damage, motionX, motionY, motionZ);
        }
    }

    // ---- redstone power ------------------------------------------------
    //
    // Four questions, and a1.1.2 really does need all four. `Side` here is the
    // face of the *asking* block that the answer arrives through, in the
    // original's numbering: 0 is -y, 1 is +y, 2 is -z, 3 is +z, 4 is -x, 5 is
    // +x. Getting that numbering wrong is a circuit that works in three
    // directions.

    // `cn.j(IIII)Z` -- isBlockProvidingPowerTo: does the block *at* x,y,z hand
    // power out of the named face itself?
    bool providesPowerTo(i32 x, int y, i32 z, int side) const;

    // `cn.k(IIII)Z` -- isBlockIndirectlyProvidingPowerTo. An opaque cube is
    // transparent to this: it answers with whatever is powering *it*, which is
    // what makes a block with a torch under it act as a source.
    bool indirectlyProvidesPowerTo(i32 x, int y, i32 z, int side) const;

    // `cn.n(III)Z` -- is any of the six neighbours handing power in?
    bool isPowered(i32 x, int y, i32 z) const;

    // `cn.o(III)Z` -- the same through blocks, which is the one a wire reads.
    bool isIndirectlyPowered(i32 x, int y, i32 z) const;

    // **`BlockRedstoneWire.wiresProvidePower`**, and it has to live here
    // rather than beside the wire because it is read through the power queries
    // above. a1.1.2 keeps it on the single shared Block object and turns it
    // off for the length of one `isBlockIndirectlyGettingPowered` call, so
    // that a wire working out its own strength does not count itself and its
    // neighbours as sources. Without it every wire is 15.
    bool wiresProvidePower() const { return wiresProvidePower_; }
    void setWiresProvidePower(bool v) { wiresProvidePower_ = v; }

    // `BlockRedstoneTorch.redstoneUpdateInfoList` -- the burnout record. A
    // torch that toggles eight times at one position inside 100 ticks stays
    // off, which is how a1.1.2 stops a torch wired to itself from oscillating
    // for ever. Static on the Block in the original, so world-scoped here.
    // Bounded, because the frame path may not allocate: the oldest entry is
    // dropped when it is full, which can only ever make a torch *less* likely
    // to be declared burnt out.
    bool noteTorchToggle(i32 x, int y, i32 z);
    int torchToggleCount(i32 x, int y, i32 z) const;

    // ---- writing -------------------------------------------------------

    // `cn.a(IIII)Z` -- the write with no notification, for a behaviour that
    // will notify once at the end.
    bool setBlockRaw(i32 x, int y, i32 z, block::BlockId id);

    // `cn.a(IIIII)Z` -- block and metadata together, no notification. The
    // metadata lands before the new block's `onBlockAdded` runs.
    bool setBlockAndDataRaw(i32 x, int y, i32 z, block::BlockId id, u8 data);

    // `cn.d(IIII)Z` -- setBlockWithNotify: write, mark for redraw, then tell
    // all six neighbours. The workhorse.
    bool setBlockWithNotify(i32 x, int y, i32 z, block::BlockId id);

    // `cn.b(IIIII)Z` -- block and metadata together, then notify.
    bool setBlockAndDataWithNotify(i32 x, int y, i32 z, block::BlockId id, u8 data);

    bool setDataRaw(i32 x, int y, i32 z, u8 data);
    bool setDataWithNotify(i32 x, int y, i32 z, u8 data);

    // ---- the recursion bound -------------------------------------------
    //
    // **A block change is reentrant, and nothing bounded it.** `writeBlock`
    // calls `blockAdded`/`blockRemoved` and behaviours call `notifyNeighbours`,
    // which dispatches `neighbourChanged` synchronously -- which can write
    // another block. A fire field going out unwinds as one recursion whose
    // depth is the size of the field, and lava starts fires. A 3DSX main thread
    // gets 32 KB of stack and nothing in the binary can enlarge it;
    // `crashlogs/001-rungame-stack-overflow` is what that looks like.
    //
    // Redstone had a guard of its own (`kPropagateDepth`) and it did not work:
    // every hop through `notifyNeighbours` restarted the count at zero, so the
    // bound applied to one straight run of wire and not to the cascade.
    //
    // So the budget is global, shared by every path that can recurse, and it is
    // counted in **bytes of stack rather than in levels**, because the levels
    // are not the same size. The figures are measured, with `-fstack-usage` on
    // the devkitARM build at -O2:
    //
    //   * a notify level is notifyNeighbours 32 + neighbourChanged 56 +
    //     the behaviour ~24 + setBlockWithNotify 48 + writeBlock 72 +
    //     blockRemoved 8  = 240 bytes
    //   * a wire level is wirePropagate 16 + wirePropagateInner 72 = 88 bytes
    //
    // 8 KB is a quarter of the stack and leaves the rest for whatever the tick
    // was called from. It allows 93 nested wire levels against the 16 the
    // original's own comment calls reachable, and 34 nested notify levels, so
    // nothing a real world does comes near it.
    static constexpr int kCascadeStackBudget = 8192;
    static constexpr int kNotifyLevelBytes = 240;
    static constexpr int kWireLevelBytes = 88;

    // Charges `bytes` against the budget. False when there is not enough left,
    // in which case the caller must *not* recurse.
    bool enterCascade(int bytes)
    {
        if (cascadeBytes_ + bytes > kCascadeStackBudget) return false;
        cascadeBytes_ += bytes;
        return true;
    }
    void leaveCascade(int bytes) { cascadeBytes_ -= bytes; }

    // Counted here rather than in redstone.cpp because Stats is private and
    // this is the only thing outside the class that has to record one.
    void noteWireRefused() { ++stats_.wireRefused; }

    // `cn.g(IIII)V` -- notifyBlocksOfNeighborChange, in Side order.
    //
    // Past the cascade budget the six calls are not made here: the centre goes
    // on a queue and is notified once the stack has unwound, before the tick
    // ends. Deferred rather than dropped, because a lost notification is a
    // fluid that stops flowing or a fire that never goes out.
    void notifyNeighbours(i32 x, int y, i32 z, block::BlockId fromId);

    // `cn.h(IIII)V` -- scheduleBlockUpdate. The delay is the block's own
    // tickRate, taken from the table, and the entry is dropped silently if the
    // 8-block box around it is not fully loaded, exactly as the original does.
    void scheduleBlockUpdate(i32 x, int y, i32 z, block::BlockId id);

    // ---- the tick ------------------------------------------------------

    struct Centre {
        i32 chunkX = 0;
        i32 chunkZ = 0;
    };

    struct Stats {
        i64 ticks = 0;
        i64 chunksTicked = 0;
        i64 randomTicks = 0;       // positions sampled, not behaviours run
        i64 randomTicksRun = 0;    // positions that actually dispatched
        i64 scheduledRun = 0;
        i64 blocksChanged = 0;
        i64 scheduledDropped = 0;  // the box was not loaded, or the pool was full

        // Notifications the cascade budget pushed out of the recursion and on
        // to the deferred queue. Not lost -- they run before the tick ends --
        // but they run in a different order than the original would have, so a
        // non-zero value is worth seeing.
        i64 notifyDeferred = 0;

        // Deferred notifications the queue had no room for. These *are* lost.
        // It should stay zero; if it does not, the queue is too small for
        // whatever the world is doing.
        i64 notifyDropped = 0;

        // Wire propagations refused because the cascade budget was spent. The
        // original has no such limit, so this is a fidelity loss and has to be
        // visible rather than silent.
        i64 wireRefused = 0;
    };

    // One 20 Hz step of the world: advance the clock, run everything the
    // scheduler owes, then random-tick the chunks around each centre.
    // `radius` is a1.1.2's own 9 unless the loaded area is smaller; see
    // `kChunkTickRadius`.
    void tick(const Centre* centres, int centreCount, int radius);

    // a1.1.2's `cn.h()` collects a 19x19 square of chunk coordinates around
    // every player. Ours cannot exceed what the streamer holds, so the caller
    // passes the smaller of this and its load radius.
    static constexpr int kChunkTickRadius = 9;

    // Random tick attempts per chunk per tick, from `cn.h()`'s
    // `for (int i = 0; i < 80; i++)`. Each attempt picks one of the 32,768
    // positions in the column, so a given block is ticked about once every
    // 410 ticks -- twenty seconds.
    static constexpr int kRandomTicksPerChunk = 80;

    i64 time() const { return time_; }
    void setTime(i64 t) { time_ = t; }

    // `cn.e` -- how much to take off sky light right now. Cached per tick
    // because every light query reads it.
    int skyDarken() const { return skyDarken_; }

    // Whether the world is one where snow forms and water freezes --
    // level.dat's `SnowCovered`, which a1.1.2 rolls once at creation. The
    // snow-and-ice pass in `cn.h()` runs only when it is set.
    void setSnowCovered(bool v) { snowCovered_ = v; }
    bool snowCovered() const { return snowCovered_; }

    // The Extra Setting `improvedFencePlacement` -- see
    // core/settings/world_settings.hpp. Off is a1.1.2's `fh` rule.
    void setImprovedFencePlacement(bool v) { improvedFencePlacement_ = v; }
    bool improvedFencePlacement() const { return improvedFencePlacement_; }

    // The column cache holds a raw pointer into whatever owns the chunks, and
    // that owner may evict between ticks. Anything that moves or frees a
    // column must say so; `tick()` does it itself.
    void invalidateColumnCache() { cachedValid_ = false; cached_ = nullptr; }

    TickScheduler& scheduler() { return scheduler_; }
    const TickScheduler& scheduler() const { return scheduler_; }
    JavaRandom& random() { return random_; }
    const Stats& stats() const { return stats_; }

private:
    TickAccess access_;
    TickScheduler scheduler_;
    JavaRandom random_;

    // `cn.f` and `cn.g`: the update LCG and the constant it advances by. The
    // positions a random tick visits come out of this and *not* out of
    // `World.rand`, which is why a tick that consumes randomness cannot shift
    // the positions of later ones.
    // Held unsigned because the original overflows it every few thousand
    // advances and signed overflow is undefined in C++ -- the sanitised host
    // build would stop on the first one. Java's `>>` is arithmetic, so the
    // shift is taken on the signed reinterpretation.
    u32 updateLcg_;
    static constexpr u32 kUpdateLcgAddend = 1013904223u;

    EntityQuery entityQuery_ = nullptr;
    void* entityQueryCtx_ = nullptr;

    SolidBoxQuery solidBoxQuery_ = nullptr;
    void* solidBoxQueryCtx_ = nullptr;

    DropSink dropSink_ = nullptr;
    void* dropSinkCtx_ = nullptr;

    FallingBlockSink fallingSink_ = nullptr;
    void* fallingSinkCtx_ = nullptr;

    PrimedTntSink tntSink_ = nullptr;
    void* tntSinkCtx_ = nullptr;

    ParticleSink particleSink_ = nullptr;
    void* particleSinkCtx_ = nullptr;
    TileEntitySink tileEntityAddedSink_ = nullptr;
    void* tileEntityAddedSinkCtx_ = nullptr;
    SoundSink soundSink_ = nullptr;
    void* soundSinkCtx_ = nullptr;

    TileEntitySink tileEntitySink_ = nullptr;
    void* tileEntitySinkCtx_ = nullptr;

    ColumnModifiedSink columnModifiedSink_ = nullptr;
    void* columnModifiedSinkCtx_ = nullptr;
    ContainerSink containerSink_ = nullptr;
    void* containerSinkCtx_ = nullptr;
    StackSink stackSink_ = nullptr;
    void* stackSinkCtx_ = nullptr;

    i64 time_ = 0;
    int skyDarken_ = 0;
    bool snowCovered_ = false;
    bool improvedFencePlacement_ = false;
    bool wiresProvidePower_ = true;

    struct TorchToggle {
        i32 x = 0;
        i32 z = 0;
        i16 y = 0;
        i64 time = 0;
    };
    static constexpr usize kTorchToggleCapacity = 64;
    TorchToggle torchToggles_[kTorchToggleCapacity];
    usize torchToggleCount_ = 0;
    Stats stats_;

    // Stack charged to the cascade in progress; see kCascadeStackBudget.
    int cascadeBytes_ = 0;

    // Notifications the budget refused, to run once the stack has unwound. The
    // centre is stored rather than its six neighbours, so one entry stands for
    // one refused notifyNeighbours call.
    struct PendingNotify {
        i32 x = 0;
        i32 z = 0;
        i16 y = 0;
        block::BlockId fromId = 0;
    };
    // Reserved once in the constructor and never allowed past capacity, so
    // nothing here allocates on the frame path.
    static constexpr usize kDeferredNotifyCapacity = 512;
    std::vector<PendingNotify> deferredNotify_;
    usize deferredNotifyHead_ = 0;
    bool drainingNotify_ = false;

    // The six calls, with the budget already charged.
    void notifyNeighboursNow(i32 x, int y, i32 z, block::BlockId fromId);
    void drainDeferredNotifications();

    // The last column looked up. A random tick reads 80 positions in one
    // chunk in a row, and a fluid reads its four neighbours; without this the
    // chunk lookup dominates.
    mutable world::ChunkColumn* cached_ = nullptr;
    mutable i32 cachedX_ = 0;
    mutable i32 cachedZ_ = 0;
    mutable bool cachedValid_ = false;

    world::ChunkColumn* columnFor(i32 chunkX, i32 chunkZ) const;
    world::ChunkColumn* columnAtBlock(i32 x, i32 z) const;

    // Keeps the column's height map right after a write at `y`. O(1) unless
    // the cell that was holding the map up has just become transparent.
    static void refreshHeight(world::ChunkColumn& column, int lx, int y, int lz);

    // The one place a block is actually written. Both raw setters go through
    // it so that onBlockRemoval and onBlockAdded cannot be forgotten by one of
    // them -- which is how a fluid came to spread one block and stop.
    bool writeBlock(i32 x, int y, i32 z, block::BlockId id, u8 data, bool setData);

    void runScheduled();
    void randomTickChunk(world::ChunkColumn& column);
    void snowAndIce(world::ChunkColumn& column);

    // `ic.b()` for every tile entity in the column that ticks -- the furnace;
    // the spawner has its own store. See core/tick/furnace.hpp.
    void tickTileEntities(world::ChunkColumn& column);

    i32 nextLcg()
    {
        updateLcg_ = updateLcg_ * 3u + kUpdateLcgAddend;
        return i32(updateLcg_) >> 2;
    }
};

}  // namespace mc::tick
