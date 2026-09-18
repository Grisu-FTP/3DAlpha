# The mobs in a1.1.2

Written 2026-09-12, when the pig, the sheep, the cow and the chicken landed, and extended the same
day when the five monsters followed. This is the derivation: the class map, every constant as it
appears in the class file, the two methods that turn out to be bugs, and the line between what
a1.1.2 does and what this port adds.

The first half is the four animals. **The monsters start at *The five hostiles*.**

Read it before changing anything in `core/entity/mob*`, `core/entity/path_finder` or
`core/render/mob_mesh`. The code cites it; it does not repeat it.

## The classes

| What it is | Class | Notes |
|---|---|---|
| `Entity` | `Entity` | `moveEntity`, `onEntityUpdate`, `applyEntityCollision`, `dropItem` |
| `EntityLiving` | `EntityLiving` | health, hurt, death, `moveEntityWithHeading`, the aimless wander |
| `EntityCreature` | `EntityCreature` | the path-following wander |
| `EntityAnimal` | `ag` | `getBlockPathWeight`, `getCanSpawnHere`, talk interval 120 |
| `EntityPig` | `mv` | `/mob/pig.png`, 0.9 x 0.9, saddle, raw porkchop |
| `EntitySheep` | `bo` | `/mob/sheep.png`, 0.9 x 1.3, fleece, wool |
| `EntityCow` | `am` | `/mob/cow.png`, 0.9 x 1.3, bucket, leather |
| `EntityChicken` | `mz` | `/mob/chicken.png`, 0.3 x 0.4, egg clock, feather |
| `SpawnerAnimals` | `SpawnerAnimals` | built by `PlayerControllerSP` (PlayerControllerSP), run every tick |
| `PathFinder` | `PathFinder` | A*, four neighbours, step one, drop four |
| `PathEntity` / `PathPoint` | `bl` / `a` | the route and its nodes |
| `ModelQuadruped` | `hg` | pig, sheep and cow are all this plus overrides |
| `ModelPig`/`ModelSheep2`/`ModelSheep1`/`ModelCow`/`ModelChicken` | `ca`/`gx`/`bx`/`dv`/`kv` | |
| `RenderLiving` | `RenderLiving` | the transform, the extra pass, the hurt flash |
| `RenderPig`/`RenderSheep`/`RenderCow`/`RenderChicken` | `gm`/`ns`/`mc`/`eq` | |
| `ItemSaddle` | `jw` | `useItemOnEntity`, not `interact` |
| `EntityList` | `ew` | the save ids -- and see the bug below |
| `IMob` | `co` | a **marker interface**, and what the 200-cap spawner counts |
| `EntityMob` | `EntityMob` | `EntityCreature` plus a target, a fist, and a reason to spawn in the dark |
| `EntityZombie`/`EntitySkeleton`/`EntityCreeper`/`EntitySpider` | `mb`/`cw`/`dd`/`ax` | |
| `EntitySlime` | `ma` | **not an `EntityCreature` at all** -- `EntityLiving` plus `co` |
| `Explosion` | `Explosion` | one class, one method, reached through `World`'s method |
| `MonsterSpawner` | `k` | `SpawnerAnimals` with one method overridden |
| `TileEntityMobSpawner` | `TileEntityMobSpawner` | **the dungeon cage** -- a tile entity, not a world sweep |
| `BlockMobSpawner` | `bj` | `BlockContainer` (BlockContainer) and three one-line overrides |
| `TileEntityMobSpawnerRenderer` | `r` | the miniature: nine GL calls and a factor of ten |
| `ModelBiped`/`ModelZombie`/`ModelSkeleton` | `ModelBiped`/`cb`/`fv` | |
| `ModelCreeper`/`ModelSpider`/`ModelSlime` | `em`/`jy`/`hh` | |
| `RenderCreeper`/`RenderSpider`/`RenderSlime` | `d`/`ok`/`gq` | each with a `preRenderCallback` |

**`ew`'s numeric ids are 90, 91, 91, 91.** Pig is 90; sheep, cow and chicken are *all* 91. That is
a bug in a1.1.2 and it is why nothing anywhere keys an animal by its number: the save uses the
string (`"Pig"`, `"Sheep"`, `"Cow"`, `"Chicken"`), and so does `MobDef::saveId`. The monsters are
50 to 55 and are distinct, and `ew` also registers **`hl` "Giant" at 53** -- a class that exists,
extends `EntityMob`, is drawn at six times scale by `nz`, and that nothing in the game ever constructs.
It is not ported and it is not a gap.

## The four, as numbers

| | Pig | Sheep | Cow | Chicken |
|---|---|---|---|---|
| `setSize` | 0.9 x 0.9 | 0.9 x 1.3 | 0.9 x 1.3 | 0.3 x 0.4 |
| Health | 10 | 10 | 10 | **4** |
| `getDropItemId` | raw porkchop | **none** | leather | feather |
| How many | `rand(3)` -- so 0, 1 or 2 | -- | `rand(3)` | `rand(3)` |
| Living sound | a field of another original class | `mob.sheep` | a field of another original class | `mob.chicken` |
| Hurt sound | a field of another original class | `mob.sheep` | `mob.cowhurt` | `mob.chickenhurt` |
| Death sound | `mob.pigdeath` | `mob.sheep` | `mob.cowhurt` | `mob.chickenhurt` |
| `getSoundVolume` | 1.0 | 1.0 | **0.4** | 1.0 |
| Its one boolean | `Saddle` | `Sheared` | -- | -- |

All four declare `public boolean a` and only two of them ever write it. That is why `Mob::flag` is
one field rather than two.

**A sheep drops nothing when it dies.** `EntityLiving`'s method -- getDropItemId -- answers zero and `bo` does not
override it. Wool comes out of `attackEntityFrom` instead, once, and only when an `EntityLiving`
did the hitting:

When an unsheared sheep is hurt by a living attacker it becomes sheared and drops one to three
wool blocks (1 plus a draw below 3). Each is nudged: vertical speed gains up to 0.05, and each
horizontal axis gains the difference of two random floats times 0.1.

So a sheep that suffocates, drowns or falls into the void keeps its coat, and one that is punched
loses it whether or not the punch kills it. There is no shearing with shears in this version and
there is no wool colour: `bo` has no colour field at all.

## The tick, in order

`EntityLiving`'s method is `onUpdate` and it runs three things in this order. Swapping the last two draws an
animal facing where it was going *last* tick.

1. **`Entity`'s method / `EntityLiving`'s method -- the counters.**
   - the idle noise: `if (rand.nextInt(1000) < livingSoundTime++) { livingSoundTime = -120; play }`
     -- the draw happens before the increment, so the odds climb from nothing to certainty over two
     minutes;
   - water: splash sound and bubbles on entering, `fallDistance = 0`, `fire = 0`;
   - fire: one damage every twentieth tick while it burns;
   - lava: ten damage and `fire = 600`;
   - the void: below y = -64, `EntityLiving`'s method is `attackEntityFrom(null, 4)` a tick -- **not** an instant
     kill, which is `Entity`'s behaviour and not `EntityLiving`'s;
   - suffocation inside an opaque block, one a tick; drowning at `air == -20`, two a tick;
   - `hurtTime`, `attackTime` and `hurtResistantTime` count down;
   - at zero health, `deathTime` counts up and at **20** the entity is removed in a puff of
     twenty `explode` particles.

2. **`EntityLiving`'s method -- onLivingUpdate.** Asks the AI (`updatePlayerActionState`), then:
   while jump is held, being in water or lava adds 0.04 to vertical motion and otherwise a grounded mob jumps; strafe and forward are multiplied by 0.98 and the random yaw velocity by 0.9; `moveEntityWithHeading` runs with the result; and finally every other entity inside the box widened by 0.2 on x and z that can be pushed gets a collision applied against this mob.
   `moveEntityWithHeading` is `EntityLiving`'s method and it is **the player's own** -- the same land branch,
   the same water and lava branches, the same ladder, the same ground friction. See
   `docs/physics-a1.1.2.md`; the only difference between a pig and a player in it is `width`,
   `height` and `yOffset`.

3. The body's heading chases the direction it actually travelled, at 30 % a tick, clamped to 75
   degrees away from where the head is looking. That clamp is what makes an animal turn its head
   first and its body after.

**`yOffset` is 0 for a mob.** Only `EntityPlayer` sets it to 1.62, so `posY` is a mob's feet and
`Pos[1]` in its save is the feet too. The player is the odd one out, not the animals.

## The AI

`EntityCreature`'s method -- `EntityCreature.updatePlayerActionState`. An animal never has an `entityToAttack`
(`ag` does not override `findPlayerToAttack`), so the attack half of the method is dead and what
is left is:

With no path, and a one-in-80 draw, or independently on another one-in-80 draw, it samples ten
random points within 6 blocks horizontally and 3 vertically, scoring each with the mob's own
cell-weight function and keeping the best, and if one was found it plans a path there with a
10-block limit. If it still has no path, or a one-in-100 draw succeeds, it falls back to the base
idle behaviour and drops the path and ends the tick.

Otherwise it follows the path. It reads the next waypoint and skips each one that is within
twice its width horizontally. It stops jumping, and if a waypoint remains it turns at most 30
degrees towards it and sets forward speed to the flat 0.7 move speed, and jumps if the waypoint is
above the cell at its feet (box bottom plus a half, floored). It also jumps when it has hit
something horizontally, and with an 80% chance while in water or lava.

a method of another original class -- getBlockPathWeight -- is two lines and is the whole reason animals are found
standing in fields:

A cell scores 10 if the block below is grass; otherwise its score is its brightness minus 0.5.

`EntityLiving`'s method is the fallback, and **it sets `moveForward` to zero**: an animal with no path stands
still and turns its head rather than milling about. It also carries the despawn:

Each tick its age increases and the nearest player, at unlimited range, is looked up. If there
is one: farther than 128 blocks (squared distance above 16384) and it is removed. Past age 600,
a one-in-800 draw either resets the age when the player is within 32 blocks (squared distance
below 1024) or removes it.

**Animals despawn in a1.1.2.** Beta 1.8 is where that stopped; here a herd left alone thins itself
out. `attackEntityFrom` resets `entityAge`, so an animal being fought does not vanish mid-fight.

## The pathfinder, and the loop that is not a loop

`PathFinder`'s method -- getVerticalOffset -- is meant to test the whole volume an entity occupies.
It does this:

```
for (int i = x; i < x + size.xCoord; i++)
  for (int j = y; j < y + size.yCoord; j++)
    for (int k = z; k < z + size.zCoord; k++) {
        Material m = world.getBlockMaterial(x, y, z);      // <- x, y, z, not i, j, k
        if (m.blocksMovement()) return 0;
        if (m == Material.water || m == Material.lava) return -1;
    }
return 1;
```

The body reads the **parameters** rather than the loop variables, so every iteration tests the same
cell and the entity's size never reaches the world: a1.1.2 paths a cow as though it were a point.
Later versions pass `i, j, k`, which is when mobs started refusing gaps they do not fit through.

**It is reproduced rather than fixed** (`entity::kSizeIsIgnored`), against this project's usual
rule, because it is not a technical limit the hardware imposed -- it is what the search actually
explores. Fixing it would change every path a mob takes and add a volume test per node on a
268 MHz ARM11.

What *is* fixed: `a`'s identity. Its constructor packs `x | y << 10 | z << 20` into a field and
`equals` compares only that, so two cells 1024 apart -- or any cell with a negative coordinate --
are the same node to the original. That is a hash written as an identity; the port hashes the same
way and then compares the three coordinates.

The rest is an ordinary A*: four neighbours, no diagonals, a step up of one **only when the cell
above the node being left is clear**, a drop of at most four, and a `maxDistance` (10 for an
animal) measured from the **target**, not from the mob.

**One budget is ours** and it is named in `core/entity/path_finder.hpp`: 1,024 nodes per search.
It stays because the jar's `IntHashMap` grows without bound and an unbounded arena means allocating
inside the 20 Hz tick, which the platform forbids; `PathFinder::exhausted()` says when it binds.

There was a second -- one search per tick across every mob in the world -- and it was **removed on
2026-09-17**. It never bound on fifteen animals (0.4 searches a tick between them) and bound hard
once the monsters landed: two hundred of them, a chasing one re-asking once in twenty ticks, is ten
requests a tick against a budget of one. Worse, the gate sat in front of the wander branch's ten
candidate draws, which the jar makes whether or not a path comes of them, so the second mob to want
a path in a tick skipped thirty `nextInt` calls and shifted the stream for every mob ticked after
it. `MobSystem::peakSearchesPerTick` counts what it used to cap.

## Where animals come from

`SpawnerAnimals` -- SpawnerAnimals -- built by `PlayerControllerSP` as `new az(15, ag.class, {bo, mv, am, mz})` and run by
`PlayerControllerSP`'s method, which `Minecraft.runTick` calls every tick the game is not paused.

**There is no world-generation spawning in this version.** `SpawnerAnimals` is referenced by `PlayerControllerSP` and by
nothing else; no chunk populator makes an animal. A fresh world is empty and fills up while
somebody stands in it.

One tick:

If the world holds fewer than 15 animals, three spawn passes of one animal each run.

and `performSpawning` walks the 9 x 9 of chunks around each player, trying one chunk in ten:

Each pass picks one of four classes at random and a position inside the chunk: a random x and z
within its 16 by 16, and a height from 0 to 127. If that cell is a normal cube, or its material
is not air, **the whole pass ends with zero spawns**.

Otherwise it tries three groups. Each group starts from that point and makes two attempts. An
attempt moves x and z by the difference of two draws below 6, and y by the difference of two
draws below 1, which is always zero. A cell is a valid spot when it has a normal cube below, is
not itself a normal cube, is not liquid, and has no normal cube above. The spawn point is its
centre in x and z. It is skipped when any player is within 24 blocks or the spot is within 24 of
the world spawn (squared distance below 576). Otherwise an entity of the chosen class is made
there with a random yaw, and if it passes its own spawn check it is counted and added.

Three things in there shape the rate and all three are kept:

- **those two `return 0`s end the pass**, not the chunk -- and since `y` is a flat `rand(128)`,
  most first draws land in rock, so most passes stop at the first chunk they look at;
- `rand(1) - rand(1)` is **always zero**, so a group never moves in y;
- the cap is checked once, before the three passes, so a tick that starts at 14 animals can finish
  at 17.

a method of another original class -- getCanSpawnHere -- is the last refusal: **grass** under the feet, a light level
**above 8** (`getFullBlockLightValue`, so the day's subtraction applies and animals stop spawning
at dusk), a box clear of blocks, entities and liquid, and a non-negative `getBlockPathWeight`.

### The order of the 9 x 9, which is not a detail

`SpawnerAnimals` collects the eligible chunks into a `HashSet<ol>` and iterates that, and the obvious reading
is that a set has no order worth reproducing. That reading is wrong, and measurably so: the two
`return 0`s above mean **whichever chunks the set hands over first are the only part of the square
most passes ever look at**. Measured over 18,000 passes of a real world, 17,986 of them ended on
their first or second chunk. Iterating row-major -- which this did -- put every monster in the
northern rows and none at all in the southern third.

Two facts fix the order:

- **`ol.hashCode()` is `(x << 8) | z`.** That is a terrible hash and that is the point: for any
  negative z the sign bits fill everything above bit 7, the x half is swallowed whole, and all nine
  columns of a row collapse on to one hash.
- **`HashMap` files a key under `(h ^ (h >>> 16)) & (n - 1)`** and iterates bucket by bucket, each
  bucket in insertion order. Eighty one entries resize the table to **128** and stop there, and
  `clear()` keeps the table it grew, so 128 is the table every pass iterates.

Insertion order is `SpawnerAnimals`'s own -- x outer, z inner, each -4 to 4 -- so the order is a stable
function of the player's chunk. `entity::eligibleOrder` computes it; `tests/monster_test.cpp` pins
it against output from a real JVM.

**One stated deviation.** A bucket of eight or more becomes a red-black tree and `moveRootToFront`
puts the tree's root at the head of the chain, so Java hoists exactly one entry of each nine. Which
one is the outcome of balancing nine insertions, and it is JVM-version-dependent besides -- Java 7
spread hashes differently and had no treeified bins. The bucket partition and the bucket order are
exact; the hoist is not reproduced.

### Why the surface is so quiet at night

**The dungeon cage is a different thing entirely.** `TileEntityMobSpawner` is a *tile entity*: one cell that counts
down while a player is within sixteen blocks and throws four candidates at the ground around
itself. It shares no code with `SpawnerAnimals`/`k` and only the word "spawner". Its derivation is in
`src/core/entity/mob_spawner.hpp` and the write-up is `docs/status.md` 38; the one thing worth
repeating here is that `getCanSpawnHere` is reached through a **virtual** call, so a dungeon's
zombies obey the same light rule `EntityMob`'s method imposes on `k`'s.

`k` -- the monster spawner -- is `SpawnerAnimals` with three changes: a cap of 200, `co.class` instead of
`ag.class`, and a y draw of **`rand(rand(120) + 8)`** instead of `rand(128)`. That draw piles up
near the bedrock, and the group loop's `gy += rand(1) - rand(1)` never moves it. So a **surface**
spawn needs the draw to land on the local ground height exactly, while a draw that lands inside a
cave is already standing in one.

Measured, held at midnight over 30,000 ticks of a real generated world: **806 monsters, 14 of them
above y 64**, and **98.9% of all drawn positions had no floor**. Monsters in a1.1.2 are a cave
population that occasionally surfaces, and every step of that was checked against
`SpawnerAnimals`'s method and a method of another original class.

`build-host/3dalpha --spawns <world-dir>` is the harness those numbers came from: the real
streamer, a generated world, the console's own tick loop, and `SpawnCounters` on the spawner. The
Info page shows the same three counters on hardware.

## What a right click does

a method of another original class -- `EntityPlayer.useCurrentItemOnEntity` -- is two steps:

The target's own interaction handler goes first, and if it returns true the click is spent. Only
if it does not, and the player holds an item and the target is a living entity, the item's
use-on-entity routine runs.

- a method of another original class -- a **cow** holding out a bucket (`Item.bucketEmpty`) hands back a milk bucket, in
  place, in the same slot;
- a method of another original class -- a **saddled pig** mounts the player and refuses otherwise;
- a method of another original class -- `ItemSaddle`, which is the *second* step: a saddle held out to an unsaddled
  pig is spent on it. Held out to a saddled one, `interact` has already answered, so the saddle is
  not eaten -- you climb on instead.

A pig in this version **cannot be steered**: `mv` reads nothing from `riddenByEntity` and there is
no carrot on a stick until 1.4. A saddled pig goes where its own AI says and takes you with it.

There is no clock, no shears, no wheat use beyond bread, and `ew` has no `Egg` entity -- so an egg
in a1.1.2 is an item and nothing else. `mz`'s own clock lays one every `6000 + rand(6000)` ticks,
five to ten minutes, with `mob.chickenplop`.

## Drawing one

`RenderLiving`'s method -- RenderLiving:

The render transform runs in this order: translate to the interpolated position; rotate about the
vertical axis by 180 degrees minus the interpolated body yaw; if the mob is dying, tip it over
around the forward axis by 90 degrees times the smaller of 1 and the square root of
(death time plus the partial tick minus 1, over 20, times 1.6); flip it with a scale of
(-1, -1, 1), because the model is built upside down; translate down by 24 sixteenths plus
0.0078125; and render the model with limb swing, limb yaw, age plus partial tick, head yaw
relative to the body, pitch, and a scale of one sixteenth.

That last translate runs **inside** the `-1` scale, so it lifts the model rather than sinking it;
the port composes the whole thing into an origin and three axes (`render::Placement`). Getting the
sign wrong buries every animal to the ears, which is what
`a_drawn_animal_stands_on_its_own_feet` exists to catch.

The models are transcriptions; `core/render/mob_mesh.cpp` carries the boxes. Two of the four are
drawn twice: `ns` adds the fleece while a sheep is unshorn and `gm` adds the saddle while a pig is
saddled, each the same model grown and drawn from its own page.

**The hurt flash is a second pass in the original** -- the whole model again in
`glColor4f(brightness, 0, 0, 0.4)` with the depth test on equal. A `DetailVertex` carries its own
colour here, so the port pulls green and blue down instead: one pass, no blending, and a white
sheep flashes pink rather than red.

## The five hostiles

`PlayerControllerSP` builds two spawners and the monsters' is the first:

```
new k(this, 200, co.class, {mb, cw, dd, ax, ma})   // 200 monsters
new az(15, ag.class, {bo, mv, am, mz})             // 15 animals
```

**`co` is an interface and `ag` is a class.** That one difference is why the slime counts towards
the monster cap without being an `EntityCreature`, and it is why `MobDef` carries a `hostile` *column* rather
than the port testing `type >= Zombie`.

### `EntityMob` -- EntityMob, which is five short methods

The basic hostile mob starts with attack strength 2 and health 20. In its living update, brightness
above 0.5 adds 2 to its age (so it despawns faster in the light). Its update removes it when the
difficulty is peaceful. It targets the closest player within 16 blocks that it can see. When it
is hurt and the base class accepts the damage, the attacker becomes its target unless the
attacker is its rider, its mount, or itself.

Melee does nothing at 2.5 blocks or more, or when the attacker's vertical extent does not overlap
the target's. Otherwise it sets its cooldown to 20 and damages the target by its attack
strength. Its path weight at a cell is 0.5 minus the cell's light brightness, so it prefers the
dark. It can spawn where the sky light is at most a draw below 32, the block light is at most a
draw below 8, and the base spawn check also passes.

Four of those are the whole difference between a monster and a cow:

- **`getBlockPathWeight` is `ag`'s with the sign reversed.** An animal scores grass at ten and
  everything else at `brightness - 0.5`; a monster scores `0.5 - brightness`. One method is why
  animals are found in fields and monsters in caves, and the same method is the last clause of
  `getCanSpawnHere` (`EntityCreature`'s method asks for a weight of at least zero), so it is also why a monster
  cannot spawn anywhere brighter than half.
- **`getCanSpawnHere`'s sky light is the *stored* value**, before the day's subtraction. So the
  time of day changes nothing about *where* a monster may spawn -- only about whether it burns
  afterwards. A roofed cave qualifies at noon and an open field does not at midnight.
- **Peaceful is a removal, not a suppression.** `world.difficulty == 0` kills the monster at the
  *end* of the tick it notices, so a1.1.2 on Peaceful still runs the 200-cap spawner, still makes
  the entities and still pays for them.
- **`entityAge += 2` in the light.** `entityAge` is what despawns a mob, so a monster that survives
  the morning in the open is gone in half the time one in a cave is.

`EntityCreature`'s attack branch, which was dead for the animals, is now the live half:

Each pass first clears the attacked flag. With no target it looks for one and, if it finds one,
paths to it with a 16-block limit. With a living target it measures the distance and, if it can
see the target, calls its attack routine with it; a dead target is dropped. Then, if it has not
attacked and has a target, it recomputes the path when there is none or a one-in-20 draw
succeeds. Otherwise it falls through to the ten-sample wander, taken when there is no path and a
one-in-80 draw succeeds, or independently on another one-in-80 draw.

**The two branches are exclusive**, which is what stops a monster that is chasing you from also
wandering: it re-asks for the route one tick in twenty and never draws the ten wander cells at all.
And `hasAttacked` -- set by the skeleton and the creeper -- does two things: it stops the path being
re-asked on the tick a shot went off, and further down it converts the path's forward into a
**sidestep about the target's heading**, which is why a skeleton circles rather than marching in.

### The four, as numbers

| | Zombie `mb` | Skeleton `cw` | Creeper `dd` | Spider `ax` |
|---|---|---|---|---|
| `setSize` | *(none)* 0.6 x 1.8 | *(none)* | *(none)* | **1.4 x 0.9** |
| Health | 20 | 20 | 20 | 20 |
| `attackStrength` | **5** | 2 | 2 | 2 |
| `moveSpeed` | **0.5** | 0.7 | 0.7 | **0.8** |
| Living sound | `mob.zombie` | `mob.skeleton` | **none** | `mob.spider` |
| Hurt sound | `mob.zombiehurt` | `mob.skeletonhurt` | `mob.creeper` | `mob.spider` |
| Death sound | `mob.zombiedeath` | `mob.skeletonhurt` | `mob.creeperdeath` | `mob.spiderdeath` |
| Drop | feather (288) | arrow (262) | gunpowder (289) | string (287) |
| Burns at dawn | yes | yes | no | no |

**The creeper has no living sound**, and that is `EntityLiving`'s method returning null with no override -- it is
the whole of why one gets behind you. The slime has none either. **A zombie drops feathers** in
this version; rotten flesh is 1.8's.

### `mb` and `cw` -- catching fire

The same eight lines in both classes, and it is the only thing the zombie's `onLivingUpdate` does:

In daytime it reads its own brightness. If that is above 0.5, it can see the sky from its cell,
and a random float times 30 is below twice (brightness minus 0.4), its burn counter is set to 300.

`isDaytime` is `skylightSubtracted < 4` -- the *world's*, not the cell's. The draw is what makes it
gradual: at full daylight `(1.0 - 0.4) * 2 = 1.2` against `rand() * 30`, which is about one tick in
twenty-five, so a zombie caught at dawn smoulders for a second or two before it goes up rather than
igniting on the exact tick the sun clears the hill.

### `cw` -- the bow

Within 10 blocks, with its cooldown at zero, the skeleton creates an arrow owned by itself, raised
1.4 above its position, and aims it at 0.2 below the target's y. It adds an arc to the vertical
component equal to the horizontal distance times 0.2, plays the bow sound at a pitch of one over
(0.8 plus up to 0.4), adds the arrow to the world, launches it at speed 0.6 with a spread of
12, and sets the cooldown to 30. Whether or not it fired, it turns to face the target by the
arctangent of the offset, rotated a quarter turn, and counts as having attacked.

Three things in there are worth naming. **The skeleton does not close**: it fires from wherever it
is inside ten blocks and `hasAttacked` stops the path being re-asked. **The arrow is slow and
inaccurate** -- 0.6 velocity and 12.0 inaccuracy against the player's 1.5 and 1.0 -- which is why
one at range misses and one at three blocks does not. And the arrow starts *inside the skeleton's
own box*: the constructor places it at the shooter, steps it 0.16 out along the facing and drops it
0.1, and `cw` then lifts it 1.4 -- so without `EntityArrow`'s method's `entity != shootingEntity ||
ticksInAir >= 5` it would shoot itself on the tick it is loosed.

### `dd` -- the fuse

Each tick the creeper first copies its fuse counter into its previous-fuse value. If the counter
is positive and the creeper is not primed, it counts down; a state of zero or above is lifted to 2;
the base behaviour runs; then any state other than 1 is reset to -1 (so priming has to be
renewed every tick).

The attack routine fires when the state is at most zero and the target is under 3 blocks away,
or when the state is above zero and it is under 7. On the first fuse tick it plays the fuse sound
at pitch 0.5. It then sets the state to 1 and increments the counter, and when the counter
reaches 30 it explodes with power 3 at its own position and dies. Either way it counts as having
attacked. If the killer was a skeleton, its death drops one of two music discs (the first of the
pair plus a draw below 2).

- **Three blocks to light and seven to stay lit.** A creeper you back away from keeps hissing for a
  while and then gives up *without exploding*, because the fuse only advances on the ticks this
  method runs -- and `updatePlayerActionState` winds it back on every tick it does not.
- `creeperState` is a one-tick flag written as a three-state: set to 2 before the AI runs, to 1 by
  the AI, and back to -1 afterwards unless the AI set it to exactly 1.
- **A creeper that explodes drops nothing and makes no death sound.** `setEntityDead` is not a
  death: it never reaches `onDeath` and never spends the twenty ticks. Gunpowder is only ever from
  one you killed.
- **`onDeath`'s music disc is the only source of a record in a1.1.2.** Nothing crafts one, no chest
  generates one, and no other entity drops one. A player who wants `record13` or `cat` has to get a
  skeleton to shoot a creeper. The two ids are 2256 and 2257, **1,910 past the end of the item
  run**, so they reach `item::def` through a side table rather than through the contiguous array --
  see `core/item/registry.hpp` and status.md 26. `ItemRecord`'s method puts one in a jukebox (`Block`'s field, id 84)
  and plays it, **and both halves of that work here**: the disc is kept in the jukebox's own block
  metadata (`1 + (item - record13)`, no tile entity) and `.mus` decodes. See
  `core/tick/behaviour.cpp`'s `ejectRecord` and `core/audio/vorbis_stream.hpp`.

### `ax` -- the spider

A rider sits at three-quarters of the spider's height minus a half. It only looks for a player
within 16 blocks when its own brightness is under 0.5, so it is passive in daylight. In its
attack routine, when it is bright and a one-in-100 draw succeeds it drops its target. Otherwise,
with the target between 2 and 6 blocks away and a one-in-10 draw succeeding, a spider on the
ground leaps: each horizontal velocity component becomes the unit direction to the target times
0.4 plus 0.2 times its old velocity, and vertical velocity is set to 0.4. Outside that range it
uses the ordinary melee attack.

- **Daylight is a forgetting, not a burning.** A spider in a bright cell will not pick a target and
  drops the one it has a tick in a hundred. Nothing sets it alight.
- `findPlayerToAttack` **does not ask `canEntityBeSeen`**, unlike `EntityMob`'s, so a spider on the far
  side of a wall still starts walking.
- **The leap has no melee fallback.** In the 2-to-6 window, one tick in ten, a spider *not* on the
  ground simply does nothing that tick.
- **There is no wall climbing in this version.** `ax` overrides no ladder method and has no
  `isBesideClimbableBlock`; that arrives later. A spider here is a fast, wide, leaping mob and
  nothing more.

### `ma` -- the slime, which is not an `EntityCreature`

A new slime picks its size as 1 shifted left by a draw below 3 (so 1, 2 or 4), has no vertical
model offset, and a jump timer of 10 plus a draw below 20. Setting a size sets the hitbox to 0.6
times the size in both dimensions, health to the size squared, and re-places it.

The spawn check passes when the size is 1 or the difficulty is above peaceful, a one-in-10 draw
succeeds, a random seeded from the chunk with a fixed constant also draws a one-in-10, and the
height is below 16. On death, a slime above size 1 whose health reached zero first spawns four
children at half its size, and then dies. Touching a player hurts them for its size, but only
if it is above size 1, can see them and is within 0.6 times its size, and the attack sound plays
when the hit lands. It drops a slimeball at size 1 and nothing otherwise, and its sound volume
is 0.6.

and `updatePlayerActionState` is its own, with no `super` anywhere in it:

It looks for the nearest player within 16 blocks and, if there is one, turns to face it (at most
10 degrees a tick). When it is on the ground and its jump timer counts down past zero, the timer
is reset to 10 plus a draw below 20 (divided by 3 when a player is near), it jumps, plays the
squish sound if larger than size 1 (pitch from two random draws), sets its squish animation to
1, picks a random strafe between -1 and 1, and sets forward movement equal to its size. The tick
ends there. On any other tick it is not jumping, and if it is on the ground its forward and
strafe input are zeroed.

Five things follow from that, and four of them are absences:

- **A slime never despawns.** `EntityLiving`'s method carries the 128-block removal and the 600-tick clock and
  `ma` overrides the method without calling it, so a slime is the one mob of the nine that is
  permanent. Left alone, a cavern fills with them.
- **No path, no `moveSpeed`, no `getBlockPathWeight`, no `entityAge`.** It hops in whatever
  direction it is facing, and the only steering is `faceEntity` towards a player.
- **`getCanSpawnHere` calls no super**, so a slime is not checked for a clear box, for liquid or for
  other entities -- only for the size-and-difficulty clause, one draw in ten, the chunk hash, and
  `posY < 16`. It is the shortest spawn check in the game.
- **It splits at `health == 0` exactly**, not at `<= 0`. A size-4 slime taking 20 damage in one blow
  ends on -4 and does not split. Reproduced rather than rounded.
- **Damage is by standing on you.** `onCollideWithPlayer`, not `attackEntity`, and a size-1 slime
  does nothing at all.

Two more that are not in `ma` at all, and both of them are why a slime looks wrong if you only read
this class:

- **It drops nothing most of the time, and that is the jar's.** `getDropItemId` answers a slimeball
  only at size 1; `ge.onDeath` then runs `for (i = 0; i < rand.nextInt(3); i++) dropItem(...)`, so
  even the smallest slime gives **0, 1 or 2** and a big one gives nothing whatever kills it. Nothing
  here is missing: a slime that dropped nothing is the common case.
- **`gq` is the one renderer in the game with a render pass**, and the slime is the one model that
  is not opaque. `RenderManager` builds it as `new gq(new hh(16), new hh(0), 0.25F)`: `shouldRender
  Pass(0)` enables `GL_BLEND` with `SRC_ALPHA, ONE_MINUS_SRC_ALPHA` and draws the 8-unit shell over
  the inner body, and `shouldRenderPass(1)` turns it off again. The shell **has** to be blended --
  `mob/slime.png`'s outer quarter is alpha 199 of 255, and the game's own alpha test at 0.1 passes
  every one of those texels at full opacity, which draws a slime with no face. See
  `core/render/mob_mesh.hpp`'s `buildMobShells`.

`getRandomWithSeed` -- `WorldGenMinable`'s method -- is what makes slime chunks a property of the *world*:

```
new Random(worldSeed + (long)(chunkX*chunkX*4987142) + (long)(chunkX*5947611)
                     + (long)(chunkZ*chunkZ) * 4392871L + (long)(chunkZ*389711)
           ^ 987234911L)
```

**The int-versus-long boundaries in that expression are load-bearing**: three of the four products
wrap at 32 bits before they are widened, and only `chunkZ * chunkZ` is widened first and multiplied
as a long. A version that promoted everything to 64 bits picks different chunks.

### `k` -- the monster spawner

Eleven lines, and the whole of the override is the y draw:

The spawn point inside a chunk is a random x and z within the chunk's 16 by 16, and a height drawn
below (8 plus a draw below 120), which biases it towards the low end.

against `SpawnerAnimals`'s flat `rand(128)`. **The inner draw picks a ceiling and the outer picks under it**, so
the distribution piles up near the bedrock -- which is why a monster pass reaches the group loop far
more often than an animal one does, and why the 200 cap is not the fiction it looks like.

Everything else is `SpawnerAnimals` and is shared, **including the spider jockey**:

When a spider is spawned, a one-in-100 draw also creates a skeleton at the same position and
yaw, adds it to the world and mounts it on the spider.

The skeleton gets **no spawn check of its own**, and `EntityLiving` never asks whether it is riding, so a
mounted skeleton still runs its AI, still aims and still fires -- only its position comes from the
spider.

### `Explosion` -- the explosion

One class, one method, three phases, and the order is observable:

1. `random.explode` at **volume 4** (the loudest thing in the game), then **1,352 rays** -- every
   cell on the surface of a 16 x 16 x 16 cube gives a direction. Each ray is walked in steps of 0.3
   while its strength lasts, losing `(getExplosionResistance + 0.3F) * 0.3F` to the block it is in
   and a flat `0.3F * 0.75F` to the step. Every cell a still-live ray passes through is recorded.
2. **Entities are hurt while the blocks are still standing.** `getBlockDensity` traces a lattice of
   points across the victim's box to the centre, so cover is a fraction and not a switch, and the
   damage is `((d*d + d) / 2) * 8 * (strength * 2) + 1` -- so nothing in reach ever takes zero.
3. The recorded cells are destroyed, each dropping its block **three times in ten**.

**`getExplosionResistance` is `blockResistance / 5`, and `blockResistance` is not the number in
`blocks.json`.** `Block.setResistance(f)` stores `f * 3` and `setHardness(f)` raises it to `f * 5`
if that is larger; every block in this version calls `setHardness` first, so the stored value is
`max(resistance * 3, hardness * 5)` -- checked against all seventy rows, with no exceptions.

Two things that look like bugs and are not. `World.getBlockDensity` decompiles as `return i2 / i3`
with both operands `int`, which would make it a truth value rather than a fraction; the class file
has `i2f` on both before the `fdiv`. And there is **no fire**: `isFlaming` arrives with 1.0's beds
and ghasts. The only `onBlockDestroyedByExplosion` override in this version is TNT's, and it is
ported now -- a recorded cell holding TNT re-primes on a 10..29-tick fuse rather than detonating,
which is why a creeper in a TNT store starts a ripple and not one bang.

A creeper's 3.0 reaches **3.7 blocks at most**: a ray in open air loses 0.315 per 0.3-block step and
starts at `strength * (0.7 .. 1.3)`.

### Drawing them

`ModelBiped` (ModelBiped) is seven boxes and is the player's. `cb` (ModelZombie) replaces the arms outright
-- both forward to horizontal with a slow idle sway on `ticksExisted` -- and **both the zombie and
the skeleton are posed with it**, because `fv` (ModelSkeleton) extends `cb` and only narrows the
four limbs to 2 x 12 x 2. `ModelBiped`'s swing block is **dead for every mob in this version**: `swingProgress`
is only ever advanced by `swingItem`, which nothing but a player calls.

`em` (ModelCreeper) builds seven boxes and **renders six** -- the headwear is constructed and never
drawn. `jy` (ModelSpider) is eleven: a head, a small thorax, the abdomen, and eight 16 x 2 x 2 legs
splayed by a fixed pair of angles that the walk modulates at **twice** the quadruped's leg
frequency. `hh` (ModelSlime) is a switch on its one argument: `hh(16)` is the inner body with the
face and `hh(0)` is the outer shell, and its `setRotationAngles` is empty -- **a slime is never
posed.**

Two renderers use `RenderLiving`'s one hook, `preRenderCallback`, which runs between the `glScalef(-1,-1,1)`
and the model's own lift:

- `d` (RenderCreeper) scales by the fuse: `(1 + s^4 * 0.4) * wobble` in x and z, where
  `s = timeSinceIgnited / (fuseTime - 2)` and `wobble = 1 + sin(s * 100) * s * 0.01`. **The divisor
  is `fuseTime - 2`**, so the swell passes 1 two ticks before the blast and keeps going.
- `gq` (RenderSlime) scales by `size` and by the squish, which is why `hh`'s boxes are the same for
  a size-1 and a size-4 slime.

`ok` (RenderSpider) adds a second pass over the whole model from `mob/spider_eyes.png`, blended at
`(1 - brightness) * 0.5`.

## What is this port's and not a1.1.2's

- The **pathfinder's node ceiling** (1,024 per search) and its coordinate identity. The ceiling
  stays because an unbounded search allocates inside the 20 Hz tick, which the platform forbids;
  `PathFinder::exhausted()` says when it binds.
  **The one-search-a-tick budget is gone** (2026-09-17). It never bound on fifteen animals and
  bound hard on two hundred monsters -- ten requests a tick against a budget of one, so nine mobs
  in ten got no path and fell back to the aimless wander -- and it sat in *front* of the wander's
  ten candidate draws, so a second mob wanting a path in one tick skipped thirty `nextInt` calls
  the jar makes. `MobSystem::peakSearchesPerTick` counts what it used to cap; the Info page's
  `path` row carries it as `pk`.
- **`entityToAttack` is a boolean here**, because the only entity a monster can target in this
  build is the player: `EntityMob`'s method asks `getClosestPlayer` and the only other writer is
  `attackEntityFrom`, whose source is also only ever the player.
- **The spider jockey's mount is an index** into a pool that swap-removes, fixed up on every
  removal. a1.1.2 holds a reference.
- ~~An arrow excludes its shooter by place rather than by identity~~ -- **fixed** (2026-09-17).
  `Mob::handle` is a session identity the pool's swap-remove cannot disturb, so `EntityArrow`'s method's
  `entity != shootingEntity` is now the comparison for a mob's arrow as well as the player's. The
  footprint test it replaced failed for any shooter that moved more than its own half-width on the
  tick it fired -- knockback, a shove, a push out of a block -- and spent the arrow on its own
  archer at a distance of zero. The handle is not saved, which is the jar's behaviour: a1.1.2 does
  not write `shootingEntity` either, so a reloaded arrow excludes nobody.
- **The difficulty lives in `<world>/3dalpha.ini`.** a1.1.2 keeps it in `options.txt`, which is a
  file this port does not have, and it is a per-world setting here.
- **The explosion's cell record is a bitset over a cube of radius 8** rather than a `HashSet` of
  `ChunkPosition`. The set is the same set; the order is not, and the order is observable.

  **Derived 2026-09-17, and the finding is that there is no single order to match.** `Explosion` is one
  method. It fills a `HashSet<mt>`, copies it with `new ArrayList(); addAll(set)`, then walks that
  list **backwards** (`for (i = size - 1; i >= 0; --i)`), drawing **three `world.rand.nextFloat()`
  per destroyed cell** for the smoke. So the set's iteration order picks the RNG stream for the
  rest of the tick, not just the order blocks vanish in.

  `mt.hashCode()` is `a * 8976890 + b * 981131 + c` (fields `a`, `b`, `c`; `equals` compares all
  three). That is exact. What is not exact is what `HashSet` does with it: the bucket a hash lands
  in depends on `HashMap`'s spread function, and that **changed between Java 7 and Java 8** -- `h ^=
  (h >>> 20) ^ (h >>> 12); h ^ (h >>> 7) ^ (h >>> 4)` against `h ^ (h >>> 16)` -- as did whether a
  resize reverses a bucket chain. a1.1.2 shipped on Java 6; a player running it today is on 8 or
  later and gets a different explosion. Reproducing "the jar's order" therefore means choosing a
  JVM and saying so, which is a decision this port has not taken yet rather than a transcription
  it has skipped.
- **Mobs in `level.dat`.** a1.1.2 writes animals into each chunk's `Entities` list; this port has
  no `entitydata` slot yet, so they ride in the port's own `Data/3DAlphaEntities` compound with the
  other six pools. Native chunk `Entities` stay preserved and unread. A world carried back to the
  real client keeps its terrain and loses these animals.
- **The death puff is drawn.** `EntityLiving`'s method throws twenty `explode` particles out of the model's own
  box the tick the corpse goes, each drifting on a **Gaussian** hundredth of a block; all twelve
  particle kinds landed together (status.md 29), so this and the slime's landing ring are no
  longer gaps.
- **Footsteps** are played through the same `stepCue` the player uses. The original plays them too
  (`Entity.moveEntity`), so this is a wiring note rather than a deviation.

## Not in this version at all

These are the things people expect from animals and do not get here, each because the version does
not have it rather than because the port skipped it. **None of them is implemented.**

- **Breeding.** a1.1.2 has none, and Beta 1.8.1 has none either -- what 1.8 gave animals was *not
  despawning*, which is a different feature. It arrives in 1.0/1.1 (`ba`, EntityAnimal). There is
  no wheat use beyond bread here, no `growingAge` on any entity, and no baby animal in `ew`'s
  table, so there is nothing to hang it on.
- **Babies**, and with them the half-size model and box `EntityAgeable` introduces. Every animal
  in this version is full size from the moment it spawns.
- **Taming or keeping one.** Nothing a player does to an animal exempts it from the despawn above:
  there is no lead, no name tag and no persistence flag in this version. A herd is a thing that
  thins itself out.
- **Shears.** The item does not exist; a sheep's wool comes off the first hit from a living thing
  and never grows back.

## What is not done

- ~~The player has no health~~ -- **stale** (checked 2026-09-17). `PlayerVitals`
  (`core/entity/player_vitals`) carries `health`, the damage path and the Peaceful regen, and
  `MobSurroundings::hurtPlayer` reaches it through `PlayerHarm::hurt` in `platform/ctr/main.cpp`.
  A monster's fist lands.
  Survival is M3 step 4 and `EntityPlayer`'s method's difficulty and armour scaling belongs with it.
- **`hl` -- the Giant.** Registered in `ew` at id 53, drawn by `nz` at six times scale, and
  constructed by nothing in the game. Not ported, and not a gap.
- **The spider's eye overlay is one part rather than a second model**, and that is now the whole
  of the deviation. `ok` draws all of `jy` again from `mob/spider_eyes.png`; the eye page is
  transparent everywhere but the head, so redrawing the head alone is the same picture for a tenth
  of the vertices.
  **The blend is no longer lost** (2026-09-17). It was an alpha cut, which drew the eyes opaque at
  every light level -- twice as bright as the jar ever draws them, and still visible in daylight
  where the jar's have faded out. They are their own blended pass now. The alpha is
  `(1 - getBrightness) * 0.5` exactly, and it is computed in the combiner rather than per draw: the
  lightmap texture is monochrome `lightBrightness(effectiveLightLevel(...))`, which is what
  `Entity.getBrightness` returns, so `1 - lightmap.r` is that spider's own term sampled at the
  light coordinate its vertices already carry. Every spider on screen gets its own value out of one
  draw call. `Renderer::drawMobs`, `render::buildMobEyes`.
- ~~A mob's water push is asked once a tick here and three times in the jar~~ -- **this was never
  true and the line was wrong** (checked 2026-09-17). `LivingBody` *is* `PlayerBody`
  (`player_body.hpp`), so a mob already gets `updateWaterEntry`'s call and both of the calls inside
  `PlayerBody::tick` -- `EntityLiving`'s method's jump branch and `EntityLiving`'s method's water test. That is the jar's count
  exactly: twice a tick, three times while the jump button is held.
- **The jukebox works** (2026-09-14): a disc goes in, plays, and comes back out on a click or a
  break. It was on this list until the `.mus` cipher was read out of `hk`.
- **No hardware run.** Every number in the rendering notes above is a vertex count, not a frame
  time -- and the monsters are the first thing here that can put two hundred models and a
  1,352-ray explosion in front of a 268 MHz ARM11 at the same moment. The first hardware report on
  them found that **nothing spawned at all** (status.md 26), which is the kind of thing only a
  console shows: the host suite has no entity query wired into its worlds.
