#include "platform/ctr/menu_preview.hpp"

#include "core/entity/player_body.hpp"
#include "core/mesh/mesher.hpp"
#include "core/preview/pack_scene.hpp"
#include "core/preview/preview_window.hpp"
#include "core/render/player_model.hpp"
#include "core/settings/world_settings.hpp"
#include "core/texture/entity_skins.hpp"
#include "core/texture/tiled.hpp"
#include "platform/ctr/heap.hpp"
#include "platform/ctr/textures.hpp"
#include "core/world/world_peek.hpp"

#include <detail_shader_shbin.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace mc::ctr {

namespace {

using texture::tiledOffsetFlipped;

constexpr int kBottomWidth = 320;
constexpr int kBottomHeight = 240;
constexpr int kSheetEdge = 256;

constexpr u8 kKindSkin = 0;
constexpr u8 kKindPack = 1;
constexpr u8 kKindWorld = 2;

// Rows either side of the cursor kept ready. Skins are 8 KB of sheet each and
// cheap; packs are 256 KB of linear memory and a zip inflate each; worlds are
// up to 576 chunk reads each. A New 3DS with room to spare gets one more of
// the two expensive ones.
constexpr int kSkinRadius = 8;
constexpr int kPackRadius = 2;
constexpr int kWorldRadius = 2;
// Read-ahead is not what more memory buys. A world is 576 chunk reads off the
// card, and the card, not the RAM, is what a scroll outruns -- so the radius
// stays where it is and the room goes into keeping what has already been read:
// `kWorldSlots` meshes and `gridLimit()` grids. Coming back to a world the
// cursor has passed over is the common motion, and it is the one this makes
// free.
constexpr u32 kRoomyLinearBytes = 24u << 20;
// Linear memory a world's tile buffers leave for everything else; below it a
// world outside the preload window gives its buffers back first.
constexpr u32 kLinearReserveBytes = 4u << 20;

// What one frame may copy into linear memory from finished work. An atlas is a
// quarter of a megabyte, a tile buffer usually under a tenth of one.
constexpr usize kUploadBytesPerFrame = 600u * 1024u;

// The most quads one draw can index: a whole diorama tile.
constexpr int kMaxQuads = preview::kDioramaMaxTileQuads;

// The row of skins: how far apart the characters stand, how big they are, and
// how many either side of the selected one are drawn.
constexpr float kSkinSpacing = 64.0f;
constexpr float kSkinPixelsPerBlock = 44.0f;
constexpr float kSkinFeetY = 170.0f;
constexpr int kSkinsDrawn = 3;

// The selected character starts from the flat pose and walks into its stride
// over this long, which is what makes the swap from the flat picture seamless.
constexpr float kStrideSeconds = 0.35f;
constexpr float kSpinRadiansPerSecond = 1.0f;

// **How far a walking player's legs swing, derived rather than picked.** On
// the ground a1.1.2 adds `kGroundAcceleration` to the velocity and multiplies
// by `kGroundFrictionBase` every tick, so the step a tick settles at
// a / (1 - f); `limbYaw` chases four times the step, capped at one (mob.cpp,
// out of the jar), and `limbSwing` gains `limbYaw` a tick at twenty ticks a
// second.
constexpr float kWalkLimbYaw = 4.0f * entity::kGroundAcceleration
                               / (1.0f - entity::kGroundFrictionBase);
constexpr float kTicksPerSecond = 20.0f;

constexpr float kDioramaRadiansPerSecond = 0.35f;
constexpr float kDioramaPitch = 0.5236f;  // 30 degrees
// Close enough that the table's top fills the width when it is turned corner
// on, with its top high on the screen and the cube's sides running off the
// bottom edge: the ground is what is worth looking at, not the slab.
constexpr float kDioramaPixelsPerBlock = 0.55f;
constexpr float kDioramaTopCentreY = 100.0f;

constexpr float kPackYaw = 0.6f;
constexpr float kPackPitch = 0.5236f;
constexpr float kPackPixelsPerBlock = 22.0f;

constexpr u32 rgbaWord(u8 r, u8 g, u8 b, u8 a)
{
    return (u32(r) << 24) | (u32(g) << 16) | (u32(b) << 8) | a;
}

// RGBA bytes into a rectangle of a linear-memory texture, in the tiled and
// flipped layout every texture here is sampled in (see core/texture/tiled.hpp).
void writeTexels(C3D_Tex* tex, const u8* rgba, int x0, int y0, int width, int height)
{
    u32* data = static_cast<u32*>(tex->data);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const usize i = (usize(y) * usize(width) + usize(x)) * 4;
            data[tiledOffsetFlipped(u32(x0 + x), u32(y0 + y), tex->width, tex->height)] =
                rgbaWord(rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]);
        }
    }
    GSPGPU_FlushDataCache(tex->data, u32(tex->width) * tex->height * 4);
}

bool makeTexture(C3D_Tex* tex, int edge)
{
    if (!C3D_TexInit(tex, u16(edge), u16(edge), GPU_RGBA8)) {
        return false;
    }
    std::memset(tex->data, 0, usize(edge) * usize(edge) * 4);
    C3D_TexSetFilter(tex, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    return true;
}

void freeTile(void** data, usize* capacity)
{
    if (*data != nullptr) {
        linearFree(*data);
        *data = nullptr;
    }
    *capacity = 0;
}

// Vertices into a tile buffer, growing it when they do not fit.
bool fillTile(void** data, usize* capacity, const u8* bytes, usize size)
{
    if (size > *capacity) {
        freeTile(data, capacity);
        *data = linearAlloc(size);
        if (*data == nullptr) {
            return false;
        }
        *capacity = size;
    }
    if (size > 0) {
        std::memcpy(*data, bytes, size);
        GSPGPU_FlushDataCache(*data, u32(size));
    }
    return true;
}

// What both of the diorama read's callbacks travel on: `ask` is whether the
// cursor still wants this world, `MenuPreview::postReadyTile` is the tile
// hand-over, and one context serves both.
struct TileReady {
    preview::PreviewWorker* worker;
    const preview::PreviewJob* job;
    MenuPreview* self;
    const preview::DioramaGrid* grid;
    const preview::DioramaColours* colours;
    u32 revision;

    bool stillWanted() const { return worker->stillWanted(*job); }

    static bool ask(void* context)
    {
        return static_cast<const TileReady*>(context)->stillWanted();
    }
};

}  // namespace

MenuPreview::~MenuPreview()
{
    shutdown();
}

bool MenuPreview::init(bool isNew3DS)
{
    shutdown();
    isNew3DS_ = isNew3DS;

    const void* shbin = detail_shader_shbin;
    dvlb_ = DVLB_ParseFile(reinterpret_cast<u32*>(const_cast<void*>(shbin)),
                           u32(detail_shader_shbin_size));
    if (dvlb_ == nullptr) {
        return false;
    }
    shaderProgramInit(&program_);
    shaderProgramSetVsh(&program_, &dvlb_->DVLE[0]);
    uMvp_ = shaderInstanceGetUniformLocation(program_.vertexShader, "mvp");
    uFog_ = shaderInstanceGetUniformLocation(program_.vertexShader, "fogparam");

    // mesh::DetailVertex, 16 bytes -- the loaders Renderer::buildPipeline
    // installs for the same shader, in the same order.
    AttrInfo_Init(&attrs_);
    AttrInfo_AddLoader(&attrs_, 0, GPU_SHORT, 4);
    AttrInfo_AddLoader(&attrs_, 1, GPU_SHORT, 2);
    AttrInfo_AddLoader(&attrs_, 2, GPU_UNSIGNED_BYTE, 4);

    ready_ = true;  // from here on shutdown() has something to free

    indices_ = static_cast<u16*>(linearAlloc(usize(kMaxQuads) * 6 * sizeof(u16)));
    playerVertices_ = static_cast<mesh::DetailVertex*>(
        linearAlloc(usize(2 * kSkinsDrawn + 1) * render::kPlayerPreviewVertices
                    * sizeof(mesh::DetailVertex)));
    if (indices_ == nullptr || playerVertices_ == nullptr) {
        shutdown();
        return false;
    }
    for (int q = 0; q < kMaxQuads; ++q) {
        const u16 base = u16(q * 4);
        u16* t = &indices_[q * 6];
        t[0] = base;
        t[1] = u16(base + 1);
        t[2] = u16(base + 2);
        t[3] = base;
        t[4] = u16(base + 2);
        t[5] = u16(base + 3);
    }
    GSPGPU_FlushDataCache(indices_, u32(usize(kMaxQuads) * 6 * sizeof(u16)));

    // Noon: the diorama is never at night, so this is written once.
    if (!lightmap_.init()) {
        shutdown();
        return false;
    }
    lightmapReady_ = true;
    lightmap_.setSkyDarken(0);

    whiteReady_ = makeTexture(&white_, 8);
    skinSheetReady_ = makeTexture(&skinSheet_, kSheetEdge);
    if (!whiteReady_ || !skinSheetReady_) {
        shutdown();
        return false;
    }
    std::memset(white_.data, 0xFF, 8 * 8 * 4);
    GSPGPU_FlushDataCache(white_.data, 8 * 8 * 4);

    // The pack scene, once for the life of the menu.
    mesh::MeshBuilder builder;
    preview::buildPackScene(&builder);
    sceneCount_ = u32(builder.detailVertexCount());
    usize sceneBytes = sceneCount_ * sizeof(mesh::DetailVertex);
    usize sceneCapacity = 0;
    if (!fillTile(&sceneVertices_, &sceneCapacity,
                  reinterpret_cast<const u8*>(builder.detailVertices()), sceneBytes)) {
        shutdown();
        return false;
    }

    // The table with no world on it: the slab's top and its four sides. Its
    // tiles do not depend on the pack's colours, only on the block table.
    preview::DioramaColours tiles;
    texture::AtlasImage none;
    preview::buildDioramaColours(none, &tiles);
    preview::DioramaGrid empty;
    empty.reset(0, 0);
    for (int t = 0; t < preview::kDioramaTiles * preview::kDioramaTiles; ++t) {
        preview::DioramaMesh mesh;
        preview::buildDioramaTile(empty, t % preview::kDioramaTiles, t / preview::kDioramaTiles,
                                  tiles, &mesh);
        TileBuffer& buffer = emptyTiles_[t];
        buffer.textured = u32(mesh.textured.size());
        for (int f = 0; f < mesh::kFaceCount; ++f) {
            buffer.run[f] = mesh.run[f];
        }
        if (!fillTile(&buffer.data, &buffer.capacity,
                      reinterpret_cast<const u8*>(mesh.textured.data()),
                      mesh.textured.size() * sizeof(mesh::DetailVertex))) {
            shutdown();
            return false;
        }
        buffer.ready = true;
    }
    preview::DioramaMesh sides;
    preview::buildDioramaSides(tiles, &sides);
    sides_.textured = u32(sides.textured.size());
    if (!fillTile(&sides_.data, &sides_.capacity,
                  reinterpret_cast<const u8*>(sides.textured.data()),
                  sides.textured.size() * sizeof(mesh::DetailVertex))) {
        shutdown();
        return false;
    }
    sides_.ready = true;

    // Core 2 on a New 3DS, the bottom priority on an Old one: either way it
    // runs in what the menu leaves while it waits on VBlank.
    worker_.start(&MenuPreview::handleJob, this, WorkerRole::Generation);
    return true;
}

void MenuPreview::shutdown()
{
    // The worker first: it reads `colours_` and writes nothing the GPU sees,
    // but it must not be mid-job while the object around it goes away.
    worker_.stop();
    havePending_ = false;
    grids_.clear();
    gridOrder_.clear();

    if (target_ != nullptr) {
        setScreen(PreviewScreen::None);
    }
    if (!ready_) {
        return;
    }

    for (PackTexture& pack : packTextures_) {
        if (pack.allocated) {
            C3D_TexDelete(&pack.tex);
        }
        pack = PackTexture{};
    }
    for (WorldMeshes& slot : worldSlots_) {
        for (TileBuffer& tile : slot.tiles) {
            freeTile(&tile.data, &tile.capacity);
        }
        slot = WorldMeshes{};
    }
    for (TileBuffer& tile : emptyTiles_) {
        freeTile(&tile.data, &tile.capacity);
        tile = TileBuffer{};
    }
    freeTile(&sides_.data, &sides_.capacity);
    sides_ = TileBuffer{};
    usize sceneCapacity = 1;
    freeTile(&sceneVertices_, &sceneCapacity);
    sceneCount_ = 0;

    if (atlasReady_) {
        C3D_TexDelete(&atlas_);
        atlasReady_ = false;
    }
    if (skinSheetReady_) {
        C3D_TexDelete(&skinSheet_);
        skinSheetReady_ = false;
    }
    if (whiteReady_) {
        C3D_TexDelete(&white_);
        whiteReady_ = false;
    }
    if (lightmapReady_) {
        lightmap_.shutdown();
        lightmapReady_ = false;
    }
    for (SkinPage& page : skinPages_) {
        page = SkinPage{};
    }
    if (playerVertices_ != nullptr) {
        linearFree(playerVertices_);
        playerVertices_ = nullptr;
    }
    if (indices_ != nullptr) {
        linearFree(indices_);
        indices_ = nullptr;
    }
    // The program is the caller's to park before C2D_Fini -- see
    // Menu::shutdown -- and it is only freed after that park has happened,
    // because citro3d dereferences the last program bound.
    shaderProgramFree(&program_);
    if (dvlb_ != nullptr) {
        DVLB_Free(dvlb_);
        dvlb_ = nullptr;
    }
    havePack_ = false;
    packRevision_ = 0;
    ready_ = false;
}

void MenuPreview::setScreen(PreviewScreen screen)
{
    if (!ready_) {
        screen = PreviewScreen::None;
    }
    if (screen == screen_) {
        return;
    }
    const bool wasOn = screen_ != PreviewScreen::None;
    screen_ = screen;

    if (screen == PreviewScreen::None) {
        if (wasOn && target_ != nullptr) {
            C3D_RenderTargetSetOutput(nullptr, GFX_BOTTOM, GFX_LEFT, 0);
            C3D_RenderTargetDelete(target_);
            target_ = nullptr;
            // The console's framebuffer, cached at consoleInit, is the one the
            // screen shows again -- and whatever the target last transferred
            // into it is cleared by the re-init.
            consoleInit(GFX_BOTTOM, nullptr);
        }
    } else if (target_ == nullptr) {
        target_ = C3D_RenderTargetCreate(kBottomHeight, kBottomWidth, GPU_RB_RGBA8,
                                         GPU_RB_DEPTH16);
        if (target_ == nullptr) {
            screen_ = PreviewScreen::None;
        } else {
            // **The output format has to be the framebuffer's, and citro3d
            // does not look it up.** It hands these flags to GX_DisplayTransfer
            // as they are, and an OUT_FORMAT of 0 is RGBA8 -- four bytes a
            // pixel into the console's two-byte RGB565 framebuffer, which
            // garbles the picture and runs off the end of the buffer into
            // whatever linear memory follows it, textures included. citro2d's
            // own screen target passes OUT_FORMAT(RGB8) for the top screen for
            // the same reason. The GSP and GX format enums share numbering.
            C3D_RenderTargetSetOutput(
                target_, GFX_BOTTOM, GFX_LEFT,
                GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO)
                    | GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8)
                    | GX_TRANSFER_OUT_FORMAT(gfxGetScreenFormat(GFX_BOTTOM)));
        }
    }

    // A screen that is not up wants nothing read for it.
    if (screen_ != PreviewScreen::Skins) {
        worker_.setWanted(kKindSkin, {});
    }
    if (screen_ != PreviewScreen::Packs) {
        worker_.setWanted(kKindPack, {});
    }
    if (screen_ != PreviewScreen::Worlds) {
        worker_.setWanted(kKindWorld, {});
    }
    wantedDirty_ = true;
}

void MenuPreview::setActivePack(const texture::AtlasImage& atlas, u32 revision)
{
    if (!ready_ || (havePack_ && revision == packRevision_)) {
        return;
    }
    havePack_ = true;
    packRevision_ = revision;

    if (atlas.rgba.size() == texture::kAtlasBytes) {
        if (!atlasReady_) {
            atlasReady_ = makeTexture(&atlas_, kSheetEdge);
        }
        if (atlasReady_) {
            writeTexels(&atlas_, atlas.rgba.data(), 0, 0, texture::kAtlasEdge,
                        texture::kAtlasEdge);
        }
    }

    // The Default skin is the active pack's own page, which the atlas already
    // holds -- no file and no job.
    defaultSkin_.assign(usize(texture::kSkinPageWidth) * texture::kSkinPageHeight * 4, 0);
    if (atlas.entityRgba.size() == texture::kEntitySheetBytes) {
        int ox = 0;
        int oy = 0;
        texture::skinOrigin(texture::EntitySkin::Player, &ox, &oy);
        for (int y = 0; y < texture::kSkinPageHeight; ++y) {
            std::memcpy(&defaultSkin_[usize(y) * texture::kSkinPageWidth * 4],
                        &atlas.entityRgba[(usize(oy + y) * texture::kEntitySheetWidth
                                           + usize(ox)) * 4],
                        usize(texture::kSkinPageWidth) * 4);
        }
    }
    writeDefaultSkin();

    auto colours = std::make_shared<preview::DioramaColours>();
    preview::buildDioramaColours(atlas, colours.get());
    {
        std::lock_guard<std::mutex> guard(coloursLock_);
        colours_ = std::move(colours);
        ++coloursRevision_;
    }
    wantedDirty_ = true;
}

void MenuPreview::writeDefaultSkin()
{
    if (!skinSheetReady_) {
        return;
    }
    for (usize row = 0; row < skinRows_.size(); ++row) {
        if (!skinRows_[row].isDefault) {
            continue;
        }
        const int page = skinPageFor(skinRows_[row].key, true);
        if (page >= 0 && defaultSkin_.size() == usize(64 * 32 * 4)) {
            writeTexels(&skinSheet_, defaultSkin_.data(), (page % 4) * 64, (page / 4) * 32,
                        64, 32);
            skinPages_[page].ready = true;
        }
    }
}

void MenuPreview::setSkinList(const std::vector<texture::SkinEntry>& skins)
{
    skinRows_.clear();
    skinRows_.reserve(skins.size());
    for (const texture::SkinEntry& skin : skins) {
        SkinRow row;
        row.key = "skin:" + skin.key;
        row.path = skin.path;
        row.fromPack = skin.source == texture::SkinSource::Pack;
        row.isDefault = skin.source == texture::SkinSource::Default;
        skinRows_.push_back(std::move(row));
    }
    writeDefaultSkin();
    wantedDirty_ = true;
}

void MenuPreview::setSkinCursor(int row)
{
    if (row != skinCursor_) {
        skinCursor_ = row;
        walkSeconds_ = 0.0f;
        limbSwing_ = 0.0f;
        wantedDirty_ = true;
    }
}

void MenuPreview::setPackList(const std::vector<texture::PackEntry>& packs)
{
    packRows_.clear();
    packRows_.reserve(packs.size() + 1);
    packRows_.push_back(PackRow{});  // "+ Extract from a jar..." shows the active pack
    for (const texture::PackEntry& pack : packs) {
        PackRow row;
        row.key = pack.builtIn ? std::string("pack:") : "pack:" + pack.path;
        row.path = pack.builtIn ? std::string() : pack.path;
        packRows_.push_back(std::move(row));
    }
    wantedDirty_ = true;
}

void MenuPreview::setPackCursor(int row)
{
    if (row != packCursor_) {
        packCursor_ = row;
        wantedDirty_ = true;
    }
}

void MenuPreview::setWorldList(const std::vector<world::WorldEntry>& worlds)
{
    worldRows_.clear();
    worldRows_.reserve(worlds.size() + 2);
    // The two pinned rows have no world behind them, so they have no table on
    // them either: an empty key is what every walk below skips.
    worldRows_.push_back(std::string());  // "+ Create New World"
    worldRows_.push_back(std::string());  // "+ Import World"
    for (const world::WorldEntry& entry : worlds) {
        worldRows_.push_back(entry.path);
    }
    wantedDirty_ = true;
}

void MenuPreview::setWorldCursor(int row)
{
    if (row != worldCursor_) {
        worldCursor_ = row;
        wantedDirty_ = true;
    }
}

void MenuPreview::quiesce(const std::string& worldPath)
{
    worker_.quiesce(worldPath);
}

void MenuPreview::forgetWorld(const std::string& worldPath)
{
    // **Quiesce first, and that is what makes touching `grids_` legal here.**
    // It is the worker's map everywhere else; after quiesce there is no job
    // about this world running or queued, so the main thread owns it for as
    // long as it takes to erase one entry -- the same window `quiesce` already
    // opens for a delete or a conversion.
    worker_.quiesce(worldPath);

    grids_.erase(worldPath);
    gridOrder_.erase(std::remove(gridOrder_.begin(), gridOrder_.end(), worldPath),
                     gridOrder_.end());

    const int slot = worldSlotFor(worldPath, false);
    if (slot >= 0) {
        for (TileBuffer& tile : worldSlots_[slot].tiles) {
            freeTile(&tile.data, &tile.capacity);
        }
        worldSlots_[slot] = WorldMeshes{};
    }

    // Nothing is read back until the next pass asks for it, which is what
    // makes this cheap to call on every step of the d-pad: the tiles stream in
    // again from the new corner while the screen keeps drawing.
    wantedDirty_ = true;
}

int MenuPreview::preloadRadius(int base) const
{
    return isNew3DS_ && linearSpaceFree() > kRoomyLinearBytes ? base + 1 : base;
}

int MenuPreview::skinPageFor(const std::string& key, bool claim)
{
    int free = -1;
    for (int i = 0; i < kSkinPages; ++i) {
        if (skinPages_[i].key == key) {
            return i;
        }
        if (free < 0 && skinPages_[i].key.empty()) {
            free = i;
        }
    }
    if (!claim) {
        return -1;
    }
    if (free < 0) {
        // The page whose row is furthest from the cursor gives way.
        int rows[kSkinPages];
        for (int i = 0; i < kSkinPages; ++i) {
            rows[i] = -1;
            for (usize r = 0; r < skinRows_.size(); ++r) {
                if (skinRows_[r].key == skinPages_[i].key) {
                    rows[i] = int(r);
                    break;
                }
            }
            if (rows[i] < 0) {
                free = i;  // a page for a row no longer listed
                break;
            }
        }
        if (free < 0) {
            free = preview::farthestOutside(rows, kSkinPages, skinCursor_, kSkinRadius);
        }
        if (free < 0) {
            return -1;
        }
    }
    skinPages_[free].key = key;
    skinPages_[free].ready = false;
    return free;
}

int MenuPreview::packTextureFor(const std::string& key, bool claim)
{
    int free = -1;
    for (int i = 0; i < kPackTextures; ++i) {
        if (!packTextures_[i].key.empty() && packTextures_[i].key == key) {
            // Claimed and still loading counts as resident only to a caller
            // that is about to fill it.
            return claim || packTextures_[i].ready || packTextures_[i].failed ? i : -1;
        }
        if (free < 0 && packTextures_[i].key.empty()) {
            free = i;
        }
    }
    if (!claim) {
        return -1;
    }
    if (free < 0) {
        int rows[kPackTextures];
        for (int i = 0; i < kPackTextures; ++i) {
            rows[i] = -1;
            for (usize r = 0; r < packRows_.size(); ++r) {
                if (packRows_[r].key == packTextures_[i].key) {
                    rows[i] = int(r);
                    break;
                }
            }
            if (rows[i] < 0) {
                free = i;
                break;
            }
        }
        if (free < 0) {
            free = preview::farthestOutside(rows, kPackTextures, packCursor_,
                                            preloadRadius(kPackRadius));
        }
        if (free < 0) {
            return -1;
        }
    }
    if (lastPackTexture_ == free) {
        lastPackTexture_ = -1;
    }
    packTextures_[free].key = key;
    packTextures_[free].ready = false;
    packTextures_[free].failed = false;
    return free;
}

bool MenuPreview::releaseFarWorld(int keep)
{
    const int radius = preloadRadius(kWorldRadius);
    for (int i = 0; i < kWorldSlots; ++i) {
        WorldMeshes& slot = worldSlots_[i];
        if (i == keep || slot.key.empty()) {
            continue;
        }
        int row = -1;
        for (usize r = 1; r < worldRows_.size(); ++r) {
            if (worldRows_[r] == slot.key) {
                row = int(r);
                break;
            }
        }
        if (row >= 0 && preview::inWindow(worldCursor_, row, radius)) {
            continue;
        }
        for (TileBuffer& tile : slot.tiles) {
            freeTile(&tile.data, &tile.capacity);
        }
        slot = WorldMeshes{};
        return true;
    }
    return false;
}

int MenuPreview::worldSlotFor(const std::string& key, bool claim)
{
    int free = -1;
    for (int i = 0; i < kWorldSlots; ++i) {
        if (!worldSlots_[i].key.empty() && worldSlots_[i].key == key) {
            return i;
        }
        if (free < 0 && worldSlots_[i].key.empty()) {
            free = i;
        }
    }
    if (!claim) {
        return -1;
    }
    if (free < 0) {
        int rows[kWorldSlots];
        for (int i = 0; i < kWorldSlots; ++i) {
            rows[i] = -1;
            for (usize r = 1; r < worldRows_.size(); ++r) {
                if (worldRows_[r] == worldSlots_[i].key) {
                    rows[i] = int(r);
                    break;
                }
            }
            if (rows[i] < 0) {
                free = i;
                break;
            }
        }
        if (free < 0) {
            free = preview::farthestOutside(rows, kWorldSlots, worldCursor_,
                                            preloadRadius(kWorldRadius));
        }
        if (free < 0) {
            return -1;
        }
    }
    WorldMeshes& slot = worldSlots_[free];
    slot.key = key;
    slot.complete = false;
    slot.revision = 0;
    for (TileBuffer& tile : slot.tiles) {
        tile.ready = false;
        tile.coloured = 0;
        tile.textured = 0;
    }
    return free;
}

void MenuPreview::refreshWanted()
{
    wantedDirty_ = false;
    int order[64];

    if (screen_ == PreviewScreen::Skins) {
        std::vector<preview::PreviewJob> jobs;
        const int n = preview::wantedOrder(skinCursor_, int(skinRows_.size()), kSkinRadius,
                                           order, 64);
        for (int i = 0; i < n; ++i) {
            const SkinRow& row = skinRows_[usize(order[i])];
            if (row.isDefault) {
                continue;
            }
            const int page = skinPageFor(row.key, false);
            if (page >= 0 && skinPages_[page].ready) {
                continue;
            }
            preview::PreviewJob job;
            job.index = order[i];
            job.key = row.key;
            job.path = row.path;
            jobs.push_back(std::move(job));
        }
        worker_.setWanted(kKindSkin, std::move(jobs));
    } else if (screen_ == PreviewScreen::Packs) {
        std::vector<preview::PreviewJob> jobs;
        const int n = preview::wantedOrder(packCursor_, int(packRows_.size()),
                                           preloadRadius(kPackRadius), order, 64);
        for (int i = 0; i < n; ++i) {
            if (order[i] == 0) {
                continue;
            }
            const PackRow& row = packRows_[usize(order[i])];
            if (packTextureFor(row.key, false) >= 0) {
                continue;
            }
            preview::PreviewJob job;
            job.index = order[i];
            job.key = row.key;
            job.path = row.path;
            jobs.push_back(std::move(job));
        }
        worker_.setWanted(kKindPack, std::move(jobs));
    } else if (screen_ == PreviewScreen::Worlds) {
        u32 revision = 0;
        {
            std::lock_guard<std::mutex> guard(coloursLock_);
            revision = coloursRevision_;
        }
        std::vector<preview::PreviewJob> jobs;
        const int n = preview::wantedOrder(worldCursor_, int(worldRows_.size()),
                                           preloadRadius(kWorldRadius), order, 64);
        for (int i = 0; i < n; ++i) {
            const std::string& path = worldRows_[usize(order[i])];
            if (path.empty()) {
                continue;  // a pinned row, which has no world to draw
            }
            const int slot = worldSlotFor(path, false);
            if (slot >= 0 && worldSlots_[slot].complete && worldSlots_[slot].revision == revision) {
                continue;
            }
            preview::PreviewJob job;
            job.index = order[i];
            job.key = path;
            job.path = path;
            jobs.push_back(std::move(job));
        }
        worker_.setWanted(kKindWorld, std::move(jobs));
    }
}

void MenuPreview::update(float seconds)
{
    if (!ready_ || screen_ == PreviewScreen::None) {
        return;
    }

    // Animation first: none of it depends on what has loaded.
    const float follow = std::min(1.0f, seconds * 12.0f);
    skinScroll_ += (float(skinCursor_) - skinScroll_) * follow;
    walkSeconds_ += seconds;
    const float stride = std::min(1.0f, walkSeconds_ / kStrideSeconds);
    limbSwing_ += kWalkLimbYaw * stride * kTicksPerSecond * seconds;
    dioramaAngle_ = std::fmod(dioramaAngle_ + kDioramaRadiansPerSecond * seconds,
                              6.2831853f);

    usize budget = kUploadBytesPerFrame;
    bool first = true;
    while (budget > 0 || first) {
        if (!havePending_) {
            if (!worker_.poll(&pending_)) {
                break;
            }
            havePending_ = true;
        }
        const usize cost = pending_.bytes.size();
        if (!first && cost > budget) {
            break;  // next frame
        }
        apply(pending_);
        havePending_ = false;
        pending_ = preview::PreviewResult{};
        budget = cost >= budget ? 0 : budget - cost;
        first = false;
    }

    if (wantedDirty_) {
        refreshWanted();
    }
}

usize MenuPreview::apply(preview::PreviewResult& result)
{
    switch (result.kind) {
    case kKindSkin: {
        const int page = skinPageFor(result.key, true);
        if (page < 0) {
            break;
        }
        if (result.ok && result.bytes.size() == usize(64 * 32 * 4)) {
            writeTexels(&skinSheet_, result.bytes.data(), (page % 4) * 64, (page / 4) * 32, 64,
                        32);
        }
        // A skin that would not decode keeps its page, blank, so it is not
        // asked for again on every cursor move.
        skinPages_[page].ready = true;
        wantedDirty_ = true;
        break;
    }
    case kKindPack: {
        const int index = packTextureFor(result.key, true);
        if (index < 0) {
            break;
        }
        PackTexture& pack = packTextures_[index];
        if (result.ok && result.bytes.size() == texture::kAtlasBytes) {
            if (!pack.allocated) {
                pack.allocated = makeTexture(&pack.tex, kSheetEdge);
            }
            if (pack.allocated) {
                writeTexels(&pack.tex, result.bytes.data(), 0, 0, texture::kAtlasEdge,
                            texture::kAtlasEdge);
                pack.ready = true;
            }
        } else {
            pack.failed = true;
        }
        wantedDirty_ = true;
        break;
    }
    case kKindWorld: {
        u32 revision = 0;
        {
            std::lock_guard<std::mutex> guard(coloursLock_);
            revision = coloursRevision_;
        }
        if (u32(result.value[3]) != revision) {
            wantedDirty_ = true;  // painted with a pack that is no longer live
            break;
        }
        const int index = worldSlotFor(result.key, true);
        if (index < 0) {
            break;
        }
        WorldMeshes& slot = worldSlots_[index];
        if (slot.revision != revision) {
            slot.revision = revision;
            slot.complete = false;
        }
        if (result.value[0] < 0) {
            slot.complete = true;
            wantedDirty_ = true;
            break;
        }
        const int tile = result.value[0];
        if (tile >= preview::kDioramaTiles * preview::kDioramaTiles) {
            break;
        }
        TileBuffer& buffer = slot.tiles[tile];
        while (result.bytes.size() > buffer.capacity
               && linearSpaceFree() < result.bytes.size() + kLinearReserveBytes
               && releaseFarWorld(index)) {
        }
        const usize expected =
            usize(result.value[1] + result.value[2]) * sizeof(mesh::DetailVertex);
        if (expected != result.bytes.size()
            || !fillTile(&buffer.data, &buffer.capacity, result.bytes.data(),
                         result.bytes.size())) {
            buffer.ready = false;
            break;
        }
        buffer.coloured = u32(result.value[1]);
        buffer.textured = u32(result.value[2]);
        for (int f = 0; f < mesh::kFaceCount; ++f) {
            buffer.run[f] = u32(result.value[4 + f]);
        }
        buffer.ready = true;
        break;
    }
    default:
        break;
    }
    return result.bytes.size();
}

// ---------------------------------------------------------------------------
// The worker's half
// ---------------------------------------------------------------------------

void MenuPreview::handleJob(void* context, preview::PreviewWorker& worker,
                            const preview::PreviewJob& job)
{
    MenuPreview* self = static_cast<MenuPreview*>(context);
    switch (job.kind) {
    case kKindSkin:
        self->runSkinJob(worker, job);
        break;
    case kKindPack:
        self->runPackJob(worker, job);
        break;
    case kKindWorld:
        self->runWorldJob(worker, job);
        break;
    default:
        break;
    }
}

void MenuPreview::runSkinJob(preview::PreviewWorker& worker, const preview::PreviewJob& job)
{
    preview::PreviewResult result;
    result.kind = job.kind;
    result.index = job.index;
    result.key = job.key;
    const bool fromPack = job.key.compare(0, 10, "skin:pack:") == 0;
    result.ok = texture::decodePlayerSkinPage(workerFs_, job.path, fromPack, &result.bytes);
    worker.post(std::move(result));
}

void MenuPreview::runPackJob(preview::PreviewWorker& worker, const preview::PreviewJob& job)
{
    texture::AtlasImage image;
    const bool ok = texture::buildTerrainAtlas(workerFs_, job.path, &image) == texture::PackError::Ok;
    preview::PreviewResult result;
    result.kind = job.kind;
    result.index = job.index;
    result.key = job.key;
    result.ok = ok && image.rgba.size() == texture::kAtlasBytes;
    if (result.ok) {
        result.bytes = std::move(image.rgba);
    }
    worker.post(std::move(result));
}

void MenuPreview::postReadyTile(void* context, int tileX, int tileZ)
{
    TileReady& ready = *static_cast<TileReady*>(context);
    // The tile, and any neighbour that has ground of its own: its walls along
    // the shared edge were standing against a table that is now terrain. A
    // neighbour still unread is bare table and has no mesh yet.
    constexpr int kSteps[5][2] = {{0, 0}, {-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    for (const auto& step : kSteps) {
        const int nx = tileX + step[0];
        const int nz = tileZ + step[1];
        if (nx < 0 || nz < 0 || nx >= preview::kDioramaTiles || nz >= preview::kDioramaTiles
            || !ready.grid->tileRead(nx, nz)) {
            continue;
        }
        ready.self->postTile(*ready.worker, *ready.job, *ready.grid,
                             nz * preview::kDioramaTiles + nx, *ready.colours, ready.revision);
    }
}

void MenuPreview::postTile(preview::PreviewWorker& worker, const preview::PreviewJob& job,
                           const preview::DioramaGrid& grid, int tile,
                           const preview::DioramaColours& colours, u32 revision)
{
    preview::DioramaMesh mesh;
    preview::buildDioramaTile(grid, tile % preview::kDioramaTiles, tile / preview::kDioramaTiles,
                              colours, &mesh);
    preview::PreviewResult result;
    result.kind = job.kind;
    result.index = job.index;
    result.key = job.key;
    result.ok = true;
    result.value[0] = tile;
    result.value[1] = i32(mesh.coloured.size());
    result.value[2] = i32(mesh.textured.size());
    result.value[3] = i32(revision);
    for (int f = 0; f < mesh::kFaceCount; ++f) {
        result.value[4 + f] = i32(mesh.run[f]);
    }
    const usize colouredBytes = mesh.coloured.size() * sizeof(mesh::DetailVertex);
    const usize texturedBytes = mesh.textured.size() * sizeof(mesh::DetailVertex);
    result.bytes.resize(colouredBytes + texturedBytes);
    if (colouredBytes > 0) {
        std::memcpy(result.bytes.data(), mesh.coloured.data(), colouredBytes);
    }
    if (texturedBytes > 0) {
        std::memcpy(result.bytes.data() + colouredBytes, mesh.textured.data(), texturedBytes);
    }
    worker.post(std::move(result));
}

usize MenuPreview::gridLimit() const
{
    // **The menu is the one time the heap is empty**, so how many grids fit is
    // measured rather than fixed. No world is loaded, and the renderer's VBO
    // pool is a cap and not a reservation -- it holds nothing until sections
    // mesh, and it is sized in `Renderer::init` and again after
    // `Menu::shutdown` has given all of this back, so nothing the menu keeps
    // comes out of the render distance.
    //
    // `heapFreeBytes` cannot see fragmentation, so like every other reader it
    // is halved before it is spent. The floor is what the previous fixed limit
    // was, for a console that reports almost nothing free; the ceiling is well
    // past any plausible world list, and is there so a bad reading cannot turn
    // into an unbounded cache.
    constexpr usize kHeapReserve = 6u << 20;
    const usize floor = isNew3DS_ ? usize(5) : usize(3);
    const usize spare = heapFreeBytes() / 2;
    const usize room =
        spare > kHeapReserve ? (spare - kHeapReserve) / preview::kDioramaGridBytes : usize(0);
    // What is held already is heap that has been handed out, so the two add.
    usize limit = gridOrder_.size() + room;
    if (limit < floor) {
        limit = floor;
    }
    constexpr usize kCeiling = 24;
    return limit > kCeiling ? kCeiling : limit;
}

void MenuPreview::keepGrid(const std::string& key)
{
    // A grid is 1.8 MB -- 96x96x128 cells, a bit and a light nibble each -- so
    // far fewer are kept than there are tile-mesh slots. Dropping one only
    // costs the reads again if the cursor comes back to it, which is the whole
    // reason to keep as many as the heap will stand.
    const usize limit = gridLimit();
    gridOrder_.erase(std::remove(gridOrder_.begin(), gridOrder_.end(), key), gridOrder_.end());
    gridOrder_.push_back(key);
    while (gridOrder_.size() > limit) {
        grids_.erase(gridOrder_.front());
        gridOrder_.erase(gridOrder_.begin());
    }
}

void MenuPreview::runWorldJob(preview::PreviewWorker& worker, const preview::PreviewJob& job)
{
    std::shared_ptr<const preview::DioramaColours> colours;
    u32 revision = 0;
    {
        std::lock_guard<std::mutex> guard(coloursLock_);
        colours = colours_;
        revision = coloursRevision_;
    }
    if (colours == nullptr) {
        return;
    }

    constexpr int kTileCount = preview::kDioramaTiles * preview::kDioramaTiles;
    world::WorldPeek peek(workerFs_);
    keepGrid(job.key);
    std::unique_ptr<preview::DioramaGrid>& grid = grids_[job.key];
    if (grid == nullptr) {
        grid.reset(new preview::DioramaGrid());
        if (peek.open(job.path)) {
            // **Where this world's table stands**, out of its own
            // `3dalpha.ini`. Read here rather than handed in with the job for
            // two reasons: it is a card read and this is the thread that is
            // allowed to make one, and it is read exactly when a grid is
            // built, which is the only moment the answer is used. World
            // Settings' Move Panorama writes the file and then calls
            // `forgetWorld`, which is what makes the next build see it.
            settings::WorldSettings worldSettings;
            settings::loadWorldSettings(workerFs_, job.path, &worldSettings);

            // **Anchored on a place the world actually has**, which by
            // default is spawn: that is where a1.1.2 put the player and
            // therefore where the generated chunks are. The table used to stand
            // on block 0, 0 whatever the world said, and on a real save that is
            // usually somewhere nobody has been -- four measured a1.1.2 worlds
            // spawn at 341, 255 / 98, 314 / -193, 151 / 35, -511, and held 148,
            // 188, 158 and 185 of the table's 576 chunks. Anchored on spawn the
            // same four hold 576, 576, 397 and 572.
            //
            // Which anchor is the world's own setting, and the tiles step away
            // from it; see settings::PanoramaAnchor.
            i32 anchorX = 0;
            i32 anchorZ = 0;
            preview::dioramaAnchorBlock(worldSettings.panoramaAnchor,
                                        preview::dioramaAnchorsOf(peek.level()), &anchorX,
                                        &anchorZ);
            i32 x = 0;
            i32 z = 0;
            preview::dioramaOrigin(&x, &z, worldSettings.panoramaTileX,
                                   worldSettings.panoramaTileZ, anchorX, anchorZ);
            grid->reset(x, z);
        } else {
            // Not a world this can read: a whole table and nothing on it.
            grid->reset(0, 0);
            std::fill(grid->chunks.begin(), grid->chunks.end(), preview::DioramaChunk::Absent);
        }
    }

    // What is known already, straight away -- which for a world seen before is
    // everything. A grid with nothing in it yet is the bare table, which the
    // menu already draws for a slot that has no tiles, so it is not worth a
    // mesh.
    bool anyRead = false;
    for (int t = 0; t < kTileCount; ++t) {
        if (grid->tileRead(t % preview::kDioramaTiles, t / preview::kDioramaTiles)) {
            anyRead = true;
        }
    }
    if (anyRead) {
        for (int t = 0; t < kTileCount; ++t) {
            postTile(worker, job, *grid, t, *colours, revision);
        }
    }

    int tileXs[kTileCount];
    int tileZs[kTileCount];
    preview::dioramaTileOrder(tileXs, tileZs);

    // **Two reads for nine tiles, not nine.** The centre alone first, so the
    // table has something on it as soon as the card can manage it, then the
    // other eight as one batch: what a batch saves is card operations, and
    // eight tiles together coalesce into far fewer reads than eight tiles
    // apart. Measured over a real packed world's 576 chunks -- 576 reads a
    // chunk at a time, 164 a tile at a time, 86 this way, with the centre
    // still landing after 24 either way.
    //
    // **The meshing does not wait for the batch.** `readDioramaTiles` hands
    // each tile over the moment its last chunk lands, so the table fills in
    // tile by tile exactly as it did when each tile was its own read -- the
    // batching is underneath, where the operations are, and not in what is on
    // the screen.
    TileReady ready{&worker, &job, this, grid.get(), colours.get(), revision};
    constexpr int kPasses[2][2] = {{0, 1}, {1, kTileCount - 1}};
    for (const auto& pass : kPasses) {
        bool owed = false;
        for (int i = pass[0]; i < pass[0] + pass[1]; ++i) {
            owed = owed || !grid->tileRead(tileXs[i], tileZs[i]);
        }
        if (!owed) {
            continue;
        }
        if (!peek.isOpen() && !peek.open(job.path)) {
            std::fill(grid->chunks.begin(), grid->chunks.end(), preview::DioramaChunk::Absent);
            break;
        }
        if (!preview::readDioramaTiles(peek, &tileXs[pass[0]], &tileZs[pass[0]], pass[1],
                                       grid.get(), &TileReady::ask,
                                       &MenuPreview::postReadyTile, &ready)) {
            return;
        }
    }

    // The whole table is read: now the air is worth following sideways, which
    // is what puts the underside on an overhang and the inside on a cave
    // mouth. Every tile is meshed once more against it.
    preview::openDioramaAir(grid.get(), preview::DioramaOpen::Full);
    for (int t = 0; t < kTileCount; ++t) {
        if (!worker.stillWanted(job)) {
            return;
        }
        postTile(worker, job, *grid, t, *colours, revision);
    }

    preview::PreviewResult done;
    done.kind = job.kind;
    done.index = job.index;
    done.key = job.key;
    done.ok = true;
    done.value[0] = -1;
    done.value[3] = i32(revision);
    worker.post(std::move(done));
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void MenuPreview::bindPipeline()
{
    C3D_BindProgram(&program_);
    C3D_SetAttrInfo(&attrs_);

    C3D_CullFace(GPU_CULL_NONE);
    // The world renderer's convention: cleared to 0, nearer is larger.
    C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
    C3D_DepthMap(true, -1.0f, 0.0f);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    // Leaves and glass are cut-outs, as they are in the world.
    C3D_AlphaTest(true, GPU_GREATER, 0x7F);

    // The world renderer's first two stages, and no fog: texture times vertex
    // colour, then times the lightmap at the (block, sky) texel the shader
    // unpacked from the vertex's light byte. The diorama carries the world's
    // stored light in that byte; everything else here is 0xFF, which is the
    // lightmap's fully lit corner.
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_MODULATE);
    C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, GPU_TEXTURE0, GPU_TEXTURE0);
    C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
    C3D_TexEnv* env1 = C3D_GetTexEnv(1);
    C3D_TexEnvInit(env1);
    C3D_TexEnvSrc(env1, C3D_RGB, GPU_PREVIOUS, GPU_TEXTURE1, GPU_PREVIOUS);
    C3D_TexEnvFunc(env1, C3D_RGB, GPU_MODULATE);
    C3D_TexEnvSrc(env1, C3D_Alpha, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
    C3D_TexEnvFunc(env1, C3D_Alpha, GPU_REPLACE);
    for (int stage = 2; stage < 6; ++stage) {
        C3D_TexEnvInit(C3D_GetTexEnv(stage));
    }
    lightmap_.bind(1);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, uFog_, 0.0f, 0.0f, 0.0f, 0.0f);
}

void MenuPreview::setView(float yaw, float pitch, float pixelsPerBlock, float centreX,
                          float centreY, float originX, float originY, float originZ,
                          float depthRadius, float unitsPerBlock)
{
    // An orthographic camera turned by `yaw` about the vertical and tipped
    // down by `pitch`, in screen pixels: x right, y down, and a depth that is
    // larger nearer the viewer. The shader has already divided positions by
    // 1024, so a mesh in other units is scaled back here.
    const float k = 1024.0f / unitsPerBlock;
    const float cy = std::cos(yaw);
    const float sy = std::sin(yaw);
    const float cp = std::cos(pitch);
    const float sp = std::sin(pitch);
    const float s = pixelsPerBlock;
    const float r = depthRadius;

    const float ax[3] = {s * cy, 0.0f, s * sy};
    const float ay[3] = {-s * sp * sy, -s * cp, s * sp * cy};
    const float az[3] = {-cp * sy / r, sp / r, cp * cy / r};
    const auto dot = [&](const float* row) {
        return row[0] * originX + row[1] * originY + row[2] * originZ;
    };

    C3D_Mtx screen;
    Mtx_Zeros(&screen);
    screen.r[0].x = ax[0] * k;
    screen.r[0].y = ax[1] * k;
    screen.r[0].z = ax[2] * k;
    screen.r[0].w = centreX - dot(ax);
    screen.r[1].x = ay[0] * k;
    screen.r[1].y = ay[1] * k;
    screen.r[1].z = ay[2] * k;
    screen.r[1].w = centreY - dot(ay);
    screen.r[3].w = 1.0f;

    C3D_Mtx projection;
    Mtx_OrthoTilt(&projection, 0.0f, float(kBottomWidth), float(kBottomHeight), 0.0f, 1.0f,
                  -1.0f, true);
    C3D_Mtx mvp;
    Mtx_Multiply(&mvp, &projection, &screen);

    // Depth by hand, into the middle half of the range so the backdrop's own
    // depth of 0 is always behind: z = -(0.5 + 0.25 * d), d in [-1, 1].
    mvp.r[2].x = -0.25f * az[0] * k;
    mvp.r[2].y = -0.25f * az[1] * k;
    mvp.r[2].z = -0.25f * az[2] * k;
    mvp.r[2].w = -0.5f + 0.25f * dot(az);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uMvp_, &mvp);
}

void MenuPreview::drawVertices(const void* vertices, u32 count, C3D_Tex* texture)
{
    if (vertices == nullptr || count < 4) {
        return;
    }
    C3D_TexBind(0, texture);
    C3D_BufInfo buffers;
    BufInfo_Init(&buffers);
    BufInfo_Add(&buffers, vertices, sizeof(mesh::DetailVertex), 3, 0x210);
    C3D_SetBufInfo(&buffers);
    u32 quads = count / 4;
    if (quads > u32(kMaxQuads)) {
        quads = u32(kMaxQuads);
    }
    C3D_DrawElements(GPU_TRIANGLES, int(quads * 6), C3D_UNSIGNED_SHORT, indices_);
}

void MenuPreview::draw()
{
    if (!ready_ || target_ == nullptr) {
        return;
    }
    C3D_FrameDrawOn(target_);
    bindPipeline();
    switch (screen_) {
    case PreviewScreen::Skins:
        drawSkins();
        break;
    case PreviewScreen::Packs:
        drawPacks();
        break;
    case PreviewScreen::Worlds:
        drawWorlds();
        break;
    case PreviewScreen::None:
        break;
    }
}

void MenuPreview::drawSkins()
{
    if (skinRows_.empty()) {
        return;
    }
    const int first = std::max(0, skinCursor_ - kSkinsDrawn);
    const int last = std::min(int(skinRows_.size()) - 1, skinCursor_ + kSkinsDrawn);

    // Feet at y = 0, face towards the camera: RenderLiving's (-1, -1, 1) scale
    // turned half round, so the model's -Z front faces +Z.
    render::Placement place;
    place.x = 0.0;
    place.y = 24.0 * render::kModelUnit;
    place.z = 0.0;
    place.ax[0] = render::kModelUnit;
    place.ay[1] = -render::kModelUnit;
    place.az[2] = -render::kModelUnit;

    render::ModelPart parts[render::kBipedParts];
    u32 written = 0;
    for (int row = first; row <= last; ++row) {
        const int page = skinPageFor(skinRows_[usize(row)].key, false);
        if (page < 0 || !skinPages_[page].ready) {
            continue;
        }
        const bool selected = row == skinCursor_;
        render::bipedModel(parts);
        float yaw = 0.0f;
        if (selected) {
            const float stride = std::min(1.0f, walkSeconds_ / kStrideSeconds);
            render::posePlayer(parts, limbSwing_, kWalkLimbYaw * stride,
                               walkSeconds_ * kTicksPerSecond, 0.0f, 0.0f);
            yaw = walkSeconds_ * kSpinRadiansPerSecond;
        }
        mesh::DetailVertex* out = playerVertices_ + written;
        const int count = render::buildPlayerPreview(parts, place, (page % 4) * 64,
                                                     (page / 4) * 32, out,
                                                     render::kPlayerPreviewVertices);
        if (count == 0) {
            continue;
        }
        GSPGPU_FlushDataCache(out, u32(count) * sizeof(mesh::DetailVertex));
        const float x = float(kBottomWidth) * 0.5f + (float(row) - skinScroll_) * kSkinSpacing;
        setView(yaw, 0.0f, kSkinPixelsPerBlock, x, kSkinFeetY, 0.0f, 0.0f, 0.0f, 2.0f,
                float(mesh::kDetailUnitsPerBlock));
        drawVertices(out, u32(count), &skinSheet_);
        written += u32(count);
    }
}

void MenuPreview::drawPacks()
{
    C3D_Tex* texture = nullptr;
    if (packCursor_ > 0 && usize(packCursor_) < packRows_.size()) {
        const int index = packTextureFor(packRows_[usize(packCursor_)].key, false);
        if (index >= 0 && packTextures_[index].ready) {
            lastPackTexture_ = index;
        }
    } else if (atlasReady_) {
        lastPackTexture_ = -1;
    }
    if (lastPackTexture_ >= 0 && packTextures_[lastPackTexture_].ready) {
        texture = &packTextures_[lastPackTexture_].tex;
    } else if (atlasReady_) {
        texture = &atlas_;
    }
    if (texture == nullptr) {
        return;
    }
    const float middle = float(preview::kPackSceneEdge) * 0.5f;
    setView(kPackYaw, kPackPitch, kPackPixelsPerBlock, float(kBottomWidth) * 0.5f, 118.0f,
            middle, 2.5f, middle, 8.0f, float(mesh::kDetailUnitsPerBlock));
    drawVertices(sceneVertices_, sceneCount_, texture);
}

void MenuPreview::drawWorlds()
{
    if (!atlasReady_) {
        return;
    }
    // Turned about the middle of the table's top. Depth has to cover the
    // top's corners (272 blocks out) and the cube reaching 384 down.
    setView(dioramaAngle_, kDioramaPitch, kDioramaPixelsPerBlock, float(kBottomWidth) * 0.5f,
            kDioramaTopCentreY, 0.0f, float(preview::dioramaTableTop()), 0.0f, 450.0f,
            float(preview::kDioramaUnitsPerBlock));

    // Which of the six facing groups are worth submitting at this angle: two
    // of the four wall directions and the undersides always point away, which
    // is about a third of the table. Nothing culls them later -- the diorama
    // draws with `GPU_CULL_NONE` like the rest of the game -- so left in they
    // would be transformed and rasterised only to lose the depth test.
    bool showFace[mesh::kFaceCount];
    for (int f = 0; f < mesh::kFaceCount; ++f) {
        showFace[f] = preview::dioramaFaceVisible(f, dioramaAngle_, kDioramaPitch);
    }

    for (int i = 0; i < 4; ++i) {
        if (!showFace[preview::kDioramaSideOrder[i]]) {
            continue;
        }
        drawVertices(static_cast<const u8*>(sides_.data) + usize(i) * 4 * sizeof(mesh::DetailVertex),
                     4, &atlas_);
    }

    const WorldMeshes* slot = nullptr;
    if (worldCursor_ > 0 && usize(worldCursor_) < worldRows_.size()
        && !worldRows_[usize(worldCursor_)].empty()) {
        const int index = worldSlotFor(worldRows_[usize(worldCursor_)], false);
        if (index >= 0) {
            slot = &worldSlots_[index];
        }
    }
    for (int t = 0; t < preview::kDioramaTiles * preview::kDioramaTiles; ++t) {
        const TileBuffer* tile = &emptyTiles_[t];
        if (slot != nullptr && slot->tiles[t].ready) {
            tile = &slot->tiles[t];
        }
        const u8* bytes = static_cast<const u8*>(tile->data);
        const usize colouredBytes = usize(tile->coloured) * sizeof(mesh::DetailVertex);
        // Neighbouring groups that are both shown go out as one draw.
        u32 at = 0;
        u32 runStart = 0;
        u32 runLength = 0;
        for (int f = 0; f <= mesh::kFaceCount; ++f) {
            const bool show = f < mesh::kFaceCount && showFace[preview::kDioramaFaceOrder[f]];
            if (show) {
                if (runLength == 0) {
                    runStart = at;
                }
                runLength += tile->run[f];
            } else if (runLength > 0) {
                drawVertices(bytes + usize(runStart) * sizeof(mesh::DetailVertex), runLength,
                             &white_);
                runLength = 0;
            }
            if (f < mesh::kFaceCount) {
                at += tile->run[f];
            }
        }
        drawVertices(bytes + colouredBytes, tile->textured, &atlas_);
    }
}

}  // namespace mc::ctr
