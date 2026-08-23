#include "core/texture/atlas_image.hpp"

#include "core/texture/dev_art.hpp"
#include "core/texture/zip_archive.hpp"

#include <cstring>

namespace mc::texture {

namespace {

// The one file in a pack that anything samples today.
constexpr char kTerrainName[] = "terrain.png";

std::string join(std::string_view dir, std::string_view name)
{
    std::string path(dir);
    if (!path.empty() && path.back() != '/') {
        path += '/';
    }
    path.append(name);
    return path;
}

// Area-average, premultiplied. See the note in the header: averaging straight
// RGB across texels whose alpha is zero drags every cutout edge toward whatever
// the pack left in its transparent pixels, which is nearly always black.
void downscale(const Image& source, u8* out)
{
    const usize edge = usize(source.width);
    for (int y = 0; y < kAtlasEdge; ++y) {
        const usize y0 = (usize(y) * edge) / kAtlasEdge;
        const usize y1 = (usize(y + 1) * edge) / kAtlasEdge;
        for (int x = 0; x < kAtlasEdge; ++x) {
            const usize x0 = (usize(x) * edge) / kAtlasEdge;
            const usize x1 = (usize(x + 1) * edge) / kAtlasEdge;

            u32 sumR = 0, sumG = 0, sumB = 0, sumA = 0;
            u32 plainR = 0, plainG = 0, plainB = 0;
            u32 count = 0;

            for (usize sy = y0; sy < y1; ++sy) {
                const u8* row = source.rgba.data() + (sy * edge) * 4;
                for (usize sx = x0; sx < x1; ++sx) {
                    const u8* p = row + sx * 4;
                    const u32 a = p[3];
                    sumR += u32(p[0]) * a;
                    sumG += u32(p[1]) * a;
                    sumB += u32(p[2]) * a;
                    sumA += a;
                    plainR += p[0];
                    plainG += p[1];
                    plainB += p[2];
                    ++count;
                }
            }

            u8* dst = out + (usize(y) * kAtlasEdge + usize(x)) * 4;
            if (count == 0) {
                // Cannot happen for edge >= kAtlasEdge, which is the only way
                // here, but a zero-area rectangle would otherwise divide by it.
                std::memset(dst, 0, 4);
                continue;
            }
            if (sumA == 0) {
                // Every source texel was fully transparent. There is no
                // meaningful colour to keep, but keeping the plain mean rather
                // than black means a pack that stores a colour under its
                // transparency still bleeds the way its author intended if the
                // alpha test is ever loosened.
                dst[0] = u8(plainR / count);
                dst[1] = u8(plainG / count);
                dst[2] = u8(plainB / count);
                dst[3] = 0;
                continue;
            }
            dst[0] = u8(sumR / sumA);
            dst[1] = u8(sumG / sumA);
            dst[2] = u8(sumB / sumA);
            dst[3] = u8(sumA / count);
        }
    }
}

// Nearest neighbour, which for a source that divides the atlas edge is exact
// pixel replication. A 8x pack should come out crisp, not interpolated: the
// whole look depends on nearest filtering.
void upscale(const Image& source, u8* out)
{
    const usize edge = usize(source.width);
    for (int y = 0; y < kAtlasEdge; ++y) {
        const usize sy = (usize(y) * edge) / kAtlasEdge;
        const u8* row = source.rgba.data() + (sy * edge) * 4;
        for (int x = 0; x < kAtlasEdge; ++x) {
            const usize sx = (usize(x) * edge) / kAtlasEdge;
            std::memcpy(out + (usize(y) * kAtlasEdge + usize(x)) * 4, row + sx * 4, 4);
        }
    }
}

// Decodes terrain.png bytes into the atlas, checking that it is a tile grid.
PackError terrainToAtlas(const std::vector<u8>& png, AtlasImage* out)
{
    Image image;
    const PngError error = decodePng(png, &image, kMaxTerrainPixels);
    if (error == PngError::TooLarge) {
        return PackError::TooLarge;
    }
    if (error != PngError::Ok) {
        return PackError::BadPng;
    }
    if (image.width != image.height) {
        return PackError::NotSquare;
    }
    if (image.width % kAtlasTilesPerEdge != 0) {
        // 16 tiles across is what the mesher's UVs assume. An edge that is not
        // a multiple of 16 is not a tile grid, and scaling it would silently
        // shift every tile boundary by a fraction of a texel.
        return PackError::NotTileGrid;
    }

    out->rgba.assign(kAtlasBytes, 0);
    out->sourceEdge = image.width;
    scaleToAtlas(image, &out->rgba);
    return PackError::Ok;
}

}  // namespace

const char* packErrorText(PackError error)
{
    switch (error) {
        case PackError::Ok: return "ok";
        case PackError::NotFound: return "there is nothing at that path";
        case PackError::NotAPack: return "that is not a texture pack";
        case PackError::NoTerrain: return "the pack has no terrain.png";
        case PackError::ReadFailed: return "the card would not read it";
        case PackError::BadPng: return "terrain.png will not decode";
        case PackError::NotSquare: return "terrain.png is not square";
        case PackError::NotTileGrid: return "terrain.png is not 16 tiles across";
        // Named rather than vague, because "too large" without a number sends
        // the player looking for the wrong thing. docs/assets.md makes naming
        // the limit a rule.
        case PackError::TooLarge: return "too large -- 8 MB jars, 1024x1024 terrain.png";
        case PackError::WriteFailed: return "the card would not be written";
    }
    return "unknown";
}

void scaleToAtlas(const Image& source, std::vector<u8>* out)
{
    out->assign(kAtlasBytes, 0);
    if (source.width <= 0 || source.height != source.width) {
        return;
    }
    if (source.width == kAtlasEdge) {
        std::memcpy(out->data(), source.rgba.data(), kAtlasBytes);
        return;
    }
    if (source.width > kAtlasEdge) {
        downscale(source, out->data());
        return;
    }
    upscale(source, out->data());
}

PackError buildAtlas(io::FileSystem& fs, std::string_view packPath, AtlasImage* out)
{
    out->rgba.clear();
    out->sourceEdge = 0;

    if (packPath.empty()) {
        buildDevArt(&out->rgba);
        return PackError::Ok;
    }

    const std::string path(packPath);

    // A directory pack: the tree lying loose, which is what a card looks like
    // after somebody unzipped a pack on a PC.
    if (fs.isDirectory(path.c_str())) {
        const std::string terrain = join(path, kTerrainName);
        std::vector<u8> png;
        if (!fs.readFile(terrain.c_str(), &png, kMaxPackBytes)) {
            return fs.exists(terrain.c_str()) ? PackError::ReadFailed : PackError::NoTerrain;
        }
        return terrainToAtlas(png, out);
    }

    if (!fs.exists(path.c_str())) {
        return PackError::NotFound;
    }

    std::vector<u8> archive;
    if (!fs.readFile(path.c_str(), &archive, kMaxPackBytes)) {
        return PackError::ReadFailed;
    }

    ZipArchive zip;
    if (zip.open(archive) != ZipError::Ok) {
        return PackError::NotAPack;
    }
    const ZipEntry* entry = zip.find(kTerrainName);
    if (entry == nullptr) {
        return PackError::NoTerrain;
    }

    std::vector<u8> png;
    if (zip.read(*entry, &png) != ZipError::Ok) {
        return PackError::ReadFailed;
    }
    return terrainToAtlas(png, out);
}

}  // namespace mc::texture
