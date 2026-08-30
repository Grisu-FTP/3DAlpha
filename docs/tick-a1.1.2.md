# The world tick, recovered from a1.1.2

Everything here was read out of `minecraft-a1.1.2_01-client.jar` -- by disassembling it with
`javap -c -p`, and, where a constructor branches, by loading the classes under a real JVM and asking
them. Obfuscated names are quoted so a claim can be re-checked; they are for **a1.1.2_01 only** and
mean nothing in another version.

The classes:

| Obfuscated | What it is |
|---|---|
| `cn` | `World` |
| `ly` | `Block` |
| `ir` | `Timer` |
| `jf` | `NextTickListEntry` |
| `gb` | `Material` |
| `ga` | `Chunk` |

## The clock -- `ir`

```java
public void updateTimer() {          // ir.a()
    long sysClock = System.currentTimeMillis();
    long dSys = sysClock - lastSyncSysClock;
    long hrClock = System.nanoTime() / 1000000L;
    if (dSys > 1000L) {              // once a second, correct one clock against the other
        long dHr = hrClock - lastSyncHRClock;
        timeSyncAdjustment += ((double) dSys / (double) dHr - timeSyncAdjustment)
                              * 0.20000000298023224D;
        lastSyncSysClock = sysClock;  lastSyncHRClock = hrClock;
    }
    if (dSys < 0L) { lastSyncSysClock = sysClock;  lastSyncHRClock = hrClock; }

    double hrSeconds = (double) hrClock / 1000.0D;
    double delta = (hrSeconds - lastHRTime) * timeSyncAdjustment;
    lastHRTime = hrSeconds;
    if (delta < 0.0D) delta = 0.0D;
    if (delta > 1.0D) delta = 1.0D;                       // one second, before scaling

    elapsedPartialTicks += delta * timerSpeed * ticksPerSecond;   // ticksPerSecond = 20.0f
    elapsedTicks = (int) elapsedPartialTicks;
    elapsedPartialTicks -= elapsedTicks;                  // subtract, *then* clamp
    if (elapsedTicks > 10) elapsedTicks = 10;             // the surplus is discarded
    renderPartialTicks = elapsedPartialTicks;
}
```

Three things are load-bearing: the one-second clamp on the raw delta, the truncate-subtract-**then**-
clamp order (so ticks past the tenth are *dropped*, and the world falls behind rather than
spiralling), and the surviving fraction, which is what keeps a day 20 minutes long at any frame rate.

`core/tick/tick_timer.hpp` reproduces all three and **omits the dual-clock correction**: a 3DS has
one clock, `svcGetSystemTick` off a fixed oscillator, so there is nothing to correct against and the
ratio would be exactly 1.0 for ever.

## `World.tick` -- `cn.g()`

```java
chunkProvider.unloadQueuedChunks();
int sub = calculateSkylightSubtracted(1.0F);
if (sub != skylightSubtracted) {
    skylightSubtracted = sub;
    for (IWorldAccess a : worldAccesses) a.updateAllRenderers();
}
worldTime++;
if (worldTime % autosaveInterval == 0) saveWorld(false, null);
tickUpdates(false);
tickBlocks();
```

The sky-light subtraction is recomputed **before** the clock advances and every light query for the
rest of the tick reads it, so the time of day is an input to block behaviour and not only to the
lightmap: grass, mushrooms, crops and saplings all read `getBlockLightValue`.

## Scheduled updates -- `cn.a(Z)Z` and `cn.h(IIII)V`

```java
public void scheduleBlockUpdate(int x, int y, int z, int blockId) {   // cn.h(IIII)V
    NextTickListEntry e = new NextTickListEntry(x, y, z, blockId);
    byte r = 8;
    if (checkChunksExist(x-r, y-r, z-r, x+r, y+r, z+r)) {
        if (blockId > 0) e.setScheduledTime(Block.blocksList[blockId].tickRate() + worldTime);
        if (!scheduledTickSet.contains(e)) { scheduledTickSet.add(e); pendingTickListEntries.add(e); }
    }
}

public boolean tickUpdates(boolean runAll) {                          // cn.a(Z)Z
    int n = pendingTickListEntries.size();
    if (n != scheduledTickSet.size()) throw new IllegalStateException("TickNextTick list out of synch");
    if (n > 1000) n = 1000;
    for (int i = 0; i < n; i++) {
        NextTickListEntry e = (NextTickListEntry) pendingTickListEntries.first();
        if (!runAll && e.scheduledTime > worldTime) break;
        pendingTickListEntries.remove(e);  scheduledTickSet.remove(e);
        byte r = 8;
        if (checkChunksExist(e.x-r, e.y-r, e.z-r, e.x+r, e.y+r, e.z+r)) {
            int id = getBlockId(e.x, e.y, e.z);
            if (id == e.blockID && id > 0) Block.blocksList[id].updateTick(this, e.x, e.y, e.z, rand);
        }
    }
    return pendingTickListEntries.size() != 0;
}
```

`NextTickListEntry` (`jf`) is equal on `(x, y, z, blockID)` and **orders on `(scheduledTime,
insertion sequence)`** -- `jf.a(Ljf;)I` compares `e` then the private `g`, a static counter bumped in
the constructor. The insertion tie-break is not decoration: it is what makes water spread in a stable
pattern rather than a different one every run.

Two numbers are the original's and are kept: the **8-block residency box** around anything scheduled
or run, and the **1,000-per-tick** cap.

## Writing a block is three things, not one -- `ga.a(IIII)Z`

`Chunk.setBlockID` is where a block actually changes, and it does more than store a number. In
order:

```java
int old = blocks[index];
if (old == id) return false;
blocks[index] = (byte) id;
if (old != 0) Block.blocksList[old].onBlockRemoval(world, X, y, Z);   // ly.b(Lcn;III)V
metadata.setNibble(x, y, z, 0);                                       // *** cleared ***
... height map, then relight sky and block light over the one column ...
if (id != 0) Block.blocksList[id].onBlockAdded(world, X, y, Z);       // ly.e(Lcn;III)V
isModified = true;
```

Two of those three are easy to miss and both are load-bearing:

- **The metadata is cleared.** A cell that held flowing water at level 5 and is set to air and then
  to water again does not inherit the 5. The five-argument form,
  `Chunk.setBlockIDWithMetadata`, keeps the metadata it is given instead -- and writes it
  **before** `onBlockAdded`, because a fluid that has just appeared reads its own level.
- **`onBlockAdded` runs on the new block**, and it is *not* the same thing as a neighbour
  notification. `BlockFlowing.onBlockAdded` schedules the new block's own update and
  `BlockSand.onBlockAdded` schedules its fall. Without it a fluid spreads exactly one block and
  stops for ever, which is precisely the bug it caused here before it existed. The overriding
  classes are: sand and gravel, both fluids, torch and redstone torch, stairs, slabs, sponge,
  furnace, rail, lever, button, both pressure plates, and redstone wire.

`onBlockRemoval` runs while the departing block's metadata is still readable. Its base is empty and
every override belongs to a behaviour not yet ported, so it is a named hook here rather than
behaviour.

## Neighbour notification -- `cn.g(IIII)V`

Six neighbours, in this order, and the order is observable:

```
x-1, x+1, y-1, y+1, z-1, z+1
```

Each becomes `Block.onNeighborBlockChange(world, x, y, z, changedId)`, skipped entirely while
`World.editingBlocks` or `World.isRemote` is set. `setBlockWithNotify` (`cn.d(IIII)Z`) is the write
plus `markBlockNeedsUpdate` plus that fan-out.

## Random ticks -- `cn.h()`

The part that costs, transcribed:

```java
activeChunkSet.clear();
for (EntityPlayer p : playerEntities) {
    int cx = MathHelper.floor_double(p.posX / 16.0D), cz = MathHelper.floor_double(p.posZ / 16.0D);
    byte r = 9;
    for (int dx = -r; dx <= r; dx++)
        for (int dz = -r; dz <= r; dz++)
            activeChunkSet.add(new ChunkCoordIntPair(dx + cx, dz + cz));
}
if (soundCounter > 0) soundCounter--;

for (ChunkCoordIntPair c : activeChunkSet) {
    int x0 = c.x * 16, z0 = c.z * 16;
    Chunk chunk = getChunkFromChunkCoords(c.x, c.z);

    ... ambient cave sound, one sampled position, on a soundCounter of zero ...

    if (snowCovered && rand.nextInt(4) == 0) {
        updateLCG = updateLCG * 3 + 1013904223;
        int r = updateLCG >> 2;
        int lx = r & 15, lz = (r >> 8) & 15;
        int y = getPrecipitationHeight(x0 + lx, z0 + lz);
        if (y >= 0 && y < 128 && chunk.getSavedLightValue(BLOCK, lx, y, lz) < 10) {
            int below = chunk.getBlockID(lx, y - 1, lz);
            if (chunk.getBlockID(lx, y, lz) == 0 && Block.snow.canPlaceBlockAt(this, X, y, Z))
                setBlockWithNotify(X, y, Z, Block.snow.blockID);
            if (below == Block.waterStill.blockID && chunk.getBlockMetadata(lx, y-1, lz) == 0)
                setBlockWithNotify(X, y - 1, Z, Block.ice.blockID);
        }
    }

    for (int i = 0; i < 80; i++) {
        updateLCG = updateLCG * 3 + 1013904223;
        int r = updateLCG >> 2;
        int lx = r & 15, lz = (r >> 8) & 15, y = (r >> 16) & 127;
        int id = chunk.blocks[lx << 11 | lz << 7 | y];
        if (Block.tickOnLoad[id])
            Block.blocksList[id].updateTick(this, lx + x0, y, lz + z0, rand);
    }
}
```

Numbers worth stating plainly:

- **19x19 chunks per player** (radius 9), regardless of render distance.
- **80 attempts per chunk per tick**, each over the whole 16x128x16 column. A given block is
  therefore ticked about once in 410 ticks -- **twenty seconds**.
- At radius 9 that is 361 x 80 = **28,880 samples a tick, 577,600 a second**. It is the largest
  single cost in the tick and it is almost all `tickOnLoad[id]` coming back false.
- `updateLCG` is seeded `new Random().nextInt()` -- **wall clock**. a1.1.2's random ticks are not
  reproducible between two runs of the same world, and nothing in the game depends on their being so.
  We seed from the world seed instead: no fidelity is lost and a test can assert on an outcome.

`updateLCG * 3 + 1013904223` overflows constantly. In C++ that is undefined behaviour on a signed
int and the sanitised host build stops on the first one, so it is held in a `u32` and the `>> 2` is
taken on the signed reinterpretation, which is what Java's arithmetic shift does.

## Which blocks tick, and how fast

Read from a running jar by `tools/extract_ticks.java`, which prints `Block.tickOnLoad[id]` and
`Block.blocksList[id].tickRate()` for every constructed block. **Three ids get the wrong answer from
the bytecode alone**, because their constructors branch:

- **9 still water: no. 11 still lava: yes.** `BlockStationary`'s constructor calls
  `setTickRandomly(false)` and then `setTickRandomly(true)` again only when the material is lava.
- **73 unlit redstone ore: no. 74 lit redstone ore: yes.** One class, constructed twice with a flag.

Non-default `tickRate()`, in ticks:

| Rate | Blocks |
|--:|---|
| 2 | redstone torch (75, 76) |
| 3 | sand (12), gravel (13) |
| 5 | water (8, 9) |
| 20 | pressure plates (70, 72), stone button (77) |
| 30 | lava (10, 11), redstone ore (73, 74) |
| 10 | everything else, from `Block.tickRate()` |

25 of the 70 blocks tick randomly: 2, 6, 8, 10, 11, 18, 37, 38, 39, 40, 50, 51, 59, 60, 70, 72, 74,
75, 76, 77, 78, 79, 80, 81, 83.

Both columns are in `data/a1.1.2/blocks.json` and reach the runtime as `BlockDef::tickRandomly` and
`BlockDef::tickRate`.

## The fluids -- `jp`, `hv`, `hn`

The largest single behaviour in the game, and three classes: `jp` (BlockFluid, the shared half),
`hv` (BlockFlowing, which ticks) and `hn` (BlockStationary, which does not).

**The pair.** Every fluid is two blocks registered adjacently -- `flowing = still - 1`, and a1.1.2
really does write `blockID + 1` and `blockID - 1`. A flowing block that finds nothing to change
becomes the still block and stops costing anything (`hv.j`, setStatic); a still block that hears a
neighbour change becomes flowing again and schedules itself (`hn.j`, setNotStationary). Neither
notifies its neighbours, and that is not an omission -- notifying would wake the neighbours, which
would set *them* not-static, which would notify back. **This pair is why an ocean is free and a
waterfall is not**, and it is the single most important thing to keep intact on a console.

**Metadata is the level**: 0 at a source, rising by `this.d` per block -- **1 for water, 2 for lava**,
set in `jp`'s constructor -- and bit 3 (a value of 8 or more) marks fluid that is falling rather than
spreading. Level 8+ becomes 1 when it lands, which is why a waterfall spreads at full strength.

`hv.a(Lcn;IIILjava/util/Random;)V`, transcribed:

```java
int level = getFlowDecay(world, x, y, z);
boolean settle = true;
if (level > 0) {
    int best = -100;
    numAdjacentSources = 0;
    best = getSmallestFlowDecay(world, x-1, y, z, best);   // and +x, -z, +z
    ...
    int next = best + this.d;
    if (next >= 8 || best < 0) next = -1;
    if (getFlowDecay(world, x, y+1, z) >= 0) {             // anything falling in wins
        int above = getFlowDecay(world, x, y+1, z);
        next = above >= 8 ? above : above + 8;
    }
    if (numAdjacentSources >= 2 && blockMaterial == Material.water) {
        if (world.isBlockNormalCube(x, y-1, z)) next = 0;                 // infinite water
        else if (world.getBlockMaterial(x, y-1, z) == blockMaterial
                 && world.getBlockMetadata(x, y, z) == 0) next = 0;
    }
    if (blockMaterial == Material.lava && level < 8 && next < 8
        && next > level && rand.nextInt(4) != 0) { next = level; settle = false; }
    if (next != level) {
        level = next;
        if (level < 0) world.setBlockWithNotify(x, y, z, 0);
        else { world.setBlockMetadata(x, y, z, level);
               world.scheduleBlockUpdate(x, y, z, blockID);
               world.notifyBlocksOfNeighborChange(x, y, z, blockID); }
    } else if (settle) setStatic(world, x, y, z);
} else setStatic(world, x, y, z);                          // a source has nothing to recompute

if (canFlowInto(world, x, y-1, z)) {                       // down first, and exclusively
    world.setBlockAndMetadataWithNotify(x, y-1, z, blockID, level >= 8 ? level : level + 8);
} else if (level >= 0 && (level == 0 || blocksFlow(world, x, y-1, z))) {
    boolean[] dirs = getOptimalFlowDirections(world, x, y, z);
    int out = level >= 8 ? 1 : level + this.d;
    if (out >= 8) return;
    for each of the four directions dirs[d] is set: flowIntoBlock(...)
}
```

Three consequences worth naming, because each is a thing players notice:

- **Down before sideways, and exclusively.** A fluid that can fall does not spread at all that tick.
- **Two sources make a third**, over an opaque cube or over more of the same source. That is
  infinite water, and it is water only -- the `Material.water` test in the middle of it.
- **Lava only thins on one roll in four.** Its slowness is *not* only its tick rate of 30; this
  extra roll is on top of it.

**Which way it spreads is a search, not a fan-out.** `getOptimalFlowDirections` (`hv.k`) looks up to
four blocks along each of the four horizontal directions for somewhere the fluid could fall, using
`calculateFlowCost` (`hv.a`, recursive, depth-limited at 4, never doubling back -- direction 0 is
-x, 1 is +x, 2 is -z, 3 is +z, and the opposite pairs are how it knows), and spreads only along the
directions **tied** for the shortest path. That search is why water finds a hole across the room
rather than creeping outwards evenly, and it is the part of the module with a cost worth measuring.

**What stops a fluid** (`hv.l`, blocksFlow) is any solid material *plus* five blocks named one by
one: both doors, a sign post, a ladder and sugar cane. **What it may enter** (`hv.m`, canFlowInto)
is anything that does not block flow, is not its own kind, and is not lava.

**Lava meeting water** is `jp.j` (checkForHarden), reached from `onBlockAdded` and
`onNeighborBlockChange`: water beside or above -- five directions, **not** below -- turns a lava
source into obsidian and any thinner lava into cobblestone.

**One part is deliberately not wired.** `hn`'s updateTick is lava setting fire to what is above it:
`rand.nextInt(3)` steps, each wandering one block in x and z and climbing one in y, looking for air
with something burnable beside it. `TickBehaviour::Fire` has no implementation yet, so placing fire
would make a block that never spreads and never goes out -- worse than no fire. The search is left
out rather than half-run, and turning it on is one call once `BlockFire` lands.

## Fire -- `og`

**Metadata is age, 0 to 15**, climbing by one per scheduled tick at a tick rate of 10. Fire does not
die of old age; it stops counting at 15 and burns as long as it is fed.

```java
int age = world.getBlockMetadata(x, y, z);
if (age < 15) { world.setBlockMetadata(x, y, z, age + 1);
                world.scheduleBlockUpdate(x, y, z, blockID); }

if (!canNeighborBurn(world, x, y, z)) {
    if (!world.isBlockOpaqueCube(x, y-1, z) || age > 3) world.setBlockWithNotify(x, y, z, 0);
    return;
}
if (!canBlockCatchFire(world, x, y-1, z) && age == 15 && rand.nextInt(4) == 0) {
    world.setBlockWithNotify(x, y, z, 0);
    return;
}
if (age % 2 == 0 && age > 2) {
    tryToCatchBlockOnFire(world, x+1, y, z, 300, rand);
    tryToCatchBlockOnFire(world, x-1, y, z, 300, rand);
    tryToCatchBlockOnFire(world, x, y-1, z, 200, rand);   // *** below is the likeliest ***
    tryToCatchBlockOnFire(world, x, y+1, z, 250, rand);
    tryToCatchBlockOnFire(world, x, y, z-1, 300, rand);
    tryToCatchBlockOnFire(world, x, y, z+1, 300, rand);

    for (int bx = x-1; bx <= x+1; bx++)
      for (int bz = z-1; bz <= z+1; bz++)
        for (int by = y-1; by <= y+4; by++) {
            if (bx == x && by == y && bz == z) continue;
            int bound = 100;
            if (by > y + 1) bound += (by - (y+1)) * 100;
            int enc = getChanceOfNeighborsEncouragingFire(world, bx, by, bz);
            if (enc > 0 && rand.nextInt(bound) <= enc)
                world.setBlockWithNotify(bx, by, bz, blockID);
        }
}
```

`tryToCatchBlockOnFire(world, x, y, z, bound, rand)` is
`if (rand.nextInt(bound) < abilityToCatchFire[id])` -- so **a lower `bound` means more likely**,
which reads backwards and is why below (200) is the eagerest direction and above (250) the second.
When it fires, the block becomes fire on one roll in two and simply vanishes on the other, which is
what makes a burning structure develop holes rather than turn into a solid block of flame. A TNT
block caught this way is primed; there are no entities, so ours is destroyed and nothing explodes.

**Three different questions about burning, from three different tables**, and a1.1.2 asks all three:

| Question | Read from | Blocks |
|---|---|--:|
| Is there something here for a fire to feed on? | `chanceToEncourageFire[id] > 0` | 6 |
| Is this block itself consumed? | `abilityToCatchFire[id]`, rolled | 6 |
| Can lava set light to this? | `Material.getCanBurn()` | 14 |

The first two are BlockFire's own instance arrays -- planks 5/20, log 5/5, leaves 30/60, wool 30/60,
TNT 15/100, bookshelf 30/20. The third is a **wider** set that also holds chests, crafting tables,
signs, doors, jukeboxes and fences: those catch from lava and are invisible to fire's own spread.
Treating the three as one lights the wrong things in both directions. All three are in
`data/a1.1.2/blocks.json` as `burnEncourage`, `burnCatch` and `canBurn`, read from a running jar by
`tools/extract_ticks.java` -- which finds BlockFire by shape (the only block carrying two *instance*
`int[]` as long as the block table) rather than by name.

**A fully aged fire drops out of the scheduled list.** The metadata write *and* the re-schedule are
both inside `if (age < 15)`, so a fire that reaches 15 and is still being fed is never scheduled
again -- it is revisited only by a **random** tick, and fire is one of the 25 blocks that get those.
It looks exactly like a leak and it is the original's behaviour; `tests/tick_test.cpp` pins it.

**Where fire may exist at all** (`og.a(Lcn;III)Z`) is: an opaque cube directly below it, **or**
anything burnable among its six neighbours. `onBlockAdded` and `onNeighborBlockChange` both re-ask
it and remove the fire when the answer is no.

**Lava's ignition** (`hn.a(Lcn;IIILjava/util/Random;)V`) is now wired, and it was the one piece of
the fluids left out while fire did not exist: `rand.nextInt(3)` steps, each wandering one block in x
and z and climbing one in y, stopping at the first solid block, and placing fire in the first air
cell that has a `canBurn` neighbour.

## Redstone -- the power model, `kf` and `bg`

**Four questions, and a1.1.2 needs all four.** `side` names the face of the *asking* block that the
answer arrives through, numbered 0 = -y, 1 = +y, 2 = -z, 3 = +z, 4 = -x, 5 = +x. A circuit built on
the wrong numbering works in some directions and not others.

```java
public boolean isBlockProvidingPowerTo(int x, int y, int z, int side) {      // cn.j
    int id = getBlockId(x, y, z);
    return id != 0 && Block.blocksList[id].isProvidingPowerTo(this, x, y, z, side);
}
public boolean isBlockIndirectlyProvidingPowerTo(int x, int y, int z, int side) {   // cn.k
    if (isBlockOpaqueCube(x, y, z)) return isBlockGettingPowered(x, y, z);   // *** solids conduct ***
    int id = getBlockId(x, y, z);
    return id != 0 && Block.blocksList[id].isIndirectlyProvidingPowerTo(this, x, y, z, side);
}
```

The line marked is the one everything rests on: **an opaque cube answers with whatever is powering
it**, which is why a torch under a block powers what stands on top of it, and why the indirect query
cannot be a wrapper round the direct one.

### The wire -- `kf`

Strength is the metadata, 0 to 15. `updateAndPropagateCurrentStrength` (`kf.h`) recomputes one
cell's strength and walks outwards from any change:

```java
int old = getBlockMetadata(x, y, z);
int strength = 0;
wiresProvidePower = false;                                    // *** or every wire reads 15 ***
boolean powered = world.isBlockIndirectlyGettingPowered(x, y, z);
wiresProvidePower = true;
if (powered) strength = 15;
else {
    for each of the four horizontal neighbours (bx, bz) {
        strength = getMaxCurrentStrength(world, bx, y, bz, strength);
        if (world.isBlockOpaqueCube(bx, y, bz)) {
            if (!world.isBlockOpaqueCube(x, y+1, z))
                strength = getMaxCurrentStrength(world, bx, y+1, bz, strength);   // climb
        } else  strength = getMaxCurrentStrength(world, bx, y-1, bz, strength);   // descend
    }
    strength = strength > 0 ? strength - 1 : 0;
}
if (old != strength) {
    world.setBlockMetadata(x, y, z, strength);
    if (strength > 0) strength--;                             // *** the compared value, not the written one ***
    for each of the four neighbours: re-run on it, and on the cell above it if it is
        solid or below it if it is not, whenever that cell is wire whose strength differs
    if (old == 0 || strength == 0) notify all seven cells
}
```

Three details are easy to lose and each breaks something specific:

- **`wiresProvidePower` is turned off for exactly one call.** Without it a wire counts itself and
  its neighbouring wire as sources and the whole net reads 15.
- **The second decrement happens after the write**, and the propagation compares against that
  smaller value. Using the written one re-walks the whole net at every step.
- **Only the edges notify.** A wire going on or off wakes what it touches; a wire merely changing
  strength does not, which is what makes a long run cheap.

**Fifteen blocks is not a rule anywhere in the code.** It is what falls out of starting at 15 and
losing one per block.

`kf.b(Lnm;IIII)Z` decides which sides a lit wire actually powers, and it works the shape out from
what is around it because a1.1.2 stores no shape in the metadata: a wire connected to nothing is a
dot and powers all four sides; a straight run powers only its two ends; **a corner powers neither**.
Straight up is always powered.

### The torch -- `bg`

The only active element in the game, and a NOT gate: lit unless the block it hangs on is powered.

```java
boolean shouldBeOff = ...;                        // bg.h: is my support powered?
if (torchActive) {
    if (shouldBeOff) { setBlockAndMetadata(idle, meta); if (isBurnedOut(true)) fizz(); }
} else {
    if (!shouldBeOff && !isBurnedOut(false)) setBlockAndMetadata(active, meta);
}
```

`ly.aQ` is 75, the dark torch, and `ly.aR` is 76, the lit one -- the same adjacent-pair arrangement
the fluids use. Tick rate is **2**, and that delay is what every a1.1.2 circuit is timed by.

**Burnout**: a static list of (position, worldTime) entries, pruned to 100 ticks. Eight toggles at
one position inside that window and the torch stays dark. That is the brake on a torch wired to
itself. Note the asymmetry -- going *out* records a toggle, coming back on only reads the count.

A lit torch's indirect power reaches every side except the one it hangs by; its **direct** power
reaches only the block above it.

### The switches and the door

**What needs a player and what does not.** Pressing a button, flipping a lever and opening a door by
hand are *inputs*, and inputs need a player. Everything else about these blocks is reachable and
testable without one, by writing the metadata the input would have written:

| Block | Needs a player | Works and is tested now |
|---|---|---|
| Lever (`no`) | flipping it | which sides it powers when on, falling off a wall that goes away |
| Button (`hu`) | pressing it | the same, plus **letting itself back out** 20 ticks later |
| Pressure plate (`al`) | an entity standing on it | which sides it powers under load, falling off |
| Door (`fw`) | opening by hand | **opening and closing because a circuit told it to**, and both halves staying in step |

A lever's and a button's metadata are the same shape: the low three bits name the face it hangs on
(1 = -x, 2 = +x, 3 = -z, 4 = +z, 5 = the floor) and bit 3 is "on". `isIndirectlyProvidingPowerTo` is
just that bit; `isProvidingPowerTo` maps the face to the one side opposite it. A lever takes five
faces and a button four -- a lever can stand on the floor and a button cannot. Both drop when the
face their metadata *names* stops being an opaque cube, not merely when they run out of faces.

A pressure plate powers straight up directly and every side indirectly, whenever its metadata is
non-zero.

**The door's metadata**: bit 3 is "this is the upper half", bit 2 is "open". The upper half decides
nothing -- it checks that the lower half is still there and hands every power question down -- and
the lower half writes both, which is what keeps a door from rendering half open. It needs an opaque
cube beneath it, and losing either half destroys the other.

**A door only looks when a power source changed.** `onNeighborBlockChange` returns early unless the
block that changed answers `canProvidePower()`, which is what stops a doorway in an ordinary wall
from costing anything at all.

**Rails are skipped, deliberately.** `BlockMinecartTrack`'s only behaviour is recomputing its shape
from its neighbours, and without minecarts -- which are entities -- a rail's shape is not something
a player can act on. It is the one part of redstone with no payoff until entities exist.

## One surprising rule, pinned rather than smoothed over

`BlockLeaves.updateLeaves` (`iz.h(Lcn;III)V`) opens with

```java
int base = world.getBlockMaterial(x, y - 1, z).isSolid() ? 16 : 0;
```

and 16 is the same answer a log gives. **A leaf block resting on anything solid therefore records a
distance of 15 and never decays.** The metadata on a leaf is a countdown to a log -- 16 next to one,
one less per step, 1 when there is none in reach -- and only the ones that settle at 1 are removed.
The pass reads five neighbours: below, both z, both x, and deliberately **not** above.

It is odd enough to look like a transcription error, so `tests/tick_test.cpp` has a test whose whole
job is to say it is not one.

## Material predicates

`Material` (`gb`) has three no-argument booleans that block behaviour reads. Checked against a
running jar over every constructed block:

- `gb.c()` is `isSolid()` -- our `BlockDef::solid`.
- `gb.a()` returns **exactly the same value as `gb.c()` for every block a1.1.2 constructs**, so it
  needs no column of its own.
- `gb.d()` is `isLiquid()` -- true for the water and lava materials alone, which is exactly
  `RenderType::Fluid`.
- `gb.b()` is **`isSolid() || isLiquid()`** for every constructed block, and false for air. Grass
  reads it through the block above; `getPrecipitationHeight` reads it on the way down. It is derived
  in `tick::solidOrLiquid` rather than carried as a fourteenth boolean column.

`Block.isOpaqueCube()` (`ly.b()Z`) is our `BlockDef::opaque`; `Block.opaqueCubeLookup` is
`BlockDef::opaqueCube`, and the difference matters for leaves -- see the note on that field.

## Where this port differs, and why

| Deviation | Why |
|---|---|
| No dual-clock correction in the timer | The console has one clock. The ratio would be 1.0 for ever. |
| Random tick area is `min(9, loadRadius)` chunks | We cannot tick a column we do not hold. At the render distances a 3DS runs, ours is the smaller number, so a world simulates a little less far out than the original. |
| The tick never generates a chunk | Generation order **is** the world (see status.md §0g). A random tick allowed to trigger generation would reorder it, and a slower console would make a different world. A fluid stops at the frontier and resumes when the ground arrives, which is what the original does for a genuinely absent chunk. |
| Scheduled ticks are a fixed-capacity heap, not a `TreeSet` | The frame path may not allocate. Ordering and identity are the original's; the pool refuses the newest entry when full and counts it, and a neighbour notification re-schedules whatever was lost. |
| Sand and gravel land in one tick | There is no entity system, so `EntityFallingSand` has nowhere to live. The resting place is the one the entity would have found, so the world ends up identical and only the fall is missing. |
| Leaf decay's budget is threaded, not a field | The original bounds its recursive `updateLeaves` with `iz.c`, a counter on the single shared `Block` object -- `if (this.c++ >= 100) return;` -- reset at the entry points. The bound is copied exactly and is not optional: two adjacent leaf blocks can each decide the other needs re-running, and a 3DSX main thread has 32 KB of stack that nothing in the binary can enlarge. What is not copied is its being a field, which is an artefact of one shared Java object rather than a rule about leaves. |
| Light is repropagated incrementally, not by re-solving the column | `LightEngine` solves a whole column against a 3x3 window over a 576 KB working set, which is far more than a block change disturbs and more than a flowing fluid can afford. `world::LightUpdater` runs the standard removal-then-addition pair over the cells the change actually reached. lighting.hpp's own argument is why that is safe -- the update rule is a monotone fixed point with a strictly positive decrement, so there is one answer and any algorithm that finds it finds the same numbers -- and `tests/light_update_test.cpp` checks the two agree cell by cell. |
| Relighting is budgeted per frame, and its queues are bounded | The frame path may not allocate, and a roof coming off relights a lot of cells at once. `drain` settles a stated number and carries the rest; past the queue capacity entries are dropped and counted. A drop leaves a patch of world holding light one edit out of date, which is a wrong shade rather than a wrong world, and it is on the debug page. |
| A cascade of neighbour notifications is bounded by a stack budget | `writeBlock` calls `blockAdded`/`blockRemoved`, those call `notifyNeighbours`, and that dispatches `neighbourChanged` synchronously -- which can write another block. The original recurses with no guard; a 3DSX main thread has 32 KB of stack it cannot enlarge, and a fire field going out unwinds as one recursion as deep as the field is wide. Past `TickWorld::kCascadeStackBudget` the notification is **deferred to a queue and run before the tick ends**, not dropped, so the work still completes -- only its order past that depth differs. The budget is counted in measured bytes of ARM stack, not in levels, because a wire level and a notify level are not the same size. |

## Not yet ported

Named here rather than left to be discovered. Each is a subsystem, not a rule:

- **The inputs to redstone**: pressing a button, flipping a lever, an entity standing on a pressure
  plate, opening a door by hand. Every other part of those blocks is done; what is missing is a
  player and an entity to *be* the input. **Rails** are skipped for the same reason -- a rail's only
  behaviour is its shape, and shape means nothing without a minecart.
- **A sapling becoming a tree**: the counter and its conditions are ported; the last step runs
  `WorldGenTrees` or `WorldGenBigTree`, and both write through `PopulationView` rather than through
  a live world. It needs an adapter, not a call.
- **TNT**, **sponge**, and the tile-entity ticks (furnace, mob spawner).
