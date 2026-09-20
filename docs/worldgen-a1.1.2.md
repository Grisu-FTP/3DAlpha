# a1.1.2 world generation, recovered from the client jar

Everything here was read out of `minecraft-a1.1.2_01-client.jar` by disassembly, not from a wiki and
not from memory. Obfuscated names are version-specific and mean nothing in any other release.

This is the working document for M4. `docs/status.md` is still the authority on *where the project
is*; this is the authority on *what the original does*.

---

## The classes

Identified by structure — field types, method descriptors, constant values, call relationships —
never by name, the same discipline `tools/extract_blocks.py` uses for the block table.

| Obf | What it is | The evidence that pins it |
|---|---|---|
| `nw` | `ChunkProviderGenerate` | `public nw(cn, long)`, `implements aw`, eight `lp` fields; `b(int,int)` allocates `newarray byte` of **32768**; the noise constants 684.412, 512.0, 8000.0, 1.4, 3.0, 12.0, −10.0 |
| `v` | `NoiseGeneratorPerlin` | `extends bk`; `int[] d` of **512**; three `nextDouble()*256.0` offsets; Fisher–Yates `nextInt(256 - i) + i`; the 6/15/10 fade polynomial |
| `lp` | `NoiseGeneratorOctaves` | `extends bk`; `v[] a` plus a count; `lp(java.util.Random, int)`; octave loop halving amplitude |
| `bk` | `NoiseGenerator` | abstract base of `v` and `lp`, constructor only |
| `cy` | `MapGenBase` | `protected int a = 8` (chunk range), `protected java.util.Random b`, `a(nw, cn, int, int, byte[])` |
| `kk` | `MapGenCaves` | `extends cy`; the tunnel carver; 3.1415927f, 1.5707964f, 0.92f, 0.75f, 0.7f, 0.9f, 0.1f |
| `ik` | `WorldGenerator` | `public abstract boolean a(cn, java.util.Random, int, int, int)`; **exactly nine subclasses** |
| `cu` | `WorldGenMinable` | `cu(int blockID, int count)`, constructed seven times in populate |
| `cg` | `WorldGenDungeons` | loot-item and mob-name helpers; writes a chest inventory |
| `gv` | `WorldGenClay` | `gv(int)`, constructed with 32 |
| `ej` | `WorldGenBigTree` | `int[][]` leaf-node list; `Math.pow` ×7, `Math.sqrt` ×3, `Math.sin`, `Math.cos` |
| `oa` | `WorldGenTrees` | the `else` branch opposite `ej` |
| `ae` | `WorldGenFlowers` | `ae(int)`, constructed four times with the flower blocks |
| `es` | `WorldGenReed` | checks four neighbours for water material |
| `da` | `WorldGenCactus` | one try, straight after reeds |
| `nn` | `WorldGenLiquids` | `nn(int)`, constructed with water and lava |
| `eo` | `MathHelper` | `static float[]` of **65536**, `a[i] = (float)Math.sin(i * 2π / 65536)` |
| `cn` | `World` | `long u` is `RandomSeed`; constructs `new nw(this, u)` |
| `ga` | `Chunk` | `ga(cn, byte[], int, int)`; `c()` is `generateSkylightMap` |

### Negative results, which are worth as much

- **There is no biome or climate class.** No `WorldChunkManager`, no temperature or humidity map.
  `nw`'s surface pass is pure noise — sand at scale 0.03125, gravel with a 109.0134 offset, stone at
  1/32, thresholds against `nextDouble() * 0.2`. **This confirms `"hasBiomes": false` in
  `versions/a1.1.2.json` and the `alpha_nobiome` slot name.** The claim was previously only
  half-verified; it is now verified.
- **There is no `SpawnerAnimals`.** No worldgen-time animal spawning to reproduce. The eighth
  octave generator was previously written up here as a `mobSpawnerNoise` that nothing reads —
  **that was wrong**. It is `nw.c`, and `populate` samples it to decide how many trees a chunk
  gets. Corrected when trees were transcribed; the name in the code is now `treeDensity_`.
- `fn` is `Packet`, not `WorldGenerator`. It has 27 subclasses and is the obvious trap for a "many
  small subclasses" search. The real hierarchy is `ik`, with nine.

---

## Seed derivation

This is the part that has to be exactly right. Every generator drawn from the same `Random` shifts
everything after it, so construction order is as load-bearing as the constants.

**Constructor**, `nw(cn world, long seed)` — octave counts in construction order:

```
j = new Random(seed)
noise1 = lp(j,16)  noise2 = lp(j,16)  noise3 = lp(j,8)
noise4 = lp(j,4)   noise5 = lp(j,4)   noise6 = lp(j,10)
noise7 = lp(j,16)  treeDensity = lp(j,8)
```

Each `lp(rand, n)` constructs `n` Perlins; each Perlin drains **three `nextDouble()`** and then
**256 `nextInt(256 - i)`**.

**Per chunk**, `nw.b(cx, cz)` — note there is **no XOR with the world seed** here:

```java
j.setSeed(cx * 341873128712L + cz * 132897987541L);
byte[] blocks = new byte[32768];
generateTerrain(cx, cz, blocks);
replaceBlocksForBiome(cx, cz, blocks);   // consumes nextDouble()/nextInt()
caves.generate(this, world, cx, cz, blocks);
new Chunk(world, blocks, cx, cz).generateSkylightMap();
```

**Populate**, `nw.a(aw, cx, cz)` — this one *does* XOR:

```java
BlockSand.fallInstantly = true;
j.setSeed(world.randomSeed);
long s1 = (j.nextLong() / 2L) * 2L + 1L;
long s2 = (j.nextLong() / 2L) * 2L + 1L;
j.setSeed((long)cx * s1 + (long)cz * s2 ^ world.randomSeed);
```

then, in exactly this order: dungeons ×8, clay ×10, dirt ×20, gravel ×10, coal ×20, iron ×20,
gold ×2, redstone ×8, diamond ×1, trees (count from a noise lookup plus `nextDouble()`, big tree when
`nextInt(10) == 0`), yellow flower ×2, red flower when `nextInt(2) == 0`, brown mushroom when
`nextInt(4) == 0`, red mushroom when `nextInt(8) == 0`, reeds ×10, cactus ×1, water ×50, lava ×20,
then a snow sweep over the 16×16.

**Caves** use the same idiom independently, with their own `Random`, over a 17×17 chunk
neighbourhood:

```java
b.setSeed(world.randomSeed);
long s1 = (b.nextLong()/2)*2+1;  long s2 = (b.nextLong()/2)*2+1;
for (x = cx-8 .. cx+8) for (z = cz-8 .. cz+8) {
    b.setSeed(x*s1 + z*s2 ^ world.randomSeed);
    recursiveGenerate(...);
}
```

---

## The noise, transcribed

`src/impl/worldgen/alpha_nobiome/noise.{hpp,cpp}` is `v` and `lp` whole, and it is bit-exact against
the jar's own classes over the shapes the generator asks for. The helpers are Perlin's standard ones
and can be read off directly:

```
fade(t)        = t*t*t*(t*(t*6 - 15) + 10)
lerp(t, a, b)  = a + t*(b - a)                 // note the interpolant comes first
grad(h, x,y,z) = ±u ± v, u = h<8 ? x : y, v = h<4 ? y : (h==12||h==14 ? x : z)
floor(v)       = (int)v, then decrement if v < (int)v    // not Math.floor
```

Two things are Minecraft's own, and both would be "corrected" by anyone reaching for a noise library.

**Octave amplitude goes up, not down.** The octave loop starts a factor at 1.0 and halves it each
pass. It multiplies the *scales* by that factor — so each successive octave is lower frequency — and
divides the *contribution* by it, so each successive octave counts for twice as much. That is
backwards from a conventional fBm, and sixteen octaves of it is what a1.1.2's terrain is.

**The lattice fill caches its Y slab, and the cache is observable.** The four X-interpolated corner
values are recomputed only when the integer Y cell changes (or on the first Y step), while the
gradients feeding them depend on the *fractional* Y too. So when two Y samples share an integer cell
— any Y scale under 1 — the second silently blends stale corners.

The obvious guess about what that looks like is wrong, and worth writing down: it does **not** make
consecutive samples equal, because `fadeY` is still recomputed every sample. It interpolates between
stale corners, which is a smaller and much harder-to-spot difference. The way to see it is to ask for
the same points batched and then one at a time — a single-sample call has `yi == 0` and so always
recomputes, which is what a cacheless implementation produces. `tests/noise_test.cpp` pins both
directions: that the two disagree at a Y scale of 0.01, and that they agree exactly at the terrain
lattice's 684.412/160 ≈ 4.28, where every sample lands in its own cell.

So a1.1.2's own terrain never triggers the quirk. A faithful generator still has to have it.

**Iteration order is X, then Z, then Y**, with the output index simply counting up — so the buffer is
**Y-fastest**, which is not the order the argument list reads in. Pinned separately, because a
transposed buffer still looks like noise.

Object sizes, since the 3DS build has an 8 KB stack-usage ceiling and this project has already lost a
launch to a large local: a `PerlinNoise` is 2,072 bytes and an `OctaveNoise` is **33,160**. The chunk
provider's eight come to roughly 265 KB. Heap, once, never a stack local.

## Terrain, transcribed

`src/impl/worldgen/alpha_nobiome/chunk_provider.{hpp,cpp}` is `nw`'s first two stages, bit-exact
against the jar's own class over whole 32,768-byte columns.

The oracle turned out to be reachable after all. `nw` takes a `World` and every World constructor
does file I/O, which looked like a wall; it is not one. The provider's constructor never *calls* the
World, only stores it, and `generateTerrain` reads exactly one field from it. So
`Unsafe.allocateInstance` hands over a zeroed World and the whole generator runs with no save
directory and no game context. That is what `genref --terrain` does.

**Shape.** A 5 × 17 × 5 density lattice from `initializeNoiseField(cx*4, 0, cz*4, …)`, four cells
across a chunk, trilinearly interpolated up to 16 × 128 × 16 — 4 blocks per cell in X and Z, 8 in Y.
**Sea level is 64.** Per block: below sea level it is stationary water, or **ice** at exactly
sea level − 1 when the world is snow-covered; then stone overwrites either if density > 0. The block
array is `x << 11 | z << 7 | y`.

### The one thing the seed does not decide

**`SnowCovered` is an independent coin flip, not a function of the seed.** The roll is
`n.nextInt(4) == 0`, and `n` is `new Random()` — the *no-arg* constructor, seeded from entropy rather
than from the world seed. Two worlds created with the same seed can therefore differ: one frozen, one
not, at one in four. Observed directly, twice, before it was explained: the same seed produced ice at
y=63 in one run and open water in the next.

So "the same seed generates the same world" is true of a1.1.2 with exactly this asterisk, and it is
the original's behaviour rather than ours to fix. Two consequences:

- `SnowCovered` is an **input** to the generator, never derived from the seed. `GeneratorOptions`
  carries it and the reference vectors pin both values.
- World creation has to roll it from something that is not the seed, or a1.1.2's own worlds would be
  unreachable — a player entering a known seed gets the frozen variant one time in four, exactly as
  they would have in 2010.

**`World.d` is `SnowCovered`**, and it was worth chasing rather than assuming. It is read from
level.dat on load and, on a *new* world, rolled as `rand.nextInt(4) == 0` — so **one in four fresh
a1.1.2 worlds is a snow world**. `cn` writes it back unconditionally on every save. `docs/world-format.md`
used to call it "not modelled, preserved", which was right until something generated terrain;
`LevelData::snowCovered` now models it.

Three details in the shaping maths that a careful rewrite would get wrong:

- **The top taper's interpolant is computed in `float`.** The bytecode emits `i2f / fdiv / f2d`, so
  the division happens at single precision inside an otherwise-double expression. Doing it in double
  changes the last bits of every block in the top 32 layers.
- **An ocean throws its peakiness term away.** When the depth noise comes out negative, `scale` is
  reset to 0 before the `+ 0.5`, rather than being scaled down.
- **The bottom taper is dead code.** Its threshold local is written once, to `0.0`, and compared
  against a loop variable that starts at 0. Confirmed against the bytecode rather than assumed; the
  transcription records it as a comment instead of an unreachable branch.

**Surface pass.** `nw.b` lays grass/dirt, sand, gravel and bedrock over the bare stone, top down.
Three things about it are load-bearing:

- **Sand and gravel come from the same octave stack**, called twice with the axes shuffled and a
  `109.0134` offset on the second. That is the original's way of getting two uncorrelated fields from
  one generator, and it is why the transcription has one `sandGravel_` member and not two.
- **`nextInt(6)` is drawn on every one of the 128 Y steps**, whatever the outcome. Hoisting it out of
  the loop, or guarding it with a `y < 5` test, is the obvious optimisation and desynchronises every
  column after the first. It is what gives bedrock its ragged underside.
- **The noise arrays are read transposed** relative to how they were generated. Kept exactly; it
  decides where beaches land.

## Caves, transcribed

`src/impl/worldgen/alpha_nobiome/caves.{hpp,cpp}` is `kk` over `cy`, bit-exact against the jar.

**The driver walks a 17 × 17 chunk neighbourhood**, reseeding per cell, because a tunnel begun eight
chunks away can still reach into this column. Generating one chunk therefore costs 289 cave rolls,
not one — and fourteen cells in fifteen roll `nextInt(15) != 0` and produce nothing, which is what
makes caves sparse. A fixture set chosen for terrain leaves the carver almost untouched, so the
vectors carry four cases picked by counting carved blocks.

`nextLong() / 2 * 2 + 1` is **truncating division, not a shift**. `(-5)/2*2+1` is −3; `(-5 >> 1) << 1`
is −6. Both odd, different worlds.

**No transcendental anywhere.** Headings go through MathHelper's 65,536-entry float table, built once
from `(float)Math.sin(i * PI * 2 / 65536)`. Our table is computed at startup and hashed against a
hash taken from a real JVM — all 65,536 entries agree, so nothing has to be compiled in and libm is
out of the path. The lookups are deliberately *quantised* (about 0.0006 rad); calling `std::sin`
instead would be more accurate and would carve different caves.

**Everything is float** except the position accumulators and the ellipsoid test. Promoting any of the
heading, taper or wobble maths to double gives a completely plausible cave system that is not the one
the seed asks for.

Three things that read like bugs and are not:

- **The carve loop's index leads its height by one.** `index` starts at `y1` while the loop starts at
  `y1 - 1`, and both step down together, so every block written sits one above the height its
  ellipsoid test was computed for. The decrement is at the tail of the loop, not the head. Verified
  by mutation: "fixing" it fails the vectors.
- **The water probe only tests the shell of the box.** An interior column jumps straight to the
  bottom via a `by = y0` assignment inside the loop.
- **Three carve steps in four are skipped** by `nextInt(4) == 0`, after the position has already
  advanced — so it thins the carving without shortening the tunnel.

Two useful properties fell out of the transcription. **Recursion depth is at most two**: a node
branches only while its width exceeds 1.0, and a branch's width is drawn as `nextFloat() * 0.5 + 0.5`,
which never reaches it — worth knowing on a console with a 32 KB stack. And caves place **flowing**
lava (id 10) below y=10, not still lava; read off the jar, where the field is constructed with
`bipush 10`, and pinned by mutation because a wrong id there is otherwise invisible.

## Population — the oracle exists, the transcription does not

Population is architecturally unlike everything above it, and that is why it needed a different
oracle before a line of it could be written.

Terrain, the surface pass and caves all operate on **one 32,768-byte array** and never touch the
World, which is what let `--terrain` drive them over an uninitialised instance. Population does the
opposite: its nine generators read and write blocks **through the World, across chunk boundaries**,
and a chunk is only populated once its neighbours exist. There is no single array to hand it.

The surface it actually needs turns out to be small — seven methods, mostly `getBlockId` and
`setBlock`, plus the block material, the height map and a tile entity for the dungeon chest. But
stubbing those would mean testing our generators against our own idea of what a World does.

**So `genref --world` runs the real one.** `new cn(dir, name, seed)` constructs an actual World with
an actual save directory, and asking it for a chunk generates, populates and lights it. It works
headlessly — World is game logic, not rendering — which was the open question and is now answered.
That makes it a complete oracle for the whole pipeline, and it covers lighting too, so Stage 4 is
unblocked by the same tool.

Two things have to be pinned or the output is not reproducible, and both were found the hard way:

- **SnowCovered must be forced**, because it is a coin flip rather than a function of the seed. See
  above.
- **The chunks must be far from spawn.** The World constructor looks for a spawn point, which
  generates chunks around the origin; asking for those reads back whatever the constructor cached.

With both pinned, two independent runs come out byte-identical.

The reference fixture is deliberately **not checked in yet**. It can only be compared once all nine
generators exist — a partially populated chunk matches nothing — so checking it in now would add a
1,700-line file that no test reads. The tool is the deliverable; the fixture lands with the
transcription.

### The populate order, recovered

Counts, Y bounds and vein sizes, read off the driver's bytecode rather than transcribed by eye:

| pass | tries | y drawn below | vein | block |
|---|---|---|---|---|
| dungeons | 8 | 128 | — | — (offset by +8 in x and z, unlike everything else) |
| clay | 10 | 128 | 32 | 82 |
| dirt | 20 | 128 | 32 | 3 |
| gravel | 10 | 128 | 32 | 13 |
| coal | 20 | 128 | 16 | 16 |
| iron | 20 | 64 | 8 | 15 |
| gold | 2 | 32 | 8 | 14 |
| redstone | 8 | 16 | 7 | 73 |
| diamond | 1 | 16 | 7 | 56 |

then, in this order, all offset by +8 in x and z:

| pass | count | notes |
|---|---|---|
| trees (`oa`, or `ej` on a 1-in-10 roll) | noise-derived | y comes from the world's **height map** |
| dandelion (`ae`, id 37) | 2 | |
| rose (`ae`, id 38) | 1, on `nextInt(2) == 0` | |
| brown mushroom (`ae`, id 39) | 1, on `nextInt(4) == 0` | class `ky`, not `mq` |
| red mushroom (`ae`, id 40) | 1, on `nextInt(8) == 0` | |
| reeds (`es`) | 10 | |
| cactus (`da`) | 1 | |
| water spring (`nn`, id 8) | 50 | y = `nextInt(nextInt(120) + 8)` |
| lava spring (`nn`, id 10) | 20 | y = `nextInt(nextInt(nextInt(112) + 8) + 8)` |
| snow sweep | 16×16 | |

Every id there was resolved out of `ly`'s static initialiser rather than remembered — scanning
forward from each `new` to the first literal, which is the method that was got wrong once already.

### Ores and clay, transcribed

`src/impl/worldgen/alpha_nobiome/ore.{hpp,cpp}`, bit-exact against `cu` and `gv` run in a real World.

A vein is a line segment with a sphere swept along it, radius rising and falling as a half sine.
Only the target block is replaced, so a vein crossing a cave comes out pitted rather than truncated.
**Clay is the same code** — in the original too, `gv` is a copy of `cu` — with a water-material guard
and sand instead of stone as the target.

Two details pinned by mutation, because both are invisible by inspection:

- **The step loop is `i <= veinSize`, not `<`.** One extra iteration, which changes the vein's length
  *and* how many doubles it draws — so getting it wrong desynchronises everything after it.
- **Clay's guard runs before any draw.** Ten dry-land tries per chunk must leave the stream exactly
  where they found it.

Population also needed a new seam: `population_view.{hpp,cpp}`. Terrain, surface and caves each own
one column; population addresses the world absolutely and writes across chunk boundaries, because
`World.setBlock` simply fetches whichever chunk owns the coordinate and **generates it if missing**.
We cannot generate on demand from inside a generator — unbounded recursion on a 32 KB stack — so the
caller hands population a window of already-generated columns. A 3×3 is provably enough for ores and
clay (largest reach about seven blocks); trees and dungeons must be re-measured before they are
added. Writes outside the window are counted separately from writes the original would also refuse,
so a test can assert the count stays zero.

One bug worth recording because the failure mode was silent. `MathHelper`'s table is built lazily,
and the ore generator did not build it — so `sin` returned **0 everywhere** and veins came out at
eight blocks against the jar's hundred and eight. A zeroed lookup table is a wrong answer, not a
crash. `sin` and `cos` now call `ensureBuilt()` themselves; the cost is one predictable branch, and
it removes the whole category.

**Lighting is not a Stage 4 afterthought; it is a Stage 3 dependency.** Established against the jar
by reading each generator's `canBlockStay`, not assumed:

| generator | needs the light engine? | why |
|---|---|---|
| liquids (`nn`) | no | block ids only |
| reeds (`es`) | no | ids and materials |
| cactus (`da`) | no | ids and `Material.isSolid` |
| flowers and both mushrooms (`ae`) | **yes** | `mq.g` is `(getBlockLightValue >= 8 or canSeeSky) && groundOk`; `ky.g` is `getBlockLightValue <= 13 && groundOk` |
| trees (`oa`, `ej`) | **yes**, indirectly | the driver places each one at `world.getHeightValue(x, z)` |

Note that the mushrooms go through the *same* generator class as the flowers — `ae` is handed a
block id — but `ky` overrides `canBlockStay` to want darkness where `mq` wants light. One generator,
two opposite predicates.

**Every population generator is transcribed and verified, and so is the driver that runs them, and
so is the driver above *that*.** Terrain, caves, lighting, the whole of population and the chunk
provider that decides what to generate and when to populate it all match a real a1.1.2 World byte
for byte. See "The chunk generator" below. **WorldGenDungeons** still places a chest whose contents
are dropped, because that needs tile entities — today `ChunkColumn` round-trips those as opaque
blobs and has never parsed one.

### The two generator bugs the Extra Settings screen can switch off

Both are a1.1.2's, both are reproduced by default, and both are optional per world from
World Settings → Extra Settings. The switches live in `<world>/alpha.ini`
(`core/settings/world_settings.hpp`) and reach the generator through
`worldgen::GeneratorOptions`, which `WorldStreamer::open` fills by reading that file. **Neither
switch changes a single random draw** — each only widens what the already-drawn shape is written
into — so a world with one on stays in step with a1.1.2 through every later pass.

**The negative-quadrant ore bug.** `cu.a` computes its per-step bounding box with six `(int)`
casts, and a Java `(int)` truncates toward zero rather than flooring. At non-negative coordinates
the two agree. At negative ones truncation rounds *up*, so the box is shifted one block toward
zero: the row at its low end is never visited, and the extra row at its high end fails the
ellipsoid test and writes nothing. It costs a row only when the fractional part falls the wrong
way — roughly half the steps — and it applies to x and z independently, y being always positive.

Measured with `tests/ore_test.cpp` (`ore_veins_lose_blocks_in_the_negative_quadrants`), the same
vein mirrored into all four quadrants over 400 seeds, as a fraction of what the positive quadrant
places:

| vein size | −x | −z | −x and −z |
|---|---|---|---|
| 8 (iron, gold) | −14.5% | −14.8% | −24.8% |
| 16 (coal) | −3.5% | −5.2% | −12.6% |
| 32 (dirt, gravel, clay) | −1.9% | −3.1% | −5.5% |

Small veins suffer most, which is what a per-step edge row predicts — and diamond and redstone are
size 7. `OreBounds::FloorBounds` floors instead, and makes all four quadrants place *exactly* the
same count; that equality is what makes this a derivation rather than a guess, since no other
cause would be removed by flooring the box. Clay goes through the same switch, because it is the
same generator with a different block and a world symmetric in its ore and asymmetric in its clay
would be neither thing.

**The bedrock hole.** `nw.b`'s floor test is `y <= random.nextInt(6) - 1`, drawn on every one of
the 128 steps of every column — the draw that gives bedrock its ragged underside, and which cannot
be hoisted or guarded without desynchronising every column after the first. At `y = 0` it fails
whenever the draw returns 0, so about a sixth of a1.1.2's columns have stone rather than bedrock
at the bottom of the world. Measured over 4,096 columns in `tests/terrain_test.cpp`. The switch
lays bedrock at `y = 0` unconditionally and **still makes the draw**. It is generation-only: it
does not fill in a hole that is already on the card, and the screen says so.

### Liquid springs, transcribed

`src/impl/worldgen/alpha_nobiome/liquids.{hpp,cpp}`, and this one gets an **exhaustive** oracle
rather than sampled cases, because it can have one. `nn.a` draws no random numbers — it takes a
`Random` and never touches it — and reads exactly seven blocks: above, below, centre, and the four
horizontal neighbours. Its behaviour is therefore a pure function of seven ids, and
`tests/liquid_vectors.hpp` is the whole truth table over {air, stone, other}: 494 cases, for water
and for lava. The harness also asserts per case that the `Random` it hands over comes back untouched.

The predicate: stone above **and** below, centre air or stone, and exactly three stone sides with
exactly one air side. A neighbour that is neither — dirt, gravel, an ore — counts toward neither
total, which is what keeps springs out of the middle of a dirt patch and is the case a hand-written
fixture would miss.

Two mutants of that predicate survive the full truth table, and both are **provably equivalent
programs** rather than gaps: with only four sides, `stoneSides >= 3` with `airSides == 1` forces
`stoneSides == 3`, and `stoneSides == 3` leaves one side over so `airSides >= 1` forces
`airSides == 1`. `== 4`, dropping a neighbour read, and skipping the above-check are all caught.

`nn` is also the only population generator that calls **`cn.d`** (setBlock *with* notify) where the
others call `cn.a`. For a fluid the notification only schedules a tick, so no block changes and one
setter covers both; the distinction is recorded because it stops being cosmetic once fluid ticking
exists.

### Reeds and cactus, transcribed

`src/impl/worldgen/alpha_nobiome/plants.{hpp,cpp}`, verified against `es` and `da` in a real World.

Unlike the liquids, **these two draw conditionally on what they find**: a try that lands on air
spends two more numbers on a height, one that lands on stone does not. So a transcription can place
every plant correctly and still be wrong by drawing a different number of times. Each fixture case
therefore carries a **stream fingerprint** — one `nextLong` taken after the generator returns —
which fails on a wrong draw count even when every block landed right. Six of the twelve cases place
nothing at all, and those are the ones that pin the draw count on the refusal path.

One C++ hazard with no Java counterpart, and it is not hypothetical:

**`x + rand.nextInt(n) - rand.nextInt(n)` is left-to-right in Java and unspecified in C++.** The
operands of `-` have no sequencing, even in C++17, so the compiler is free to swap the two draws —
producing a plausible plant distribution on one compiler and a different world on another. Both
draws are named locals in a `perturb` helper, and mutating the helper to swap them fails the
fingerprint, so the ordering is pinned rather than assumed.

### What the plant oracle cost, and why it is built the way it is

The `--plant` scene is built **underground at y = 40, in solid rock, at a site probed for rather
than chosen**. All three of those are the fix for one bug, and the first two diagnoses of it were
wrong.

Building the scene at sea level died with a `StackOverflowError` thousands of frames deep. The first
guess was flowing water, since the trace ran through a class called `kn`; shrinking the water pool
changed nothing. Moving the scene to y = 100 to get away from the ocean made it worse. `kn` turns
out not to be a block at all — it holds a light type and a bounding box — so what was recursing was
**a1.1.2's light propagation**, and a 41×41 slab of solid blocks written into open sky is close to
the worst possible input to it: every column underneath loses direct skylight and the relight walks
the lot.

Underground it is a non-event, because skylight there is already zero. The site probe then rules out
the other half: a box containing water, lava or a cave brings back both cascades, so a site is
accepted only if every block in it plus a two-block margin is already solid and dry.

Worth keeping in mind for the generators still to come — **the oracle's binding constraint is the
light engine's recursion, not the generator under test.**

### Lighting, transcribed — and the one place parity has an asterisk

`src/core/world/lighting.{hpp,cpp}`. Sky light and the height map are **bit-exact** against a real
a1.1.2 World whose light queue has been drained to a standstill; block light is exact everywhere the
renderer can sample it, and the exception is characterised below rather than waved at.

**What the original does.** `Chunk.generateSkylightMap` fills each column downward from its own
height map, and everything else — light crossing a chunk boundary, block light, the corrections
needed once a neighbour finally loads — goes through a queue of bounding boxes (`kn`) that
`World.updateLights` drains a thousand at a time, from the *end* of the list, re-scanning each box
and rescheduling its neighbours. The rule it applies, per cell, is

    light(p) = max( emitted(p), max over the six neighbours n of ( light(n) - max(1, opacity(p)) ) )

clamped at zero. That is a fixed-point iteration of a monotone operator with a strictly positive
decrement, so it has exactly one solution — the familiar "best any source can do after paying the
opacity along the way". **Any algorithm that finds that solution finds the same numbers**, which is
what makes it legitimate to replace the box queue with something faster.

**What we do.** A bucketed breadth-first search: sixteen queues, one per light level, drained 15 down
to 1, each cell settled once. `tools/genref.java --light` provides the ground truth — it drains
`updateLights` until the queue is empty and **refuses to emit a fixture if it ever fails to
converge** — and `tests/light_test.cpp` compares all 32,768 sky and 32,768 block values per case.

**Three quirks that had to be reproduced.**

  * `getSavedLightValue` answers with the light type's *default* for any y outside [0, 128), and
    Sky's default is 15. So **the world's floor and ceiling are both sky sources**: the cell at
    y = 127 sees 15 from above and the cell at y = 0 sees 15 from below, whether or not either can
    see the actual sky. Generated terrain reaches neither end, so no fixture exercised it until one
    was built for it — see below.
  * Opacity 0 is substituted with 1 for the decrement, but the *height map* tests the raw value. Air
    and glass are both raw 0; clamping before the height test would make air stop the height map.
  * An id this version does not define has opacity 0, not 255. Our block table gives unknown ids a
    solid opaque cube on purpose, but `Block.lightOpacity` in the jar is a plain 256-entry array that
    was never written for those ids, so the original lets light straight through.

**The asterisk: block light inside fully-opaque blocks.** `Chunk.setBlock` schedules both a Sky and a
Block box for every block it writes — but terrain and the cave carver do not go through `setBlock`,
they write the byte array directly. So **lava carved into a cave is never scheduled for block light
at all** and stays at zero unless some later `setBlock` happens to enclose it in a box. In one
measured chunk, 326 of 385 lava blocks were lit by the original and 59 were not.

Our engine computes the fixed point, so it lights all 385. Measured, the 59 are all **buried**:

| | count |
|---|---|
| dark lava touching air | **0** of 59 |
| lit lava touching air | 45 |
| air cells beside lava, lit to >= 14 by the original | **45 of 45** |

So the original lights every lava block a player could ever see the glow of, and leaves dark only
the ones packed inside other lava. The difference is confined to the light nibble stored *at* those
coordinates: opacity 255 means nothing can put light into them, and the mesher reads the light of
the cell a face *looks into*, skipping opaque neighbours entirely. **Not one non-opaque cell
differs** -- the illumination in the world is identical, and only a byte-comparison of the saved
`BlockLight` array would show it.
The test asserts exactly that split rather than a tolerance: exact equality wherever light can go,
and "never darker, and always an emitter" where it cannot.

Reproducing the original here would mean porting the box queue itself, interleaving it with
population, and giving up light being a function of the block array — which would also mean a loaded
chunk could never be re-lit. Recorded as a deliberate deviation, like the far-from-origin camera.

**`kn` is not a block.** Worth writing down because it cost two wrong diagnoses in the plant oracle
before it cost anything here: `kn` holds a light type and a bounding box, and a stack trace cycling
through `cn.e` and `kn.a` is the **light engine** recursing, not fluid flow.

**Efficiency.** The window is 48 x 128 x 48 = 294,912 cells. On a real chunk the search settles
11,520 of them for sky and 5,449 for block — under 4% — so the cost was never the search but the
full-window passes feeding it. Reading the block array once and deriving the opacity field, the
height map and the source list together, and having the sky seed write its own zeros instead of
following a memset, took the host time from 0.61 ms to **0.39 ms** a chunk. The engine holds two
bytes per cell, 576 KB, allocated once for its lifetime and reused — never per chunk, never on a
stack.

### Flowers and mushrooms, transcribed — and the light they actually read

`src/impl/worldgen/alpha_nobiome/flowers.{hpp,cpp}`. One class, `ae`, plants all four: dandelion
(37), rose (38), brown mushroom (39) and red mushroom (40). The id decides which `canBlockStay`
answers — `mq` for the flowers, `ky` for the mushrooms — and the two want **opposite** things:

| | light | ground |
|---|---|---|
| `mq` (flowers) | at least 8, **or** a clear view of the sky | grass, dirt or farmland |
| `ky` (mushrooms) | 13 or less | any opaque cube |

**The light they read is not the light the world ends up with**, and establishing that was most of
the work. Population runs inside chunk generation, and `World.updateLights` only ever runs from the
game tick — so at that moment the box queue has not been drained at all. Measured against a real
World rather than reasoned about:

  * **Block light is exactly zero** during population. Every byte of a freshly generated chunk's
    block-light array. Nothing writes it synchronously; only the queue does.
  * **`skyLightSubtracted` is zero**, because a new world starts at time 0.
  * **Sky light is the per-column fill** `Chunk.relightBlock` writes on the spot: 15 at and above
    the height map, decaying downward by each block's opacity until it hits zero. No horizontal
    spreading, because that is exactly the part that is queued.

So `getBlockLightValue` during generation depends only on the column it is asked about, which is why
`PopulationView` can answer it without the light engine. It is also why a flower grows in the shade
under a tree: the canopy drops the height map, and the decay beneath it is 14, 13, 12 rather than 0.

**The oracle had to be corrected for this.** The first `--flower` run drained the light queue after
building each scene, and the jar duly planted roses nine blocks in from the edge of a solid stone
roof — where the *converged* sky light had settled at exactly 8. Real population never sees that.
Draining now happens only while probing for a site, never after the scene is built.

Three other things the oracle got wrong before it got them right, all worth recording because each
produced a plausible fixture:

  * **Scenes carved underground light nothing.** Flowers need 8 or sky and a buried room has
    neither, so all ten flower cases came back empty and all the mushroom cases came back full —
    correct behaviour, useless fixture. The scenes now sit on the terrain's own grass.
  * **A roof raises the height map by five.** Reading the ground height *after* building the scene
    started the generator on the roof, and with only ±3 of vertical perturbation it could never
    reach the ground.
  * **The terrain is real terrain, so it already contains flowers** that a1.1.2's own population
    put there. Scanning a wider window than the oracle did counted two of them as ours.

Two mutants of the mushroom bound are worth distinguishing. `13 -> 12` fails, once a scene exists
with leaves one block above the ground — the only place a plantable cell reads exactly 13.
`13 -> 14` survives and is **provably equivalent**: the height map stops at the first block with any
opacity, so the cell at `height - 1` is never air, and reaching an air cell costs that block's
opacity plus one more. 15 minus at least two is at most 13, so no plantable cell can ever read 14.

**A block-table subtlety, recorded because it is a trap.** `ky`'s ground test reads
`Block.opaqueCubeLookup`, an array filled in the Block constructor — *not* a live `isOpaqueCube()`
call. For leaves the two disagree: `BlockLeaves.isOpaqueCube` returns `!fancyGraphics`, and the
client only sets that flag later from the video options, so the cached answer is always "opaque"
while a live call under fancy graphics says otherwise. Our block table records the fancy answer,
because that is what the mesher wants. Every other block agrees, verified id by id.

### Ordinary trees, transcribed

`src/impl/worldgen/alpha_nobiome/trees.{hpp,cpp}` -- `oa` (WorldGenTrees). The big variant, `ej`,
is 1,555 lines of bytecode with fourteen fields and carries the `Math.pow` risk; it is not here yet.

**How many trees is a noise question, not a random one**, and answering it corrected a claim that had
been in this document from the start. `populate` samples the **eighth** octave generator -- the one
written up here for months as a `mobSpawnerNoise` that nothing reads -- at half the block scale:

    count = (int)((noise(x/2, z/2) / 8.0 + nextDouble() * 4.0 + 4.0) / 3.0)     clamped at 0
    if (nextInt(10) == 0) count++
    big  = nextInt(10) == 0

That is `nw.c`, and the member is now called `treeDensity_`. Both tens are rolled whatever the count
is, so a treeless chunk still costs three numbers. The kind is decided **per chunk, not per tree**:
one roll, and every tree in the batch is ordinary or every one is big.

**The 2D noise sample puts the caller's z on the noise's y axis.** `lp.a(DD)D` forwards to
`v.a(first, second, 0.0)`, so the third coordinate is pinned at zero and z never reaches it. Passing
z as z produces a perfectly plausible forest that is not this seed's; mutating it fails the fixture.
That also meant `PerlinNoise` needed a genuine single-sample `sample()` alongside the lattice fill --
the same maths, but with no Y-slab cache, because a single sample has nothing to cache against.

Three details in the tree itself, all mutation-checked:

  * **The clearance check tolerates leaves**, not just air, which is what lets a forest interlock.
    Refusing them thins every dense stand.
  * **A random number is drawn only at a canopy corner**, and only when both offsets are at the
    radius -- Java's `||` short-circuits, so an edge that is not a corner never reaches the draw.
    Drawing unconditionally shifts the stream for every tree in the chunk.
  * **The leaf loop tests `opaqueCubeLookup`**, so a canopy is clipped by terrain rather than
    overwriting it. This is the first caller of the `opaqueCube` column added for mushrooms.

The oracle's clipping scene is worth recording because the obvious version did not work. A stone
wall beside the tree is caught by the *clearance* check and the tree simply refuses -- no clipping
observed. The clearance check uses radius 1 below the top layers while the canopy's lower layers use
radius 2, so the scene is now a short pillar two blocks out and low down, sitting in the gap between
the two radii: the tree plants, and comes out with a bite taken from it.

### Big trees, transcribed -- and the `Math.pow` risk, resolved

`src/impl/worldgen/alpha_nobiome/big_tree.{hpp,cpp}` -- `ej`, WorldGenBigTree. Bit-exact against the
jar over twelve fixture cases of 73 to 518 blocks each.

**The `Math.pow` risk this class carried for the whole project does not exist.** All seven calls have
a literal exponent of `2.0`, and `pow(x, 2.0)` is bit-identical to `x * x` -- measured over 608,011
values across the exact argument shapes `ej` produces (integers, integers plus a half, widened
floats), against both `Math.pow` and `StrictMath.pow`, zero mismatches. They are written as
multiplications. The three `sqrt` calls are IEEE-exact by specification.

**The real transcendental is the one `sin`/`cos` pair** that places a cluster around the trunk, and
the situation is worse than the `pow` scare it replaces: `Math.sin` and `StrictMath.sin` disagree
with *each other* on **3.4%** of the arguments this class generates, so the original is not
self-consistent across JVMs. glibc differs from this machine's `Math.sin` on 0.3%, by at most 1 ulp.

That is tolerable, and the margin is measured rather than asserted. The result is multiplied by a
branch length under ~30 and floored to a block, so 1 ulp moves a coordinate by about 1e-15 blocks;
sampling where those coordinates land, the closest any came to an integer boundary was **6.8e-4**.
Eleven orders of margin, and nothing in the stream depends on the answer -- a tip would misplace one
branch by one block and nothing else. Transcribing fdlibm's `sin` would buy agreement with
`StrictMath`, which is *not* what the original calls, so it is deliberately not done.

**`ej` draws exactly one number from the shared stream** -- a `nextLong` -- and runs on its own
Random thereafter. It is the only generator in population that cannot desynchronise a chunk however
wrong its internals are.

**The oracle was wrong before the transcription was.** The first fixture disagreed at 432 blocks
against our 518, and the cause was `setScale`: the driver calls `gen.setScale(1.0, 1.0, 1.0)` before
every big tree, `1.0 > 0.5` pushes `leafDistanceLimit` from 4 to 5, and that changes the height of
every leaf cluster and the y of every node. The harness skipped the call and measured a tree the game
never builds. Dumping the jar's own leaf-node plan through reflection and diffing it against ours --
13 nodes, every coordinate identical -- is what settled which side was wrong.

The site probe also had to be loosened. Requiring 41x41 of grass flat to +-2 finds nothing in real
a1.1.2 terrain; it walked 400 sites and rejected every one. A big tree only *stands* on the centre
column, so the strict test covers a radius of 8 and the rest of the patch is carried as-is.

**One known gap, stated rather than implied.** `clearLineLength` truncates where `drawLine`
round-to-nearests, so the checked line is not the drawn line -- and mutating it to match passes the
whole suite. Instrumented, the two roundings visit a different cell on **48 of 521 steps** across the
fixture, so the difference is live; but on all 48 the two cells agree on whether the line is blocked.
Three obstruction schemes failed to force a disagreement, all for the same reason: anything close
enough to be hit by a check line also clips the tree short via `chooseHeight`, and a short tree walks
5 to 25 steps against 125. Closing it needs a hand-built scene rather than a probed one.

### Dungeons, transcribed -- the last generator

`src/impl/worldgen/alpha_nobiome/dungeon.{hpp,cpp}` -- `cg`, WorldGenDungeons. Eight tries a chunk,
before the ores, and **the only generator that produces anything other than blocks**: a mob spawner
carrying a mob name, and up to two chests carrying contents.

The shape, from the bytecode:

  * `spanX` and `spanZ` are each `nextInt(2) + 2`, so the floor is 5x5, 5x7 or 7x7. Both are drawn
    **before** anything is inspected, so a refused dungeon still costs two numbers -- and eight
    refusals a chunk is the common case.
  * Pass one counts **wall openings**: an air cell on the shell ring at floor level with air above
    it. Fewer than one or more than five and the room is refused. That is what makes dungeons sit
    against caves rather than float in rock.
  * Pass two carves the interior and builds the shell. The floor gets mossy cobblestone on
    `nextInt(4) != 0` and plain cobble otherwise -- **three quarters mossy**, which is the way round
    that is easy to get backwards.
  * Then up to two chests, three placement tries each, breaking on success. A chest needs air with
    **exactly one** solid neighbour, so it stands against a wall and not in a corner.
  * The spawner goes in last, at the centre, and the mob is picked from `nextInt(4)` over
    {Skeleton, Zombie, Zombie, Spider} -- **Zombie twice**, so half of all dungeons.

**The draw that is easiest to get wrong** is in the chest loop. Eight loot rolls per chest, each
followed by a `nextInt(27)` for the slot -- but only when the roll produced an item. The original
tests for null before calling `setInventorySlotContents`, so a failed roll costs no position draw.
Moving the slot draw ahead of that test shifts every subsequent item in the world, and the fixture
catches it.

Two rolls can also land in the same slot, and the later one wins, which is why the output is built
by slot rather than appended.

**The loot table's empty rate is derived, not sampled.** Roll 10 always yields nothing; 7 needs a
1-in-100, 8 a coin flip, 9 a 1-in-10. So `P(empty) = (1 + 99/100 + 1/2 + 9/10) / 11 = 0.3082`. The
first version of the test guessed "about a fifth" and failed against the generator, which was right.

**The oracle's scene had to be built, not probed**, and sized carefully. A dungeon needs a
room-shaped void inside solid rock, which real terrain effectively never has. The first attempt
carved an 11x11 cavity and **every case was refused**: the generator inspects a shell at exactly
`spanX + 1` from the centre, at most 4, so a wider cavity puts air where it looks for a wall, counts
zero openings, and fails the minimum. A dungeon does not go in a big cave -- it goes in a hole its
own size. The cavity is now 3, so the shell lands on stone when the roll is 2 and inside carved air
when it is 3, and both cases appear.

**Tile entities.** The generator emits a `DungeonOutput` -- spawner position and mob, chest positions
and stacks -- rather than writing tile entities itself, so the transcription is verifiable without a
storage layer. That matches the settled decision in `docs/status.md`: chests and spawners need no
per-block runtime object, only a side table, and saves keep round-tripping the `TileEntities` NBT
list unchanged.

Two obfuscated-name traps worth recording, both of which produced wrong lookups before being checked:
`fe` has **two methods called `c`** -- `c()` is the inventory size and `c(int)` is the slot getter,
while `a(int, ev)` is the *setter* -- and `ev`'s fields are id in `c` and count in `a`, established by
reading `ev.<init>(II)` rather than by guessing from declaration order.

### The whole population pass -- the check no generator test can make

`src/impl/worldgen/alpha_nobiome/populate.{hpp,cpp}` drives all nine passes in `nw.a(aw, int, int)`'s
own order, and `tests/populate_test.cpp` compares the result against a real a1.1.2 World, every byte
of a 5x5 of chunk columns.

Every generator was already verified block-for-block on its own. What this adds is the thing they
share: **one `java.util.Random`**. The order the passes run in and the number of draws each makes are
as much a part of the seed as their contents, so one pass out of place -- or one draw too many --
moves everything after it, while each generator still passes its own test. Mutating the driver to
swap the dirt and gravel passes fails immediately; so does flattening one nested `nextInt`.

Two things about the driver are worth stating outright:

  * **The population seed is not the chunk seed.** Terrain uses `cx * 341873128712 + cz * 132897987541`
    with no XOR. Population draws two odd multipliers from the world seed and *then* XORs the world
    seed back in: `setSeed(cx * a + cz * b ^ worldSeed)`. The `/ 2 * 2 + 1` is truncating division,
    the same trap the cave carver has.
  * **Everything is offset by +8 in x and z except clay and the ores.** That is why one chunk's
    population lands squarely on the 2x2 quadrant to its east and south and touches nothing else --
    asserted exactly, `CHECK_EQ(changedColumns, 4)`, over a 5x5 window that exists precisely to prove
    the reach *stops*. The snow sweep is offset too, so it covers 64 of its own chunk's 256 columns
    and the other three quarters land in the neighbours.

**Two consequences worth having written down, because both were reported from hardware as bugs and
only one of them is.**

  * **A tree with snow on half its leaves is a1.1.2 doing what it does.** Snow is the last thing a
    pass does, over its own +8 square; a tree's canopy reaches two blocks past its trunk and can
    cross into the next pass's square. Whether that half is snowed depends on whether the
    neighbouring pass runs before the tree exists or after it, and `ft`'s trigger order follows where
    the player walked. There is no order in which both halves are guaranteed, and nothing in the
    original tries for one.
  * **A tree with only half its blocks is not.** `changedColumns == 4` is the whole point: every
    block a pass writes lands in its own 2×2 quadrant, so both halves of a tree on a chunk border
    come out of one call. If half a tree is missing, the blocks were written and then lost — see
    `docs/status.md` §0m, where they were.

**Getting a before-and-after pair out of an eagerly-populating World took four wrong attempts, and
each failure is worth recording because each looked plausible.**

  1. *A "plus" neighbourhood for the before state.* Population is usually described as needing all
     eight neighbours; it does not. `ft.b(II)` populates `(x, z)` once a **2x2 quadrant** exists, and
     checks four overlapping quadrants per call. The plus contained three of the four.
  2. *`ga.p` as the populated flag.* It is **`ga.n`** -- read off `ft`'s bytecode, where the trigger
     tests `getfield ga.n`. `ga.p` is a different boolean that stays false, so the assertion failed
     against a world that had populated correctly.
  3. *A shared scratch directory.* `ft.a(II)` counts a chunk **saved to disk** as existing, so
     leftovers from an earlier run complete quadrants nobody loaded. Two rounds of confusing diffs
     traced back to this.
  4. *Reading the 5x5 out of the populated World.* Every `getChunk` outside the quadrant loads a
     chunk, which completes further quadrants: measured, reading a 5x5 after loading one quadrant
     leaves **sixteen** chunks populated. The fixture then held sixteen chunks' work where the test
     drives one -- which is exactly the symptom that looked like our generator placing a third of the
     blocks it should.

The working shape is one World per column: 25 fresh Worlds for the "before" state and 25 for the
"after", each loading only what it needs. Slow, and completely unambiguous. `ga.n` is asserted on
both sides rather than assumed.

Also: **every case must be far from the origin.** The World constructor runs a spawn search that
generates chunks around (0, 0) before any of this code sees the world, so a case near there finds its
"lone" chunks already populated.

**Two mutants survive, and both are fixture gaps rather than equivalences.** Giving clay the +8
offset, and changing the brown-mushroom roll from 1-in-4 to 1-in-8, both pass -- because none of the
six fixture chunks has a sandy shoreline in the shifted square, and none rolls a brown mushroom.
Recorded in the source at each site. Closing them needs a case with a beach.

## The chunk generator — `ft`, and what a seed does not decide

`src/impl/worldgen/alpha_nobiome/chunk_generator.{hpp,cpp}` is `ft`,
ChunkProviderLoadOrGenerate: the driver *above* the one in `populate.cpp`. It decides whether a
chunk is read from the save or generated, and **when** it is populated. `tests/generate_test.cpp`
compares whole columns — blocks, both light planes and the height map — against a real a1.1.2 World
driven through the same sequence of chunk loads, via `tools/genref.java --generate`.

### A seed does not determine an Alpha world

This looks like a fidelity bug and is not, so it is worth stating plainly.

`ft.b(x, z)` populates a chunk once a 2×2 quadrant containing it is resident, checked four
overlapping ways per call, and residency is a consequence of where the player walked and of a
1024-entry cache that evicts on collision. Population reads the world it writes into — a tree
refuses ground something already stands on — so where two chunks' passes reach the same blocks, the
result depends on which ran first. **Two a1.1.2 clients given the same seed and different routes
produce different worlds.** Terrain, the surface pass and caves are pure functions of the seed;
population's *order* is not.

So "seed-exact" means what it has always meant here: the same rule, applied to our own load order.
The generator sweeps a 6×6 row-major per requested column, which is the order a client produces
whose chunk loads happen to arrive row-major — a real `ft` order, and unlike the original's, one
that is reproducible. The fixture is captured from a World driven through exactly that sequence.

### What has to exist before a column is finished

Three gates, each measured or derived rather than guessed:

- **A population pass reads and writes a 3×3 of columns centred on its own chunk.** The 5×5 the
  populate fixture was captured with is not needed: running every fixture case both ways gives
  identical blocks, pinned by `a_3x3_window_is_as_good_as_the_5x5_the_fixture_uses`. Writes land
  only in the 2×2 quadrant east and south — measured, `changedColumns == 4` — and the generator
  re-checks that on every chunk it makes rather than trusting six cases to speak for a world
  (`populationEscapes`, asserted zero).
- **A column is final once the four passes that can write into it have run**, at (cx−1..cx, cz−1..cz).
  **"Have run" includes "ran in an earlier session."** A column that comes out of the save with
  `terrainPopulated` set has had its pass, and `ft` skips it on that basis; skipping the *record* of
  it as well is what walled off the ground east of any world made elsewhere. See status.md §0k.
- **Lighting the centre needs the 3×3 around it final.** The original never has to make this call:
  it relights lazily from a queue, so it converges to the same answer after the fact. We compute the
  fixed point once, so the blocks have to have stopped moving first.

Composing those gives passes over (cx−2..cx+1) and terrain over (cx−3..cx+2). `ft`'s trigger then
populates one ring *less* than the sweep on the east and south, so the sweep actually runs the passes
over (cx−3..cx+1) — and that extra west-and-north ring is not slack, it is what the original
populates for the same sequence of loads.

### Two things the original does that could not be copied, and what replaced them

**Generation and population are mutually recursive in the original.** A generator reading one block
past the quadrant makes `World.getBlockId` fetch a chunk that is not loaded, and `ft.b` generates it
on the spot — unbounded recursion on a 32 KB stack. Here the missing window columns are generated up
front from the same pure terrain function, which needs no recursion because it runs no triggers.

**But they must not be kept**, and that is the part that took a debugging session. The original keeps
such a chunk, so `chunkExists` answers true for it from then on — and `ft.b` skips its whole body,
triggers included, for a chunk that already exists. A column pulled in that way is therefore never
the one that completes a quadrant, and the passes that would have written into its neighbours never
run. The original does not care, because it has no notion of a column being finished. Here it is
fatal: a column whose passes can never run can never be lit, and the generator refuses to deliver
anything near it for the rest of the session. It showed up as the fourth column of a spiral failing
outright.

Holding them in scratch instead keeps the invariant that makes the whole thing work: **a column
enters the cache only through `ensure()`, so every resident column has had the triggers run on it.**
A chunk is then populated as soon as the last of its four quadrant columns arrives — whichever of
the four that is, since all four trigger cases are transcribed — so every pass the closure needs is
guaranteed rather than hoped for. It is safe because population *writes* only inside the quadrant,
which the trigger's own precondition makes resident, and that claim is counted rather than assumed.

The scratch is cached, and the reason is a measured one: neighbouring passes in one sweep want the
same ring of columns, and without it **61 % of all terrain generated was scratch being made again**.

### The bug this found, which no per-generator test could

`ej` — WorldGenBigTree — has one field that survives from one tree to the next, and the driver
constructs a single `ej` **before** its tree loop:

```java
if (this.e == 0) this.e = 5 + this.b.nextInt(this.m);   // ej.a, first act
```

So the height limit is rolled for the **first** big tree of a chunk and every later one reuses it —
including the shrink `validTreeLocation` applies at a site short of headroom, so one cramped tree
makes the rest of the chunk's big trees short. It reads like a bug in the original and it is one.

Our transcription made a fresh generator per tree, on the reasoning that the driver's `new ej()` is
per chunk anyway. True, and the wrong conclusion: per chunk is exactly what makes the state persist
across the trees *inside* one. The second big tree of a chunk came out at height 3 against the jar's
8.

**Nothing below the whole-pipeline comparison could see it.** The draw is from the tree's *private*
Random, so the chunk's shared stream never moved and no fingerprint could catch it; the state only
exists between two trees of one chunk, so no per-tree fixture could carry it; and the populate
fixture's six chunks happen not to contain a second big tree. It surfaced as 360 wrong blocks four
chunks away from where it happened, reached through neighbour population, and was pinned by running
one real population pass over our own block state — `tools/genref.java`-style reflection into
`ft.a(aw, int, int)` with the chunk arrays overwritten. That technique is worth remembering: it
isolates a single pass from everything before it.

### Measured cost

Dev host, `-O3`, no sanitizers, `./build-host-o3/3dalpha --generate <seed> <radius> [snow] [cache] [raster]`,
filling a fresh world of (2r+1)² columns and handing every one over.

| radius | columns | nearest-first | raster | terrain per column, nearest / raster | peak resident |
|---|---|---|---|---|---|
| 4 | 81 | 7.4 ms | — | 4.09 | 98 columns, 3.1 MB |
| 8 | 289 | 6.1 ms | — | 3.34 | 162 columns, 5.1 MB |
| 11 | 529 | 5.9 ms | **3.8 ms** | 3.20 / **1.59** | 210 columns, 6.6 MB |

Two things fall out of it and both are decisions for whoever wires this up.

**The request order is worth 1.6× of the whole cost.** Nearest-first is what the streamer wants —
the player wants the ground under their feet before the horizon — but each request sweeps a 6×6, and
a spiral re-sweeps ground a raster would have kept. Raster generates 1.59 terrain columns per
delivered column against 3.20.

**A column costs milliseconds, not microseconds.** Meshing a section is 30 µs and a column is eight
of them, so generating a column is roughly 25× meshing it, on a host perhaps 20× faster than a
268 MHz ARM11. A fresh world cannot be made at the streamer's one-column-per-frame budget; this is
worker-thread work with a progress indicator, not something to hide inside a frame.

**The generator holds two rings of frontier**, because a column cannot be handed over until it and
its neighbours are final. That band's length grows with the radius, which is why the cache is sized
by `cacheColumnsFor(loadRadius)` rather than by a constant. Undersizing it is not a slow path but a
wrong world: `evictedLive` counts the case and must stay zero.

### Wired

`WorldStreamer` asks the generator for any chunk storage does not have, and writes back everything it
gets. `tests/streamer_generate_test.cpp` drives the real streaming loop over a world with no chunks
in it and checks the three things that can be wrong without anything else noticing: that a missing
chunk reaches the generator, that what comes back is on the card rather than only in memory, and that
a second session **reads** it instead of making it again — which matters more than the time it saves,
because population order follows the sequence of loads and a second pass over the same ground is a
different world.

**Generation runs on a worker thread.** The main thread appends columns to a queue in spiral order
and the worker consumes them one at a time, so job N always starts against the world left by every
job before it. Which column is job N is **the one nearest the camera**, not the oldest — see
[status.md](status.md) §0g for why, and for the measurement that forced it: strict oldest-first
stranded a player who outran the generator behind hundreds of columns of ground they had already
left, and a1.1.2 keeps no queue to be faithful to, generating inline and nearest-to-the-player.

The consequence for this document's own rule — population order is the world — is that the order is
a function of the camera path **while the generator keeps up**, and of how far behind it got once it
is not. `a_worker_thread_produces_the_same_world_as_inline_while_it_keeps_up` pins down the first
half: it settles at each waypoint so nothing is behind, then compares every chunk file across
inline, threaded, and threaded-with-the-chunk-cache. What is a pure function of the seed regardless
is the land itself — terrain, caves, ores, the Far Lands — which is checked against a real JVM
elsewhere in this document. The full account is in [status.md](status.md) §0 and §0g.

Two rules the wiring has to keep, both learned the hard way:

- **Everything delivered is saved, in range or not.** The generator evicts what it has handed over
  and reaches it again through the load callback; a column that was never written comes back as bare
  terrain with its neighbours' population missing, and the world is quietly wrong rather than
  obviously so.
- **A cell is only ever marked absent with generation off.** An absent cell is never revisited, so a
  frame that ran out of generation budget and marked one would leave a permanent hole. The first
  wiring did exactly that and reported 53 of a 121-cell grid absent while calling itself settled.

The load callback returns a pointer rather than filling a column, so the generator reads the
streamer's resident grid directly. Its own neighbourhood is what it asks about most, and answering
those from storage would be a gzip inflate per neighbour per sweep.

## Hazards, and what each one costs

Ranked by how likely it is to silently produce a *plausible but wrong* world.

**`java.util.Random`** — solved. `src/core/util/java_random.hpp` is exact, pinned by
`tests/java_random_vectors.hpp` (nine seeds × eight draws × every method, as bit patterns) against a
real JVM. Two pieces of undefined behaviour had to be rewritten to get there: `nextLong`'s shift of a
sign-extended negative, and `nextInt(bound)`'s rejection test, which the JDK writes as a deliberate
signed overflow.

**`nextGaussian` / `StrictMath.log`** — solved, and it turned out not to matter. glibc's `log` is
correctly rounded and fdlibm's is not; Java specifies fdlibm, so the two disagreed by one ulp and
`src/core/util/strict_math.hpp` now carries the fdlibm transcription, pinned against 380 JVM vectors.
Separately: **no worldgen class calls `nextGaussian`** — checked across `nw`, `v`, `lp`, `kk` and all
nine `ik` subclasses.

**`Math.pow` in `ej` (big trees)** — **settled, and it was never a risk.** All seven calls have a
literal exponent of 2.0, which is bit-identical to a multiplication (608,011 values checked, zero
mismatches). `sqrt` is IEEE-exact. The one `sin`/`cos` pair is the only genuine transcendental, and
its worst case is 1 ulp against a margin of 6.8e-4 -- see the big-tree section above. What follows
was the reasoning before that was measured, kept because the shape of the argument still applies to
any `pow` with a non-trivial exponent.

The nasty part is the failure mode. A big tree happens on `nextInt(10) == 0`, and the divergence
**does not desynchronise the stream** — it changes where blocks land, not how many numbers are drawn.
So a mismatch shows up as one subtly misshapen tree, not as a wholly different chunk, which is
exactly the kind of error that survives a casual look. Needs an fdlibm `pow` alongside `log`, or a
golden-vector test aimed at `ej` specifically.

**`Math.sin`/`cos` elsewhere** — mostly dodged by the game itself. Caves, ores and clay all go
through `eo`'s **65536-entry float table**, built once from `Math.sin`. Reproduce that table
bit-exactly offline (`genref` can dump it) and the runtime path becomes pure float indexing with no
transcendental at all.

**float versus double, and the split is load-bearing.** Noise (`v`, `lp`, `nw`) is entirely double.
Caves (`kk`) are entirely float — `3.1415927f`, `0.92f`, `nextFloat()` — with double only for the
final position arithmetic. `eo`'s table is float narrowed from double. **Accidentally promoting the
cave chain to double changes cave shapes**, and would look like a plausible cave system rather than
like a bug.

**`nextLong() / 2 * 2 + 1`** — Java's `/` truncates toward zero and `nextLong()` is negative half the
time. `(-5)/2*2+1` is `-3`; the shift-based `(-5 >> 1) << 1 | 1` is `-5`. Must be truncating
division.

**64-bit overflow in seed derivation** — `cx * 341873128712L` and `cx * s1 + cz * s2` both rely on
defined wraparound. Signed overflow is undefined in C++, so this arithmetic happens in `u64`, the
same way `java_random.hpp` already does it for the LCG.

**Narrowing a double to an int** — solved, and it is the reason the Far Lands work. Java's `(int)`
cast is fully specified and saturates; C++'s is undefined once the truncated value will not fit, and
x86-64 answers `INT_MIN` for an overflow of *either* sign while ARM saturates the way Java does. The
generator drives that conversion off its range by design past block 12,550,824. See the section
below. `src/core/util/java_cast.hpp`, pinned by `tests/java_cast_test.cpp` against a JVM run.

**HashMap iteration order** — not a hazard. Nothing in the generation path iterates a `Map`, `Set` or
`List`; it is arrays and plain fields throughout.

**`-ffp-contract`** — GCC defaults to `fast`, which fuses `a*b+c` into one FMA where Java specifies
two roundings. Now `off` for the whole project in `CMakeLists.txt`, with the reasoning at the flag.

---

## The Far Lands

They are not a bonus feature and they are not emulated. They are what a1.1.2's generator does, and
reproducing them exactly meant reproducing one conversion that C++ leaves undefined.

**Where they are.** `lp` (`NoiseGeneratorOctaves`) hands each octave a coordinate scaled by
`684.412`, and `v` (`NoiseGeneratorPerlin`) floors it with a `d2i` at bytecode offsets 69, 156 and
243 of `v.a([DDDDIIIDDDD)V`. The chunk provider samples on a lattice of four cells per chunk, so the
argument to that floor is `chunkX * 4 * 684.412 + offset`, with the offset in `[0, 256)`. Setting
that equal to `Integer.MAX_VALUE`:

    2147483647 / (4 * 684.412) = 784426.50 chunks = block 12,550,824

which is the coordinate the Far Lands have always been reported at. The first octave overflows first,
because `lp` gives octave 0 the largest scale and halves it thereafter.

**Why it produces a wall.** Past that point the `d2i` stops being a conversion and becomes a clamp,
so `xFloor` sticks at 2147483647 while `fx` keeps growing. The sampler then computes `fx -= xFloor`
expecting a fraction in `[0, 1)` and gets a number in the millions, feeds it to a quintic fade, and
the lattice returns values with no relation to the terrain. The density field saturates and the
column fills to the build ceiling. Measured across the fixtures: an ordinary chunk has **zero** solid
blocks at y = 100 or above, a Far Lands chunk has 49 to 144 of its 256 columns solid at y = 120 and
the same count at y = 127 — the vertical-wall signature.

**Why a1.1.2 has both walls.** The `% 16777216` wrap that later versions apply to the octave
coordinate is **not in this version** — `lp.a([DDDDIIIDDDD)[D` is fourteen instructions long and does
nothing but scale, call and halve. Verified by disassembly, not assumed.

**What had to change.** `i32(value)` in `noise.hpp` was undefined behaviour out there, and on the
host it evaluated to `INT_MIN` where Java gives `INT_MAX` — so before the fix the generator produced
*water* at chunk 784,427 where the jar produces stone. `src/core/util/java_cast.hpp` now implements
JLS 5.1.3 directly. Two details in it are easy to get wrong:

  * The upper guard must be `value >= 2147483648.0`, not `>= double(INT_MAX)`. 2147483647 is not
    representable as a double, so the latter rounds its comparand up and clamps values that Java
    converts exactly.
  * The floor's `i - 1` **underflows** at the negative Far Lands: the cast has already clamped to
    `Integer.MIN_VALUE`, `d < i` is still true, and Java wraps the decrement round to
    `Integer.MAX_VALUE`. Confirmed on a JVM — every negative overflow floors to `+2147483647`. In C++
    that is signed overflow, so it is written in `u32`.

**How it is verified.** Four fixtures in `tests/terrain_vectors.hpp` — chunks `784426,0` (the
boundary chunk, only partly walled), `784427,0`, `0,784427` and `-784427,0` — compared byte for byte
against the jar through all three stages: terrain, surface and caves. Two further cases assert the
fixtures still *are* Far Lands fixtures, since deleting those four rows would leave every other test
passing while the question stopped being asked.

**A footnote on why this hid for so long.** GCC's `-fsanitize=undefined` **does not include**
`float-cast-overflow`; it is one of two checks left out of the group. It is now named explicitly, and
at the same time the sanitizers moved from the test executable onto `3dalpha_core` — they had been
instrumenting `tests/*.cpp` and nothing else, so the parsers, the mesher and the generator were all
built unsanitised. That change immediately found a second, unrelated piece of undefined behaviour in
`MeshBuilder::copyTo` (a zero-length `memcpy` from an empty vector's null `data()`).

**Rendering out there is a separate problem with a separate answer.** See
`docs/architecture.md`, "Rendering far from the origin": the terrain is faithful, the one-block
camera stutter that Java also shows is not, and the two are independent.

---

## Why the reference vectors come from a real JVM

`tools/javap.py` can *read* this bytecode — it is what identified the classes above — but it
**cannot run it**. Its interpreter returns `UNKNOWN` for every `java/*` call, so `Random` and `Math`
evaluate to nothing; it has no 64-bit wrapping, so the LCG would compute in Python's arbitrary
precision; it has no `dreturn`; and it bails at 20,000 steps against a chunk's millions. Extending it
far enough would mean writing a real JVM.

`tools/genref.java` runs the actual classes under an actual JVM instead, and its output is checked
in. Maintainer-run, never part of the build, never a player step — the same rule as
`extract_blocks.py`. See CONTRIBUTING.md.
