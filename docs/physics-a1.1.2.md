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
| `kh` | `Entity` |
| `ge` | `EntityLiving` (extends `kh`) |
| `dm` | `EntityPlayer` (extends `ge`) |
| `cf` | `AxisAlignedBB` |
| `cn` | `World` |
| `ly` | `Block` |
| `eo` | `MathHelper` |

`dm` overrides neither `moveEntity` nor `moveEntityWithHeading`, so the player's movement is
`EntityLiving`'s and nothing else.

## Fields

On `kh`:

| Field | Meaning |
|---|---|
| `ak` `al` `am` | `posX` `posY` `posZ` |
| `an` `ao` `ap` | `motionX` `motionY` `motionZ` |
| `aq` | `rotationYaw`, in **degrees** |
| `au` | `boundingBox` |
| `av` `aw` `ax` `ay` | `onGround`, `isCollidedHorizontally`, `isCollidedVertically`, `isCollided` |
| `az` | the "cancel the whole move" flag — see below |
| `aB` | `yOffset` |
| `aC` `aD` | `width`, `height` |
| `aH` | `fallDistance` |
| `aL` | `ySize` |
| `aM` | `stepHeight` |
| `aN` | `noClip` |

On `cf`: `a b c` = `minX minY minZ`, `d e f` = `maxX maxY maxZ`, all `double`.

On `ly`: `bf..bk` = the block's own `minX..maxZ`; `bo` = `slipperiness`.

## **`posY` is the eye, not the feet**

`dm`'s constructor sets `yOffset = 1.62f`, and `Entity.setPosition` sets

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
| Player size | `0.6f` × `1.8f` | `dm` ctor, `setSize` |
| `yOffset` (eye) | `1.62f` | `dm` ctor |
| — and these four are **floats**, so they widen to 1.6200000047683716, 0.60000002384185791 and 1.7999999523162842, **not** to 1.62, 0.6 and 1.8 | | |
| `stepHeight` | `0.5f` | added to `ySize` on a successful step |
| Gravity | `0.08d` | `ge.b(FF)` |
| Vertical drag | `0.9800000190734863d` | `ge.b(FF)` — this is `0.98f` widened, **not** `0.98` |
| Jump impulse | `0.41999998688697815d` | `ge.C()` — `0.42f` widened |
| Air friction | `0.91f` | `ge.b(FF)` |
| Ground friction base | `0.54600006f` | `ge.b(FF)` — used when the block below is air |
| Acceleration normaliser | `0.16277136f` | `ge.b(FF)` |
| Ground acceleration | `0.1f` | `ge.b(FF)` |
| Air acceleration | `0.02f` | `ge.b(FF)` |
| Sneak probe step | `0.05d` | `kh.c(DDD)` |
| Sneak input scale | `0.3d` | `gd.a(dm)` — `MovementInput`, **not** the entity; see below |
| `ySize` decay | `0.4f` per move | end of `kh.c(DDD)` |
| Pi, for heading | `3.1415927f` | `kh.a(FFF)` — the float literal, not a double pi |
| Block slipperiness | `0.6f`, and `0.98f` for ice alone | `ly` ctor; `he` (id 79) |

`0.54600006f` is `0.6 × 0.91` as a float, and the two are **not** interchangeable — the jar stores
the product, so computing it at runtime can differ in the last bit.

## `az` is always true, so two branches are dead

`moveEntity` has three copies of

```
if (!az && originalDelta != clippedDelta) { dx = dy = dz = 0; }
```

`az` is assigned exactly once in the whole jar — `iconst_1` in the `Entity` constructor — and no
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

## `Entity.moveEntity` — `kh.c(DDD)`

```
if (noClip) { boundingBox.offset(dx,dy,dz); syncPosFromBox(); return; }

double origDx = dx, origDy = dy, origDz = dz;
AABB startBox = boundingBox;

if (onGround && isSneaking()) {
    // Walk each horizontal component down towards zero while the box, dropped
    // one block, would hit nothing -- this is what stops a sneaking player
    // walking off a ledge. a1.1.2 has only the two single-axis loops; the
    // combined dx-and-dz loop is a later addition.
    while (dx != 0 && noCollisions(box.offset(dx, -1, 0))) {
        if (dx < 0.05 && dx >= -0.05) dx = 0; else if (dx > 0) dx -= 0.05; else dx += 0.05;
        origDx = dx;
    }
    while (dz != 0 && noCollisions(box.offset(0, -1, dz))) { ...same... origDz = dz; }
}

List boxes = collidingBoxes(box.extend(dx,dy,dz));   // one query, reused for all three axes
for (b : boxes) dy = b.calculateYOffset(box, dy);  box = box.offset(0,dy,0);
bool stepCandidate = onGround || (origDy != dy && origDy < 0);
for (b : boxes) dx = b.calculateXOffset(box, dx);  box = box.offset(dx,0,0);
for (b : boxes) dz = b.calculateZOffset(box, dz);  box = box.offset(0,0,dz);

// The step-up: retry the whole move from the original box, lifted.
if (stepHeight > 0 && stepCandidate && ySize < 0.05 && (origDx != dx || origDz != dz)) {
    double flatDx = dx, flatDy = dy, flatDz = dz;
    AABB flatBox = box;
    dx = origDx; dy = stepHeight; dz = origDz;
    box = startBox;
    boxes = collidingBoxes(box.extend(dx,dy,dz));
    ... the same three axes again ...
    if (flatDx*flatDx + flatDz*flatDz >= dx*dx + dz*dz) {
        dx = flatDx; dy = flatDy; dz = flatDz; box = flatBox;   // the step gained nothing
    } else {
        ySize += 0.5f;                                          // keep it, and smooth the camera
    }
}

syncPosFromBox();                       // posX,posZ from the box centre; posY = minY + yOffset - ySize
isCollidedHorizontally = (origDx != dx || origDz != dz);
isCollidedVertically   = (origDy != dy);
onGround               = (origDy != dy && origDy < 0);
isCollided             = isCollidedHorizontally || isCollidedVertically;
if (onGround) { if (fallDistance > 0) { fall(fallDistance); fallDistance = 0; } }
else if (dy < 0) fallDistance -= dy;
if (origDx != dx) motionX = 0;
if (origDy != dy) motionY = 0;
if (origDz != dz) motionZ = 0;
... walk distance, then the footstep: step sound and onEntityWalking, both on the one cell
    underfoot; then onEntityCollidedWithBlock for every cell the box overlaps, then the burn ...
ySize *= 0.4f;                          // unconditional, at the very end
```

Note that the collision list is gathered **once**, from the box swept along all three axes at once,
and then reused for each axis in turn. Re-querying per axis would be a different game.

### `collidingBoxes` has an entity half — `cn.a(Lkh;Lcf;)Ljava/util/List;`

The list is not only blocks, and missing that is what made a minecart here something to walk
through. The method is:

```
list.clear();                                     // one reusable ArrayList on the world
i  = floor(box.minX);  j  = floor(box.maxX + 1);
k  = floor(box.minY);  l  = floor(box.maxY + 1);
i1 = floor(box.minZ);  j1 = floor(box.maxZ + 1);
for (x = i; x < j; x++) for (z = i1; z < j1; z++) if (blockExists(x, 64, z))
    for (y = k - 1; y < l; y++)                   // one below, for the 1.5-tall fence
        Block.blocksList[getBlockId(x,y,z)]?.getCollidingBoundingBoxes(world,x,y,z,box,list);

double d = 0.25;
List near = getEntitiesWithinAABBExcludingEntity(entity, box.expand(d,d,d));
for (e : near) {
    AABB b = e.getBoundingBox();                  // kh.f_()
    if (b != null && b.intersectsWith(box)) list.add(b);
    AABB c = entity.getCollisionBox(e);           // kh.b_(kh) -- on the MOVER, not on e
    if (c != null && c.intersectsWith(box)) list.add(c);
}
return list;
```

Two facts fall out of it, and both were checked against every class in the jar:

- **Exactly two classes override `f_()` (getBoundingBox) with a box**: `dc` (EntityBoat) and `oc`
  (EntityMinecart), each `return this.boundingBox`. `kh`'s own is `return null`, and nothing else
  overrides it — no mob, item, arrow, painting, TNT, falling block or particle. So the whole of
  "which entities are solid" in a1.1.2 is *boat and minecart*, and it is why you can stand on a
  cart and walk through a cow.
- **`b_(kh)` (getCollisionBox) is called on the moving entity**, not on the neighbour, and the boat
  and the cart both answer with the *argument's* box — `return e.boundingBox`, one instruction, with
  no null, liveness or `canBeCollidedWith` test in front of it. A moving boat or cart therefore
  collides with **everything near it**, not only with the other boats and carts; a moving player,
  mob, item or falling block reaches only the first branch, because `kh.b_` is null.

`getEntitiesWithinAABBExcludingEntity` (`cn.b(kh,cf)` into `ga.a(kh,cf,List)`) excludes **only the
entity itself** — not its rider and not its vehicle, and `ga.a(kh,cf,List)` filters on nothing else:
`e != excluded && e.boundingBox.intersectsWith(box)` is the whole of it. That costs nothing for a
rider on either side. The rider does not move — `updateRidden` assigns the position and `moveEntity`
never runs while mounted — and the vehicle is not stopped by the rider it carries, because
`oc.h()` and `dc.h()` (getMountedYOffset) are both `height * 0.0 - 0.3`, which puts the rider's box
*overlapping* the vehicle's on all three axes, and `calculateOffset` clips only against a box that
is ahead and clear.

What the list holds is a1.1.2's **world** entity list, so one thing this port has is deliberately
absent from it: `nq` (EntityFX) never enters that list. `bq.a(nq)` is
`lists[particle.getFXLayer()].add(particle)` and nothing else, so a cart is not stopped by smoke.

See `core/tick/tick_world.hpp` (`forEachSolidBox`), `core/entity/entity_boxes.hpp` and
`core/entity/sweep.cpp` (`Mover`) for how this half is carried here, and
`tests/entity_boxes_test.cpp` for what it is checked against.

## `EntityLiving.moveEntityWithHeading` — `ge.b(FF)`

Three branches. Water and lava first, land last:

```
if (isInWater()) {
    double y0 = posY;
    moveFlying(strafe, forward, 0.02f);
    moveEntity(motionX, motionY, motionZ);
    motionX *= 0.800000011920929; motionY *= same; motionZ *= same;   // 0.8f widened
    motionY -= 0.02;
    if (isCollidedHorizontally && isOffsetPositionInLiquid(motionX, motionY + 0.6000000238418579 - posY + y0, motionZ))
        motionY = 0.30000001192092896;
} else if (isInLava()) {
    ... identical, but the three factors are 0.5 ...
} else {
    float friction = 0.91f;
    if (onGround) {
        friction = 0.54600006f;
        int below = world.getBlockId(floor(posX), floor(boundingBox.minY) - 1, floor(posZ));
        if (below > 0) friction = Block.blocksList[below].slipperiness * 0.91f;
    }
    float accel = 0.16277136f / (friction*friction*friction);
    moveFlying(strafe, forward, onGround ? 0.1f * accel : 0.02f);

    friction = 0.91f;                       // recomputed, because moveFlying may have left the ground
    if (onGround) { ...the same lookup again... }

    if (isOnLadder()) { fallDistance = 0; if (motionY < -0.15) motionY = -0.15; }
    moveEntity(motionX, motionY, motionZ);
    if (isCollidedHorizontally && isOnLadder()) motionY = 0.2;
    motionY -= 0.08;
    motionY *= 0.9800000190734863;
    motionX *= friction;
    motionZ *= friction;
}
```

Two details that are easy to get wrong: the friction lookup uses **`boundingBox.minY - 1`**, not
`posY`, and it happens **twice** — once to size the acceleration and once to damp the result.

## `Entity.moveFlying` — `kh.a(FFF)`

```
float m = sqrt(strafe*strafe + forward*forward);
if (m < 0.01f) return;
if (m < 1.0f) m = 1.0f;
m = friction / m;
strafe *= m; forward *= m;
float s = MathHelper.sin(rotationYaw * 3.1415927f / 180.0f);
float c = MathHelper.cos(rotationYaw * 3.1415927f / 180.0f);
motionX += strafe*c - forward*s;
motionZ += forward*c - strafe*s * -1;   // i.e. forward*c + strafe*s
```

`rotationYaw` is degrees. Note the input magnitude is clamped **up** to 1, so pushing a stick half
way gives half speed but pushing two axes fully does not give √2.

## Sneaking scales the stick — `gd.a(dm)`

The crouch in a1.1.2 is split across two classes and only one half of it works. `Entity.isSneaking`
is a hardcoded `false` (see *What the oracle does not cover*), which kills the stance, the ledge
walk-back and the silent footstep. The **slowdown is alive**, because it never asks the entity
anything — it is the tail of `MovementInputFromOptions.updatePlayerMoveState`, reading the sneak key
out of its own array:

```
this.a = 0.0f;  this.b = 0.0f;           // moveStrafe, moveForward
if (this.f[0]) this.b += 1.0f;           // f[] is the key array; 0 = Forward
if (this.f[1]) this.b -= 1.0f;           // 1 = Back
if (this.f[2]) this.a += 1.0f;           // 2 = Left
if (this.f[3]) this.a -= 1.0f;           // 3 = Right
this.d = this.f[4];                      // 4 = Jump
this.e = this.f[5];                      // 5 = Sneak
if (this.e) {
    this.a = (float)((double)this.a * 0.3D);
    this.b = (float)((double)this.b * 0.3D);
}
```

`gd.a(int,boolean)` is what fills that array, and it fills it by comparing the pressed key against
`GameSettings`' bindings in a fixed order — so the indices above are read off the jar, not guessed:
`fr.j` "Forward" is 0, `fr.l` "Back" is 1, `fr.k` "Left" is 2, `fr.m` "Right" is 3, `fr.n` "Jump" is
4 and `fr.s` "Sneak" (LWJGL 42, left shift) is 5. Note in passing that **Left is the one that adds**
to `moveStrafe`, which is the other end of the strafe negation in `readBodyInput`.

Three things this settles:

- **It is period.** b1.6.2, b1.8.1 (`gh.a(sz)`) and 1.8.9 all disassemble to the same guarded pair
  of `f2d; 0.3d; dmul; d2f` multiplications, differing only in which obfuscated names they wear, so
  unlike the camera drop this number needs no era label — it was always here.
- **It scales the input, not the speed.** The 0.3 lands before `moveFlying`, so what it divides is
  the *acceleration* a tick adds, not a cap on velocity. Because `moveFlying` clamps the magnitude
  **up** to 1 and never normalises an input inside the unit circle back up, the tick stays linear in
  the stick: a third of the push really does settle at a third of the speed. Scaling the ground
  acceleration or the resulting motion instead would be wrong, and wrong differently.
- **The double round trip is not decoration.** `f2d; ldc2_w 0.3d; dmul; d2f` is a different float
  from `x * 0.3f` for plenty of inputs — 0.7 comes out `0.20999999344348907` one way and
  `0.21000001` the other.

Ported as `applySneakSlowdown` in `core/entity/player_body.hpp`, called from the input reader rather
than from `PlayerBody`, which is where the jar puts it: the oracle drives `moveEntityWithHeading`
directly, so a body that scaled its own input would disagree with its own fixture.

## `EntityLiving.jump` — `ge.C()`

```
motionY = 0.41999998688697815;
```

That is the whole method. a1.1.2 has no sprint and no jump boost.

## Who calls it, and what the button does in a liquid — `ge.j()`

`EntityLiving.onLivingUpdate` is the only caller, and the branch it sits in is **three-way, not
one**:

```
boolean water = isInWater();          // kh.g_()
boolean lava  = handleLavaMovement();  // kh.G()
if (isJumping) {
    if (water)      motionY += 0.03999999910593033;
    else if (lava)  motionY += 0.03999999910593033;
    else if (onGround) jump();
}
moveStrafing *= 0.98f; moveForward *= 0.98f; randomYawVelocity *= 0.9f;
moveEntityWithHeading(moveStrafing, moveForward);
```

Two things fall out of that shape:

- **In a liquid the button is not a jump.** It adds a flat `0.03999999910593033` to the motion every
  tick it is held, the same number in water and in lava, and `jump()` is never reached. Against the
  swim branch's `0.8` drag and `0.02` sink that settles at `0.06` a tick — a block and a fifth a
  second — which is what swimming up *is*. A port that reaches `jump()` instead launches the player
  off the bottom of the pool once and then sinks, which is exactly the bug this section was written
  for; see the note under the liquid branches below.
- **There is no jump cooldown.** Later versions gate on a `jumpTicks` counter; a1.1.2 goes straight
  from `onGround` to `jump()`, so the button fires again the tick you land.

Both liquid predicates are read *before* the branch and read again inside `moveEntityWithHeading`.
Nothing between the two moves the body, so `PlayerBody::tick` asks each once and uses the answer
twice.

## Particles are entities — `nq`, `iw`, and `bq.a(III)V`

A breaking block's flecks are `EntityDiggingFX`, and `EntityFX extends Entity`: they run the
*same* `moveEntity` the player does, which is why one lands on the ground, slides along it and
stops in a corner. That is the whole reason `core/entity/sweep.hpp` exists — the sweep was
inside `PlayerBody` until a second thing needed it.

`EffectRenderer.addBlockDestroyEffects` cuts the cell into a 4x4x4 grid and starts one particle
at each sub-cell centre, thrown along the offset from the block's middle:

```java
for (int a = 0; a < 4; a++) for (int b = 0; b < 4; b++) for (int c = 0; c < 4; c++) {
    double x = i + (a + 0.5D) / 4.0D, y = j + (b + 0.5D) / 4.0D, z = k + (c + 0.5D) / 4.0D;
    addEffect(new EntityDiggingFX(world, x, y, z, x - i - 0.5D, y - j - 0.5D, z - k - 0.5D, block));
}
```

Sixty-four per block, and it is spawned **before** the block is cleared, because the particle
takes the block's texture on the way past.

`EntityFX`'s constructor then jitters that velocity, normalises it, scales it by a triangular
draw and adds `0.1` to y — so most flecks rise before they fall, and the few thrown hardest
downward do not. `onUpdate` is six lines: age, `motionY -= 0.04 * particleGravity`, `moveEntity`,
`motion *= 0.98`, and `motionX/Z *= 0.7` once it has landed. `particleGravity` is **1.0 for all
seventy blocks** in a1.1.2, measured off the constructed blocks rather than assumed.

**Neither of the two random streams can be reproduced**, and that is the original's doing: the
velocity jitter comes from `Math.random()` and the texture, scale and lifetime from the entity's
own `new Random()`, both time-seeded. What is copied is the distribution and the order of the
draws, not the sequence.

One rounding trap is worth recording, because it cost a fall through the world here.
`moveEntity` clips the **box** and reads the position back out of it; deriving the box from a
rounded position instead puts its bottom a fraction of an ulp below the block it just landed on,
and `calculateYOffset`'s `mover.minY >= block.maxY` then declines to stop it on the next tick.
The box is the authority for the same reason it is in the jar.

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

### The selection box is `getSelectedBoundingBoxFromPool`, not the ray's residue

This one was wrong here for a while and it was visible: **a ladder's outline was a whole block**, and
a ray could not be aimed past one.

`Block.collisionRayTrace` tests the ray against the block's own `bf..bk` fields and nothing writes
them for it. A torch gets away with this by overriding `collisionRayTrace` itself and setting its
bounds inline before delegating — which is why reading the fields *after* a ray trace is the right
way to learn a torch's shape. Three blocks do the opposite:

| class | overrides `collisionRayTrace` | overrides `getSelectedBoundingBoxFromPool` |
|---|---|---|
| `br` ladder | — | **yes** |
| `hy` cactus | — | **yes** |
| `km` stairs | — | yes, delegating to the model block |
| `fw` door | yes | yes |
| `if` rail, `mj` | yes | — |

So a generator that restores each block's constructor defaults and then reads what the ray left
records the ladder's constructor cube, and the cactus's collision box rather than its outline. The
fixture now asks **both**, in the order a frame asks them — `rayTraceBlocks` and then
`drawSelectionBox`'s `f` — which keeps the torch's post and gains the ladder's plate. Two blocks
moved out of 1,120 rows: 65 at metadata 2..5, and 81, whose selection box is inset a sixteenth on x
and z and is **full height** where its collision box stops at 0.9375.

**The ray-trace oracle had recorded the same litter**, because its scene is a world two statements
old in which nothing has ever asked a ladder for a box. A running client is never in that state: `f`
is called on the targeted block every frame and `d` on every block the body overlaps every tick, and
both leave the real bounds behind. `--raytrace` therefore primes every block in its scene through
`f` before the sweep, and sixteen rays that stopped on a phantom cube now pass through the ladder.

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

  The **input scaling** above is a separate matter and is not missing from the game, only from the
  oracle: it happens in `MovementInput` before `moveEntityWithHeading` is called at all, so no
  capture of that method could contain it either way. It is tested by hand too.
- **The fluid current.** `World.handleMaterialAcceleration` is fully ported now -- both its answer
  ("is the body in this material, with the surface reaching the top of the probe") and its side
  effect, which is asking each fluid cell for a flow vector, normalising the sum and adding
  **0.004** of it to the motion. The flow field is `core/block/fluid_flow.hpp`, shared with the
  mesher, which needs the same `jp.e(nm,III)` to spin a flowing block's top texture.

  **What the oracle does not cover is the current, not the code.** Every captured case is a
  **still** pool, and deliberately: a pool of source blocks has no flow at its interior, so the
  vectors all come out zero and the comparison is of the branches around them. A moving oracle would
  need a running JVM with a river in it. `tests/fluid_push_test.cpp` pins what is checkable without
  one -- the direction (downstream is toward the *higher* decay), the magnitude (0.004 of a unit
  vector, whatever the box), and the cases that must produce nothing: still water, water that does
  not reach the inset probe, and lava, which in this version has no vector at all.

  Two of the original's own oddities are transcribed rather than tidied. The sum of the cells' unit
  vectors is **normalised a second time**, so what the cells decide is the direction and never the
  speed. And a player holding jump in a current is pushed **twice** that tick, because `ge.j()` and
  `ge.b(FF)` each call `handleWaterMovement()` and the method pushes as a side effect of answering.

## The liquid branches, and the four methods behind them

The two branches at the top of `moveEntityWithHeading` are implemented. They are the same code with
a different drag -- `0.800000011920929` in water, `0.5` in lava -- which is how the class file has
it, so the port is one function and two constants rather than two near-copies.

What took the reading was not the branches but the predicates under them, and three of the four had
a surprise in:

- **`kh.g_()` -- `isInWater`.** `world.handleMaterialAcceleration(box.expand(0, -0.4000000059604645,
  0), Material.water, this)`. Not a plain material test: a cell only counts if the fluid's *surface*
  reaches the probe, `(double)l >= (y + 1) - BlockFluid.getPercentAir(metadata)` -- and `l` there is
  the loop's **upper bound**, `floor(maxY + 1)`, not the cell being examined. Reading it as the cell
  makes every puddle deep enough to swim in.

- **`kh.G()` -- `isInLava`.** `world.isMaterialInBB(box.expand(0, -0.4000000059604645, 0),
  Material.lava)`, and that *is* the plain test. **The same inset as water**, despite the shape of
  the two methods suggesting lava also pulls in horizontally -- it does not, and reading the two
  side by side is what settles it.

- **`kh.b(DDD)` -- `isOffsetPositionInLiquid`.** Named the opposite way round from what it answers:
  it returns true when the box moved by that offset is **free**, of collision boxes *and* of liquid.
  The swimming branch uses it to decide whether pushing into a wall should lift you, which is how a
  player climbs out of water onto a shore.

- **`cn.b(cf)` -- `isAnyLiquid`.** It takes `floor_double` of each minimum and then **decrements it
  again if the value was negative**, so the probe is one cell wider on the negative side of every
  axis. That is the original's, not a transcription slip, and it is reproduced -- this project tests
  negative coordinates on purpose and a quiet disagreement there is exactly the sort that survives
  for a year.

Seven cases were added to `tests/player_body_vectors.hpp` for it, each run twice as all the others
are: sinking in water, swimming forward, holding jump to rise, wading out onto a shore, sinking in
lava, swimming in lava, and a one-block puddle that is deliberately **not** deep enough to swim in.
All fourteen match a real `EntityPlayer` bit for bit.

**`swim_up_by_holding_jump` did not, for a while, swim up.** The oracle is only as good as the
harness driving the jar, and `tools/genref.java` drove it with `if (jumping && onGround) jump()` --
onLivingUpdate's third branch and neither of its first two. So the reference player sank, the port
sank with it, and fourteen cases agreed bit for bit on the wrong answer. The generator now
reproduces all three branches (see `ge.j()` above), the fixture was regenerated, and the only rows
that moved were that case's two: **240 lines of 13,317**, which is also the evidence that
regenerating is deterministic. The lesson is a cheap one to reuse -- a fixture whose *name* claims a
behaviour deserves a test that the rows show it, and there is one now in
`tests/player_body_test.cpp`.

## What the crosshair is on -- `cn.a(aj,aj,Z)` and `ly.a(cn,III,aj,aj)`

**Reach is 4.0, not 5.0.** `PlayerController.getBlockReachDistance` returns `5.0f` in the base
class, but both concrete controllers -- `ia` and `nj`, the single-player and multiplayer ones --
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

### An entity in front of the block takes the crosshair -- `iq.a(F)`

`EntityRenderer.getMouseOver` runs the block ray above and then a second pass for entities, and an
entity it finds **replaces** the block as `objectMouseOver`. `clickMouse` then attacks or
`interact`s with the entity and never touches the block; `drawSelectionBox` outlines tiles only,
so the outline goes too.

```
d1 = blockHit != null ? blockHit.hitVec.distanceTo(eye) : 4.0;
if (controller instanceof il) d = d1 = 32.0; else { if (d1 > 3.0) d1 = 3.0; d = d1; }
end = eye + look * d
for each entity touching player.boundingBox.addCoord(look * d), if canBeCollidedWith():
    hit = entity.boundingBox.expand(0.1F, 0.1F, 0.1F).calculateIntercept(eye, end)
    if hit != null: dist = eye.distanceTo(hit.hitVec)
                    if (dist < best || best == 0.0) { pointed = entity; best = dist; }
if (pointed != null && !(controller instanceof il)) objectMouseOver = pointed
```

- **Three blocks, and nearer than the block.** The block ray reaches four, the entity pass three,
  and the segment ends at the block hit. So a cart behind a wall, or three and a half blocks off,
  leaves the crosshair on the block.
- **The border is 0.1**, as an inline `ldc 0.1f`, the same for every class. The 0.3 is
  `EntityArrow`'s sweep. The port's crosshair used 0.3 until 2026-09-11. That border reached
  the top of a grounded cart's cell, so no shot could put a rail back under it (status.md §21).
- **Nearest across every entity**, by the intercept point and not the centre. `distanceTo` takes
  its square root as a float (`eo.a(D)F`). `calculateIntercept` picks its face on **squared**
  distance, unlike `Block.collisionRayTrace`.
- `canBeCollidedWith` (`c_()`) is `!isDead` for the boat (`dc`) and the cart (`oc`), `true` for
  the painting (`jc`), and false for items and the base class. The ridden vehicle is not excluded.
- `il` is a test controller (it fills the hotbar and keeps `hq.b()`'s 5.0). The port does not
  model it, and its Creative uses the single-player rules.
- The broad-phase filter over the swept player box is not modelled. From the body's eye, the
  segment is at least 0.18 inside that box, and the border reaches only 0.1.

In the port: `entity::entityPickReach` / `interceptDistance` (core/entity/ray_trace) and
`item::pickEntity` (core/item/use). `tests/minecart_test.cpp` covers it; search "crosshair".

## Which way a placed block faces

The sequence, found by following a right-click down rather than guessed:

```
av.a(...)   ItemBlock.onItemUse -> world.setBlockWithNotify(x, y, z, id)   (runs onBlockAdded)
                                -> block.onBlockPlaced(world, x, y, z, side)
```

**That is the whole sequence.** a1.1.2 has no `Block.onBlockPlacedBy` at all: `Block` takes an
`EntityPlayer` in exactly three methods — `a(dm)`, `a(cn,III,dm)` (blockActivated) and
`b(cn,III,dm)` (onBlockClicked) — and the last of those is reached from
`PlayerController.clickBlock`, which is the *left* button starting a break.

And the result of sweeping all seventy blocks against all six faces at sixteen player headings:

- **Twelve blocks vary with the struck face** in the sweep: torch, fire, both staircases, redstone
  wire, both furnaces, ladder, lever, both redstone torches, button.
- **None consults the player's heading.** Not one, at any face, at any of the sixteen.

So **a1.1.2 stairs and furnaces do not face the player.** That is worth knowing before someone
"fixes" it to match a later version (pumpkins bring `onBlockPlacedBy` in a1.2).

**But they do not face the struck face either**, and this section used to say they did. The sweep
measures `onBlockAdded` and `onBlockPlaced` together against a single stone cube, and only **six**
classes override `onBlockPlaced` (`ly.d(Lcn;IIII)V`): the torch (`mj`, and `bg` through it), the
ladder (`br`), the button (`hu`) and the lever (`no`). The sweep now asks the jar that directly and
lists them in `tests/placement_vectors.hpp`. The furnace and the staircases vary by face only
because `onBlockAdded` turns them from their **neighbours**, and the stone cube was the only
neighbour there was:

- **Furnace** — `ku.h(Lcn;III)V`, from `ku.e`. Default 3 (mouth on +Z). Then, in this order and the
  last that holds winning: opaque at −Z and not +Z → 3; +Z and not −Z → 2; −X and not +X → 5; +X and
  not −X → 4. `ly.p` is the opaque-cube *array*. The write is `cn.b(IIII)V`, which in a1.1.2 is the
  bare chunk write. So a furnace faces away from a wall, keeps +Z between two walls, and in a
  corner answers to the X wall.
- **Staircase** — `km.a(Lcn;IIII)V` (onNeighborBlockChange, which `km.e` also calls). With a solid
  material above, it `setBlockWithNotify`s itself into its **model block**, so wooden stairs become
  planks and cobblestone stairs cobblestone. That includes being put under a block in the first
  place. Otherwise `km.h` re-shapes it and the eight staircases one step up and down on each side.
  `km.h` has three passes, each asked only if the one before found nothing: a staircase one step up
  on a side (climb towards it), then a solid material on one side and not the other (back onto it),
  then a staircase one step down (climb away). It uses the bare write again and leaves the metadata
  alone if nothing fired.

The port had these as face-table rows, which agree with the jar only while the clicked block is the
only neighbour. They are now `tick::blockAdded`/`neighbourChanged` (`TickBehaviour::Furnace` and
`::Stairs`), their table rows are 0, and `tests/placement_test.cpp` runs the port's own
write-then-`blockAdded` for every case of the sweep and gets the jar's metadata back. The model block
is a generated column (`model` in blocks.json, `block::modelOf`), read by the extractor off `km`'s
Block-typed constructor argument. `BlockStairs` forwards `onBlockClicked` (`b(cn,III,dm)`) to that
model too. This section once called that override `onBlockPlacedBy`, and it isn't.

**The furnace's mouth was also never drawn where the metadata put it.** The cube stream draws
`faces`, which is the inventory answer and always has the mouth on +Z. `ku.a(Lnm;IIII)I` puts it on
the side equal to the metadata, and a lit furnace's mouth there is `bb + 16` (tile 61), not the
unlit 44. The extractor now asks that method with a world that answers only the block's own
metadata, which gives `metadataFaces` in blocks.json. Grass and the chest still probe their
neighbours and get nothing. A block with that table is not `unitCube`, and the mesher draws it
through the out-of-line path from `block::worldFaces`. **The chest is still drawn from `faces`**:
its world texture reads its neighbours (double chests), and that is still to port.

The one thing in the game that *does* read a heading when something is placed is
`ItemDoor.onItemUse`, and it reads it in the item rather than in the block; `core/item/use.cpp`
carries that and this table has nothing to do with it.

### The lever's "heading rule", which was neither

This section used to say the lever consulted the player's heading on its top face, and gave the
sequence 14, 13, 13, 14, 13 … as a pattern the harness could not explain. **Both halves were the
harness.**

- The `+8` on every row was a *punch*. The sweep called `ly.b(Lcn;IIILdm;)V` after each placement,
  on the mistaken reading that it was `onBlockPlacedBy`; it is `onBlockClicked`, so every block the
  sweep put down was then hit. A lever flips when hit, a button presses, a door opens and redstone
  ore lights — which is why the shipped table placed levers switched on (13/14 rather than 5/6),
  buttons pressed (9…12 rather than 1…4), doors open (4 rather than 0), and refused to place
  redstone ore at all (it turned into id 74 and the sweep read that as "did not survive").
- The 13-or-14 alternation was `BlockLever.onBlockAdded`'s `5 + rand.nextInt(2)`, which is which way
  round a floor lever's handle lies. The sweep now reseeds `World.rand` before every case, so the
  table is a function of (id, face) the way `placementMetadata` is.

The table therefore says 6 for a floor lever, and **that is not the end of it**. The roll is not
cosmetic: `no.c(Lcn;IIII)Z` names orientations 1 to 5 and not 6, so a floor lever lying the second
way round hands no *direct* power to the block it stands on — it still powers wire beside it, which
goes through the indirect answer. Pinning the table at 6 would have given every floor lever in the
game that quirk. So the roll happens where the original's last one happens: `tick::leverPlaced`,
which is `BlockLever.onBlockAdded`, off `World.rand`. See `tools/genref.java --place`,
`tools/gen_selection.py` and `core/tick/redstone.cpp`.

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

## What a right-click does — `hq.a(Ldm;Lcn;Lev;IIII)Z`

`PlayerController.onPlayerRightClick` is four lines and their order is the whole rule:

```java
int id = world.getBlockId(i, j, k);
if (id > 0 && Block.blocksList[id].blockActivated(world, i, j, k, player)) return true;
if (itemstack == null) return false;
return itemstack.useItem(player, world, i, j, k, l);
```

**The block is asked first, and there is no sneak override** — that is a later version's. A door
opens rather than taking a block to the face, an empty hand still opens it, and a block that answers
true has spent the click. Redstone ore is the one that lights *and* lets the click through: its
override glows and then returns the base class's `false`. See `core/tick/behaviour.hpp` for the four
activations that are ported and the four containers that are not.

### `ItemBlock.onItemUse` — `av.a(Lev;Ldm;Lcn;IIII)Z`

Offset by the struck face, ask the world, write one block, run `onBlockPlaced`, spend one from the
stack. Two details are worth naming:

- **A snow layer is replaced rather than built on.** The method's first line: if the clicked block is
  snow, the side is rewritten to 0 and *no offset happens at all*, so the block lands in the snow's
  own cell.
- **The world's test is `canBlockBePlacedAt`, not `Block.canPlaceBlockAt`.** They are different
  methods and only the first is asked here:

  ```java
  if (box != null && !checkIfAABBIsClear(box)) return false;                  // the player is in the way
  if (existing == water || lava || fire || snow) return true;                 // and the override is never reached
  return id > 0 && existing == null && blocksList[id].canPlaceBlockAt(...);
  ```

  That middle line is a rule this port had quietly tightened: a cell holding water, lava, fire or
  snow takes the block **whatever the block's own condition says**, so a1.1.2 lets you plant a
  sapling in water and then takes it away on the next tick. `checkIfAABBIsClear` fails on any entity
  in the box with `preventEntitySpawning` set, which is the player, a boat and a minecart — so you
  cannot place a block inside yourself, and that is also why a slab cannot be completed into a
  double slab while you are standing on it.

**The box it asks about is the one the block will actually have**, and this port asked at metadata
0, which was a bug with one visible symptom. `getCollisionBoundingBoxFromPool` reads the *world's*
metadata, and at the empty cell a placement is aimed at that is 0 — which for a ladder is outside
the 2..5 the game writes. a1.1.2 answers that case with whatever the shared `Block` singleton was
last left holding, an order-dependent value; this port answers with the constructor's full cube (see
`core/block/collision.hpp`). A full cube in the cell in front of you overlaps the body, so **a ladder
could not be hung on the wall you were standing against**, which is exactly where a ladder goes.
Asking with the metadata `onBlockPlaced` is about to write is deterministic, needs no leftover
state, and agrees with the original everywhere the original is not reading its own litter.

### `ItemFlintAndSteel.onItemUse` — `nx.a(Lev;Ldm;Lcn;IIII)Z`

**Not an `ItemBlock`**, and the differences are all audible or visible:

```java
if (l == 0) j--; ... the six faces, as an offset
if (world.getBlockId(i, j, k) == 0) {
    world.playSoundEffect(i + 0.5, j + 0.5, k + 0.5, "fire.ignite", 1.0F,
                          itemRand.nextFloat() * 0.4F + 0.8F);
    world.setBlockWithNotify(i, j, k, Block.fire.blockID);
}
itemstack.damageItem(1, entityplayer);
return true;
```

- **No `canBlockBePlacedAt`.** No clearance test against the player and no `canPlaceBlockAt`, so a
  fire is lit at your own feet and in a cell whose support is about to refuse it —
  `BlockFire.onBlockAdded` then removes it on the same call. That sequence is the original's and it
  is what "the flame went out immediately" is supposed to look like on stone.
- **Air only.** `ItemBlock` treats water, lava, fire and snow as free space; this tests
  `getBlockId == 0` and nothing else.
- **`fire.ignite`, not the block's place cue**, and a pitch drawn from `Item.itemRand` — a *static
  on Item*, not the world's Random, which is why `core/item/use.cpp` keeps one of its own.
- **It returns true either way**, because the durability is spent whatever happened.

### `ItemDoor.onItemUse` — `ec.a(Lev;Ldm;Lcn;IIII)Z`

**The one item that places two blocks**, and the reason a door placed by any other path deletes
itself: `onNeighborBlockChange` destroys a half that cannot find its other half, so half a door
survives exactly until something beside it changes.

```java
if (side != 1) return false;                       // the top face only
j++;
if (!door.canPlaceBlockAt(world, i, j, k)) return false;
int facing = MathHelper.floor_double(((yaw + 180.0F) * 4.0F / 360.0F) - 0.5) & 3;
// dx,dz from facing: 0 -> +z, 1 -> -x, 2 -> -z, 3 -> +x
// count normal cubes on each side, and look for a door on each side
if ((doorBehind && !doorAhead) || solidAhead > solidBehind) facing = ((facing - 1) & 3) + 4;
setBlockWithNotify(i, j,     k, door); setBlockMetadata(i, j,     k, facing);
setBlockWithNotify(i, j + 1, k, door); setBlockMetadata(i, j + 1, k, facing + 8);
```

Three things a player sees: **a door goes on a top face and nowhere else**; the facing is the
player's heading through a float expression that has to stay in float; and **the hinge mirrors**
against a door or a wall already on one side, which is what makes a pair meet in the middle.

There is no `checkIfAABBIsClear` on this path — ItemDoor asks the block's own condition and never
the world's placement test — so a1.1.2 lets you close a door on yourself.

The table below was measured rather than derived, by using a real door item from a real
`EntityPlayer` at each heading in a real world:

| yaw | 0 | 45 | 90 | 135 | 180 | 270 | -90 |
|---|---|---|---|---|---|---|---|
| facing | 1 | 2 | 2 | 3 | 3 | 0 | 0 |

## Ladders — `ge.A()`, and the two lines that use it

```java
public boolean isOnLadder() {
    int i = MathHelper.floor_double(posX);
    int j = MathHelper.floor_double(boundingBox.minY);
    int k = MathHelper.floor_double(posZ);
    return world.getBlockId(i, j, k) == Block.ladder.blockID
        || world.getBlockId(i, j + 1, k) == Block.ladder.blockID;
}
```

**It reads the box's bottom, not `posY`** — `posY` is the eye — and **it looks at two cells**. The
second is the one that matters to a player: a body is 1.8 tall, so the feet leave a ladder's top
cell before the chest does, and without it the last block of every climb drops you and the first
block cannot be got on to.

The land branch of `moveEntityWithHeading` asks it twice:

```java
if (isOnLadder()) { fallDistance = 0.0F; if (motionY < -0.15D) motionY = -0.15D; }
moveEntity(motionX, motionY, motionZ);
if (isCollidedHorizontally && isOnLadder()) motionY = 0.2D;
```

Which is: **a clamped fall, and a wall you can push into.** There is no separate climb input in
a1.1.2 — the ladder's collision box is two sixteenths thick, so walking at it is what makes
`isCollidedHorizontally` true, and letting go slides you back down at the clamp. Both constants are
written as double literals in the class file, so neither is a widened float.

**There is one here, and it is jump.** On a mouse the wall-press costs nothing, because the hand
holding W is not the hand that aims; on a 3DS the circle pad both steers and looks, so a player who
turns their head to see where they are going stops pressing the wall and slides back down. Jump is
otherwise dead on a ladder and means "up" everywhere else in the game, so `PlayerBody::tick` takes it
as a climb at `kLadderClimb` — the *same* 0.2 a tick the wall-press gives, so neither route is faster
than the other. It is tested before `onGround`, so standing at the foot of a ladder climbs rather
than jumps; a ladder whose bottom rung is a jump you have to land on first is one you fall off. This
is an invented input, like Creative flight below, and it is the only one in the land branch.

**The ladder is drawn as one flat quad**, `0.05F` off the wall — `bc.g` writes four vertices and
stops. Drawing its collision box instead put a face inside the wall it hangs on, which z-fought.

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

## Survival — health, breaking, wear, and the four screens

Everything above this line is the body. This is what a1.1.2 lays on top of it, transcribed the same
way: a name from the class file next to every rule.

The classes: `dm` EntityPlayer, `ge` EntityLiving, `kh` Entity, `nj` PlayerControllerSP, `hq` its
base, `eu` InventoryPlayer, `ev` ItemStack, `di` Item, `bs` ItemTool, `mr` ItemArmor, `oj` ItemFood,
`ly` Block, `dw` CraftingManager, `bv` ShapedRecipes, `ke` TileEntityFurnace, `fe` TileEntityChest,
`ar` Container, `ee` GuiContainer, `lu` GuiIngame, `au` GuiGameOver.

### Taking a hit — `dm.a(Lkh;I)Z`, then `ge.a(Lkh;I)Z`

**The invulnerability window is checked in `dm`, before `ge` ever sees the hit**, and so is death:

    if (E <= 0) return false;                  // already dead
    if (aI > 10) return false;                 // hurtResistantTime; kPlayerHurtResistantTime is 20

**Difficulty scales the damage and only for two sources.** The test is on the *attacker's* class —
`dq` (a monster) or `kg` (an arrow) — so a fall, lava, a cactus, a blast and drowning are the same
on every setting:

| `cn.l` | what happens to the damage |
|---|---|
| 0 Peaceful | 0 — the hit lands and costs nothing |
| 1 Easy | `dmg / 3 + 1` |
| 2 Normal | unchanged |
| 3 Hard | `dmg * 3 / 2` |

**Armour absorbs in twenty-fifths and wears on the way**, carrying the remainder between hits:

    total = dmg * (25 - eu.f()) + carry;       // f() is armourValue
    eu.e(dmg);                                 // damageArmour: every worn piece takes dmg
    dmg   = total / 25;
    carry = total % 25;

`eu.f()` is `(points - 1) * remaining / total + 1` per worn piece, so a nearly-broken helmet is
worth about as much as none. This is why `armourCarry` is saved nowhere: a1.1.2 does not save it
either.

What `ge.a` then does: `prevHealth = health`, the window goes to 20, `health -= dmg`, `hurtTime` to
10, `random.hurt` plays, and the knockback pushes away from the attacker — or, with no attacker,
`attackedAtYaw = (int)(random * 2) * 180`, which is what makes drowning and fire flash the screen
from a direction at all.

### Per-tick harm — `kh.y()` and `ge.y()`, in this order

| what | rule |
|---|---|
| Fall | `ceil(distance - 3)`, so three blocks are free; the landing also plays the step sound at 0.5 volume and 0.75 pitch |
| Fire | 1 every 20 ticks while the fire counter runs |
| Lava | 10, and the fire counter is set to 600 |
| Void | `posY < -64` → 4 a tick |
| Suffocation | 1 while the cell at `floor(posY + 0.12)` is opaque |
| Drowning | the eye in water drains `air` from 300; at **−20** it is 2 damage and `air` goes back to 0. The fire counter is zeroed under water |
| Peaceful | `ticksExisted % 20 == 0` and below full health heals 1 (`dm.j()`) |

`heal` caps at 20 and sets the window to 10, which is why a healed player cannot be hit again for
half a second.

**Death** is `dm.b(Lkh;)V`: the box shrinks to 0.2, `motionY = 0.1`, **the whole inventory is
scattered** — each stack at `posY - 0.3 + 0.12` with a 40-tick pickup delay, a random speed
`rand * 0.5` at a random angle and `motionY = 0.2` — and the eye height drops to 0.1. `deathTime`
counts 20 ticks. `au` (GuiGameOver) is opened the moment health reaches zero with no other screen
up; it does **not** pause the world (`au.b()` is false).

Respawn is `Minecraft.o()`: `cn.a()` first (below), then the old player is removed, a brand-new one
is built at `spawnX + 0.5, spawnY + 1, spawnZ + 0.5`, `bi.q()` lifts it clear of the ground and
`nj.a(dm)` turns it to face yaw −180.

**A corpse picks nothing up.** `dm.j()` wraps the whole nearby-entity sweep — the
`boundingBox.expand(1, 0, 1)` list and the `onCollideWithPlayer` call on each of them — in
`if (health > 0)`. It has to: death scatters the inventory at the player's own feet with a 40-tick
pickup delay, and the game-over screen stands there for far longer than forty ticks. Without the
guard the body vacuums its own grave back up while the player is still reading "Game over!", and
respawns holding everything it just dropped.

### Where a world starts you — `cn`'s constructor, `cn.a()` and `kh.q()`

Four methods, and between them they say something surprising: **y is never searched for.**

    cn.g(int,int)   int y = 63; while (getBlockId(x, y + 1, z) != 0) y++; return getBlockId(x, y, z);
    cn.f(int,int)   g(x, z) == Block.sand.blockID
    cn.a()          if (spawnY <= 0) spawnY = 64;
                    while (g(spawnX, spawnZ) == 0) { spawnX += r(8) - r(8); spawnZ += r(8) - r(8); }
    kh.q()          while (posY > 0) { setPosition(...); if (no colliding boxes) break; posY++; }
                    motionX = motionY = motionZ = 0; rotationPitch = 0;

- `g` **only ever climbs**. It starts at 63 — one below sea level, not at it — and reports whatever
  it stopped on, so a column whose ground is under 63 reports what 63 holds, which over an ocean is
  water and not the sea bed.
- The constructor, on the branch it takes when there is no `level.dat`, sets `(0, 64, 0)` and then
  walks by `nextInt(64) - nextInt(64)` on both axes **until `f` accepts**, with no bound. That one
  test — the top block has to be *sand* — is why every Alpha world begins on a beach. The draws come
  off the World's own unseeded `Random`, the same one that has just flipped `SnowCovered`.
- `cn.a()` runs again on every load and every respawn, and is the weaker test: it only walks off a
  column of pure air, by `nextInt(8) - nextInt(8)`.
- **`spawnY` stays 64** through all of it. What puts the player on top of the ground instead of
  inside it is `kh.q()`, run from `bi.q()` when the body is built, and nothing else.

Two consequences worth writing down. `q()` runs **before** `cn.a(dm)` reads `Player` out of
`level.dat`, so a *saved* player is placed exactly where the file says and is never lifted, however
buried. And a *fresh* player is lifted, which is the only reason `spawnY = 64` is a survivable
number to start at.

The port does all four, with one deviation each way. The walk is bounded — see
`core/world/spawn_point.hpp` — because on this console every step of it is a column generated on the
main thread while the player waits at the menu, a median of about twenty and a tail into the low
hundreds. And the lift is **owed rather than made**: columns stream in behind the player here, so a
lift at the moment the body is built would find air, decide there was nothing to climb out of, and
leave the player to be buried by the terrain arriving underneath them. It is paid on the first tick
the body's own column is resident, and until then the body is held still — which is what
`respawnPending` in `platform/ctr/main.cpp` already did for a respawn.

### The save tags

`Health` → `E`, `HurtTime` → `G`, `DeathTime` → `J`, `AttackTime` → `K`, `Fire` → `aT`,
`Air` → `aX`. **A file with no `Health` reads as 10, not 20** — that is the field's initialiser in
the read path, and it is what a world saved by another tool will give you.

### Breaking a block — `nj`

    a(IIII)  clickBlock       // also runs onBlockClicked, and breaks at once if hardness >= 1
    c(IIII)  damageBlock      // once a tick while the button is held
    a()      resetBlockRemoving

- A click on a **new** cell only records it that tick; the progress starts on the next.
- After a break there is a **five-tick delay** before anything can be hit again.
- Progress accumulates `ly.a(Ldm;)F` — the *relative* hardness — and the block goes at 1.0.
- The step sound plays every fourth tick, at volume `(b + 1) / 8` and pitch `c * 0.5`.
- `reset()` clears the progress and the delay **only if something was being hit**.
- `cn.i` puts out a fire on the struck face, which is why a click on a burning block does that
  first and breaks nothing.

**Relative hardness**, which is the whole of how long a block takes:

    hardness < 0            -> 0            // bedrock: never
    !dm.b(ly)               -> 1 / h / 100  // cannot harvest it: 100 ticks a point
    otherwise               -> s / h / 30   // s is dm.a(ly), the tool's speed

and `dm.a(Ldm;)F` divides `s` by 5 with the eye in water and by 5 again off the ground — so mining
while swimming and jumping is 25 times slower.

**Harvest order matters and is not the order it reads as.** `b(III)` destroys the block, *then*
wears the held item, *then* asks `canHarvestBlock`, *then* drops. A pickaxe on its last point of
durability breaks the stone and drops nothing, because by the time the question is asked the hand
is empty.

### Wearing things out — `ev.b(I)V`

Past `getMaxDamage` the stack loses one and the damage goes back to zero, which is how a stack of
tools works. A hit on a mob costs **2** on a tool and **1** on a sword; breaking a block costs
**1** on a tool and **2** on a sword — the sword is the expensive way to dig on purpose.

Using something up: every `ItemBlock` placement, a door, reeds, seeds, redstone, a sign, a
painting, a boat, a minecart and a saddle spend one on success; a hoe and flint and steel take a
point of damage; food spends one and heals; the bow needs an arrow and takes it from the first
slot that has one (`eu.b(I)`).

### The HUD — `lu.a(FZII)`

Hearts at `x = w/2 - 91 + i*8`, `y = h - 32`. The container tile is `(16 + flash*9, 0)`, where the
flash is on while `(aW/3) % 2 == 1 && aW >= 10`; while it flashes the **previous** health is
outlined with `(70,0)`/`(79,0)` behind the current `(52,0)`/`(61,0)`. At four health or less every
heart jitters by `new Random(h*312871).nextInt(2)` pixels. Armour runs the other way from
`x = w/2 + 91 - i*8 - 9` with `(16,9)`, `(25,9)`, `(34,9)`; bubbles sit a row above the hearts,
`(16,18)` whole and `(25,18)` popping.

**Ported to the top screen, and moved on it**, which is the one deliberate departure in this
section. The row positions above are all measured from a hotbar that is not there: a1.1.2 centres
the hearts on the hotbar at the bottom of the same screen, and this port's hotbar is on the other
screen. So the hearts go to the top **left**, the armour to the top **right**, the bubbles to the
row **below** the hearts rather than above them — above is now the screen edge — and every cell is
drawn at `kHudTexelUnits` (30 sixteenths of a pixel, so 1.875) times its texel size, which is what
makes a 9-pixel heart legible on a 400 x 240 panel. It was a flat 2 and came down 6.25 % later;
see `status.md` 48. The arithmetic inside a row is untouched: same halves, same flash, same one-pixel
jitter (scaled with the rest), same bubble count. See `docs/3ds-performance.md` §11.

### The container screens — `ee.a(III)V`

One cursor stack, and the rules a click follows:

- **Pick up**: the whole stack with the left button, half rounded up with the right. A result slot
  always gives the whole thing.
- **Put down**: all with the left, one with the right, capped at the slot's limit, and only if the
  slot accepts it.
- **A different item** swaps, but only when the cursor holds no more than the slot's limit.
- **The same id** merges, clamped by the limit and the stack size.
- **A slot that refuses** a stack it already holds more than one of, when the cursor's would fit,
  takes the lot onto the cursor and runs `onPickupFromSlot`.
- **Outside the window**: the left button throws the whole cursor, the right throws one.

Closing drops the cursor (`ar.a(Ldm;)V`) and, on the two crafting screens, whatever is in the grid.
A chest's and a furnace's contents stay where they are.

**Crafting is shaped only in a1.1.2** — there is no shapeless recipe class. `dw` walks its list in
order and the first match wins; a recipe is tried at every offset that fits, **mirrored first and
then straight**, and every cell of the 3×3 has to agree (−1 meaning blank). The 2×2 grid is
matched as the top-left corner of the 3×3. Taking the result spends one from every cell that had
anything in it.

Slot order, which is what a click's index means: the result, the grid, the armour (helmet first,
held in `armorInventory[3 - i]`), then the backpack (slots 9..35) and then the hand (0..8) — the
last two in that order in all four constructors.

### The furnace — `ke.b()`

    burn--
    if (burn == 0 && canSmelt) { burn = currentBurn = fuelValue(slot 1); consume one fuel }
    if (burning && canSmelt)   { if (++cook == 200) smelt() } else cook = 0

Fuel: any wood-material block 300, a stick 100, coal 1600, a lava bucket 20000. Seven things smelt.
The lit/unlit swap (`ku.a`) keeps the metadata **and the tile entity** — writing the block without
putting the entity back is how a furnace forgets what it was cooking. Breaking a furnace spills
nothing; breaking a chest spills everything, each stack at `rand * 0.8 + 0.1` inside the cell, in
clumps of `nextInt(21) + 10`, with `gaussian * 0.05` motion and `+0.2` upward.

A chest refuses to open with an opaque block on it — or on the chest next to it. A double chest
nests −x/−z first and +x/+z last (`hs`), which is why the left half is slots 0..26.

## What is not derived yet

- ~~Fall damage, and everything else that belongs to Survival rather than to the body.~~ —
  **derived**, and it is the section above.
- ~~`isOnLadder`, `isInWater` and `isInLava`~~ — **all three are written**, and the ladder is the
  last of them. See *Ladders* below.
- ~~`Block.onBlockPlaced`~~ — **derived**, as a generated table: `tools/genref.java --place` sweeps
  every block against every face at sixteen headings, and `block::placementMetadata` reads the
  result. Nothing varies with the heading, so the table is per-face and complete. The one thing a
  `(id, face)` table cannot hold is the floor lever's `5 + rand.nextInt(2)`, and that is rolled in
  `tick::leverPlaced` instead. See *Which way a placed block faces* above.
- The replaceable-material test. Placement goes into air only; a1.1.2 also replaces water, lava and
  snow. Survival landed without it -- it is a placement question rather than a Survival one, and
  nothing above depends on the answer.
