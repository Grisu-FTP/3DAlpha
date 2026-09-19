#pragma once

// **The bottom screen of the main menu's Skins, Texture Pack and World
// screens**: a row of players wearing the listed skins, a little scene in the
// pack under the cursor, and a turning diorama of the world under it.
//
// **This is the one place the bottom screen is a GPU target.** Everywhere else
// it is libctru's console with pixels written into it -- see
// core/gui/paint.hpp -- and in game it still is, because the world renderer
// owns the frame and the VRAM there. On the main menu nothing else wants
// either, so while one of these three screens is up a render target is linked
// to the bottom screen and drawn in the same frame as the menu's top screen;
// leaving them deletes it and hands the framebuffer back to the console with
// `consoleInit`, which is safe to repeat (see overlay.cpp).
//
// **Nothing here reads the card in a frame.** A skin page, a pack's terrain
// and a world's chunks are read by `preview::PreviewWorker` on a thread of
// its own, for the rows around the cursor, nearest first; this class uploads
// what came back -- a bounded number of bytes a frame -- and draws whatever is
// ready. A row that is not ready yet draws what it can (the table without its
// world, the last pack's scene), so the cursor never waits.
//
//   * **Skins.** One 256 x 256 sheet of 64 x 32 pages, 32 of them, a page per
//     skin near the cursor. Every character is the player model drawn
//     head-on with an orthographic camera, which is a flat picture of it; the
//     selected one is the same model at the same place that starts to walk and
//     turn, so the swap cannot be seen.
//   * **Packs.** One 256 x 256 atlas per pack near the cursor. The scene's
//     vertices never change -- see core/preview/pack_scene.hpp -- so changing
//     pack is changing which texture is bound.
//   * **Worlds.** Nine tile buffers per world near the cursor, filled in as
//     the worker reads them, over a table that is always complete. The angle
//     it turns through belongs to this object and is never reset.

#include "core/io/posix_file_system.hpp"
#include "core/preview/diorama.hpp"
#include "core/preview/preview_worker.hpp"
#include "core/texture/atlas_image.hpp"
#include "core/texture/pack_list.hpp"
#include "core/texture/skin_list.hpp"
#include "core/util/types.hpp"
#include "core/world/world_list.hpp"
#include "platform/ctr/textures.hpp"

#include <3ds.h>
#include <citro3d.h>

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace mc::ctr {

enum class PreviewScreen {
    None,
    Skins,
    Packs,
    Worlds,
    // **The bottom screen as a screen, with nothing drawn on it here.** The
    // online host's lobby wants the player list on the dirt rather than on the
    // console's text grid, and that means a render target: this is the target
    // and no picture behind it, so the menu draws its own backdrop and its own
    // rows into it. Nothing is read from the card for it and no worker is
    // asked for anything.
    Plain,
};

class MenuPreview {
public:
    MenuPreview() = default;
    ~MenuPreview();

    MenuPreview(const MenuPreview&) = delete;
    MenuPreview& operator=(const MenuPreview&) = delete;

    // The shader, the shared buffers and the worker. False when there is no
    // memory for them, in which case the menu keeps its console.
    bool init(bool isNew3DS);
    void shutdown();

    // Links the bottom screen to a render target for one of the screens that
    // want one, or gives it back to the console for None. Outside a frame
    // only.
    void setScreen(PreviewScreen screen);
    PreviewScreen screen() const { return screen_; }
    C3D_RenderTarget* target() const { return target_; }

    // What the active pack is: the atlas the table's slab and the Default skin
    // come from. Cheap when `revision` has not changed.
    void setActivePack(const texture::AtlasImage& atlas, u32 revision);

    void setSkinList(const std::vector<texture::SkinEntry>& skins);
    void setSkinCursor(int row);
    // Row 0 is "+ Extract from a jar..."; row n is `packs[n - 1]`.
    void setPackList(const std::vector<texture::PackEntry>& packs);
    void setPackCursor(int row);
    // The first `pinnedRows` rows ("+ Create New World", and "+ Import World"
    // outside hosting) have no world; row n after them is `worlds[n - pinnedRows]`.
    void setWorldList(const std::vector<world::WorldEntry>& worlds, int pinnedRows);
    void setWorldCursor(int row);

    // Stops every read of this world and waits for one in flight to notice.
    void quiesce(const std::string& worldPath);

    // The same, and then throws away everything remembered about the world:
    // its grid, its tile meshes and its place in the grid cache. The next pass
    // reads it again from scratch -- **including its `3dalpha.ini`**, which is
    // the point: this is how World Settings' Move Panorama makes a new table
    // position take effect without leaving the screen.
    void forgetWorld(const std::string& worldPath);

    // Once a frame, before drawing: picks up finished work within this frame's
    // upload allowance and moves the animations on by `seconds`.
    void update(float seconds);

    // The 3D half of the bottom screen, into `target()`. The caller has begun
    // the frame, cleared the target and drawn the backdrop, and puts citro2d's
    // state back afterwards. `Plain` has no 3D half and draws nothing.
    void draw();

private:
    struct SkinRow {
        std::string key;   // job key
        std::string path;
        bool fromPack = false;
        bool isDefault = false;
    };
    struct SkinPage {
        std::string key;
        bool ready = false;
    };

    struct PackRow {
        std::string key;
        std::string path;
    };
    struct PackTexture {
        std::string key;
        C3D_Tex tex{};
        bool allocated = false;
        bool ready = false;
        bool failed = false;
    };

    struct TileBuffer {
        void* data = nullptr;
        usize capacity = 0;
        u32 coloured = 0;
        u32 textured = 0;
        // How `coloured` splits into facing groups, in `kDioramaFaceOrder`:
        // the draw issues only the ones the camera can be looking at.
        u32 run[mesh::kFaceCount] = {};
        bool ready = false;
    };
    struct WorldMeshes {
        std::string key;
        u32 revision = 0;
        bool complete = false;
        TileBuffer tiles[preview::kDioramaTiles * preview::kDioramaTiles];
    };

    // Worker side.
    static void handleJob(void* context, preview::PreviewWorker& worker,
                          const preview::PreviewJob& job);
    void runSkinJob(preview::PreviewWorker& worker, const preview::PreviewJob& job);
    void runPackJob(preview::PreviewWorker& worker, const preview::PreviewJob& job);
    void runWorldJob(preview::PreviewWorker& worker, const preview::PreviewJob& job);
    void postTile(preview::PreviewWorker& worker, const preview::PreviewJob& job,
                  const preview::DioramaGrid& grid, int tile,
                  const preview::DioramaColours& colours, u32 revision);
    // `readDioramaTiles`' per-tile hand-over: meshes and posts the tile whose
    // last chunk just landed, without waiting for the rest of the batch.
    static void postReadyTile(void* context, int tileX, int tileZ);

    // Main side.
    usize apply(preview::PreviewResult& result);
    void refreshWanted();
    int preloadRadius(int base) const;
    int skinPageFor(const std::string& key, bool claim);
    int packTextureFor(const std::string& key, bool claim);
    int worldSlotFor(const std::string& key, bool claim);
    // Frees the tile buffers of one world outside the preload window, other
    // than slot `keep`. False when there is none to free.
    bool releaseFarWorld(int keep);
    void writeDefaultSkin();

    void bindPipeline();
    void setView(float yaw, float pitch, float pixelsPerBlock, float centreX, float centreY,
                 float originX, float originY, float originZ, float depthRadius,
                 float unitsPerBlock);
    void drawVertices(const void* vertices, u32 count, C3D_Tex* texture);
    void drawSkins();
    void drawPacks();
    void drawWorlds();

    bool ready_ = false;
    bool isNew3DS_ = false;
    PreviewScreen screen_ = PreviewScreen::None;
    C3D_RenderTarget* target_ = nullptr;

    DVLB_s* dvlb_ = nullptr;
    shaderProgram_s program_{};
    C3D_AttrInfo attrs_{};
    s8 uMvp_ = -1;
    s8 uFog_ = -1;
    u16* indices_ = nullptr;
    C3D_Tex white_{};
    bool whiteReady_ = false;
    // The world's own lightmap, held at noon: the diorama's vertices carry the
    // light their chunks were saved with.
    Lightmap lightmap_;
    bool lightmapReady_ = false;

    // Skins.
    C3D_Tex skinSheet_{};
    bool skinSheetReady_ = false;
    static constexpr int kSkinPages = 32;
    SkinPage skinPages_[kSkinPages];
    std::vector<SkinRow> skinRows_;
    std::vector<u8> defaultSkin_;
    int skinCursor_ = 0;
    float skinScroll_ = 0.0f;
    float walkSeconds_ = 0.0f;
    float limbSwing_ = 0.0f;
    mesh::DetailVertex* playerVertices_ = nullptr;

    // Packs.
    static constexpr int kPackTextures = 7;
    PackTexture packTextures_[kPackTextures];
    std::vector<PackRow> packRows_;
    int packCursor_ = 0;
    int lastPackTexture_ = -1;
    void* sceneVertices_ = nullptr;
    u32 sceneCount_ = 0;

    // Worlds.
    // Tile meshes are linear memory, of which the menu has tens of megabytes
    // free, and a slot outside the preload window is kept until something
    // needs the space (`farthestOutside`, and `releaseFarWorld` under
    // pressure). So the count is what a world list plausibly holds, not what
    // the read-ahead window asks for: a world already read keeps its mesh and
    // costs nothing to come back to.
    static constexpr int kWorldSlots = 16;
    WorldMeshes worldSlots_[kWorldSlots];
    std::vector<std::string> worldRows_;
    int worldCursor_ = 0;
    float dioramaAngle_ = 0.0f;
    TileBuffer emptyTiles_[preview::kDioramaTiles * preview::kDioramaTiles];
    TileBuffer sides_;

    // The active pack.
    C3D_Tex atlas_{};
    bool atlasReady_ = false;
    u32 packRevision_ = 0;
    bool havePack_ = false;

    // Shared with the worker: what the diorama is painted with.
    std::mutex coloursLock_;
    std::shared_ptr<const preview::DioramaColours> colours_;
    u32 coloursRevision_ = 0;

    // Worker only: the worlds read most recently, whole or in part, oldest
    // first in `gridOrder_`.
    void keepGrid(const std::string& key);
    // How many grids the heap can stand right now, measured, not fixed.
    usize gridLimit() const;
    std::map<std::string, std::unique_ptr<preview::DioramaGrid>> grids_;
    std::vector<std::string> gridOrder_;
    io::PosixFileSystem workerFs_;

    preview::PreviewWorker worker_;
    bool wantedDirty_ = false;
    preview::PreviewResult pending_;
    bool havePending_ = false;
};

}  // namespace mc::ctr
