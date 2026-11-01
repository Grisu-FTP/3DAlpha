#pragma once

// The player in the inventory, drawn on the CPU onto a software page.
//
// a1.1.2 draws the player in its inventory window (`lo`), turned toward the mouse and
// lit with `RenderHelper.enableStandardItemLighting`. The 3DS draws that page in
// software into a surface it repaints only when something on it changes, so this
// does the same with the player model core already builds: the same boxes, the same
// UVs and the same skin page, rasterised here rather than handed to a GPU. It costs
// nothing while the page is still.
//
// Orthographic, as the jar's GUI is: the feet stand at (`footX`, `footY`) and one
// block is `pixelsPerBlock` pixels. Faces are drawn far to near and never read a
// depth buffer, which is enough for a convex box model at this size.

#include "core/gui/paint.hpp"
#include "core/util/types.hpp"

namespace mc::gui {

struct DollPose {
    // Turned about the vertical, in degrees: positive turns the face toward +x,
    // which is the right of the surface.
    float bodyYaw = 0.0f;
    // The head tips against the body, in degrees: positive looks down.
    float headPitch = 0.0f;
    // `ItemArmor.renderIndex` per slot, helmet first, or -1 for nothing worn.
    int armour[4] = {-1, -1, -1, -1};
};

// The two sheets it samples, borrowed: the 256 x 256 entity sheet, with the player's
// skin on its own page, and the armour sheet. A null `entity` draws nothing, and a
// null `armour` draws the player without what they are wearing.
struct DollSheets {
    const u8* entity = nullptr;
    const u8* armour = nullptr;
};

// Clips to the surface and to the rectangle given. Main thread: the vertices are held
// in a static buffer rather than on a 3DSX's 32 KB stack.
void drawPlayerDoll(const Surface& surface, int footX, int footY, float pixelsPerBlock,
                    int clipX, int clipY, int clipW, int clipH, const DollSheets& sheets,
                    const DollPose& pose);

}  // namespace mc::gui
