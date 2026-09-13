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
| `Entity` | `kh` | `moveEntity`, `onEntityUpdate`, `applyEntityCollision`, `dropItem` |
| `EntityLiving` | `ge` | health, hurt, death, `moveEntityWithHeading`, the aimless wander |
| `EntityCreature` | `ek` | the path-following wander |
| `EntityAnimal` | `ag` | `getBlockPathWeight`, `getCanSpawnHere`, talk interval 120 |
| `EntityPig` | `mv` | `/mob/pig.png`, 0.9 x 0.9, saddle, raw porkchop |
| `EntitySheep` | `bo` | `/mob/sheep.png`, 0.9 x 1.3, fleece, wool |
| `EntityCow` | `am` | `/mob/cow.png`, 0.9 x 1.3, bucket, leather |
| `EntityChicken` | `mz` | `/mob/chicken.png`, 0.3 x 0.4, egg clock, feather |
| `SpawnerAnimals` | `az` | built by `ia` (PlayerControllerSP), run every tick |
| `PathFinder` | `cz` | A*, four neighbours, step one, drop four |
| `PathEntity` / `PathPoint` | `bl` / `a` | the route and its nodes |
| `ModelQuadruped` | `hg` | pig, sheep and cow are all this plus overrides |
| `ModelPig`/`ModelSheep2`/`ModelSheep1`/`ModelCow`/`ModelChicken` | `ca`/`gx`/`bx`/`dv`/`kv` | |
| `RenderLiving` | `dn` | the transform, the extra pass, the hurt flash |
| `RenderPig`/`RenderSheep`/`RenderCow`/`RenderChicken` | `gm`/`ns`/`mc`/`eq` | |
| `ItemSaddle` | `jw` | `useItemOnEntity`, not `interact` |
| `EntityList` | `ew` | the save ids -- and see the bug below |
| `IMob` | `co` | a **marker interface**, and what the 200-cap spawner counts |
| `EntityMob` | `dq` | `ek` plus a target, a fist, and a reason to spawn in the dark |
| `EntityZombie`/`EntitySkeleton`/`EntityCreeper`/`EntitySpider` | `mb`/`cw`/`dd`/`ax` | |
| `EntitySlime` | `ma` | **not an `ek` at all** -- `ge` plus `co` |
| `Explosion` | `je` | one class, one method, reached through `cn.a(Lkh;DDDF)V` |
| `MonsterSpawner` | `k` | `az` with one method overridden |
| `TileEntityMobSpawner` | `bd` | **the dungeon cage** -- a tile entity, not a world sweep |
| `BlockMobSpawner` | `bj` | `jt` (BlockContainer) and three one-line overrides |
| `TileEntityMobSpawnerRenderer` | `r` | the miniature: nine GL calls and a factor of ten |
| `ModelBiped`/`ModelZombie`/`ModelSkeleton` | `cr`/`cb`/`fv` | |
| `ModelCreeper`/`ModelSpider`/`ModelSlime` | `em`/`jy`/`hh` | |
| `RenderCreeper`/`RenderSpider`/`RenderSlime` | `d`/`ok`/`gq` | each with a `preRenderCallback` |

**`ew`'s numeric ids are 90, 91, 91, 91.** Pig is 90; sheep, cow and chicken are *all* 91. That is
a bug in a1.1.2 and it is why nothing anywhere keys an animal by its number: the save uses the
string (`"Pig"`, `"Sheep"`, `"Cow"`, `"Chicken"`), and so does `MobDef::saveId`. The monsters are
50 to 55 and are distinct, and `ew` also registers **`hl` "Giant" at 53** -- a class that exists,
extends `dq`, is drawn at six times scale by `nz`, and that nothing in the game ever constructs.
It is not ported and it is not a gap.

## The four, as numbers

| | Pig | Sheep | Cow | Chicken |
|---|---|---|---|---|
| `setSize` | 0.9 x 0.9 | 0.9 x 1.3 | 0.9 x 1.3 | 0.3 x 0.4 |
| Health | 10 | 10 | 10 | **4** |
| `getDropItemId` | raw porkchop | **none** | leather | feather |
| How many | `rand(3)` -- so 0, 1 or 2 | -- | `rand(3)` | `rand(3)` |
| Living sound | `mob.pig` | `mob.sheep` | `mob.cow` | `mob.chicken` |
| Hurt sound | `mob.pig` | `mob.sheep` | `mob.cowhurt` | `mob.chickenhurt` |
| Death sound | `mob.pigdeath` | `mob.sheep` | `mob.cowhurt` | `mob.chickenhurt` |
| `getSoundVolume` | 1.0 | 1.0 | **0.4** | 1.0 |
| Its one boolean | `Saddle` | `Sheared` | -- | -- |

All four declare `public boolean a` and only two of them ever write it. That is why `Mob::flag` is
one field rather than two.

**A sheep drops nothing when it dies.** `ge.g()` -- getDropItemId -- answers zero and `bo` does not
override it. Wool comes out of `attackEntityFrom` instead, once, and only when an `EntityLiving`
did the hitting:

```
if (!sheared && attacker instanceof EntityLiving) {
    sheared = true;
    int n = 1 + rand.nextInt(3);
    for (i = 0; i < n; i++) {
        EntityItem wool = dropItemWithOffset(Block.cloth.blockID, 1, 1.0F);
        wool.motionY += rand.nextFloat() * 0.05F;
        wool.motionX += (rand.nextFloat() - rand.nextFloat()) * 0.1F;
        wool.motionZ += (rand.nextFloat() - rand.nextFloat()) * 0.1F;
    }
}
```

So a sheep that suffocates, drowns or falls into the void keeps its coat, and one that is punched
loses it whether or not the punch kills it. There is no shearing with shears in this version and
there is no wool colour: `bo` has no colour field at all.

## The tick, in order

`ge.e_()` is `onUpdate` and it runs three things in this order. Swapping the last two draws an
animal facing where it was going *last* tick.

1. **`kh.y()` / `ge.y()` -- the counters.**
   - the idle noise: `if (rand.nextInt(1000) < livingSoundTime++) { livingSoundTime = -120; play }`
     -- the draw happens before the increment, so the odds climb from nothing to certainty over two
     minutes;
   - water: splash sound and bubbles on entering, `fallDistance = 0`, `fire = 0`;
   - fire: one damage every twentieth tick while it burns;
   - lava: ten damage and `fire = 600`;
   - the void: below y = -64, `ge.E()` is `attackEntityFrom(null, 4)` a tick -- **not** an instant
     kill, which is `Entity`'s behaviour and not `EntityLiving`'s;
   - suffocation inside an opaque block, one a tick; drowning at `air == -20`, two a tick;
   - `hurtTime`, `attackTime` and `hurtResistantTime` count down;
   - at zero health, `deathTime` counts up and at **20** the entity is removed in a puff of
     twenty `explode` particles.

2. **`ge.j()` -- onLivingUpdate.** Asks the AI (`updatePlayerActionState`), then:
   ```
   if (isJumping) { if (inWater || inLava) motionY += 0.04; else if (onGround) jump(); }
   moveStrafing *= 0.98; moveForward *= 0.98; randomYawVelocity *= 0.9;
   moveEntityWithHeading(moveStrafing, moveForward);
   for (entity in world.getEntitiesWithinAABBExcludingEntity(this, box.expand(0.2, 0, 0.2)))
       if (entity.canBePushed()) entity.applyEntityCollision(this);
   ```
   `moveEntityWithHeading` is `ge.b(FF)` and it is **the player's own** -- the same land branch,
   the same water and lava branches, the same ladder, the same ground friction. See
   `docs/physics-a1.1.2.md`; the only difference between a pig and a player in it is `width`,
   `height` and `yOffset`.

3. The body's heading chases the direction it actually travelled, at 30 % a tick, clamped to 75
   degrees away from where the head is looking. That clamp is what makes an animal turn its head
   first and its body after.

**`yOffset` is 0 for a mob.** Only `EntityPlayer` sets it to 1.62, so `posY` is a mob's feet and
`Pos[1]` in its save is the feet too. The player is the odd one out, not the animals.

## The AI

`ek.b_()` -- `EntityCreature.updatePlayerActionState`. An animal never has an `entityToAttack`
(`ag` does not override `findPlayerToAttack`), so the attack half of the method is dead and what
is left is:

```
if (pathToEntity == null && rand(80) == 0 || rand(80) == 0) {
    best = -99999; found = false;
    for (i = 0; i < 10; i++) {
        x = floor(posX + rand(13) - 6);
        y = floor(posY + rand(7) - 3);
        z = floor(posZ + rand(13) - 6);
        w = getBlockPathWeight(x, y, z);
        if (w > best) { best = w; found = true; ... }
    }
    if (found) pathToEntity = world.getEntityPathToXYZ(this, x, y, z, 10.0F);
}
if (pathToEntity == null || rand(100) == 0) { super.updatePlayerActionState(); pathToEntity = null; return; }
Vec3D vec = pathToEntity.getPosition(this);
double d = width * 2;
while (vec != null && vec.squareDistanceTo(posX, vec.yCoord, posZ) < d * d) { ...advance... }
isJumping = false;
if (vec != null) {
    ...turn at most 30 degrees towards it...
    moveForward = moveSpeed;                      // 0.7, flat
    if (vec.yCoord - floor(boundingBox.minY + 0.5) > 0) isJumping = true;
}
if (isCollidedHorizontally) isJumping = true;
if (rand.nextFloat() < 0.8F && (inWater || inLava)) isJumping = true;
```

`ag.a(III)F` -- getBlockPathWeight -- is two lines and is the whole reason animals are found
standing in fields:

```
if (world.getBlockId(x, y - 1, z) == Block.grass.blockID) return 10.0F;
return world.getBrightness(x, y, z) - 0.5F;
```

`ge.b_()` is the fallback, and **it sets `moveForward` to zero**: an animal with no path stands
still and turns its head rather than milling about. It also carries the despawn:

```
entityAge++;
EntityPlayer p = world.getClosestPlayer(this, -1.0D);
if (p != null) {
    double distSq = ...;
    if (distSq > 16384.0D) setEntityDead();                       // 128 blocks
    if (entityAge > 600 && rand(800) == 0) {
        if (distSq < 1024.0D) entityAge = 0; else setEntityDead(); // 32 blocks
    }
}
```

**Animals despawn in a1.1.2.** Beta 1.8 is where that stopped; here a herd left alone thins itself
out. `attackEntityFrom` resets `entityAge`, so an animal being fought does not vanish mid-fight.

## The pathfinder, and the loop that is not a loop

`cz.a(Lkh;IIILa;)I` -- getVerticalOffset -- is meant to test the whole volume an entity occupies.
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

Two budgets are ours and are named in `core/entity/path_finder.hpp`: 1,024 nodes per search and one
search per tick across all animals. Neither binds in ordinary play -- fifteen animals asking about
once every forty ticks each is 0.4 searches a tick.

## Where animals come from

`az` -- SpawnerAnimals -- built by `ia` as `new az(15, ag.class, {bo, mv, am, mz})` and run by
`ia.c()`, which `Minecraft.runTick` calls every tick the game is not paused.

**There is no world-generation spawning in this version.** `az` is referenced by `ia` and by
nothing else; no chunk populator makes an animal. A fresh world is empty and fills up while
somebody stands in it.

One tick:

```
if (world.countEntities(EntityAnimal.class) < 15)
    for (i = 0; i < 3; i++) performSpawning(world, 1, null);
```

and `performSpawning` walks the 9 x 9 of chunks around each player, trying one chunk in ten:

```
type = classes[rand(4)]
x = chunkX * 16 + rand(16); y = rand(128); z = chunkZ * 16 + rand(16)
if (world.isBlockNormalCube(x, y, z)) return 0;          // ends the whole pass
if (world.getBlockMaterial(x, y, z) != Material.air) return 0;   // likewise
for (group = 0; group < 3; group++) {
    gx = x; gy = y; gz = z;
    for (try = 0; try < 2; try++) {
        gx += rand(6) - rand(6);
        gy += rand(1) - rand(1);                          // always zero
        gz += rand(6) - rand(6);
        if (normalCube(gx, gy - 1, gz) && !normalCube(gx, gy, gz)
            && !material(gx, gy, gz).isLiquid() && !normalCube(gx, gy + 1, gz)) {
            fx = gx + 0.5; fy = gy; fz = gz + 0.5;
            if (world.getClosestPlayer(fx, fy, fz, 24.0D) != null) continue;
            if (distanceSq(fx, fy, fz, world.spawnX, spawnY, spawnZ) < 576.0F) continue;
            entity = new type(world);
            entity.setLocationAndAngles(fx, fy, fz, rand.nextFloat() * 360.0F, 0.0F);
            if (entity.getCanSpawnHere()) { count++; world.spawnEntityInWorld(entity); }
        }
    }
}
```

Three things in there shape the rate and all three are kept:

- **those two `return 0`s end the pass**, not the chunk -- and since `y` is a flat `rand(128)`,
  most first draws land in rock, so most passes stop at the first chunk they look at;
- `rand(1) - rand(1)` is **always zero**, so a group never moves in y;
- the cap is checked once, before the three passes, so a tick that starts at 14 animals can finish
  at 17.

`ag.a()Z` -- getCanSpawnHere -- is the last refusal: **grass** under the feet, a light level
**above 8** (`getFullBlockLightValue`, so the day's subtraction applies and animals stop spawning
at dusk), a box clear of blocks, entities and liquid, and a non-negative `getBlockPathWeight`.

### The order of the 9 x 9, which is not a detail

`az` collects the eligible chunks into a `HashSet<ol>` and iterates that, and the obvious reading
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

Insertion order is `az`'s own -- x outer, z inner, each -4 to 4 -- so the order is a stable
function of the player's chunk. `entity::eligibleOrder` computes it; `tests/monster_test.cpp` pins
it against output from a real JVM.

**One stated deviation.** A bucket of eight or more becomes a red-black tree and `moveRootToFront`
puts the tree's root at the head of the chain, so Java hoists exactly one entry of each nine. Which
one is the outcome of balancing nine insertions, and it is JVM-version-dependent besides -- Java 7
spread hashes differently and had no treeified bins. The bucket partition and the bucket order are
exact; the hoist is not reproduced.

### Why the surface is so quiet at night

**The dungeon cage is a different thing entirely.** `bd` is a *tile entity*: one cell that counts
down while a player is within sixteen blocks and throws four candidates at the ground around
itself. It shares no code with `az`/`k` and only the word "spawner". Its derivation is in
`src/core/entity/mob_spawner.hpp` and the write-up is `docs/status.md` 38; the one thing worth
repeating here is that `getCanSpawnHere` is reached through a **virtual** call, so a dungeon's
zombies obey the same light rule `dq.a()Z` imposes on `k`'s.

`k` -- the monster spawner -- is `az` with three changes: a cap of 200, `co.class` instead of
`ag.class`, and a y draw of **`rand(rand(120) + 8)`** instead of `rand(128)`. That draw piles up
near the bedrock, and the group loop's `gy += rand(1) - rand(1)` never moves it. So a **surface**
spawn needs the draw to land on the local ground height exactly, while a draw that lands inside a
cave is already standing in one.

Measured, held at midnight over 30,000 ticks of a real generated world: **806 monsters, 14 of them
above y 64**, and **98.9% of all drawn positions had no floor**. Monsters in a1.1.2 are a cave
population that occasionally surfaces, and every step of that was checked against
`az.a(Lcn;ILnu;)I` and `k.a(Lcn;II)Lmt;`.

`build-host/3dalpha --spawns <world-dir>` is the harness those numbers came from: the real
streamer, a generated world, the console's own tick loop, and `SpawnCounters` on the spawner. The
Info page shows the same three counters on hardware.

## What a right click does

`bi.a_(Lkh;)V` -- `EntityPlayer.useCurrentItemOnEntity` -- is two steps:

```
if (entity.interact(player)) return;
ItemStack stack = getCurrentEquippedItem();
if (stack != null && entity instanceof EntityLiving) stack.useItemOnEntity((EntityLiving) entity);
```

- `am.a(Ldm;)Z` -- a **cow** holding out a bucket (`Item.bucketEmpty`) hands back a milk bucket, in
  place, in the same slot;
- `mv.a(Ldm;)Z` -- a **saddled pig** mounts the player and refuses otherwise;
- `jw.b(Lev;Lge;)V` -- `ItemSaddle`, which is the *second* step: a saddle held out to an unsaddled
  pig is spent on it. Held out to a saddled one, `interact` has already answered, so the saddle is
  not eaten -- you climb on instead.

A pig in this version **cannot be steered**: `mv` reads nothing from `riddenByEntity` and there is
no carrot on a stick until 1.4. A saddled pig goes where its own AI says and takes you with it.

There is no clock, no shears, no wheat use beyond bread, and `ew` has no `Egg` entity -- so an egg
in a1.1.2 is an item and nothing else. `mz`'s own clock lays one every `6000 + rand(6000)` ticks,
five to ten minutes, with `mob.chickenplop`.

## Drawing one

`dn.a(Lge;DDDFF)V` -- RenderLiving:

```
glTranslatef(x, y, z);                          // interpolated position
glRotatef(180 - renderYawOffset, 0, 1, 0);      // the body's heading, interpolated
if (deathTime > 0) glRotatef(min(sqrt((deathTime + partial - 1) / 20 * 1.6), 1) * 90, 0, 0, 1);
glScalef(-1, -1, 1);                            // the model is built upside down
glTranslatef(0, -24 * 0.0625F - 0.0078125F, 0);
model.render(limbSwing, limbYaw, ticksExisted + partial, yaw - renderYawOffset, pitch, 0.0625F);
```

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

`ia` builds two spawners and the monsters' is the first:

```
new k(this, 200, co.class, {mb, cw, dd, ax, ma})   // 200 monsters
new az(15, ag.class, {bo, mv, am, mz})             // 15 animals
```

**`co` is an interface and `ag` is a class.** That one difference is why the slime counts towards
the monster cap without being an `ek`, and it is why `MobDef` carries a `hostile` *column* rather
than the port testing `type >= Zombie`.

### `dq` -- EntityMob, which is five short methods

```
dq(World w)      { attackStrength = 2; health = 20; }
onLivingUpdate() { if (getBrightness(1.0F) > 0.5F) entityAge += 2;  super.onLivingUpdate(); }
onUpdate()       { super.onUpdate(); if (world.difficulty == 0) setEntityDead(); }
findPlayerToAttack() { p = world.getClosestPlayer(this, 16.0); return (p != null && canEntityBeSeen(p)) ? p : null; }
attackEntityFrom(e, n) { if (!super(e, n)) return false; if (e != riddenBy && e != riding && e != this) entityToAttack = e; return true; }
attackEntity(e, dist)  { if (dist >= 2.5F || e.box.maxY <= box.minY || e.box.minY >= box.maxY) return;
                         attackTime = 20; e.attackEntityFrom(this, attackStrength); }
getBlockPathWeight(x,y,z) { return 0.5F - world.getLightBrightness(x, y, z); }
getCanSpawnHere()  { return world.getSavedLightValue(Sky, x, y, z) <= rand(32)
                         && world.getBlockLightValue(x, y, z) <= rand(8) && super.getCanSpawnHere(); }
```

Four of those are the whole difference between a monster and a cow:

- **`getBlockPathWeight` is `ag`'s with the sign reversed.** An animal scores grass at ten and
  everything else at `brightness - 0.5`; a monster scores `0.5 - brightness`. One method is why
  animals are found in fields and monsters in caves, and the same method is the last clause of
  `getCanSpawnHere` (`ek.a()` asks for a weight of at least zero), so it is also why a monster
  cannot spawn anywhere brighter than half.
- **`getCanSpawnHere`'s sky light is the *stored* value**, before the day's subtraction. So the
  time of day changes nothing about *where* a monster may spawn -- only about whether it burns
  afterwards. A roofed cave qualifies at noon and an open field does not at midnight.
- **Peaceful is a removal, not a suppression.** `world.difficulty == 0` kills the monster at the
  *end* of the tick it notices, so a1.1.2 on Peaceful still runs the 200-cap spawner, still makes
  the entities and still pays for them.
- **`entityAge += 2` in the light.** `entityAge` is what despawns a mob, so a monster that survives
  the morning in the open is gone in half the time one in a cave is.

`ek`'s attack branch, which was dead for the animals, is now the live half:

```
hasAttacked = false;
if (entityToAttack == null) {
    entityToAttack = findPlayerToAttack();
    if (entityToAttack != null) path = world.getPathToEntity(this, entityToAttack, 16.0F);
} else if (entityToAttack.isEntityAlive()) {
    float d = entityToAttack.getDistanceToEntity(this);
    if (canEntityBeSeen(entityToAttack)) attackEntity(entityToAttack, d);
} else entityToAttack = null;

if (!hasAttacked && entityToAttack != null && (path == null || rand(20) == 0))
    path = world.getPathToEntity(this, entityToAttack, 16.0F);
else if ((path == null && rand(80) == 0) || rand(80) == 0)
    ...the ten-sample wander...
```

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

**The creeper has no living sound**, and that is `ge.c()` returning null with no override -- it is
the whole of why one gets behind you. The slime has none either. **A zombie drops feathers** in
this version; rotten flesh is 1.8's.

### `mb` and `cw` -- catching fire

The same eight lines in both classes, and it is the only thing the zombie's `onLivingUpdate` does:

```
if (world.isDaytime()) {
    float b = getBrightness(1.0F);
    if (b > 0.5F && world.canBlockSeeTheSky(floor(posX), floor(posY), floor(posZ))
        && rand.nextFloat() * 30.0F < (b - 0.4F) * 2.0F)
        fire = 300;
}
```

`isDaytime` is `skylightSubtracted < 4` -- the *world's*, not the cell's. The draw is what makes it
gradual: at full daylight `(1.0 - 0.4) * 2 = 1.2` against `rand() * 30`, which is about one tick in
twenty-five, so a zombie caught at dawn smoulders for a second or two before it goes up rather than
igniting on the exact tick the sun clears the hill.

### `cw` -- the bow

```
attackEntity(target, dist) {
    if (dist < 10.0F) {
        double dx = target.posX - posX, dz = target.posZ - posZ;
        if (attackTime == 0) {
            EntityArrow a = new EntityArrow(world, this);
            a.posY += 1.4D;
            double dy = (target.posY - 0.2D) - a.posY;
            float arc = sqrt(dx*dx + dz*dz) * 0.2F;
            world.playSoundAtEntity(this, "random.bow", 1.0F, 1.0F / (rand.nextFloat() * 0.4F + 0.8F));
            world.spawnEntityInWorld(a);
            a.setThrowableHeading(dx, dy + arc, dz, 0.6F, 12.0F);
            attackTime = 30;
        }
        rotationYaw = atan2(dz, dx) * 180 / PI - 90;
        hasAttacked = true;
    }
}
```

Three things in there are worth naming. **The skeleton does not close**: it fires from wherever it
is inside ten blocks and `hasAttacked` stops the path being re-asked. **The arrow is slow and
inaccurate** -- 0.6 velocity and 12.0 inaccuracy against the player's 1.5 and 1.0 -- which is why
one at range misses and one at three blocks does not. And the arrow starts *inside the skeleton's
own box*: the constructor places it at the shooter, steps it 0.16 out along the facing and drops it
0.1, and `cw` then lifts it 1.4 -- so without `kg.e_()`'s `entity != shootingEntity ||
ticksInAir >= 5` it would shoot itself on the tick it is loosed.

### `dd` -- the fuse

```
updatePlayerActionState() {                 // wraps ek's
    lastActiveTime = timeSinceIgnited;
    if (timeSinceIgnited > 0 && creeperState < 0) timeSinceIgnited--;
    if (creeperState >= 0) creeperState = 2;
    super.updatePlayerActionState();
    if (creeperState != 1) creeperState = -1;
}
attackEntity(target, dist) {
    if ((creeperState <= 0 && dist < 3.0F) || (creeperState > 0 && dist < 7.0F)) {
        if (timeSinceIgnited == 0) world.playSoundAtEntity(this, "random.fuse", 1.0F, 0.5F);
        creeperState = 1;
        if (++timeSinceIgnited == fuseTime)   // 30
            { world.createExplosion(this, posX, posY, posZ, 3.0F); setEntityDead(); }
        hasAttacked = true;
    }
}
onDeath(killer) { super.onDeath(killer); if (killer instanceof EntitySkeleton) dropItem(record13 + rand(2), 1); }
```

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
  see `core/item/registry.hpp` and status.md 26. `lg.a(...)` puts one in a jukebox (`ly.aZ`, id 84)
  and plays it; there is no jukebox behaviour here and `.mus` is undecoded, so a disc is an item you
  can hold and nothing more.

### `ax` -- the spider

```
getMountedYOffset()  { return height * 0.75D - 0.5D; }       // where a jockey sits
findPlayerToAttack() { return getBrightness(1.0F) < 0.5F ? world.getClosestPlayer(this, 16.0) : null; }
attackEntity(target, dist) {
    if (getBrightness(1.0F) > 0.5F && rand(100) == 0) { entityToAttack = null; return; }
    if (dist > 2.0F && dist < 6.0F && rand(10) == 0) {
        if (onGround) {
            double dx = target.posX - posX, dz = target.posZ - posZ;
            float f = sqrt(dx*dx + dz*dz);
            motionX = dx / f * 0.5D * 0.8D + motionX * 0.2D;
            motionZ = dz / f * 0.5D * 0.8D + motionZ * 0.2D;
            motionY = 0.4D;
        }
    } else super.attackEntity(target, dist);
}
```

- **Daylight is a forgetting, not a burning.** A spider in a bright cell will not pick a target and
  drops the one it has a tick in a hundred. Nothing sets it alight.
- `findPlayerToAttack` **does not ask `canEntityBeSeen`**, unlike `dq`'s, so a spider on the far
  side of a wall still starts walking.
- **The leap has no melee fallback.** In the 2-to-6 window, one tick in ten, a spider *not* on the
  ground simply does nothing that tick.
- **There is no wall climbing in this version.** `ax` overrides no ladder method and has no
  `isBesideClimbableBlock`; that arrives later. A spider here is a fast, wide, leaping mob and
  nothing more.

### `ma` -- the slime, which is not an `ek`

```
EntitySlime(World w) { slimeSize = 1 << rand(3); yOffset = 0; jumpDelay = rand(20) + 10; setSlimeSize(slimeSize); }
setSlimeSize(n)      { slimeSize = n; setSize(0.6F * n, 0.6F * n); health = n * n; setPosition(posX, posY, posZ); }
getCanSpawnHere()    { return (slimeSize == 1 || world.difficulty > 0) && rand(10) == 0
                          && world.getChunkFromBlockCoords(x, z).getRandomWithSeed(987234911L).nextInt(10) == 0
                          && posY < 16.0D; }
setEntityDead()      { if (slimeSize > 1 && health == 0) for (i = 0; i < 4; i++) { ...four children of size/2... }
                       super.setEntityDead(); }
onCollideWithPlayer(p) { if (slimeSize > 1 && canEntityBeSeen(p) && getDistanceToEntity(p) < 0.6D * slimeSize
                             && p.attackEntityFrom(this, slimeSize)) playSound("mob.slimeattack", ...); }
getDropItemId()      { return slimeSize == 1 ? slimeball : 0; }
getSoundVolume()     { return 0.6F; }
```

and `updatePlayerActionState` is its own, with no `super` anywhere in it:

```
EntityPlayer p = world.getClosestPlayer(this, 16.0D);
if (p != null) faceEntity(p, 10.0F);
if (onGround && jumpDelay-- <= 0) {
    jumpDelay = rand(20) + 10;
    if (p != null) jumpDelay /= 3;
    isJumping = true;
    if (slimeSize > 1) playSound("mob.slime", getSoundVolume(), ((rand() - rand()) * 0.2F + 1.0F) * 0.8F);
    squishAmount = 1.0F;
    moveStrafing = 1.0F - rand.nextFloat() * 2.0F;
    moveForward = 1 * slimeSize;
    return;
}
isJumping = false;
if (onGround) { moveForward = 0; moveStrafing = 0; }
```

Five things follow from that, and four of them are absences:

- **A slime never despawns.** `ge.b_()` carries the 128-block removal and the 600-tick clock and
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

`getRandomWithSeed` -- `cu.a(J)` -- is what makes slime chunks a property of the *world*:

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

```
protected ChunkPosition getRandomSpawningPointInChunk(World w, int x, int z) {
    return new ChunkPosition(x + w.rand.nextInt(16),
                             w.rand.nextInt(w.rand.nextInt(120) + 8),
                             z + w.rand.nextInt(16));
}
```

against `az`'s flat `rand(128)`. **The inner draw picks a ceiling and the outer picks under it**, so
the distribution piles up near the bedrock -- which is why a monster pass reaches the group loop far
more often than an animal one does, and why the 200 cap is not the fiction it looks like.

Everything else is `az` and is shared, **including the spider jockey**:

```
if (entity instanceof EntitySpider && w.rand.nextInt(100) == 0) {
    EntitySkeleton s = new EntitySkeleton(w);
    s.setLocationAndAngles(fx, fy, fz, entity.rotationYaw, 0.0F);
    w.spawnEntityInWorld(s);
    s.mountEntity(entity);
}
```

The skeleton gets **no spawn check of its own**, and `ge` never asks whether it is riding, so a
mounted skeleton still runs its AI, still aims and still fires -- only its position comes from the
spider.

### `je` -- the explosion

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

`cr` (ModelBiped) is seven boxes and is the player's. `cb` (ModelZombie) replaces the arms outright
-- both forward to horizontal with a slow idle sway on `ticksExisted` -- and **both the zombie and
the skeleton are posed with it**, because `fv` (ModelSkeleton) extends `cb` and only narrows the
four limbs to 2 x 12 x 2. `cr`'s swing block is **dead for every mob in this version**: `swingProgress`
is only ever advanced by `swingItem`, which nothing but a player calls.

`em` (ModelCreeper) builds seven boxes and **renders six** -- the headwear is constructed and never
drawn. `jy` (ModelSpider) is eleven: a head, a small thorax, the abdomen, and eight 16 x 2 x 2 legs
splayed by a fixed pair of angles that the walk modulates at **twice** the quadruped's leg
frequency. `hh` (ModelSlime) is a switch on its one argument: `hh(16)` is the inner body with the
face and `hh(0)` is the outer shell, and its `setRotationAngles` is empty -- **a slime is never
posed.**

Two renderers use `dn`'s one hook, `preRenderCallback`, which runs between the `glScalef(-1,-1,1)`
and the model's own lift:

- `d` (RenderCreeper) scales by the fuse: `(1 + s^4 * 0.4) * wobble` in x and z, where
  `s = timeSinceIgnited / (fuseTime - 2)` and `wobble = 1 + sin(s * 100) * s * 0.01`. **The divisor
  is `fuseTime - 2`**, so the swell passes 1 two ticks before the blast and keeps going.
- `gq` (RenderSlime) scales by `size` and by the squish, which is why `hh`'s boxes are the same for
  a size-1 and a size-4 slime.

`ok` (RenderSpider) adds a second pass over the whole model from `mob/spider_eyes.png`, blended at
`(1 - brightness) * 0.5`.

## What is this port's and not a1.1.2's

- The **pathfinder's budgets** (1,024 nodes, one search a tick) and its coordinate identity. **The
  monsters share the one search a tick with the animals**, and they use it harder: a chasing
  monster asks once in twenty ticks where a wandering animal asks once in forty.
- **`entityToAttack` is a boolean here**, because the only entity a monster can target in this
  build is the player: `dq.i()` asks `getClosestPlayer` and the only other writer is
  `attackEntityFrom`, whose source is also only ever the player.
- **The spider jockey's mount is an index** into a pool that swap-removes, fixed up on every
  removal. a1.1.2 holds a reference.
- **An arrow excludes its shooter by place rather than by identity**, for the same reason: it skips
  a mob still standing over where it was fired from, for the five ticks the jar skips the shooting
  entity itself.
- **The difficulty lives in `<world>/3dalpha.ini`.** a1.1.2 keeps it in `options.txt`, which is a
  file this port does not have, and it is a per-world setting here.
- **The explosion's cell record is a bitset over a cube of radius 8** rather than a `HashSet` of
  `ChunkPosition`. The set is the same set; what a set has no order to match is the order the jar
  iterates it in, so the destruction runs back-to-front over the cube instead.
- **Mobs in `level.dat`.** a1.1.2 writes animals into each chunk's `Entities` list; this port has
  no `entitydata` slot yet, so they ride in the port's own `Data/3DAlphaEntities` compound with the
  other six pools. Native chunk `Entities` stay preserved and unread. A world carried back to the
  real client keeps its terrain and loses these animals.
- **The death puff is drawn.** `ge.e_()` throws twenty `explode` particles out of the model's own
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

- **The player has no health**, so a monster's fist leaves through a seam
  (`MobSurroundings::hurtPlayer`) rather than landing: `platform/ctr/main.cpp` takes the two parts
  that exist -- `ge.a(Lkh;IDD)V`'s knockback and `random.hurt` -- with `ge`'s ten-tick
  invulnerability window in front of them, and counts the damage instead of subtracting it.
  Survival is M3 step 4 and `dm.a(Lkh;I)Z`'s difficulty and armour scaling belongs with it.
- **`hl` -- the Giant.** Registered in `ew` at id 53, drawn by `nz` at six times scale, and
  constructed by nothing in the game. Not ported, and not a gap.
- **The spider's eye overlay is one part rather than a second model.** `ok` draws the whole of
  `jy` again from `mob/spider_eyes.png` and blends it at `(1 - brightness) * 0.5`; the eye page is
  transparent everywhere but the head, so this redraws the head alone and lets the detail pass's
  alpha test do the rest. The blend becomes an alpha cut, so the eyes do not dim in daylight.
- A mob's **water push** is asked once a tick here and three times in the jar (`onEntityUpdate`,
  `onLivingUpdate`, `moveEntityWithHeading`), each adding four thousandths of the current. The same
  is true of the player body and is noted in `docs/physics-a1.1.2.md`.
- **The jukebox.** `ly.aZ` is placeable and drawable and does nothing with a record put on it.
- **No hardware run.** Every number in the rendering notes above is a vertex count, not a frame
  time -- and the monsters are the first thing here that can put two hundred models and a
  1,352-ray explosion in front of a 268 MHz ARM11 at the same moment. The first hardware report on
  them found that **nothing spawned at all** (status.md 26), which is the kind of thing only a
  console shows: the host suite has no entity query wired into its worlds.
