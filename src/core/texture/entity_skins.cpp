// The five small entity textures packed into one 256 x 64 sheet, and the
// painting sheet beside it. See the header for the argument.

#include "core/texture/entity_skins.hpp"

#include "core/texture/png.hpp"

#include <cstddef>
#include <cstring>

namespace mc::texture {
namespace {

// Where each page begins. The first four are where they have always been --
// the sheet grew to the right, so nothing moved and no model's UVs changed.
// The arrow's page is only 32 wide, and the 32 texels beside it are left
// transparent rather than filled, because a stray sample there should look like
// a hole and not like a colour somebody chose. The three slots past the
// player's are spare.
struct Slot {
    int x, y;
    int width, height;
    const char* file;
};

constexpr Slot kSlots[kEntitySkinCount] = {
    {0, 0, kSkinPageWidth, kSkinPageHeight, "item/boat.png"},
    {64, 0, kSkinPageWidth, kSkinPageHeight, "item/cart.png"},
    {0, 32, kSkinPageWidth, kSkinPageHeight, "item/sign.png"},
    {64, 32, 32, 32, "item/arrows.png"},
    {128, 0, kSkinPageWidth, kSkinPageHeight, kSkinFile},
    // The animals, on the two rows the sheet grew downwards to make. Every page
    // above this line kept its origin.
    {0, 64, kSkinPageWidth, kSkinPageHeight, "mob/pig.png"},
    {64, 64, kSkinPageWidth, kSkinPageHeight, "mob/saddle.png"},
    {128, 64, kSkinPageWidth, kSkinPageHeight, "mob/cow.png"},
    {192, 64, kSkinPageWidth, kSkinPageHeight, "mob/sheep.png"},
    {0, 96, kSkinPageWidth, kSkinPageHeight, "mob/sheep_fur.png"},
    {64, 96, kSkinPageWidth, kSkinPageHeight, "mob/chicken.png"},
    // The monsters, on the two rows the sheet grew downwards to make when it
    // went from 128 to 256 tall. Every page above this line kept its origin.
    {128, 96, kSkinPageWidth, kSkinPageHeight, "mob/zombie.png"},
    {192, 96, kSkinPageWidth, kSkinPageHeight, "mob/skeleton.png"},
    {0, 128, kSkinPageWidth, kSkinPageHeight, "mob/creeper.png"},
    {64, 128, kSkinPageWidth, kSkinPageHeight, "mob/spider.png"},
    {128, 128, kSkinPageWidth, kSkinPageHeight, "mob/spider_eyes.png"},
    {192, 128, kSkinPageWidth, kSkinPageHeight, "mob/slime.png"},
    // The sky's two, in the row the sign and the arrow left half empty. 32 x 32
    // each and a slot apart, so the sheet's one page pitch still describes it.
    {128, 32, kCelestialPagePixels, kCelestialPagePixels, "terrain/sun.png"},
    {192, 32, kCelestialPagePixels, kCelestialPagePixels, "terrain/moon.png"},
    // The one free page in the top row, and the same file the player's page is
    // built from -- see the note on `EntitySkin::OtherPlayer`.
    {192, 0, kSkinPageWidth, kSkinPageHeight, kSkinFile},
};

const Slot& slotOf(EntitySkin skin)
{
    const int index = int(skin);
    return kSlots[index >= 0 && index < kEntitySkinCount ? index : 0];
}

// The art sheet's own edge. `er` indexes it in absolute texels and the largest
// painting reaches (192 + 64, 192 + 64), so 256 is not a round number chosen
// here -- it is the table's range.
constexpr int kArtEdge = 256;
constexpr usize kArtBytes = usize(kArtEdge) * kArtEdge * 4;

// One texel of `out`, which is `width` texels across.
u8* texel(std::vector<u8>* out, int width, int x, int y)
{
    return out->data() + (usize(y) * usize(width) + usize(x)) * 4;
}

// Area-average a source rectangle into a destination rectangle, or replicate it
// nearest-neighbour when the source is the smaller.
//
// **The average is premultiplied by alpha**, for exactly the reason
// `scaleSquare` gives: every one of these sheets is a cutout, and averaging
// straight RGB across a transparent texel drags the edge towards whatever
// happens to be in the unused part of the image -- usually black. That is the
// dark halo a naively downscaled boat gets around its rim.
//
// This is not `scaleSquare` because none of these pages is square: a1.1.2's
// entity sheets are 64 x 32 and that shape is the class file's, not a choice.
void blitScaled(const Image& source, std::vector<u8>* out, int outWidth, const Slot& slot)
{
    if (source.width <= 0 || source.height <= 0) {
        return;
    }
    for (int y = 0; y < slot.height; ++y) {
        // The source rows this destination row covers. Half-open, and at least
        // one row wide however extreme the ratio.
        const int y0 = (y * source.height) / slot.height;
        int y1 = ((y + 1) * source.height) / slot.height;
        if (y1 <= y0) {
            y1 = y0 + 1;
        }
        for (int x = 0; x < slot.width; ++x) {
            const int x0 = (x * source.width) / slot.width;
            int x1 = ((x + 1) * source.width) / slot.width;
            if (x1 <= x0) {
                x1 = x0 + 1;
            }

            u32 r = 0, g = 0, b = 0, a = 0;
            int n = 0;
            for (int sy = y0; sy < y1 && sy < source.height; ++sy) {
                for (int sx = x0; sx < x1 && sx < source.width; ++sx) {
                    const u8* p =
                        source.rgba.data() + (usize(sy) * usize(source.width) + usize(sx)) * 4;
                    const u32 alpha = p[3];
                    r += u32(p[0]) * alpha;
                    g += u32(p[1]) * alpha;
                    b += u32(p[2]) * alpha;
                    a += alpha;
                    ++n;
                }
            }
            if (n == 0) {
                continue;
            }
            u8* dst = texel(out, outWidth, slot.x + x, slot.y + y);
            // Divide the premultiplied sums by the alpha total, not by the
            // sample count: a texel covering three transparent texels and one
            // opaque one is that one texel's colour, not a quarter of it.
            dst[0] = u8(a > 0 ? r / a : 0);
            dst[1] = u8(a > 0 ? g / a : 0);
            dst[2] = u8(a > 0 ? b / a : 0);
            dst[3] = u8(a / u32(n));
        }
    }
}

// **The upper half of a 64 x 64 skin, which is the whole of a 64 x 32 one.**
//
// Every coordinate a1.1.2's model uses is inside the top `width / 2` rows, so
// this is a crop and not a conversion: a classic skin comes back unchanged and
// a 1.8-era one loses the second layer this version has nowhere to draw.
// Scaling the whole 64 x 64 into a 64 x 32 page instead would squash the head
// and the body into each other, which is what "my skin looks wrong" would have
// meant.
//
// Returns false when the image is not a skin shape at all; the caller then
// leaves the page alone.
bool classicHalf(const Image& source, Image* out)
{
    if (source.width <= 0 || source.height <= 0) {
        return false;
    }
    const int wanted = source.width / 2;
    if (source.height == wanted) {
        return false;  // already classic; the caller uses the source as it is
    }
    if (source.height < wanted) {
        return false;  // not a skin: too short to hold even the classic layout
    }
    out->width = source.width;
    out->height = wanted;
    const usize keep = usize(source.width) * usize(wanted) * 4;
    if (source.rgba.size() < keep) {
        return false;
    }
    out->rgba.assign(source.rgba.begin(), source.rgba.begin() + std::ptrdiff_t(keep));
    return true;
}

// Reads one file out of the pack and lays it into its page. Silent on every
// failure; see the header.
void pageFromPack(io::FileSystem& fs, std::string_view packPath, const Slot& slot,
                  std::vector<u8>* out, int outWidth)
{
    std::vector<u8> png;
    if (readPackFile(fs, packPath, slot.file, &png) != PackError::Ok) {
        return;
    }
    Image image;
    if (decodePng(png, &image, kMaxTerrainPixels) != PngError::Ok) {
        return;
    }
    // **The player's page is the one exception to the line below**, because for
    // it a 64 x 64 is not a mistake -- it is the later format, and its top half
    // is this one. Every other page has no such second shape.
    Image cropped;
    if (std::strcmp(slot.file, kSkinFile) == 0 && classicHalf(image, &cropped)) {
        blitScaled(cropped, out, outWidth, slot);
        return;
    }
    // A pack whose page is a different *aspect* is still laid in: a 64 x 64
    // boat.png is somebody's mistake and squashing it is a more useful answer
    // than a stand-in, because it shows them what they did.
    blitScaled(image, out, outWidth, slot);
}

// A flat body with a one-texel grid over it, in a hue of its own.
//
// The grid is the point. A flat colour would say "this page is the boat's" and
// nothing else; a grid also shows whether the UVs are the right way up and the
// right way round, which is the failure mode a box model actually has.
void devArtPage(std::vector<u8>* out, int outWidth, const Slot& slot, u8 r, u8 g, u8 b)
{
    for (int y = 0; y < slot.height; ++y) {
        for (int x = 0; x < slot.width; ++x) {
            const bool line = (x % 8) == 0 || (y % 8) == 0;
            // A corner mark in the page's top-left, so "which way up" has an
            // answer that does not depend on counting grid squares.
            const bool corner = x < 3 && y < 3;
            u8* dst = texel(out, outWidth, slot.x + x, slot.y + y);
            dst[0] = corner ? 255 : u8(line ? r / 2 : r);
            dst[1] = corner ? 255 : u8(line ? g / 2 : g);
            dst[2] = corner ? 255 : u8(line ? b / 2 : b);
            dst[3] = 255;
        }
    }
}

// **The slime's stand-in, which is a grid with a hole in its opacity.**
//
// `gq` draws the slime twice: `hh(16)`, the inner body with the face on it, and
// then `hh(0)`, the shell, blended over the top -- and the shell is blended
// because the jar's own `mob/slime.png` has it at alpha 199 of 255. A flat
// opaque stand-in therefore draws a slime with no face, which is exactly the
// bug the second pass exists to fix and is not something a stand-in should be
// able to hide. The shell's net is the top-left quarter of the page (the 8-cube
// at texture offset (0, 0) in a 64 x 32), so that quarter gets the jar's alpha
// and the rest -- the inner body, the eyes and the mouth -- stays opaque.
void slimePage(std::vector<u8>* out, int outWidth, const Slot& slot, u8 r, u8 g, u8 b)
{
    constexpr u8 kJellyAlpha = 199;
    devArtPage(out, outWidth, slot, r, g, b);
    for (int y = 0; y < slot.height / 2; ++y) {
        for (int x = 0; x < slot.width / 2; ++x) {
            texel(out, outWidth, slot.x + x, slot.y + y)[3] = kJellyAlpha;
        }
    }
}

// A page of opaque black. See the header on why the player's stand-in is not a
// grid like the other four.
void blackPage(std::vector<u8>* out, int outWidth, const Slot& slot)
{
    for (int y = 0; y < slot.height; ++y) {
        for (int x = 0; x < slot.width; ++x) {
            u8* dst = texel(out, outWidth, slot.x + x, slot.y + y);
            dst[0] = 0;
            dst[1] = 0;
            dst[2] = 0;
            dst[3] = 255;
        }
    }
}

// The spider's eye overlay: **clear, with two red dots where the eyes are.**
//
// The head box of `jy` is at (32, 4) in a 64 x 32 page and is 8 x 8 x 8, so its
// front face -- the one the eyes are on -- occupies texels (40, 12) to (47,
// 19). The dots go in that square. Everything else stays at alpha zero, which
// the detail pass's alpha test cuts away for free.
void eyePage(std::vector<u8>* out, int outWidth, const Slot& slot)
{
    for (int y = 0; y < slot.height; ++y) {
        for (int x = 0; x < slot.width; ++x) {
            u8* dst = texel(out, outWidth, slot.x + x, slot.y + y);
            dst[0] = dst[1] = dst[2] = dst[3] = 0;
        }
    }
    constexpr int kEyeRows[2] = {14, 14};
    constexpr int kEyeCols[2] = {41, 45};
    for (int e = 0; e < 2; ++e) {
        for (int dy = 0; dy < 2; ++dy) {
            for (int dx = 0; dx < 2; ++dx) {
                u8* dst = texel(out, outWidth, slot.x + kEyeCols[e] + dx,
                                slot.y + kEyeRows[e] + dy);
                dst[0] = 230;
                dst[1] = 40;
                dst[2] = 40;
                dst[3] = 255;
            }
        }
    }
}

}  // namespace

void skinOrigin(EntitySkin skin, int* x, int* y)
{
    const Slot& slot = slotOf(skin);
    *x = slot.x;
    *y = slot.y;
}

const char* skinFileName(EntitySkin skin)
{
    return slotOf(skin).file;
}

void buildDevArtSkins(std::vector<u8>* out)
{
    out->assign(kEntitySheetBytes, 0);
    // Four hues far enough apart to name over a phone: oak, iron, pine, flint.
    devArtPage(out, kEntitySheetWidth, kSlots[0], 150, 105, 60);
    devArtPage(out, kEntitySheetWidth, kSlots[1], 120, 125, 135);
    devArtPage(out, kEntitySheetWidth, kSlots[2], 165, 140, 95);
    devArtPage(out, kEntitySheetWidth, kSlots[3], 190, 190, 200);
    // The animals, each in roughly its own colour so a model drawn from the
    // wrong page is obvious at a glance: pork pink, saddle leather, cowhide
    // brown, fleece white, wool cream, chicken white-yellow.
    // **Each red is its own**, which `each_page_has_its_own_colour` holds them
    // to: a page is identified by that channel alone, so two animals that look
    // alike on a phone still differ where the test reads.
    devArtPage(out, kEntitySheetWidth, kSlots[int(EntitySkin::Pig)], 235, 145, 150);
    devArtPage(out, kEntitySheetWidth, kSlots[int(EntitySkin::Saddle)], 145, 90, 45);
    devArtPage(out, kEntitySheetWidth, kSlots[int(EntitySkin::Cow)], 95, 70, 55);
    devArtPage(out, kEntitySheetWidth, kSlots[int(EntitySkin::Sheep)], 225, 215, 205);
    devArtPage(out, kEntitySheetWidth, kSlots[int(EntitySkin::SheepFur)], 240, 235, 225);
    devArtPage(out, kEntitySheetWidth, kSlots[int(EntitySkin::Chicken)], 205, 230, 180);
    // The monsters. Same rule -- **each red is its own** -- and each roughly the
    // colour the mob is: rotted green, bone white, creeper green, spider
    // charcoal, and slime green. The eye page is the exception: it is an
    // overlay, so it is **transparent everywhere but two red dots**, which is
    // what the file it stands in for is and is the only stand-in here that is
    // not a full page.
    devArtPage(out, kEntitySheetWidth, kSlots[int(EntitySkin::Zombie)], 80, 130, 90);
    devArtPage(out, kEntitySheetWidth, kSlots[int(EntitySkin::Skeleton)], 200, 200, 190);
    devArtPage(out, kEntitySheetWidth, kSlots[int(EntitySkin::Creeper)], 110, 190, 100);
    devArtPage(out, kEntitySheetWidth, kSlots[int(EntitySkin::Spider)], 60, 45, 40);
    slimePage(out, kEntitySheetWidth, kSlots[int(EntitySkin::Slime)], 130, 220, 120);
    eyePage(out, kEntitySheetWidth, kSlots[int(EntitySkin::SpiderEyes)]);
    // The sky's two. Warm for the sun and pale for the moon, so a stand-in sky
    // still reads as a sky -- and a grid rather than a disc, because these are
    // placeholders and the grid is what shows a flipped UV.
    devArtPage(out, kEntitySheetWidth, kSlots[int(EntitySkin::Sun)], 250, 225, 120);
    devArtPage(out, kEntitySheetWidth, kSlots[int(EntitySkin::Moon)], 170, 180, 200);
    // **The player's page is black and not a grid**, which is the header's
    // argument: the only thing that reads it is the arm of an empty hand, and a
    // silhouette is an honest answer where an orange grid would read as a bug.
    // Opaque, because the alpha test would otherwise cut the arm away entirely.
    blackPage(out, kEntitySheetWidth, kSlots[int(EntitySkin::Player)]);
    // Other people are silhouettes for the same reason, and deliberately the
    // same silhouette -- which is why `each_page_has_its_own_colour` excuses
    // this page along with the spider's eyes.
    blackPage(out, kEntitySheetWidth, kSlots[int(EntitySkin::OtherPlayer)]);
}

void buildDevArtArt(std::vector<u8>* out)
{
    out->assign(kArtBytes, 0);
    // **A 16 x 16 grid of framed squares**, because that is what the art sheet
    // is: `er`'s rectangles are all multiples of 16 and every one of them is a
    // whole number of these cells. A painting placed with a stand-in shows a
    // frame with a flat middle, which reads as a painting rather than as a
    // missing texture, and its size is still legible.
    for (int y = 0; y < kArtEdge; ++y) {
        for (int x = 0; x < kArtEdge; ++x) {
            const int cx = x % 16;
            const int cy = y % 16;
            const bool frame = cx == 0 || cy == 0 || cx == 15 || cy == 15;
            // A stable hue per cell, hashed the way the terrain placeholder is
            // -- so two different paintings are two different colours and a
            // wrong offset in the art table is visible as one.
            const u32 cell = u32((y / 16) * 16 + (x / 16));
            const u32 hash = cell * 2654435761u;
            u8* dst = texel(out, kArtEdge, x, y);
            dst[0] = frame ? 60 : u8(90 + (hash >> 24) % 140);
            dst[1] = frame ? 45 : u8(90 + (hash >> 16) % 140);
            dst[2] = frame ? 30 : u8(90 + (hash >> 8) % 140);
            dst[3] = 255;
        }
    }
}

void buildEntitySkins(io::FileSystem& fs, std::string_view packPath, std::vector<u8>* out)
{
    // **The stand-ins first, then the pack over the top.** That is what makes a
    // pack with three of the four files work: the page it does not carry keeps
    // the generated one rather than going black. Dev Art simply stops here.
    buildDevArtSkins(out);
    if (packPath.empty()) {
        return;
    }
    for (int i = 0; i < kEntitySkinCount; ++i) {
        pageFromPack(fs, packPath, kSlots[i], out, kEntitySheetWidth);
    }
}

namespace {

// A skin file or a pack's `char.png`, read and decoded. The half both callers
// below share.
bool readSkinImage(io::FileSystem& fs, std::string_view path, bool fromPack, Image* out)
{
    std::vector<u8> png;
    if (fromPack) {
        if (readPackFile(fs, path, kSkinFile, &png) != PackError::Ok) {
            return false;
        }
    } else if (!fs.readFile(std::string(path).c_str(), &png, kMaxPackBytes)) {
        return false;
    }
    return decodePng(png, out, kMaxTerrainPixels) == PngError::Ok;
}

}  // namespace

bool applyPlayerSkin(io::FileSystem& fs, std::string_view path, bool fromPack,
                     std::vector<u8>* sheet)
{
    if (sheet == nullptr || sheet->size() != kEntitySheetBytes || path.empty()) {
        return false;
    }

    Image image;
    if (!readSkinImage(fs, path, fromPack, &image)) {
        return false;
    }

    const Slot& slot = slotOf(EntitySkin::Player);
    Image cropped;
    blitScaled(classicHalf(image, &cropped) ? cropped : image, sheet, kEntitySheetWidth, slot);
    return true;
}

bool decodePlayerSkinPage(io::FileSystem& fs, std::string_view path, bool fromPack,
                          std::vector<u8>* page)
{
    if (page == nullptr || path.empty()) {
        return false;
    }

    Image image;
    if (!readSkinImage(fs, path, fromPack, &image)) {
        return false;
    }

    // The player's page, moved to the origin of a buffer that is only that
    // page: the same scale and the same crop the sheet gets.
    const Slot whole{0, 0, kSkinPageWidth, kSkinPageHeight, kSkinFile};
    page->assign(usize(kSkinPageWidth) * kSkinPageHeight * 4, 0);
    Image cropped;
    blitScaled(classicHalf(image, &cropped) ? cropped : image, page, kSkinPageWidth, whole);
    return true;
}

void buildArtSheet(io::FileSystem& fs, std::string_view packPath, std::vector<u8>* out)
{
    buildDevArtArt(out);
    if (packPath.empty()) {
        return;
    }
    std::vector<u8> png;
    if (readPackFile(fs, packPath, "art/kz.png", &png) != PackError::Ok) {
        return;
    }
    Image image;
    if (decodePng(png, &image, kMaxTerrainPixels) != PngError::Ok) {
        return;
    }
    const Slot whole{0, 0, kArtEdge, kArtEdge, "art/kz.png"};
    blitScaled(image, out, kArtEdge, whole);
}

}  // namespace mc::texture
