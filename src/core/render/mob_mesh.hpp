#pragma once

// **All nine mobs as geometry** -- `hg` (ModelQuadruped) and its three
// subclasses, `kv` (ModelChicken), `cr`/`cb`/`fv` (biped, zombie, skeleton),
// `em` (creeper), `jy` (spider), `hh` (slime), and `dn` (RenderLiving) which
// poses all of them.
//
// **Six boxes, or nine, or eight.** `hg` is the shape three of the four animals
// share: a head, a body laid on its side and four legs whose length is the one
// number its constructor takes. The sheep replaces the head and the body with
// smaller ones; the cow replaces both and adds two horns and an udder; the
// chicken is not a quadruped at all and has a model of its own.
//
// **The monsters are five, seven, six, eleven and five.** A zombie and a
// skeleton are the player's seven-box biped, posed by `cb` -- which replaces
// both arms outright and is what both of them use, because `fv` extends it and
// only narrows the four limbs. A creeper is six boxes drawn out of seven built:
// `em` constructs a headwear box and `em.b` never renders it. A spider is eleven
// and a slime is five across the two passes `gq` makes.
//
// **Two of them are not a fixed size**, and both use `dn`'s one hook,
// `preRenderCallback`, which runs between the `glScalef(-1, -1, 1)` and the
// model's own lift: the creeper's swell is `(1 + s^4 * 0.4) * wobble` on
// `s = fuse / (fuseTime - 2)`, and a slime is scaled by its size -- `hh`'s boxes
// are identical for a size-1 and a size-4. `Placement` carries a per-axis scale
// for them, and because the hook runs *before* the lift, the lift scales too:
// a swollen creeper rises off the ground rather than sinking into it.
//
// **Two of them are drawn twice.** `ns` (RenderSheep) and `gm` (RenderPig) pass
// a second model to `RenderLiving` and answer `shouldRenderPass` for it: the
// sheep's fleece is the same quadruped grown by 0.6 to 1.75 model units in
// different places, drawn from `mob/sheep_fur.png` while the sheep is unshorn,
// and the saddle is the pig's own model grown by a half, drawn from
// `mob/saddle.png` while it is saddled. So a sheared sheep and an unsaddled pig
// are one model each and the other two are two.
//
// **All seventeen pages are in one sheet**, so this is still a single draw call
// with a single bind -- see core/texture/entity_skins.hpp. That is the whole
// reason the mobs cost the frame what a boat costs it.
//
// **The spider's eyes are one part rather than a second model**, and that is a
// stated deviation. `ok` draws the whole of `jy` again from
// `mob/spider_eyes.png` and blends it at `(1 - brightness) * 0.5`; that page is
// transparent everywhere but the head, so the other ten boxes would write 240
// vertices for the alpha test to throw away. Redrawing the head alone is the
// same picture. What is lost is the blend: the eyes are an alpha cut here and do
// not dim in daylight.
//
// **Where `RenderLiving` puts the model**, which is four transforms and an
// eighth of a block that looks like a mistake and is not:
//
//     glTranslatef(x, y, z);                       // interpolated position
//     glRotatef(180 - renderYawOffset, 0, 1, 0);   // the *body's* heading
//     glRotatef(deathAngle, 0, 0, 1);              // a corpse falls over
//     glScalef(-1, -1, 1);                         // model +y is down
//     glTranslatef(0, -24 * 0.0625 - 0.0078125, 0);
//
// The 24 is the model's own height in model units and the 0.0078125 is an
// eighth of a model unit of lift, which is what keeps the feet out of the floor.
// Composed, that is an origin and three axes -- `render::Placement` -- so
// nothing here needs a matrix library.
//
// **The head turns and the body follows.** `renderYawOffset` is the body's
// heading and chases the direction of travel; the head is drawn at
// `rotationYaw - renderYawOffset` and is clamped to 75 degrees either side by
// the entity tick, which is why an animal watching a player walk past turns its
// head first and its body after. See core/entity/mob.hpp.
//
// **The hurt flash is a tint here and an extra pass in the original.** `dn`
// draws the whole model again in `glColor4f(brightness, 0, 0, 0.4)` with the
// depth test on equal, which is a second pass with blending. A `DetailVertex`
// carries its own colour, so a hurt animal is drawn once with green and blue
// pulled down instead. The difference is that the original's red is *added*
// over the texture and this multiplies it -- a white sheep flashes pink rather
// than red -- and it costs no pass, no state change and no second bind.

#include "core/entity/mob.hpp"
#include "core/mesh/vertex.hpp"
#include "core/render/box_model.hpp"
#include "core/util/types.hpp"

namespace mc::render {

// The most parts one mob can need, and **three different models reach it**: a
// sheep's six plus its fleece's six, a pig's six plus its saddle's six, and a
// spider's eleven plus its eye overlay. A biped is seven, a creeper six and a
// slime five.
inline constexpr int kMobMaxParts = 12;
inline constexpr int kMobVerticesEach = kMobMaxParts * kBoxVertices;

// **What one frame draws**, which is not what exists. a1.1.2 caps *spawning* at
// 15 animals (core/entity/mob_spawn.hpp) and nothing caps a pen somebody bred,
// so past this many in range the nearest are drawn -- see draw_budget.hpp.
inline constexpr int kMobDrawBudget = 16;
inline constexpr int kMobMaxVertices = kMobDrawBudget * kMobVerticesEach;

// `dn.a(Lge;DDDFF)V` for every live animal, nearest first when the buffer
// cannot hold them all. Returns how many vertices were written.
int buildMobs(const entity::MobSystem& system, double originX, double originY, double originZ,
              float partial, mesh::DetailVertex* out, int max);

// One animal's parts, posed, for the test suite -- the interesting half is the
// pose and it is a pure function of the mob. `skins` takes one page per part.
// Returns the number of parts written, at most `kMobMaxParts`.
int poseMob(const entity::Mob& mob, float partial, ModelPart* parts,
            texture::EntitySkin* skins, int max);

// The origin and axes `RenderLiving` composes for this mob -- see the header.
Placement placeMob(const entity::Mob& mob, double originX, double originY, double originZ,
                   float partial);

}  // namespace mc::render
