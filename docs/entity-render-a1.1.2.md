# Drawing an entity

Written 2026-09-07, because `docs/todo-m3.md` step 5 says a design note with a measured budget
comes before any of this and four things on a play-session list are queued behind it: the bow, the
boat, the minecart and the painting. Signs are queued behind the *other* half of the same box, the
tile entity, and are in here for the same reason — they are drawn by a renderer and not by the
mesher.

The note that opened step 5 said the expensive part is that **nothing in the renderer can draw an
entity**, and gave a reason: chunk meshes go through `VboPool` with a 12-byte vertex that has no
room even for a face-flip bit, so entity models would be a different vertex format, a different
draw cadence and a different budget on a 268 MHz ARM11.

**Half of that turned out to be wrong, and it was already disproved once.** Dropped items landed in
§9 of `status.md` and needed no new vertex format: the 16-byte `DetailVertex` that the torches, the
crossed squares and the particles already use takes arbitrary corners with arbitrary UVs, carries
its own light nibbles and rides the world shader — so it fogs and lights for free. A spinning
quarter-size block and a yaw-billboarded sprite both went down the existing detail pass. The
falling block followed it.

So this note is not "design a new pipeline". It is: **what is actually left**, and what does it
cost.

## What is actually left

Three things, and only the third is new.

1. **Geometry.** Solved. `DetailVertex`, the existing detail pass, the existing quad index buffer.
2. **A place to put entities.** Partly solved. `ItemEntitySystem` and the falling-block pool are
   fixed-capacity flat pools that outlive the columns under them. What is *not* solved is that they
   are session pools — see the `entitydata` slot, still `none`, still open, and still not what this
   note unblocks.
3. **Texture.** The new thing, and the only one that costs anything.

### The texture problem, stated

The detail pass samples exactly one texture per draw. That is why a dropped item is already two
draws: `terrain.png` for the ids below 256 and `gui/items.png` for the rest, built into disjoint
halves of one buffer and drawn with two different bindings.

a1.1.2 draws these five entities out of five *more* files:

| Class | File | Size | Draws |
|---|---|---|---|
| `gk` RenderArrow | `item/arrows.png` | 32 x 32 | the arrow |
| `cp` RenderBoat | `item/boat.png` | 64 x 32 | the boat |
| `kt` RenderMinecart | `item/cart.png` | 64 x 32 | the minecart |
| `in` TileEntitySignRenderer | `item/sign.png` | 64 x 32 | the sign |
| `bw` RenderPainting | `art/kz.png` | 256 x 256 | every painting |

Five files is not five textures. **Four of them are small and go into one sheet; the fifth is
already a full sheet and stays one.**

### The decision: two more 256 x 256 sheets, and no new pipeline

* **`entity` sheet.** `arrows.png`, `boat.png`, `cart.png` and `sign.png` packed into one 256 x 256
  RGBA8 at fixed offsets. Together they are 32x32 + three 64x32 = 7,168 texels, which is 11 % of a
  256 x 256 — so the packing is not tight and does not need to be. Fixed offsets rather than a
  computed atlas, because the offsets are compiled into the model tables and a rectangle packer
  whose output can move is a table that has to be regenerated to stay true.
* **`art` sheet.** `kz.png` verbatim, 256 x 256, because it already is one and the art table
  indexes it in absolute texels.

Both are **ordinary linear memory, not VRAM**, for the reason `gui/items.png` already is: VRAM is
6 MB with render targets in it, the block atlas earns its place because every fragment of every
chunk samples it, and these are sampled by at most a few hundred quads a frame. 256 KB each.

**Everything else is unchanged.** Same `DetailVertex`, same detail pipeline, same shader, same
index buffer, same `BufInfo_Add` trick that already gives the two item-sheet passes separate base
pointers out of one buffer. The `i16` UV is in 1/16384 of *the bound texture*, not of the block
atlas specifically, so a 256 x 256 entity sheet addresses at 1/64 of a texel with no change to the
encoding and no change to the vertex.

This is the whole reason the note is short. The pipeline question was answered by the dropped item;
what was left was a texture-management question wearing its clothes.

### Cost, stated as a budget

Per frame, worst case, with the caps this note sets:

| | Draw calls | Binds | Vertices |
|---|---|---|---|
| Dropped items (existing) | 2 | 1 | 6,144 |
| Box-model entities (boat, cart, arrow) | 1 | 1 | 1,536 |
| Paintings | 1 | 1 | 1,024 |
| Signs | 1 | shares `entity` | 512 |

**Four draw calls and three texture binds added to a frame that already makes several dozen.** The
binds are the part worth watching, not the calls: a bind on the PICA is a register write, but it
splits the batch, and the existing item pass already pays two for the same reason.

The vertex counts come from the caps below and are ceilings, not measurements. A boat is 5 boxes =
30 quads = 120 vertices; sixteen of them is 1,920. (The caps are now draw budgets only -- see
"Caps".)

**No hardware number is in this table**, and CONTRIBUTING requires one of anything that touches the
renderer. That debt is real and is the same one the selection outline and the whole of Creative
carry. What this note commits to is that the *shape* is measurable: each of these is a separate
draw call with a separate vertex count, `frameStats_` already counts both, and the debug page
already shows them.

## The box model

Boat, minecart and sign are `ip` — `ModelRenderer` — which is a1.1.2's cuboid-with-texture-offset
system and the thing that has no counterpart here yet. Its whole API is five methods:

```
ip(int textureOffsetX, int textureOffsetY)
void a(float x, float y, float z, int w, int h, int d, float scale)   // addBox
void a(float x, float y, float z)                                     // setRotationPoint
void a(float scale)                                                   // render
void b(float scale)                                                   // renderWithRotation
```

A box is in **model units, which are 1/16 of a block**, and its six faces take their UVs from the
texture offset by walking the standard unwrapped-cuboid layout — depth, width, depth, width across,
depth then height down. Rendering applies the rotation point and the three angles.

`core/render/box_model.{hpp,cpp}` is that, and it is the one genuinely new piece of geometry code
this note authorises. It is host-testable — a box's 24 vertices and their UVs are a pure function
of seven numbers — and it should be tested that way, because it is the part where a sign texture
comes out mirrored and nobody can tell from a screenshot which of the six faces is wrong.

**The models themselves are transcriptions**, not drawings: `cl` (ModelBoat, 5 boxes), `hj`
(ModelMinecart, 7) and `jk` (ModelSign, 2) are constructors full of integer literals and they get
read out of the class file the way the collision boxes were.

## Caps

**Superseded (2026-09-11): the pools have no cap.** a1.1.2 caps none of these -- `spawnEntityInWorld`
and `EffectRenderer.addEffect` (`bq.a(Lnq;)V`, read off the jar) each add to a list -- so a fixed
pool was this port's limit, and players hit it as arrows that stopped firing and carts that would not
go down. Every pool is now a `SegmentedPool` (`core/util/segmented_pool.hpp`): the old number is the
first segment, taken at construction so ordinary play still never allocates, and past it the pool
grows a segment at a time until `poolGrowthAllowed` (`core/util/memory.hpp`) says the heap needs the
room more. A refusal lands on the path a full array used to take; it never ends the process.

What stayed bounded is what is **drawn**, because each pass's vertex buffer is taken once at init.
Past a budget the nearest are drawn (`core/render/draw_budget.hpp`). The budgets, per pass:

* **Arrows: 128 in range.** One per click, gone on impact or after a minute.
* **Boats: 32, minecarts: 32.** Placed one at a time by hand, and they persist.
* **Paintings: eight full-size pictures' worth of vertices.** A wall of them is a thing players do;
  the far end of a long gallery goes first.
* **Signs: 64 within the render distance**, boards and text together.
* **Dropped items: 64 one-block drops' worth; falling blocks: 64; particles: 512.**

The original argument for each number, now only a draw budget: *arrows* -- a player firing as fast as
the five-tick repeat allows has four a second in the air over the eight seconds one lives; *vehicles*
-- more than a player builds by accident; *signs* -- tile entities belong to columns, so the cap is
on what is drawn, not on what exists.

## What this did not decide, and what happened next

* **Riding** was left open here on the grounds that it is a `PlayerBody` question rather than a
  renderer one. It is settled now, and the answer was smaller than expected: **`EntityLiving` never
  checks whether it is riding.** There is no reference to `ridingEntity` anywhere in `ge`, so
  `moveEntityWithHeading` keeps turning the movement keys into `motionX`/`motionZ` exactly as it
  would on foot; a vehicle reads that motion (`motionX += riddenByEntity.motionX * 0.2`) and only
  the rider's *position* is overwritten. `PlayerBody::tickRiding` is that, and
  `core/entity/rider.hpp` is the seam that keeps `boat.hpp` from ever including `player_body.hpp`.
* **The `entitydata` slot** is still open and these are still session pools. A boat placed and a
  world reloaded is a boat gone, exactly as a dropped item is. Signs are the same and it stings
  more, because a sign's whole content is text somebody typed -- see `core/world/sign_store.hpp`.
* **Mobs.** Nothing here is about animation, and a mob model has a skeleton this does not. What
  is known now (status.md §18) is where the cost will be:
  - Every pass here rebuilds its vertices on the CPU every frame. That is fine for a handful of
    boats. At a1.1.2's own monster cap (200) of six-box bipeds it is 28,800 vertices a frame.
  - So a mob model should be built once per type into a static buffer, and posed on the GPU from
    per-part matrices.

## One thing the budget above got wrong

The table says three binds. It is **five**, because two more textures arrived that this note did not
foresee: the painting sheet earns one of its own, and **sign text needs the pack's bitmap font bound
into the world pass** -- a sign is two draws, the board off the entity sheet and the glyphs off
`default.png`. That is the one place Dev Art is less than a real pack: there is no generated font,
so a sign with no pack behind it shows a blank board.
