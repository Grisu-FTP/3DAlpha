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
void downscale(const Image& source, int target, u8* out)
{
    const usize edge = usize(source.width);
    for (int y = 0; y < target; ++y) {
        const usize y0 = (usize(y) * edge) / usize(target);
        const usize y1 = (usize(y + 1) * edge) / usize(target);
        for (int x = 0; x < target; ++x) {
            const usize x0 = (usize(x) * edge) / usize(target);
            const usize x1 = (usize(x + 1) * edge) / usize(target);

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

            u8* dst = out + (usize(y) * usize(target) + usize(x)) * 4;
            if (count == 0) {
                // Cannot happen for edge >= target, which is the only way
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
void upscale(const Image& source, int target, u8* out)
{
    const usize edge = usize(source.width);
    for (int y = 0; y < target; ++y) {
        const usize sy = (usize(y) * edge) / usize(target);
        const u8* row = source.rgba.data() + (sy * edge) * 4;
        for (int x = 0; x < target; ++x) {
            const usize sx = (usize(x) * edge) / usize(target);
            std::memcpy(out + (usize(y) * usize(target) + usize(x)) * 4, row + sx * 4, 4);
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

void scaleSquare(const Image& source, int edge, std::vector<u8>* out)
{
    const usize bytes = usize(edge) * usize(edge) * 4;
    out->assign(bytes, 0);
    if (edge <= 0 || source.width <= 0 || source.height != source.width) {
        return;
    }
    if (source.width == edge) {
        std::memcpy(out->data(), source.rgba.data(), bytes);
        return;
    }
    if (source.width > edge) {
        downscale(source, edge, out->data());
        return;
    }
    upscale(source, edge, out->data());
}

void scaleToAtlas(const Image& source, std::vector<u8>* out)
{
    scaleSquare(source, kAtlasEdge, out);
}

PackError readPackFile(io::FileSystem& fs, std::string_view packPath, std::string_view name,
                       std::vector<u8>* out)
{
    out->clear();

    const std::string path(packPath);
    if (path.empty()) {
        return PackError::NotFound;
    }

    // A directory pack: the tree lying loose, which is what a card looks like
    // after somebody unzipped a pack on a PC.
    if (fs.isDirectory(path.c_str())) {
        const std::string file = join(path, name);
        if (!fs.readFile(file.c_str(), out, kMaxPackBytes)) {
            return fs.exists(file.c_str()) ? PackError::ReadFailed : PackError::NotFound;
        }
        return PackError::Ok;
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
    const ZipEntry* entry = zip.find(name);
    if (entry == nullptr) {
        return PackError::NotFound;
    }
    if (zip.read(*entry, out) != ZipError::Ok) {
        return PackError::ReadFailed;
    }
    return PackError::Ok;
}

PackError buildAtlas(io::FileSystem& fs, std::string_view packPath, AtlasImage* out)
{
    out->rgba.clear();
    out->sourceEdge = 0;

    if (packPath.empty()) {
        buildDevArt(&out->rgba);
        return PackError::Ok;
    }

    std::vector<u8> png;
    const PackError read = readPackFile(fs, packPath, kTerrainName, &png);
    if (read == PackError::NotFound) {
        // A pack that opened and has no terrain.png in it, as against a path
        // with nothing at it -- which readPackFile reports the same way and
        // which the pack list has already ruled out by the time it gets here.
        return fs.exists(std::string(packPath).c_str()) ? PackError::NoTerrain
                                                        : PackError::NotFound;
    }
    if (read != PackError::Ok) {
        return read;
    }
    return terrainToAtlas(png, out);
}

}  // namespace mc::texture
