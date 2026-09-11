# The world tick, recovered from a1.1.2

Everything here was read out of `minecraft-a1.1.2_01-client.jar` -- by disassembling it with
`javap -c -p`, and, where a constructor branches, by loading the classes under a real JVM and asking
them. Obfuscated names are quoted so a claim can be re-checked; they are for **a1.1.2_01 only** and
mean nothing in another version.

The classes:

| Obfuscated | What it is |
|---|---|
| `World` | `World` |
| `Block` | `Block` |
| `ir` | `Timer` |
| `jf` | `NextTickListEntry` |
| `Material` | `Material` |
| `ga` | `Chunk` |

## The clock -- `ir`

The timer reads both the system clock and a high-resolution clock. Whenever the system clock has
moved more than a second since the last sync, the stored adjustment is moved 20% of the way
(a float literal widened to double) towards the ratio of the two clocks' elapsed time, and both
reference points are updated. If the system clock has gone backwards, both reference points are
reset. The elapsed time since the previous call is then taken from the high-resolution clock in
seconds, scaled by the adjustment and clamped to the range 0 to 1 second before any further scaling.

That elapsed time, multiplied by the timer speed and 20 ticks per second, is added to the partial
tick count. The whole ticks are the integer part; that integer is **subtracted first**, so the
fraction stays; and only then is the number of whole ticks clamped to 10, so any surplus is
discarded rather than carried. The fraction left is the render partial tick.

Three things are load-bearing: the one-second clamp on the raw delta, the truncate-subtract-**then**-
clamp order (so ticks past the tenth are *dropped*, and the world falls behind rather than
spiralling), and the surviving fraction, which is what keeps a day 20 minutes long at any frame rate.

`core/tick/tick_timer.hpp` reproduces all three and **omits the dual-clock correction**: a 3DS has
one clock, `svcGetSystemTick` off a fixed oscillator, so there is nothing to correct against and the
ratio would be exactly 1.0 for ever.

## `World.tick` -- `World`'s method

The order inside one world tick: unload any queued chunks; recompute the sky-light
subtraction for full sun and, if it changed, store it and tell every world listener to refresh
all renderers; increment the world time; autosave when the time is an exact multiple of the
autosave interval; run the scheduled block updates (not run-all); run the random block ticks.

The sky-light subtraction is recomputed **before** the clock advances and every light query for the
rest of the tick reads it, so the time of day is an input to block behaviour and not only to the
lightmap: grass, mushrooms, crops and saplings all read `getBlockLightValue`.

## Scheduled updates -- `World`'s method and `World`'s method

Scheduling builds an entry for the cell and block. Only if every chunk within 8 cells of it on
each axis exists does it do anything: the entry's due time is set to the block's own tick rate
plus the current world time (for a real block), and if the entry is not already in the scheduled
set it is added to both the set and the ordered pending list.

Running the queue first checks that the list and the set have the same size, and treats any
difference as a fatal error. It handles at most 1000 entries per call. Each time it takes the
earliest entry and, unless it was asked to run everything, stops at the first one due in the
future. A handled entry is removed from both collections. Only if the chunks within 8 cells of it
all exist does it look at the block in that cell, and only if that block still has the entry's
id and is not air does it call its update with the world random. It returns whether anything is
still pending.

`NextTickListEntry` (`jf`) is equal on `(x, y, z, blockID)` and **orders on `(scheduledTime,
insertion sequence)`** -- a method of another original class compares `e` then the private `g`, a static counter bumped in
the constructor. The insertion tie-break is not decoration: it is what makes water spread in a stable
pattern rather than a different one every run.

Two numbers are the original's and are kept: the **8-block residency box** around anything scheduled
or run, and the **1,000-per-tick** cap.

## Writing a block is three things, not one -- a method of another original class

`Chunk.setBlockID` is where a block actually changes, and it does more than store a number. In
order:

The order matters. If the new id equals the old one, nothing happens and the call reports
failure. Otherwise the id is stored; then, if there was a previous block, its removal hook
runs; then the metadata nibble is **cleared to zero**; then the height map is updated and sky and
block light are recomputed over the single column; then, if the new id is not air, its
placed-in-world hook runs; and finally the chunk is marked modified.

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

## Neighbour notification -- `World`'s method

Six neighbours, in this order, and the order is observable:

```
x-1, x+1, y-1, y+1, z-1, z+1
```

Each becomes `Block.onNeighborBlockChange(world, x, y, z, changedId)`, skipped entirely while
`World.editingBlocks` or `World.isRemote` is set. `setBlockWithNotify` (`World`'s method) is the write
plus `markBlockNeedsUpdate` plus that fan-out.

## Random ticks -- `World`'s method

The part that costs, transcribed:

Each tick the active set is cleared and refilled: for every player, every chunk within 9 chunks
on both axes of the chunk containing them (the position divided by 16 and floored). The ambient
sound counter counts down if positive.

Then, for every active chunk: the ambient cave sound takes one sampled position when the counter
is zero (`TickWorld::ambientCave`). If the chunk's snow condition holds and a one-in-four draw
from the world random succeeds, the update LCG is advanced (multiplied by 3 and offset by
1013904223), shifted right by 2, and the low four bits and bits 8 to 11 pick a column. At that
column's precipitation height, if it lies from 0 to 127 and the block light there is under 10,
snow is placed in an empty cell where snow can be placed, and ice replaces a still-water cell
below it whose metadata is zero.

Then the chunk gets **80** random block ticks: each advances the same LCG and shifts it right by
2, takes x from the low four bits, z from bits 8 to 11 and y from bits 16 to 22, reads that
block, and, if the block has its tick-on-load flag, calls its update with world coordinates and
the world random.

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

## The fluids -- `BlockFluid`, `BlockFlowing`, `hn`

The largest single behaviour in the game, and three classes: `BlockFluid` (BlockFluid, the shared half),
`BlockFlowing` (BlockFlowing, which ticks) and `hn` (BlockStationary, which does not).

**The pair.** Every fluid is two blocks registered adjacently -- `flowing = still - 1`, and a1.1.2
really does write `blockID + 1` and `blockID - 1`. A flowing block that finds nothing to change
becomes the still block and stops costing anything (`BlockFlowing`'s field, setStatic); a still block that hears a
neighbour change becomes flowing again and schedules itself (a field of another original class, setNotStationary). Neither
notifies its neighbours, and that is not an omission -- notifying would wake the neighbours, which
would set *them* not-static, which would notify back. **This pair is why an ocean is free and a
waterfall is not**, and it is the single most important thing to keep intact on a console.

**Metadata is the level**: 0 at a source, rising by `this.d` per block -- **1 for water, 2 for lava**,
set in `BlockFluid`'s constructor -- and bit 3 (a value of 8 or more) marks fluid that is falling rather than
spreading. Level 8+ becomes 1 when it lands, which is why a waterfall spreads at full strength.

`BlockFlowing`'s method, transcribed:

The cell reads its flow level and starts out assuming it will settle. A level above zero is not
a source and is recomputed: the smallest neighbouring decay over the four horizontal cells is
found (counting adjacent sources on the way), the candidate level is that plus the fluid's flow
step, and it becomes "none" if it reaches 8 or the neighbours offered nothing. **Fluid in the
cell above wins outright**: the candidate becomes that level if it is already 8 or more, and that
level plus 8 otherwise. Water with at least two adjacent sources becomes a source itself if the
cell below is a solid cube, or is water with this cell at level zero. That is the infinite-water
rule. Lava that would drop to a level above its current one, within the flowing range, instead
keeps its level with a three-in-four chance and does not settle.

If the level changed, a "none" result removes the block; otherwise the new level is written, a
tick is scheduled and neighbours are notified. If it did not change and the cell may settle, it
becomes static. A source has nothing to recompute and simply goes static.

Then it flows, **down first and exclusively**: if it can flow into the cell below, that cell gets
this fluid at the current level if 8 or more, else the level plus 8. Otherwise, if the level is
non-negative and either zero or the cell below blocks flow, it works out the best directions,
gives the outgoing level as 1 for a falling cell or the level plus the flow step otherwise,
stops if that reaches 8, and flows into each of the four directions that were chosen.

Three consequences worth naming, because each is a thing players notice:

- **Down before sideways, and exclusively.** A fluid that can fall does not spread at all that tick.
- **Two sources make a third**, over an opaque cube or over more of the same source. That is
  infinite water, and it is water only -- the `Material.water` test in the middle of it.
- **Lava only thins on one roll in four.** Its slowness is *not* only its tick rate of 30; this
  extra roll is on top of it.

**Which way it spreads is a search, not a fan-out.** `getOptimalFlowDirections` (`BlockFlowing`'s field) looks up to
four blocks along each of the four horizontal directions for somewhere the fluid could fall, using
`calculateFlowCost` (`BlockFlowing`'s field, recursive, depth-limited at 4, never doubling back -- direction 0 is
-x, 1 is +x, 2 is -z, 3 is +z, and the opposite pairs are how it knows), and spreads only along the
directions **tied** for the shortest path. That search is why water finds a hole across the room
rather than creeping outwards evenly, and it is the part of the module with a cost worth measuring.

**What stops a fluid** (`BlockFlowing`'s field, blocksFlow) is any solid material *plus* five blocks named one by
one: both doors, a sign post, a ladder and sugar cane. **What it may enter** (`BlockFlowing`'s field, canFlowInto)
is anything that does not block flow, is not its own kind, and is not lava.

**Lava meeting water** is `BlockFluid`'s field (checkForHarden), reached from `onBlockAdded` and
`onNeighborBlockChange`: water beside or above -- five directions, **not** below -- turns a lava
source into obsidian and any thinner lava into cobblestone.

**One part is deliberately not wired.** `hn`'s updateTick is lava setting fire to what is above it:
`rand.nextInt(3)` steps, each wandering one block in x and z and climbing one in y, looking for air
with something burnable beside it. `TickBehaviour::Fire` has no implementation yet, so placing fire
would make a block that never spreads and never goes out -- worse than no fire. The search is left
out rather than half-run, and turning it on is one call once `BlockFire` lands.

## Fire -- `BlockFire`

**Metadata is age, 0 to 15**, climbing by one per scheduled tick at a tick rate of 10. Fire does not
die of old age; it stops counting at 15 and burns as long as it is fed.

Each update reads the fire's age from its metadata and, below 15, increments it and reschedules
the next tick. If no neighbour can burn, the fire is removed when the block under it is not an
opaque cube or its age exceeds 3, and the update ends. If the block under it cannot catch fire,
its age is 15 and a one-in-four draw succeeds, it is removed and the update ends.

On even ages above 2 it then tries to ignite its six neighbours, each with its own resistance
number (a lower number is likelier): 300 for east, west, north and south, 250 above, and **200
below, the likeliest**. After that it sweeps the 3 by 3 columns around it from one cell below to
four above, skipping its own cell. The bound for each cell starts at 100 and grows by 100 for each
level above one over the fire. Where the neighbours' encouragement for that cell is positive and a
draw below the bound comes out at most that value, a new fire is placed there.

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

**Where fire may exist at all** (`BlockFire`'s method) is: an opaque cube directly below it, **or**
anything burnable among its six neighbours. `onBlockAdded` and `onNeighborBlockChange` both re-ask
it and remove the fire when the answer is no.

**Lava's ignition** (a method of another original class) is now wired, and it was the one piece of
the fluids left out while fire did not exist: `rand.nextInt(3)` steps, each wandering one block in x
and z and climbing one in y, stopping at the first solid block, and placing fire in the first air
cell that has a `canBurn` neighbour.

## Redstone -- the power model, `kf` and `bg`

**Four questions, and a1.1.2 needs all four.** `side` names the face of the *asking* block that the
answer arrives through, numbered 0 = -y, 1 = +y, 2 = -z, 3 = +z, 4 = -x, 5 = +x. A circuit built on
the wrong numbering works in some directions and not others.

Direct power looks up the block at the cell; air provides none, and anything else is asked for
its power on the given side. Indirect power differs in one rule: **opaque cubes conduct**. If the
cell is an opaque cube, the answer is whether that cell is receiving power from anywhere;
otherwise the block is asked for its indirect power on that side.

The line marked is the one everything rests on: **an opaque cube answers with whatever is powering
it**, which is why a torch under a block powers what stands on top of it, and why the indirect query
cannot be a wrapper round the direct one.

### The wire -- `kf`

Strength is the metadata, 0 to 15. `updateAndPropagateCurrentStrength` (a field of another original class) recomputes one
cell's strength and walks outwards from any change:

The wire reads its current metadata, then asks whether the cell is indirectly receiving power,
with the "wires provide power" flag **switched off for that one query and back on after it**;
otherwise every wire would read the maximum from its neighbours. If it is powered, strength is
15. If not, for each of the four horizontal neighbours it takes the larger of the running
strength and that neighbour's current strength. When the neighbour is an opaque cube and the cell
above this wire is not, it also considers the cell above the neighbour (a climb); when the
neighbour is not opaque it considers the cell below the neighbour (a descent). The result is
reduced by one, floored at zero.

If the strength changed, the new value is written, **but the value then carried forward is the
written value minus one (floored at zero), which is what neighbours are compared against, not
the value that was written**. Each of the four neighbours is re-run, along with the cell above it
if it is solid or below it if it is not, whenever that cell is a wire whose strength differs.
When the old or new strength was zero, all seven cells (the wire and its six neighbours) are
notified.

Three details are easy to lose and each breaks something specific:

- **`wiresProvidePower` is turned off for exactly one call.** Without it a wire counts itself and
  its neighbouring wire as sources and the whole net reads 15.
- **The second decrement happens after the write**, and the propagation compares against that
  smaller value. Using the written one re-walks the whole net at every step.
- **Only the edges notify.** A wire going on or off wakes what it touches; a wire merely changing
  strength does not, which is what makes a long run cheap.

**Fifteen blocks is not a rule anywhere in the code.** It is what falls out of starting at 15 and
losing one per block.

a method of another original class decides which sides a lit wire actually powers, and it works the shape out from
what is around it because a1.1.2 stores no shape in the metadata: a wire connected to nothing is a
dot and powers all four sides; a straight run powers only its two ends; **a corner powers neither**.
Straight up is always powered.

### The torch -- `bg`

The only active element in the game, and a NOT gate: lit unless the block it hangs on is powered.

The torch first asks whether its support is powered, which decides whether it should be off.
A lit torch that should be off switches to the unlit block, keeping its metadata, and plays the
fizz if the burn-out check (run in recording mode) says it has burnt out. An unlit torch that
should not be off, and whose burn-out check (not recording) passes, switches back to the lit
block.

`Block`'s field is 75, the dark torch, and `Block`'s field is 76, the lit one -- the same adjacent-pair arrangement
the fluids use. Tick rate is **2**, and that delay is what every a1.1.2 circuit is timed by.

**Burnout**: a static list of (position, worldTime) entries, pruned to 100 ticks. Eight toggles at
one position inside that window and the torch stays dark. That is the brake on a torch wired to
itself. Note the asymmetry -- going *out* records a toggle, coming back on only reads the count.

A lit torch's indirect power reaches every side except the one it hangs by; its **direct** power
reaches only the block above it.

### The switches and the door

**What needs a player and what does not.** These four were built from the circuit end first, because
everything except the input is reachable without one -- write the metadata the input would have
written and the rest of the block is testable. **All four inputs now exist.**

| Block | The input | State |
|---|---|---|
| Lever (`BlockLever`) | flipping it | **done** -- `leverActivated`, reached from a right-click |
| Button (`hu`) | pressing it | **done** -- `buttonActivated`, which also schedules the release 20 ticks later |
| Pressure plate (`al`) | an entity standing on it | **done** -- `pressurePlateSense`, off `TickWorld::anyEntityIn` |
| Door (`BlockDoor`) | opening by hand | **done** -- `doorActivated`, from either half; an iron door refuses |

**The plate is the only behaviour that asks the world about entities**, so it is the only one with a
seam of its own. a method of another original class looks in a box an eighth in on the four sides and a quarter of a
block tall, arms on a method of another original class (onEntityCollidedWithBlock, and only while it is up),
disarms on its own scheduled update at rate 20 (and only while it is down), and re-books that update
for as long as something is standing there. It writes metadata with `setBlockMetadata`, which
notifies nothing, and then notifies its own neighbours and the block *below* itself -- the two calls
are the plate's, not the write's.

`TickWorld::setEntityQuery` is how it reaches them, and it is deliberately **not** on `TickAccess`:
that structure is how the tick reaches chunks and is built by whatever owns them, while nothing owns
the player and the dropped items together except the frame loop. Unset means "there are no
entities", which is the right answer for every headless tool here.

**The two plates are two tick behaviours**, `pressure_plate_all` (block 72, wooden) and
`pressure_plate_mobs` (block 70, stone), because `al` takes a `js` -- EnumMobType -- as its third
constructor argument and the jar passes a different one to each. The observable difference is that a
**dropped item presses the wooden plate and not the stone one**. a field of another original class ("players") is modelled and
no block in this version takes it.

The scan that arms it is `tick::entityCollidedWithBlocks`, which is the tail of `Entity`'s method
(moveEntity): every block whose cell the entity's box overlaps, with the original's **inclusive**
floor bounds rather than the half-open range every other box walk here uses.

The circuit halves were done first and are unchanged: which sides each powers, falling off a wall
that goes away, the button letting itself back out, and a door opening because a circuit told it to
with both halves in step.

**The input is `Block.blockActivated`, and it arrives through `tick::blockActivated`** --
dispatched on the behaviour column like everything else here, and called from `item::rightClick`,
which is the port of `PlayerController.onPlayerRightClick`. The order in that method is the rule
worth remembering: **the block is asked before the item**, with no sneak override, so a lever flicks
rather than disappearing under the block you were holding. See core/tick/behaviour.hpp.

A lever's and a button's metadata are the same shape: the low three bits name the face it hangs on
(1 = -x, 2 = +x, 3 = -z, 4 = +z, 5 = the floor) and bit 3 is "on". `isIndirectlyProvidingPowerTo` is
just that bit; `isProvidingPowerTo` maps the face to the one side opposite it. A lever takes five
faces and a button four -- a lever can stand on the floor and a button cannot. Both drop when the
face their metadata *names* stops being an opaque cube, not merely when they run out of faces.

**A lever has a sixth value, and it behaves differently from the other five.** `BlockLever.
onBlockAdded` writes `5 + rand.nextInt(2)` for a lever on the floor, so a floor lever is 5 **or 6**,
and 6 is which way round the handle lies. Two methods treat it as a value they have never heard of,
and both are the class file's own:

- `onNeighborBlockChange` sets its drop flag from a chain of five `meta == n` tests, `n` from 1 to
  5. A 6 is named nowhere, so a lever wearing it never drops for its face -- only for
  `canPlaceBlockAt` running out of walls. Answering "unsupported" for 6 here deleted floor levers
  the moment they were flicked, because a flick notifies the lever's own cell.
- `isProvidingPowerTo` runs the same five tests, so **a floor lever at orientation 6 hands no direct
  power to the block it stands on**. It still powers wire beside it: that is the indirect answer,
  which only reads bit 3.

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

## Rails -- `if` and `RailLogic`

**The largest single behaviour in a1.1.2's block set**, and none of it is a placement decision:
`Block.onBlockPlaced` answers 0 for a rail, which is why `data/a1.1.2/placement.json` carries six
zeroes on that row. The shape is worked out afterwards, from the neighbours, by `RailLogic` -- RailLogic --
which `core/tick/rail.{hpp,cpp}` transcribes whole.

The ten shapes, and they are the same numbering the mesher draws from:

| metadata | shape |
|---|---|
| 0 | flat, along z |
| 1 | flat, along x |
| 2 | ascending towards **+x** |
| 3 | ascending towards **-x** |
| 4 | ascending towards **-z** |
| 5 | ascending towards **+z** |
| 6 | curve joining +x and +z |
| 7 | curve joining -x and +z |
| 8 | curve joining -x and -z |
| 9 | curve joining +x and -z |

`setBasicRail` maps each of them to the **two cells it joins**, and everything else is built on that
list:

```
0: (x, y, z-1) (x, y, z+1)      5: (x, y, z-1)   (x, y+1, z+1)
1: (x-1, y, z) (x+1, y, z)      6: (x+1, y, z)   (x, y, z+1)
2: (x-1, y, z) (x+1, y+1, z)    7: (x-1, y, z)   (x, y, z+1)
3: (x-1, y+1, z) (x+1, y, z)    8: (x-1, y, z)   (x, y, z-1)
4: (x, y+1, z-1) (x, y, z+1)    9: (x+1, y, z)   (x, y, z-1)
```

`refreshTrackShape(powered)` asks four questions -- is there a rail I can join at each side, counting
one block up and one down -- and reads the answers through a table:

The shape starts as undecided (-1) and is decided from which of the four neighbours connect.
A connection on the z axis with none on x gives shape 0, and the reverse gives shape 1. With
exactly two perpendicular connections and nothing else: +z and +x give 6, +z and -x give 7, -z
and -x give 8, and -z and +x give 9.

If that left it undecided, any z connection gives 0 and any x connection gives 1, and the four
corners are tried again, in one order if the rail is powered and the reverse order if not. Then
an ascending neighbour is checked: for shape 0, a rail one up at -z gives 4 and one up at +z
gives 5; for shape 1, a rail one up at +x gives 2 and one up at -x gives 3. A shape still
negative at the end becomes 0.

Five things in it are worth knowing before reading the code:

- **A freshly placed rail is written with metadata 15 first.** `BlockMinecartTrack`'s field does that before it refreshes,
  and 15 matches none of `setBasicRail`'s ten cases -- so the connection list is empty. It means "I
  have no previous shape to be biased by", spelled as a metadata value because there is nowhere else
  to put it.
- **`powered` only reorders the four corner preferences**, so the *last* match wins differently and
  a T-junction points the other way. That is a1.1.2's rail switch, two years before powered rails,
  and it is the only thing `isBlockIndirectlyGettingPowered` is asked for here.
- **`connectTo` re-derives the neighbour's shape from its remembered list, not from the world.**
  That is the whole difference between it and `refreshTrackShape`, and it is what stops the two ends
  of a new join disagreeing.
- **`canConnectFrom`'s tail is dead code**: the class file compares two heights and returns true on
  both paths, so the whole method is "yes unless I am already full".
- **`onNeighborBlockChange` drops the rail** when the block under it stops being an opaque cube --
  and, for an ascending rail, when the block under the neighbour it *climbs towards* does. Only a
  block that `canProvidePower` re-points a junction, and only one with exactly three adjacent
  tracks.

## One surprising rule, pinned rather than smoothed over

`BlockLeaves.updateLeaves` (a method of another original class) opens with

The base offset is 16 when the material one cell down is solid, and 0 otherwise.

and 16 is the same answer a log gives. **A leaf block resting on anything solid therefore records a
distance of 15 and never decays.** The metadata on a leaf is a countdown to a log -- 16 next to one,
one less per step, 1 when there is none in reach -- and only the ones that settle at 1 are removed.
The pass reads five neighbours: below, both z, both x, and deliberately **not** above.

It is odd enough to look like a transcription error, so `tests/tick_test.cpp` has a test whose whole
job is to say it is not one.

## Material predicates

`Material` (`Material`) has three no-argument booleans that block behaviour reads. Checked against a
running jar over every constructed block:

- `Material`'s method is `isSolid()` -- our `BlockDef::solid`.
- `Material`'s method returns **exactly the same value as `Material`'s method for every block a1.1.2 constructs**, so it
  needs no column of its own.
- `Material`'s method is `isLiquid()` -- true for the water and lava materials alone, which is exactly
  `RenderType::Fluid`.
- `Material`'s method is **`isSolid() || isLiquid()`** for every constructed block, and false for air. Grass
  reads it through the block above; `getPrecipitationHeight` reads it on the way down. It is derived
  in `tick::solidOrLiquid` rather than carried as a fourteenth boolean column.

`Block.isOpaqueCube()` (`Block`'s method) is our `BlockDef::opaque`; `Block.opaqueCubeLookup` is
`BlockDef::opaqueCube`, and the difference matters for leaves -- see the note on that field.

## Where this port differs, and why

| Deviation | Why |
|---|---|
| No dual-clock correction in the timer | The console has one clock. The ratio would be 1.0 for ever. |
| Random tick area is `min(9, loadRadius)` chunks | We cannot tick a column we do not hold. At the render distances a 3DS runs, ours is the smaller number, so a world simulates a little less far out than the original. |
| The tick never generates a chunk | Generation order **is** the world (see status.md §0g). A random tick allowed to trigger generation would reorder it, and a slower console would make a different world. A fluid stops at the frontier and resumes when the ground arrives, which is what the original does for a genuinely absent chunk. |
| Scheduled ticks are a fixed-capacity heap, not a `TreeSet` | The frame path may not allocate. Ordering and identity are the original's; the pool refuses the newest entry when full and counts it, and a neighbour notification re-schedules whatever was lost. |
| Sand and gravel land in one tick **when nobody is watching** | Which is `BlockSand.fallInstantly`, a real static on the class file, true while a chunk is populated. Here the flag is "is there an entity pool to spawn into": world generation and the headless tools have none and take the instant path, the frame loop has one and gets `EntityFallingSand`. The resting place is the same either way. See `core/entity/falling_block.hpp`. |
| A block behaviour reaches entities, drops and sound through three function-pointer seams | `core/tick/` may not know about the entity pools or the mixer: the tick runs on a worker, the mixer does not, and `TickAccess` is how the tick reaches *chunks* and is built by the thing that owns them. `setEntityQuery`, `setDropSink`, `setFallingBlockSink` and `setSoundSink` are set by the frame loop instead. **Unset is a real answer in every case** -- no entities, no pool, silence -- and the random draws still happen either way, so a world ticked headless takes the same random path as one ticked with a player in it. |
| Leaf decay's budget is threaded, not a field | The original bounds its recursive `updateLeaves` with a field of another original class, a counter on the single shared `Block` object -- `if (this.c++ >= 100) return;` -- reset at the entry points. The bound is copied exactly and is not optional: two adjacent leaf blocks can each decide the other needs re-running, and a 3DSX main thread has 32 KB of stack that nothing in the binary can enlarge. What is not copied is its being a field, which is an artefact of one shared Java object rather than a rule about leaves. |
| Light is repropagated incrementally, not by re-solving the column | `LightEngine` solves a whole column against a 3x3 window over a 576 KB working set, which is far more than a block change disturbs and more than a flowing fluid can afford. `world::LightUpdater` runs the standard removal-then-addition pair over the cells the change actually reached. lighting.hpp's own argument is why that is safe -- the update rule is a monotone fixed point with a strictly positive decrement, so there is one answer and any algorithm that finds it finds the same numbers -- and `tests/light_update_test.cpp` checks the two agree cell by cell. |
| Relighting is budgeted per frame, and its queues are bounded | The frame path may not allocate, and a roof coming off relights a lot of cells at once. `drain` settles a stated number and carries the rest; past the queue capacity entries are dropped and counted. A drop leaves a patch of world holding light one edit out of date, which is a wrong shade rather than a wrong world, and it is on the debug page. |
| A cascade of neighbour notifications is bounded by a stack budget | `writeBlock` calls `blockAdded`/`blockRemoved`, those call `notifyNeighbours`, and that dispatches `neighbourChanged` synchronously -- which can write another block. The original recurses with no guard; a 3DSX main thread has 32 KB of stack it cannot enlarge, and a fire field going out unwinds as one recursion as deep as the field is wide. Past `TickWorld::kCascadeStackBudget` the notification is **deferred to a queue and run before the tick ends**, not dropped, so the work still completes -- only its order past that depth differs. The budget is counted in measured bytes of ARM stack, not in levels, because a wire level and a notify level are not the same size. |

## Not yet ported

Named here rather than left to be discovered. Each is a subsystem, not a rule:

- **A sapling becoming a tree**: the counter and its conditions are ported; the last step runs
  `WorldGenTrees` or `WorldGenBigTree`, and both write through `PopulationView` rather than through
  a live world. It needs an adapter, not a call.
- **TNT** and the tile-entity ticks (furnace, mob spawner). **Not sponge**: `BlockSponge`'s field walks a 5x5x5 box
  comparing each cell's material against water and the body of that comparison is *empty* in this
  version -- the absorption is Classic's and then 1.8's. Its `onBlockRemoval`, which notifies the
  same box, is ported.
