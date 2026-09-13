#pragma once

// **Survival's left button on a block** -- `nj`, the single-player
// PlayerController, which is the only place a1.1.2 decides that a block takes
// time to break.
//
// Creative has none of this and never did: one press there is one broken block
// (see platform/ctr/main.cpp's `editBlocks`). Survival has two separate clocks
// on the same button, and they are easy to conflate:
//
//   * **The click**, `nj.a(IIII)V`, runs on the press and again on
//     `Minecraft.clickMouse`'s five-tick repeat. It tells the block it was
//     clicked -- a door, a button and a lever *activate* on a left click in this
//     version, and redstone ore lights -- and it breaks the block on the spot
//     when one tick's worth of progress is already a whole break.
//   * **The damage**, `nj.c(IIII)V`, runs every tick the button is held on a
//     block: `Minecraft.a(IZ)V` calls it with the crosshair's cell, and calls
//     `nj.a()V` -- reset -- on every tick it is not. Progress accumulates
//     `ly.a(Ldm;)F` a tick; a step sound plays every four ticks; at 1.0 the
//     block goes, the progress is zeroed and **five ticks** pass before the next
//     block can take any.
//
// Moving the crosshair to another cell restarts the progress from nothing,
// which is what the original does and why a break has to be finished on the
// block it was started on.
//
// The break itself is `nj.b(IIII)Z`, in this order: `hq.b` removes the block
// (particles, sound, `onBlockDestroyedByPlayer`), the held stack takes its
// `hitBlock` wear and is destroyed if that used it up, and **only then**, and
// only if the removal happened and the player could harvest the block, does
// `harvestBlock` drop anything. **`canHarvestBlock` is asked after the wear**,
// of whatever the hand holds by then -- so a pickaxe that breaks on its last
// block has already left the hand, and that block drops nothing.

#include "core/item/item_def.hpp"
#include "core/util/types.hpp"

namespace mc::tick {
class TickWorld;
}

namespace mc::item {

struct Effects;
struct Inventory;

// Everything one click or one tick of damage reaches.
struct BreakContext {
    tick::TickWorld& world;
    Inventory& inventory;
    const Effects& effects;
    // `dm.a(Lly;)F`'s two penalties, asked of the body this tick.
    bool eyeInWater = false;
    bool onGround = true;
};

class BlockBreaker {
public:
    // `nj.a(IIII)V` -- clickBlock. Returns whether the block broke.
    bool click(BreakContext& ctx, i32 x, int y, i32 z, int face);

    // `nj.c(IIII)V` -- onPlayerDamageBlock, once a tick while held on a block.
    // Returns whether the block broke.
    bool damage(BreakContext& ctx, i32 x, int y, i32 z, int face);

    // `nj.a()V` -- resetBlockRemoving, once a tick while not.
    void reset();

    // `nj.c()V` -- the per-tick bookkeeping: last tick's progress, for the crack
    // to interpolate from. Call once a tick, before `damage` or `reset`.
    void update() { prevProgress_ = progress_; }

    // The crack drawn over the block this frame -- `RenderGlobal`'s
    // `(int)(damagePartialTime * 10)`, where the partial time is `nj.a(F)V`'s
    // interpolation -- or -1 for none.
    int crackStage(float partial) const;

    // The cell the progress belongs to; meaningless while `progress()` is 0.
    i32 x() const { return x_; }
    int y() const { return y_; }
    i32 z() const { return z_; }
    float progress() const { return progress_; }

private:
    i32 x_ = -1;           // `c`
    int y_ = -1;           // `d`
    i32 z_ = -1;           // `e`
    float progress_ = 0.0f;      // `f`
    float prevProgress_ = 0.0f;  // `g`
    float soundCounter_ = 0.0f;  // `h`
    int hitDelay_ = 0;           // `i`
    bool hitting_ = false;       // `j` -- whether `reset` has anything to undo
};

// `nj.b(IIII)Z` -- the break: removal, wear, and the drop if it was earned.
bool harvestBlock(BreakContext& ctx, i32 x, int y, i32 z);

// `ly.b(Lcn;IIILdm;)V` -- onBlockClicked. Four classes answer it in a1.1.2 and
// all four do what a right click does: `fw` the door, `hu` the button and `no`
// the lever call their own `blockActivated`, and `ai` redstone ore lights. The
// stairs forward to their model block, which answers nothing.
void blockClicked(tick::TickWorld& world, i32 x, int y, i32 z);

// `cn.i(IIII)V` -- the other thing a left click does before it reaches the
// controller: **fire on the struck face is put out**, with a fizz. Returns
// whether there was any.
bool extinguishFireOnFace(tick::TickWorld& world, i32 x, int y, i32 z, int face);

}  // namespace mc::item
