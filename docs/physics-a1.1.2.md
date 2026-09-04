# The player body in a1.1.2

Everything here was read out of the client jar with `javap -p -c`, or measured by running it. Class
and field names are the obfuscated ones, given so the derivation can be re-run and argued with. The
collision half is implemented in `src/core/block/collision.hpp` and the movement half in
`src/core/entity/player_body.{hpp,cpp}`, checked tick-for-tick against a real `EntityPlayer` by
`tests/player_body_test.cpp`. One section at the end is **not** derived and says so: Creative flight
is ours.

Nothing in this document was recalled. Where a constant looks like a familiar round number, the
value actually in the class file is given instead — several of them are not round.

## The classes

| Obfuscated | What it is |
|---|---|
| `Entity` | `Entity` |
| `EntityLiving` | `EntityLiving` (extends `Entity`) |
| `EntityPlayer` | `EntityPlayer` (extends `EntityLiving`) |
| `cf` | `AxisAlignedBB` |
| `World` | `World` |
| `Block` | `Block` |
| `eo` | `MathHelper` |

`EntityPlayer` overrides neither `moveEntity` nor `moveEntityWithHeading`, so the player's movement is
`EntityLiving`'s and nothing else.

## Fields

On `Entity`:

| Field | Meaning |
|---|---|
| `ak` `al` `am` | `posX` `posY` `posZ` |
| `an` `ao` `ap` | `motionX` `motionY` `motionZ` |
| `aq` | `rotationYaw`, in **degrees** |
| `au` | `boundingBox` |
| `ItemBlock` `aw` `ax` `ay` | `onGround`, `isCollidedHorizontally`, `isCollidedVertically`, `isCollided` |
| `SpawnerAnimals` | the "cancel the whole move" flag — see below |
| `aB` | `yOffset` |
| `aC` `aD` | `width`, `height` |
| `aH` | `fallDistance` |
| `aL` | `ySize` |
| `aM` | `stepHeight` |
| `aN` | `noClip` |

On `cf`: `a b c` = `minX minY minZ`, `d e f` = `maxX maxY maxZ`, all `double`.

On `Block`: `bf..bk` = the block's own `minX..maxZ`; `bo` = `slipperiness`.

## **`posY` is the eye, not the feet**

`EntityPlayer`'s constructor sets `yOffset = 1.62f`, and `Entity.setPosition` sets

```
boundingBox.minY = posY - yOffset + ySize
```

so `posY` sits 1.62 above the feet. **`level.dat`'s `Pos[1]` is therefore an eye height**, and the
real world confirms it: the reference save reads `Pos = [252.903…, 70.62000000476837, 218.822…]`,
whose `.62` is the giveaway — feet at 69, eye at 70.62.

That has a consequence for the code as it stands. `WorldStreamer::spawnPosition`
(`src/core/render/world_streamer.cpp:737`) returns `level.dat`'s `Pos` **verbatim** when a player is
present, and `src/platform/ctr/main.cpp:561` then adds 1.62 to it. For a fresh world that is right —
the fallback path returns `spawnY`, which is a block coordinate. For a world with a saved player it
is wrong, and because `setPlayerState` writes the camera's `y` straight back out, **the player rises
1.62 blocks on every save-and-reload cycle**. A player body that owns the feet and derives both the
eye and `Pos[1]` from them closes this; until then the two paths disagree about what they return.

The same save also carries `Motion = [9.57e-67, -0.0784000015258789, 3.22e-67]`, and
`-0.0784000015258789` is exactly `-0.08 × 0.98` — the gravity and drag constants below, caught in a
real file.

## Constants, as they appear in the class file

| Constant | Value in the jar | Where |
|---|---|---|
| Player size | `0.6f` × `1.8f` | `EntityPlayer` ctor, `setSize` |
| `yOffset` (eye) | `1.62f` | `EntityPlayer` ctor |
| — and these four are **floats**, so they widen to 1.6200000047683716, 0.60000002384185791 and 1.7999999523162842, **not** to 1.62, 0.6 and 1.8 | | |
| `stepHeight` | `0.5f` | added to `ySize` on a successful step |
| Gravity | `0.08d` | `EntityLiving`'s method |
| Vertical drag | `0.9800000190734863d` | `EntityLiving`'s method — this is `0.98f` widened, **not** `0.98` |
| Jump impulse | `0.41999998688697815d` | `EntityLiving`'s method — `0.42f` widened |
| Air friction | `0.91f` | `EntityLiving`'s method |
| Ground friction base | `0.54600006f` | `EntityLiving`'s method — used when the block below is air |
| Acceleration normaliser | `0.16277136f` | `EntityLiving`'s method |
| Ground acceleration | `0.1f` | `EntityLiving`'s method |
| Air acceleration | `0.02f` | `EntityLiving`'s method |
| Sneak probe step | `0.05d` | `Entity`'s method |
| `ySize` decay | `0.4f` per move | end of `Entity`'s method |
| Pi, for heading | `3.1415927f` | `Entity`'s method — the float literal, not a double pi |
| Block slipperiness | `0.6f`, and `0.98f` for ice alone | `Block` ctor; `he` (id 79) |

`0.54600006f` is `0.6 × 0.91` as a float, and the two are **not** interchangeable — the jar stores
the product, so computing it at runtime can differ in the last bit.

## `SpawnerAnimals` is always true, so two branches are dead

`moveEntity` has three copies of

```
if (!az && originalDelta != clippedDelta) { dx = dy = dz = 0; }
```

`SpawnerAnimals` is assigned exactly once in the whole jar — `iconst_1` in the `Entity` constructor — and no
subclass touches it. For a player it is therefore permanently true and all three branches are
unreachable. Worth knowing before someone reimplements them and wonders why nothing exercises them.

## `MathHelper` is shared, and its sine table is lossy on purpose

`moveFlying` takes its sine and cosine from `eo`'s 65,536-entry table, the **same table the cave
carver uses** — argument scaled by `10430.378f` and truncated, so the result is a quantised sine.
Our implementation of it already exists and is hash-checked against a real JVM
(`src/impl/worldgen/alpha_nobiome/math_helper.hpp`, `tests/sin_table_test.cpp`).

It is in a **worldgen slot**, which `src/core/entity/` may not include. Moving it to
`src/core/util/` is a prerequisite for the movement code — it is `a1.1.2`'s `MathHelper`, not
anything to do with terrain, and two unrelated subsystems now want it.

## `Entity.moveEntity` — `Entity`'s method

**The move, in order.** A no-clip entity offsets its box by the requested movement, syncs its
position from the box and returns. Otherwise the requested movement and the starting box are
remembered.

**Sneaking at an edge.** On the ground and sneaking, each horizontal component is reduced towards
zero in steps of 0.05, one axis at a time, for as long as the box offset by that component and
dropped one block would collide with nothing; a component within 0.05 of zero is set to zero, and
the remembered original is updated at each step. a1.1.2 has these two single-axis loops only; the
combined loop is a later addition.

**Collision.** One query fetches every box overlapping the box extended by the movement, and the
three axes all reuse it, in the order y, x, z: each axis shrinks its component against every box
and then moves the box. Whether the move can step up is decided after the y pass: the entity is
on the ground, or its original vertical movement was downward and was changed.

**Stepping up.** If the step height is above zero, a step is possible, the camera smoothing
offset is under 0.05, and horizontal movement was blocked on x or z, the flat result is saved and
the move is retried from the starting box with the original x and z and a vertical movement equal
to the step height, over the same three axes. If the flat result covered at least as much
horizontal distance (squared) as the retry, the flat result is restored. Otherwise the retry is
kept and the smoothing offset grows by 0.5.

**After the move.** Position comes from the box: x and z from its centre and y from its bottom
plus the model offset minus the smoothing offset. Collided horizontally means x or z was changed
by collision; collided vertically means y was; on ground means y was changed while the original
was downward. On the ground a positive fall distance is applied and cleared; in the air a
downward move adds to it. Any axis whose movement was changed has its motion zeroed. Then come
the walked distance and the footstep (step sound and walked-on hook, both on the one cell
underfoot), the collided-with-block hook for every cell the box overlaps, and the burn check.
Last, the smoothing offset is multiplied by 0.4, unconditionally.

Note that the collision list is gathered **once**, from the box swept along all three axes at once,
and then reused for each axis in turn. Re-querying per axis would be a different game.

## `EntityLiving.moveEntityWithHeading` — `EntityLiving`'s method

Three branches. Water and lava first, land last:

**In water**, it remembers its height, applies flying input at 0.02, moves, multiplies all three
motion components by 0.8 (a float widened to double), and subtracts 0.02 from vertical motion. If
it hit something horizontally and the point offset by its motion, with height 0.6 above the
remembered height, is in liquid, vertical motion becomes 0.3. **In lava** it is the same with
factors of 0.5.

**Otherwise** friction starts at 0.91. On the ground it is 0.54600006, and if there is a block
under the feet it is that block's slipperiness times 0.91. Acceleration is 0.16277136 divided by
friction cubed. Flying input is applied with 0.1 times acceleration on the ground and 0.02 in the
air. Friction is then recomputed by the same lookup, because the flying call can change the
ground state. The ladder clamp follows, the move runs, and the ladder climb check follows it.
Vertical motion then loses 0.08, is multiplied by 0.98 (the double literal
0.9800000190734863), and both horizontal components are multiplied by the friction.

Two details that are easy to get wrong: the friction lookup uses **`boundingBox.minY - 1`**, not
`posY`, and it happens **twice** — once to size the acceleration and once to damp the result.

## `Entity.moveFlying` — `Entity`'s method

The input vector's length is taken; below 0.01 nothing happens, and below 1.0 it is raised to
1.0. The speed is divided by that length and applied to both inputs. With the yaw converted to
radians using the float pi (3.1415927 over 180), horizontal motion gains strafe times cosine minus
forward times sine on x, and forward times cosine plus strafe times sine on z.

`rotationYaw` is degrees. Note the input magnitude is clamped **up** to 1, so pushing a stick half
way gives half speed but pushing two axes fully does not give √2.

## `EntityLiving.jump` — `EntityLiving`'s method

```
motionY = 0.41999998688697815;
```

That is the whole method. a1.1.2 has no sprint and no jump boost.

## Collision shapes

Fully derived and implemented — see `src/core/block/collision.hpp` and
`tests/collision_box_vectors.hpp`, which is the complete truth table of all seventy blocks crossed
with all sixteen metadata values, taken from a running jar.

Three results from that measurement are worth repeating here:

- **A collision box is a pure function of `(id, metadata)`.** Nothing reads a neighbour — not even
  the top half of a door, which was the obvious candidate and turns out to read its own low three
  bits and ignore the top-half flag entirely. Fences do not connect. This is why
  `block::collisionBoxes` takes no world.
- **Stairs are the only block with more than one box**, and they have exactly two.
- **A ladder with metadata outside 2..5 sets no bounds at all**, and the original then answers with
  whatever the shared `Block` singleton was left holding by the previous query — ask twice in a
  different order and it answers differently. We answer with the constructor's default, a full cube.
  The game never writes those values.

## Three things the oracle corrected that reading alone did not

All three were written the obvious way first, all three compiled and looked right, and all three were
caught on a specific tick by `tests/player_body_vectors.hpp`. They are the argument for the fixture.

- **`yOffset` is a float.** Declaring `kEyeHeight = 1.62` as a double is wrong by 4.8e-9, because the
  jar holds `1.62f` and `setPosition` widens it at the point of use. The oracle caught it on tick
  zero of the first case. `width` and `height` have the same shape, and the half-width is halved
  **in float** before widening — `double(0.6f) / 2.0` and `double(0.6f / 2.0f)` are different
  numbers.

- **`fallDistance` is updated in double and narrowed**, not accumulated in float. The bytecode is
  `getfield; f2d; dload; dsub; d2f; putfield` — so it is `fallDistance = (float)((double)fallDistance
  - dy)`, and doing the subtraction in float instead drifts by about 1e-7 a tick. Caught at tick 6 of
  a fall.

- **`posY` is written during the move, not derived after it.** `moveEntity` computes
  `posY = box.minY + yOffset - ySize` at its sync point and *then* decays `ySize` by 0.4 before
  returning. A body that stores only the feet and recomputes the eye on demand reads the decayed
  `ySize` and is wrong for the whole tick after a step up — by 0.3 of a block, which is not subtle
  once you are looking at it. So `posY` is a stored field here too.

## What the oracle does not cover

- **Sneaking.** `Entity.isSneaking` is a hardcoded `false` and only the client-side player class
  overrides it, so an `EntityPlayer` cannot be made to sneak from outside the game. The ledge
  walk-back is tested by hand in `tests/player_body_test.cpp` against the invariant it exists to
  hold, rather than against a captured number.
- **Water and lava.** The C++ implements the land branch; the other two are transcribed above and
  not yet written.

## What the crosshair is on -- `World`'s method and `Block`'s method

**Reach is 4.0, not 5.0.** `PlayerController.getBlockReachDistance` returns `5.0f` in the base
class, but both concrete controllers -- `PlayerControllerSP` and `nj`, the single-player and multiplayer ones --
override it with `4.0f`, so 5.0 is a value the running game never uses.

`World.rayTraceBlocks` walks the ray cell by cell, at most **twenty** steps (`bipush 20`, not the
200 later versions use), and asks each block to intersect itself:

```
if any coordinate is NaN return nothing
end = floor(v2);  cell = floor(v1)
loop up to 20 times:
    if cell == end return nothing                 // the far end, nothing hit
    wall{X,Y,Z} = the next cell wall on each axis, or 999 if that axis is still
    t{X,Y,Z}    = (wall - v1) / (v2 - v1)         // 999 stays 999
    step v1 to the nearest wall, and remember which one:
        x first: side = movingPositive ? 4 : 5    // and 0/1 for y, 2/3 for z
    cell = floor(v1), minus one on the axis if the wall crossed was a max face
    if the block is air, or canCollideCheck says no, keep walking
    hit = block.collisionRayTrace(v1, v2); if it is not null, that is the answer
```

`side` is 0 −Y, 1 +Y, 2 −Z, 3 +Z, 4 −X, 5 +X -- **the same numbering as `mc::mesh::Face`**, which is
why the two share an enum rather than needing a translation.

**The block the eye is inside is never tested.** The loop compares against the end cell and steps
before it looks at anything, so a player standing in tall grass is not aiming at it.

`Block.collisionRayTrace` is six plane crossings against the block's own bounds, keeping the nearest
that actually lands on the box, with ties going to the earlier candidate. Three details in it are
load-bearing and none of them is guessable:

- **It works in the block's own coordinates and translates back at the end.** `v1` and `v2` are
  shifted by `(-x, -y, -z)`, intersected, and the winner is shifted back. Intersecting the box where
  it stands agrees to about fifteen digits and then disagrees in the last two, because
  `a + (b-a)t` and `(a-o) + ((b-o)-(a-o))t + o` round differently. One ray in 3,744 of the fixture
  caught it.
- **The parallel-ray epsilon is `1.0E-7f` widened** -- stored as 1.0000000116860974E-7, not 1.0E-7.
- **The nearest candidate is chosen on real distance, not squared distance.** Those order the same
  way in arithmetic but not in floating point: two faces can be close enough that `sqrt` rounds both
  to the same double, and the original's strict `<` then keeps the earlier one where a comparison of
  squares keeps the later.

### What paces a held button

`Minecraft.runTick` repeats a click while the mouse button is down, gated on

```
ticksRan - lastClickTick >= Timer.ticksPerSecond / 4.0
```

and the timer is constructed as `new Timer(20.0F)`, so the interval is **five ticks -- a quarter of
a second -- and it is the same for the left button and the right one**.

**Hardness has nothing to do with it.** Hardness paces the *progress* of a break: the controller
accumulates `Block.getPlayerRelativeBlockHardness(player)` into `curBlockDamage` once per tick while
the button is held and breaks the block when it reaches 1.0. That is a separate mechanism, it is
Survival's, and confusing the two makes a fixed five-tick timer look like it is waiting on a
subsystem that does not exist yet.

### The selection shape is not the collision shape

`Block.collisionRayTrace` tests the block's own `bf..bk` bounds, and those are **not** the collision
box. A torch has no collision box at all -- `getCollisionBoundingBoxFromPool` returns null -- and a
perfectly good selection box, which is exactly why a torch can be aimed at and broken but not stood
on. Water and lava have the opposite problem: solid-looking and targetable by nothing, because
`canCollideCheck` (which in a1.1.2 is just `isCollidable()`) is false for them and for fire.

Two traps in capturing that table, both of which produced a plausible wrong answer first:

- **`setBlockBoundsBasedOnState` is an empty method on `Block`, and the torch does not override it.**
  The torch overrides `collisionRayTrace` and sets its bounds inline before delegating. Asking
  `setBlockBoundsBasedOnState` reports every torch in the game as a full cube. The fixture calls
  `collisionRayTrace` and reads what it leaves on the singleton.
- **`setBlockAndMetadata` is not raw.** It runs `onBlockAdded`, which deletes an unsupported torch
  outright and rewrites a supported one's metadata to 5. Enumerating a shape table through it
  enumerates the placement rules instead. The fixture places a support block, then forces the nibble
  through `Chunk.setBlockMetadata`, which writes it and nothing else.

The result ships as generated data rather than a hand-written switch: seventy blocks make eighteen
families, seven of which change with metadata, and 1,120 (id, metadata) pairs hold only 40 distinct
boxes. See `tools/gen_selection.py` and `data/a1.1.2/selection.json` -- about 5 KB in `.rodata`, and
no number in it was ever typed by a person.

## Which way a placed block faces

The sequence, found by following a right-click down rather than guessed:

```
av.a(...)   ItemBlock.onItemUse -> world.setBlockWithNotify(x, y, z, id)
                                -> block.onBlockPlaced(world, x, y, z, side)
ia / nj     the controller      -> block.onBlockPlacedBy(world, x, y, z, player)
```

**ItemBlock does not call `onBlockPlacedBy`** — the controller does, after `onItemUse` returns true.

And the result of sweeping all seventy blocks against all six faces at sixteen player headings:

- **Twelve blocks orient from the struck face**: torch, fire, both staircases, redstone wire, both
  furnaces, ladder, lever, both redstone torches, button.
- **Exactly one consults the player's heading** — the lever, and only on its top face.

So **a1.1.2 stairs and furnaces do not face the player**; they face according to the block face you
clicked. That is worth knowing before someone "fixes" it to match a later version. `BlockStairs`
does declare `onBlockPlacedBy`, but only to forward it to the block it is made of, whose
implementation is `Block`'s empty one.

The lever's heading rule is **not implemented and not guessed**. Sixteen headings produced
14, 13, 13, 14, 13, 13, 14, 13, 13, 13, 13, 13, 14, 13, 14, 13 — not a quadrant pattern, so something
this harness does not supply feeds it. The shipped table is per-face, taken at heading 0, and a
floor lever gets that. See `tools/gen_selection.py`.

## Drawing the selection box

`RenderGlobal` takes `getSelectedBoundingBoxFromPool` — which is the block's own bounds offset into
the world, the same bounds `collisionRayTrace` tests — and expands it by **`0.002f`**, then draws it
as a `GL_LINE_STRIP` in black at four tenths alpha.

**The PICA200 has no line primitive.** It offers triangles, strips, fans and the geometry primitive,
so ours is twelve thin boxes, one per edge — a deviation, and the only one available short of not
drawing the selection at all. The consequence is that our edges have a thickness in *world* units
rather than in pixels, so they thin with distance where the original's would not; across a four-block
reach that is the difference between about two pixels and one. Half-thickness is 1/128 of a block and
is ours, since a line has no thickness to copy. See `src/core/render/outline.hpp`.

## Creative flight — ours, and the only invented thing in this file

**a1.1.2 has no flight of any kind and no Creative mode to hang it on.** Everything above this
heading came out of the class file; `PlayerBody::tickFlying` did not, and the two are kept apart
here for the same reason `core/item/creative_palette.hpp` argues its own case: a reader has to be
able to tell which numbers can be checked against a jar and which cannot.

What it is: `flyCamera` — Spectator's frame-rate free camera — with `Entity.moveEntity` under it.

    motion = 0
    moveFlying(strafe, forward, yaw, speed)      // the original's own, unchanged
    motionY = ascend ? speed : descend ? -speed : 0
    moveEntity(motionX, motionY, motionZ)        // the original's own, unchanged
    fallDistance = 0
    motion = 0

Two pieces of it are the game's and are not touched: `moveFlying` turns the stick heading into a
direction, so a heading means the same thing flying as walking, and `moveEntity` sweeps the box, so
**flight collides**. That sweep is the entire difference between this and Spectator, which has no
body at all.

Three things about it are decisions:

- **A tick of flight is a velocity, not an acceleration.** The motion is cleared before and after,
  so there is no drag term and letting go stops you dead. That is what a camera does, and a camera
  with momentum is a camera you fight.
- **The two speeds are Spectator's, converted.** `flyCamera` moves 12 blocks a second and 40 held
  down; at 20 Hz that is 0.6 and 2.0 a tick, which is `kFlightSpeed` and `kFlightSprintSpeed`.
  Vertical is the same as horizontal, because there is no gravity to make them differ.
- **`fallDistance` is cleared every tick.** You cannot fall while flying, and leaving it to
  accumulate would bank a fall for Survival to cash in the moment flight was switched off.

Because `moveEntity` is a swept AABB rather than a ray, even the sprint speed's two blocks a tick
cannot pass through a one-block wall. `tests/creative_flight_test.cpp` checks exactly that, at both
speeds and again across the negative axis.

## What is not derived yet

- Fall damage, and everything else that belongs to Survival rather than to the body.
- `isOnLadder`, `isInWater` and `isInLava` as predicates over our own world reads.
- The block-walked callback at the end of `moveEntity` (step sounds, and pressure plates).
- ~~`Block.onBlockPlaced`~~ — **derived**, as a generated table: `tools/genref.java --place` sweeps
  every block against every face at sixteen headings, and `block::placementMetadata` reads the
  result. The one gap left is the lever's top face, which came out non-quadrant-shaped under that
  harness and is documented rather than guessed. See *Which way a placed block faces* above.
- The replaceable-material test. Placement goes into air only; a1.1.2 also replaces water, lava and
  snow, and answering that properly is a Survival-shaped question.
