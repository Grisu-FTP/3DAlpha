#pragma once

// **What is in your hand, in the bottom right of the top screen** -- `jh`,
// which is `ItemRenderer`, and the last thing `EntityRenderer.renderWorld`
// draws before the GUI.
//
// Two halves of one class, which is why they are one module:
//
//   * `HeldItemState` is `jh.a()V` -- `updateEquippedItem` -- plus the arm
//     swing it reads, which is `EntityPlayer.updateArmSwingProgress` (`dm.b_`)
//     and `swingItem` (`dm.w`). Three counters, all on the 20 Hz clock.
//   * `buildHeldItem` is `jh.a(F)V` -- `renderItemInFirstPerson` -- and
//     `jh.a(Lev;)V` -- `renderItem` -- transcribed into quads.
//
// **The geometry comes out in camera space**, in blocks, with the eye at the
// origin looking down -Z. That is exactly what the original does: `renderHand`
// (`iq.b(FI)V`) calls `glLoadIdentity()` on the modelview and then this, so the
// item never sees the camera's position or its yaw. The caller therefore draws
// this with the projection matrix alone and no view -- see
// `Renderer::drawHeldItem`.
//
// **The original clears the depth buffer first**, `glClear(GL_DEPTH_BUFFER_BIT)`
// at offset 704 of `iq.c(F)V`, so the hand can never be clipped by a wall the
// player is standing against. citro3d has no mid-frame depth clear; the
// platform gets the same effect by compressing this pass into the nearest
// sliver of the depth range, which is a note in renderer.cpp rather than here.
//
// **An empty hand draws the arm**, which is the original's `else` branch:
// `bu.b()` -- `RenderPlayer.drawFirstPersonHand` -- and the whole of it is
// `ModelBiped.bipedRightArm.render(0.0625F)` after
// `setRotationAngles(0, 0, 0, 0, 0, 0.0625F)`. One box, twelve model units
// long, off the player skin.
//
// **The skin is a pack file, `char.png`.** It sits at the root of a1.1.2's jar
// rather than under `item/`, but it is a texture pack's to supply exactly as
// `terrain.png` is, and `core/texture/entity_skins.hpp` gives it a page of the
// entity sheet like the boat and the cart. Nothing here ships a skin: a pack
// without one gets **a black arm**, which is that header's argument -- a
// silhouette is honest about being a shape with no skin on it, where a
// placeholder grid would read as a bug.
//
// **The transform is not the item's.** The two branches share their shape and
// none of their numbers: 0.3/0.4/0.4 against 0.4/0.2/0.2 for the swing offset,
// 0.8 and -0.75 against 0.7 and -0.65 for the rest position, and the arm turns
// `+70` degrees about Y where the item turns `-20` and takes no X rotation at
// all. Both are transcribed rather than shared.
//
// **The one deviation is horizontal, and the screen is why.** a1.1.2 frames the
// hand for a 4:3 window and the top screen is 5:3: the same camera-space x
// divides by a wider half-extent, so an untouched transcription puts the item
// two thirds of the way out instead of five sixths and it reads as floating
// near the middle. `buildHeldItem` takes the aspect ratio and shifts the whole
// item sideways by what restores the original's framing. A shift and not a
// scale -- scaling camera-space x would stretch the item as well as move it.

#include "core/item/item_def.hpp"
#include "core/mesh/vertex.hpp"
#include "core/util/types.hpp"

namespace mc::render {

// `Minecraft.timer`'s aspect, and the window a1.1.2's constants were chosen
// against. Only the horizontal shift above reads it.
inline constexpr float kOriginalAspect = 4.0f / 3.0f;

// The largest vertex count `buildHeldItem` can write.
//
// The sprite is the biggest of the three branches and it is not close: a front
// face, a back face and **four runs of sixteen one-texel edge strips**, which
// is how the original gives a flat icon its thickness. 66 quads against a
// block's 9 boxes x 6 faces, and against the arm's single box.
inline constexpr int kHeldSpriteQuads = 2 + 4 * 16;
inline constexpr int kMaxHeldItemVertices = kHeldSpriteQuads * 4;

// Which texture the one draw has to be bound to.
//
// Three, not two: the two the item table already distinguishes plus the entity
// sheet the arm comes off. It is its own enum rather than `item::IconSheet`
// because an empty hand has no item and therefore no icon sheet at all.
enum class HeldSheet : u8 {
    Terrain,
    Items,
    PlayerSkin,
};

// What `buildHeldItem` wrote, and what it has to be drawn with. One hand is one
// texture, so unlike the item-entity pass this is a single draw and the answer
// comes back rather than being asked for.
struct HeldItemMesh {
    int vertices = 0;
    HeldSheet sheet = HeldSheet::Terrain;
};

// `jh` minus the drawing: the item the hand is *showing*, how far it has been
// raised, and where the swing is.
//
// **The shown item lags the selected one on purpose.** `updateEquippedItem`
// drops `equippedProgress` to zero before it adopts the new stack, which is
// what makes switching hotbar slots lower one item and raise the next instead
// of swapping them in place.
class HeldItemState {
public:
    // One 20 Hz tick. `inHand` is what the hotbar currently selects.
    void tick(item::ItemId inHand);

    // `EntityPlayer.swingItem`. Left click always; right click only when the
    // use was accepted -- `Minecraft.clickMouse`'s own split.
    void swing();

    // `jh.b()` and `jh.c()`, which are the same one line: put the item back on
    // the floor of its own animation so it is raised again from nothing.
    // `Minecraft.clickMouse` calls one or the other whenever a use *changed*
    // what is in the hand -- a bucket becoming a water bucket -- so the swap is
    // shown rather than happening between two frames.
    void reequip() { equipped_ = 0.0f; }

    // What is being drawn, which is not necessarily what is selected.
    item::ItemId item() const { return item_; }

    // Both interpolated across the frame, as the original's callers do.
    float equippedProgress(float partial) const;
    float swingProgress(float partial) const;

private:
    item::ItemId item_ = 0;
    float equipped_ = 0.0f;
    float prevEquipped_ = 0.0f;
    float swing_ = 0.0f;
    float prevSwing_ = 0.0f;
    int swingTicks_ = 0;
    bool swinging_ = false;
};

// Fills `out` with the held item's quads in camera space and says how many and
// off which sheet.
//
// `equipped` and `swing` are `HeldItemState`'s two interpolated values;
// `aspect` is the viewport's width over its height; `light` is the
// `(sky << 4) | block` byte where the player is standing, which is this pass's
// stand-in for `glColor4f(brightness, brightness, brightness, 1)`.
//
// **Item 0 is an empty hand and draws the arm**, not nothing. An item this
// build does not know is the one case that writes zero -- the same call
// `drawItemIcon` makes, and for the same reason.
HeldItemMesh buildHeldItem(item::ItemId item, float equipped, float swing, float aspect,
                           u8 light, mesh::DetailVertex* out, int max);

}  // namespace mc::render
