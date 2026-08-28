# The map, and the bottom screen it lives on

## What this is

**a1.1.2 has no maps.** There is no map item, no `MapData`, no `MapColor`, and nothing in the client
that projects a column of blocks onto a plane. So unlike the world generator, the chunk format or
the lighting, there is no oracle here to be byte-exact against, and "faithful" has to mean something
else.

What it means is this: **be the map a later version would draw of the same ground.** That is a real
specification — `MapData.updateVisitedBlocks` — and following it is what makes the picture
recognisable as Minecraft rather than as a height field with colours on it. Where it cannot be
followed, the reason is written down below rather than quietly worked around.

The map is the **Spectator** gamemode's bottom screen. Survival and Creative have their own screens,
which today say what will be built on them. The three debug pages behind `SELECT + Y` are unchanged
and are the same in every mode: they are the maintainer's, not the player's.

## The picture

One pixel is one block — later versions' `scale = 0` — and the window is 192 by 192 blocks centred
on the block the player is standing in. Three things follow, and the first two are the point:

* **It lines up with the chunks.** Sixteen pixels to a chunk, always, because the window's origin is
  a whole block and a chunk is sixteen blocks. `d-pad left/right` draws the chunk lines so you can
  see it rather than take it on trust.
* **It lines up with the maps of later versions.** A map at scale 0 covers the 128 blocks starting
  at `tile * 128 - 64` (`ItemMap`'s centre arithmetic, `MathHelper.floor((x + 64) / 128)`). Our
  pixel at a given block is that map's pixel at the same block, and the red grid is that 128-block
  boundary. The left-hand column names which tile the player is standing on. Every tile edge is also
  a chunk edge, which is what lets both claims be true at once — 64 is a multiple of 16.
* **The player is an indicator on it, not the centre of a compass.** A filled diamond with a tick in
  the direction they are facing. `map::drawMarker` takes a position, a facing and a colour, so
  **every other player in a multiplayer session is that call again** — nothing about it assumes
  there is one.

## Where the colours come from

Later versions key the map colour off `Material`: `Material.grass` is green, `Material.ground` is
brown, `Material.water` is blue. **That table cannot be borrowed, because a1.1.2's material set is
coarser than the one it was written for** — grass, dirt and farmland are all one material in this
version, so a material-keyed table paints every meadow the colour of a ploughed field. Writing a
per-block colour table by hand instead would work and is exactly the version-specific data
[CONTRIBUTING.md](../CONTRIBUTING.md) keeps out of code.

So a block's colour is **the average of its top face in whatever `terrain.png` is loaded**,
alpha-weighted so a cutout tile is not averaged towards black through pixels that are never drawn.
Three things fall out of that, and all three are wanted:

* the map agrees with the world, because it is sampling the same picture the top screen draws;
* a texture pack recolours the map for free; and
* a version this project has not met yet needs no new data to be mapped.

The store holds **block ids, not pixels**, which is what makes the second one work: changing the
pack recolours ground that was sampled long ago and is no longer loaded, without re-reading
anything.

Measured on the real 1,119-chunk world with a1.1.2's own `terrain.png`, the surface census and the
colours it produces:

| block | share of the surface | colour |
|---|---|---|
| grass | 58.6 % | `#75B049` |
| water | 30.1 % | `#2A5EFF` |
| sand | 6.0 % | `#D9D29E` |
| leaves | 4.4 % | `#3BBF28` |
| stone | 0.4 % | `#7D7D7D` |

`--map <world> <pack>` prints that table. It is the check that matters when the colours are derived
rather than declared: a world that is 59 % grass and a map that is not mostly green is a palette
bug, not a matter of taste.

## The shading

Straight out of `MapData.updateVisitedBlocks`, at scale 0:

* **On land, the step to the block one to the north.** Higher is the bright shade, lower is the dark
  one, level is the middle. The later code is
  `(d1 - d0) * 4 / (depth + 4) + ((x & 1) - 0.5) * 0.4`, bright above 0.6 and dark below -0.6 — and
  at scale 0 the heights are whole numbers and `depth` is zero on that branch, so the dither term
  (±0.2) can neither carry a step of 0 past 0.6 nor hold a step of 1 below it. It is that
  comparison exactly, not an approximation of it.
* **On water, the depth, on a checkerboard.** `depth * 0.1 + ((x + z) & 1) * 0.2`, bright under 0.5
  and dark over 0.9. This is what makes a shoreline read as a shoreline, and it is why the sample
  records how far down the floor is rather than only what is on top.
* **The three brightnesses are 180, 220 and 255 out of 255**, from `MapColor.getMapColor`. The
  fourth, 135, is only ever used by blocks this version does not have.

The one rule that had to be invented: later versions branch on `getMapColor() == MapColor.waterColor`
and there is no map-colour table here to compare against. **A fluid that emits no light is the water
case and one that does is the lava case** — derived from the block table rather than written down per
version, and it agrees with later versions, whose lava carries the TNT colour and not the water one.

The surface itself is found the way later versions find it: start at the height map — one above the
highest block that stops light, the same array `getHeightValue()` returns — and walk down past
anything whose map colour is the air colour. That set is torches, rails, levers, buttons, redstone,
fire and air, which here is a question about the **render type**, since every one of them is drawn by
something other than a cube and none would read as anything at one pixel across. The deliberate
divergence is **glass**, which later versions make invisible on a map and this does not: it is a cube
here with no material distinction from stone to key on, so a glass roof shows as glass.

## What it costs — and the hardware number that changed the design

Three things happen at three different rates, and keeping them apart is the whole of the
performance story:

| | how often | host, `-O3`, real world |
|---|---|---|
| **sample** a chunk into surface / height / depth | once per chunk, ever | **1.3 µs** |
| **draw** a chunk's 16 × 16 patch of pixels | when it is sampled, when its northern neighbour arrives, or when the palette or grid changes | **0.8 µs** |
| **copy** the 192 × 192 window onto the screen | when the player crosses a block | **17.4 µs** |

**The second row did not exist at first, and a console is why it does.** The original design kept
only the samples and shaded every pixel on its way to the screen — 132 µs on the host, which said
nothing useful, so the screen was made to print its own redraw time and the question was left to
hardware. Hardware answered **5,000 µs**: a third of a frame, on every block the player crosses, and
all of it re-deriving pixels that had been derived before.

Everything a pixel depends on is either inside its chunk or knowable from the chunk's coordinates —
its own heights, the row of heights immediately north of it, the palette and the grid style. So a
chunk's patch is drawn once and kept beside its sample, and a redraw is a copy: **17.4 µs on the
host against the 132 µs it replaced**, which on the same console should be something near 700 µs. A
patch costs 512 bytes, taking a chunk to 1,536.

The copy has a `memcpy` fast path, and it is why a patch is stored **x-major with z running
backwards** — `patch[x * 16 + (15 - z)]`. The console's bottom screen is stored in columns from the
bottom up, so going south steps *back* through the framebuffer; written in reverse, a chunk column
of the map and the sixteen framebuffer halfwords it lands on run the same direction, and the copy is
32 bytes moved rather than sixteen loads, stores and pointer bumps. `renderMapWindow` takes that
path when the surface's z stride is exactly −1 and walks pixel by pixel otherwise, so a host test
reading a row-major buffer gets the same picture — which `tests/map_test.cpp` checks both ways.

What still costs the old price is a **texture pack change or a grid toggle**: every patch in the
window is stale at once, which is 156 of them, about 4.7 ms on that console — once, at a moment the
screen is already expected to stop and think. That is deliberately not budgeted: spreading it over
frames would mean showing the old pack's colours for a while, which is worse than the hitch.

A redraw happens when the player crosses into another block, when a chunk is sampled, or when the
compass point changes — never otherwise. Standing still costs nothing. **The screen still prints its
last redraw time**, because that number is what found this.

One behaviour changed with the split, and for the better: a chunk whose northern neighbour has never
been sampled shades its first row *level* rather than against a height of zero. Zero is not a
neutral seed — every surface is above it — so the old continuous walk gave the first row after every
gap the bright step, and the northern edge of explored ground was a bright fringe that moved as the
player walked. The rule the window's own first row already had now applies at every gap.

**The sample store is memory, not a file**, and that is a decision rather than an omission. Two
things stand between it and a map on the SD card, and both are real:

* **The card is not allowed on the render thread.** Tiles would have to be written by the I/O thread
  or at the two moments a world already blocks — open and close. That is a design step of its own.
* **A packed world would swallow them.** The converter stashes every file it does not recognise into
  the container at its own path, so a map directory would vanish from plain view the first time a
  world was packed unless it were given the same "carried as well as stashed" treatment
  `3dalpha.ini` has (see [packed-worlds.md](packed-worlds.md)). Getting that wrong is data loss on a
  conversion, which is the one class of bug this project does not trade for a feature.

So the map lives for as long as the world is open: 512 chunks on an old 3DS and 1,536 on a New one,
which is a remembered area of roughly 512 and 780 blocks square.

## Where it is

| | |
|---|---|
| `src/core/map/map_sample.hpp` | one chunk reduced to surface block, height and water depth |
| `src/core/map/map_palette.hpp` | block to colour, out of the texture pack |
| `src/core/map/map_store.hpp` | what has been sampled, what it was drawn as, and the later-version tile grid |
| `src/core/map/map_render.hpp` | the shading, the patch, the window copy, and the player marker |
| `src/platform/ctr/map_screen.hpp` | the bottom screen: layout, the framebuffer, the readout |
| `src/platform/ctr/overlay.hpp` | which screen each gamemode gets |
| `tests/map_test.cpp` | all of the above, including the framebuffer's strides |
