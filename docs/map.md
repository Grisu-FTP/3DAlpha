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

The map is one of the player's three bottom-screen pages, switched by touching the tabs along the
top: **Map**, **Items** and **Look**. It is offered in every gamemode; Items is not, because
Spectator has no inventory. The three debug pages behind `SELECT + Y` are unchanged and are the same
in every mode: they are the maintainer's, not the player's. See
[the bottom screen](#the-bottom-screen-it-lives-on) below.

## The picture

One pixel is one block — later versions' `scale = 0` — and the window is 192 by 176 blocks centred
on the block the player is standing in. (It was 192 square; the sixteen rows went to the tab strip
and the frame around it, which makes the copy below cheaper rather than dearer — 33,792 pixels
against the 36,864 the hardware number was measured on.) Three things follow, and the first two are the point:

* **It lines up with the chunks.** Sixteen pixels to a chunk, always, because the window's origin is
  a whole block and a chunk is sixteen blocks. The debug settings page draws the chunk lines so you
  can see it rather than take it on trust.
* **It lines up with the maps of later versions.** A map at scale 0 covers the 128 blocks starting
  at `tile * 128 - 64` (`ItemMap`'s centre arithmetic, `MathHelper.floor((x + 64) / 128)`). Our
  pixel at a given block is that map's pixel at the same block, and the red grid the debug settings
  page can turn on is that 128-block boundary. Every tile edge is also
  a chunk edge, which is what lets both claims be true at once — 64 is a multiple of 16.
* **The player is an indicator on it, not the centre of a compass.** An arrowhead at their position,
  pointing where they are looking. `map::drawMarker` takes a position, a yaw and a colour, so
  **every other player in a multiplayer session is that call again** — nothing about it assumes
  there is one.

### The marker, and the two faults it used to have

It was a filled diamond with a tick, and both of its problems were the same problem: **it was built
out of whole-block steps.** The tick walked one of `kFacingStep`'s eight directions, so

* it could only ever point eight ways — every yaw inside a 45° bucket drew the same picture, and
  turning moved it in visible jerks; and
* a diagonal block step is √2 longer than a straight one, so the diagonal marker was 41 % longer
  than the straight one and the thing changed size as the player turned through it.

It is now a rotated shape rather than a stepped one — `gui::drawArrow`, a four-point polygon with a
notched back, rasterised from a real angle. That fixes both at once: the same arrowhead at every
angle, pointing at whatever angle it is given.

Two details are worth writing down, because both were arrived at by looking at the result rather
than by reasoning about it:

* **It is sampled four by four inside each pixel, not once at its centre.** An eleven-pixel arrow
  tested at pixel centres is a *different shape* at every angle: a corner falling a tenth of a pixel
  the wrong side of a centre takes a whole pixel with it, and the marker comes out visibly lopsided
  in a way that changes as you turn. Sixteen samples and a half-cover rule fixes it and keeps the
  edges hard, which is what a map drawn one pixel to the block wants. It costs 16 tests on about 200
  pixels, once per redraw.
* **It is eleven pixels long and six across**, which is bigger than the diamond it replaced. Smaller
  and the raster has too little to work with — at five pixels the same shape reads as a lump at
  half the angles it is drawn at.

**A redraw is a copy of the whole window**, so something has to say when a turn is worth one. The yaw
is rounded to **64 steps** — 5.6°, which moves the tip of an eleven-pixel arrow by less than a pixel,
so the rounding is below what the marker can draw and it reads as continuous. The *same* rounded
angle is what the marker is drawn with, so what is on the screen and what the screen thinks is on it
can never disagree.

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
| **copy** the 192 × 176 window onto the screen | when the player crosses a block, or turns far enough to move the marker | **14.2 µs** |

The copy row is a best of four runs of `--map`, which is how the 17.4 µs it replaced was taken: this
machine spreads that measurement over 14–22 µs depending on what else is running, and the same run
that gives 14.2 gives 1.3 and 0.7 for the two rows above it. The window losing sixteen rows to the
tab strip is worth about three of those microseconds; it is not worth reading anything else into the
difference.

**The second row did not exist at first, and a console is why it does.** The original design kept
only the samples and shaded every pixel on its way to the screen — 132 µs on the host, which said
nothing useful, so the screen was made to print its own redraw time and the question was left to
hardware. Hardware answered **5,000 µs**: a third of a frame, on every block the player crosses, and
all of it re-deriving pixels that had been derived before.

Everything a pixel depends on is either inside its chunk or knowable from the chunk's coordinates —
its own heights, the row of heights immediately north of it, the palette and the grid style. So a
chunk's patch is drawn once and kept beside its sample, and a redraw is a copy: **14.2 µs on the
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

A redraw happens when the player crosses into another block, when a chunk is sampled, or when their
yaw crosses one of the marker's 64 steps — never otherwise. Standing still costs nothing. **The last
redraw time is still measured and still on a screen**, because that number is what found this; it is
on the Info page rather than on the map itself. See [Coordinates, and nothing
else](#coordinates-and-nothing-else).

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

## The bottom screen it lives on

The screen is 320 × 240 in three bands: the tab strip on rows 1–3, the page on rows 4–27, and a
strip of control hints on rows 28–30.

**It is still libctru's text console, and that is still deliberate** — every message this shell says
to the player goes through it, and the three debug pages are built on it. What changed is that the
console is no longer the *only* thing on it. Panels, slots, frames and the map are written straight
into the RGB565 framebuffer by the CPU through `core/gui/paint.hpp`, and the console's glyphs are
printed on top of them.

**The two coexist because the console can be told what colour to draw on.** libctru's
`consoleDrawChar` writes all 64 pixels of a cell — the glyph in `fg` and the rest in `bg` — so text
over a panel used to mean a black rectangle punched through it. `\x1b[48;2;R;G;Bm` sets that
background to an exact RGB565 value; `con_write` reads the three parameters and packs them itself,
which is checkable in `console.o` rather than taken on faith. Every label is printed that way, so
nothing in the layout has to route around the character grid.

The colours are a1.1.2's own GUI: a panel is `#C6C6C6` with a white bevel above and a `#555555` one
below, and a slot is the same two bevels the other way round. The dark readouts are not the
original's — it has no second screen to put one on — and they are the one place a colour here was
chosen rather than read: they are what makes a number legible over a picture of terrain without a
border thick enough to eat the picture. The backdrop behind them is the pack's `dirt.png`, tiled and
darkened exactly as `GuiScreen.drawBackground` tiles it, so the two screens do not disagree about
what this pack looks like.

### Coordinates, and nothing else

The chunk, the later-version map tile, the count of chunks remembered and the redraw time were all
on this page and all four are the maintainer's questions rather than the player's — a page that
answers them is a debug page with a picture on it. The redraw time in particular is still measured
and still worth having, and it is on the **Info** page now, beside every other number that exists to
be watched.

The 128-block and 16-block grids went the same way. They are claims this project makes about the map
— that it lines up with the chunks and with the maps of later versions — and being able to check
them on the hardware is worth keeping, so the switch is a row on the **debug settings** page. It is
off by default, because neither is anything a player wants drawn over their world.

### Who owns a touch

The bottom screen is both the game's UI and the only pointing device an old 3DS has, and while the
whole screen was a debug console those two never collided. A map you can touch does collide:
dragging on it used to turn the camera, which is the opposite of what touching a map means.

So the page says who owns the press. The **Look** page hands the pad below the tab strip to the
camera and draws a compass ribbon over it, which is what makes it worth looking at while you drag on
it; the map and the inventory keep their own presses; the debug pages hand over the whole screen, as
the bottom screen always did. A press that begins on the tab strip belongs to the strip until it is
let go, so a drag that wanders down onto the page cannot end up turning the view with it.

## Where it is

| | |
|---|---|
| `src/core/map/map_sample.hpp` | one chunk reduced to surface block, height and water depth |
| `src/core/map/map_palette.hpp` | block to colour, out of the texture pack |
| `src/core/map/map_store.hpp` | what has been sampled, what it was drawn as, and the later-version tile grid |
| `src/core/map/map_render.hpp` | the shading, the patch, the window copy, and the player marker |
| `src/core/gui/paint.hpp` | panels, slots, bevels, tiled backdrops and the arrow, into any framebuffer |
| `src/platform/ctr/hud.hpp` | the tab strip, the inventory page, the look pad, and text in arbitrary colours |
| `src/platform/ctr/map_screen.hpp` | the map page: layout, the framebuffer, the coordinate readout |
| `src/platform/ctr/overlay.hpp` | which tabs each gamemode gets, and who owns a touch |
| `tests/map_test.cpp` | all of the above, including the framebuffer's strides |
| `tests/paint_test.cpp` | the primitives, through both a row-major buffer and the console's strides |
