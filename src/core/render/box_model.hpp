#pragma once

// **`ip` -- ModelRenderer -- transcribed**: a1.1.2's cuboid-with-texture-offset,
// which is what a boat, a minecart and a sign are made of.
//
// This is the one genuinely new piece of geometry `docs/entity-render-a1.1.2.md`
// authorises. Everything else about drawing an entity turned out to be solved
// already: the 16-byte `mesh::DetailVertex` takes arbitrary corners with
// arbitrary UVs and rides the world shader, which the dropped item proved and
// the falling block confirmed. What had no counterpart here was the *model* --
// the thing that turns seven numbers into a textured box.
//
// **The whole of a1.1.2's model API is five methods**, and only two of them
// carry data:
//
//     ip(int textureOffsetX, int textureOffsetY)
//     void addBox(float x, float y, float z, int w, int h, int d, float grow)
//     void setRotationPoint(float x, float y, float z)
//     void render(float scale)
//     void renderWithRotation(float scale)
//
// A `ModelRenderer` in this version holds **exactly one box**: `addBox`
// *assigns* its vertex and quad arrays rather than appending to them, so
// calling it twice replaces the first box. That is why `ModelPart` below is one
// box and not a list, and it is a fact about the class file rather than a
// simplification.
//
// **Model units are 1/16 of a block** and the conversion is the caller's
// `scale` argument -- `0.0625F` everywhere in this game. It is folded into the
// `Placement` axes here rather than passed separately, because the caller
// already has to build those axes and a second scale would be a second place to
// get it wrong.
//
// **The texture space is 64 x 32 and there is no field that says so.** `ll` --
// TexturedQuad -- divides every u by a hard-coded `64.0F` and every v by
// `32.0F`; `textureWidth`/`textureHeight` on `ModelBase` are a later version's.
// So a model's UVs are written in 64 x 32 page coordinates and
// core/texture/entity_skins.hpp says where each page sits in the sheet.
//
// **The 0.1-texel inset is the original's**, not a fix of ours for the PICA:
// `ll`'s constructor subtracts `0.0015625F` from one u edge and adds it to the
// other, which is 0.1/64 -- a tenth of a texel. It happens to be exactly the
// medicine the PICA's sampling wants as well, which is why nothing here adds
// `mesh::kUvInset` on top of it.
//
// **Winding does not matter and that is not luck.** These are drawn in the
// opaque detail pass, which runs with `GPU_CULL_NONE` because a crossed square
// has two sides -- so a box whose faces came out inside-out is still drawn.
// The quad orders below are the class file's anyway; the point is that a
// mistake in them is a shading question and not an invisible boat.

#include "core/mesh/vertex.hpp"
#include "core/texture/entity_skins.hpp"
#include "core/util/types.hpp"

namespace mc::render {

// `0.0625F`, the `scale` every `render(scale)` call in this game passes. Named
// because it is the model-unit-to-block conversion and appears in each caller.
inline constexpr float kModelUnit = 0.0625f;

// One `ip`: its box, its rotation point and its three angles.
//
// The names are the original's fields spelled out -- `a`,`b`,`c` are the
// rotation point and `d`,`e`,`f` the angles, in radians.
struct ModelPart {
    // `addBox`, in model units. `x`,`y`,`z` is the **minimum** corner and is
    // relative to the rotation point, not to the model's origin.
    float x = 0.0f, y = 0.0f, z = 0.0f;
    int w = 0, h = 0, d = 0;

    // `addBox`'s last argument, which the original calls `f` and which every
    // reader calls "grow": every face moves out by this many model units, so a
    // box passed 0.5 is one unit bigger on each axis. Used to keep two boxes
    // that share a plane from z-fighting.
    float grow = 0.0f;

    // The `ip` constructor's two arguments, in 64 x 32 page coordinates.
    int texU = 0, texV = 0;

    // `ip.g` -- mirror. Swaps the box's two x extents before the quads are
    // built, which flips the texture on four of the six faces. It is how one
    // page textures a left and a right paddle.
    bool mirror = false;

    // `setRotationPoint`, model units.
    float pivotX = 0.0f, pivotY = 0.0f, pivotZ = 0.0f;

    // `rotateAngleX/Y/Z`, radians. Applied X, then Y, then Z -- which is the
    // order the three `glRotatef` calls compose to, and *not* the order they
    // are written in the method.
    float angleX = 0.0f, angleY = 0.0f, angleZ = 0.0f;
};

// Where a model sits in the world, as an origin and three axes.
//
// A matrix rather than a position and a yaw, because every one of these
// entities composes its own transform out of something different -- a boat
// rocks about its length, a minecart tilts along a rail, a sign turns by a
// sixteenth. Handing this the finished axes keeps the trigonometry in the
// renderer that knows which entity it is drawing, and keeps this file pure
// geometry.
//
// The axes are **in blocks per model unit**, so `kModelUnit` is already in
// them. Coordinates are relative to whatever origin the caller is meshing
// against, exactly as the item and particle builders take theirs.
struct Placement {
    double x = 0.0, y = 0.0, z = 0.0;
    float ax[3] = {kModelUnit, 0.0f, 0.0f};
    float ay[3] = {0.0f, kModelUnit, 0.0f};
    float az[3] = {0.0f, 0.0f, kModelUnit};
};

// An identity placement at a world position, with an optional turn about Y.
//
// `yawRadians` turns the model about the vertical, which is what every one of
// these entities needs and three of them need only. Built here so that three
// callers do not each write the same two sines.
Placement placeAt(double x, double y, double z, float yawRadians, float scale = kModelUnit);

// **A model placed the way the game's entity renderers place one**, which is
// not the way `placeAt` places a mesh that was built in world orientation.
//
// `RenderLiving.doRenderLiving` does `glScalef(-1, -1, 1)` and *then*, inside
// that flip, translates down by `24 * 0.0625 + 0.0078125` -- so a model's own
// +Y points at the ground and the whole body hangs from a point a block and a
// half above the entity's feet. A `ModelPart` mesh placed with `placeAt`
// instead comes out upside down and buried, which is exactly what it looked
// like on hardware. `y` is the entity's `posY`, as the mob pass takes it.
//
// `fallRadians` is a dying body's turn about the model's Z -- `deathFall` --
// which `rotateCorpse` applies inside the yaw and before the flip's lift, so
// the body tips over about its feet.
Placement placeModel(double x, double y, double z, float yawRadians, float fallRadians = 0.0f);

// **`dn.a(ge, F)`'s fall**, in radians: `sqrt((deathTime + partial - 1) / 20 *
// 1.6)`, clamped at one, of ninety degrees -- quickly at first, and flat on its
// side for the last few of the twenty ticks. Zero for a body that is not dying.
float deathFall(int deathTime, float partial);

// **The hurt flash**, and a dying body keeps it while it falls: how far green
// and blue are pulled down. The original blends a red at 0.4 alpha over the
// model; this multiplies instead -- see mob_mesh.hpp.
inline constexpr u8 kHurtChannel = 90;

// How many vertices one part writes. Six quads, four corners each -- always,
// with no face ever skipped: a box model has no neighbours to hide behind.
inline constexpr int kBoxVertices = 6 * 4;

// Writes one part's 24 vertices and returns how many it wrote, or 0 when there
// was not room for all of them.
//
// `light` is `(sky << 4) | block` where the entity is, the same byte every
// other detail vertex carries. `skin` picks the page of the entity sheet the
// UVs are relative to.
//
// **An entity further from the origin than a 16-bit detail position can express
// is the caller's problem, not this one's.** The item builder skips such an
// entity rather than clamping it; a box model is drawn part by part and the
// decision has to be made once for the whole model, so it belongs above here.
int buildBox(const ModelPart& part, const Placement& place, texture::EntitySkin skin, u8 light,
             mesh::DetailVertex* out, int max);

}  // namespace mc::render
