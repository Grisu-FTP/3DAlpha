#include "core/texture/jar_import.hpp"

#include "core/texture/pack_list.hpp"
#include "core/texture/zip_archive.hpp"
#include "core/texture/zip_builder.hpp"
#include "core/util/fat_name.hpp"

#include <cstring>

namespace mc::texture {

namespace {

bool endsWithNoCase(std::string_view text, std::string_view suffix)
{
    if (text.size() < suffix.size()) {
        return false;
    }
    const usize offset = text.size() - suffix.size();
    for (usize i = 0; i < suffix.size(); ++i) {
        const char c = text[offset + i];
        const char lower = (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
        if (lower != suffix[i]) {
            return false;
        }
    }
    return true;
}

bool startsWithNoCase(std::string_view text, std::string_view prefix)
{
    if (text.size() < prefix.size()) {
        return false;
    }
    for (usize i = 0; i < prefix.size(); ++i) {
        const char c = text[i];
        const char lower = (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
        if (lower != prefix[i]) {
            return false;
        }
    }
    return true;
}

// The file name out of a path, without its directories or its extension.
std::string stemOf(std::string_view path)
{
    const usize slash = path.find_last_of('/');
    std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const usize dot = name.find_last_of('.');
    if (dot != std::string_view::npos && dot != 0) {
        name = name.substr(0, dot);
    }
    return std::string(name);
}

bool isKnownName(std::string_view name)
{
    for (int i = 0; i < kA112FileCount; ++i) {
        if (name == kA112Files[i]) {
            return true;
        }
    }
    return false;
}

}  // namespace

std::string packNameForJar(std::string_view jarPath)
{
    std::string stem;
    if (!util::sanitizeFatName(stemOf(jarPath), &stem)) {
        return std::string();
    }
    return stem + ".zip";
}

ImportResult importJar(io::FileSystem& fs, std::string_view jarPath, std::string_view packsDir)
{
    ImportResult result;

    const std::string packName = packNameForJar(jarPath);
    if (packName.empty()) {
        result.error = PackError::NotFound;
        return result;
    }

    const std::string jar(jarPath);

    // Measured before it is read. The 3DS build has no exceptions, so a jar
    // that will not fit has to be refused rather than allocated for -- and
    // "that jar is too large" is a far better answer than an abort. See the
    // ceilings in atlas_image.hpp.
    usize size = 0;
    if (!fs.fileSize(jar.c_str(), &size)) {
        result.error = PackError::NotFound;
        return result;
    }
    if (size > kMaxJarBytes) {
        result.error = PackError::TooLarge;
        return result;
    }

    std::vector<u8> bytes;
    if (!fs.readFile(jar.c_str(), &bytes, kMaxJarBytes)) {
        result.error = PackError::ReadFailed;
        return result;
    }

    ZipArchive archive;
    if (archive.open(bytes) != ZipError::Ok) {
        result.error = PackError::NotAPack;
        return result;
    }

    ZipBuilder builder;
    for (const ZipEntry& entry : archive.entries()) {
        if (!endsWithNoCase(entry.name, ".png") || startsWithNoCase(entry.name, "meta-inf/")) {
            ++result.skipped;
            continue;
        }
        const ConstByteSpan raw = archive.rawBytes(entry);
        if (raw.empty() && entry.compressedSize != 0) {
            // The central directory pointed at a local header that is not
            // there. One damaged entry does not condemn the pack; it is
            // counted as skipped and the rest goes across.
            ++result.skipped;
            continue;
        }
        if (!builder.addRaw(entry.name, entry.method, entry.crc, entry.uncompressedSize, raw)) {
            ++result.skipped;
            continue;
        }
        ++result.copied;
        if (isKnownName(entry.name)) {
            ++result.known;
        }
    }

    if (result.copied == 0) {
        result.error = PackError::NoTerrain;
        return result;
    }

    builder.finish();

    if (!fs.makeDirectories(std::string(packsDir).c_str())) {
        result.error = PackError::WriteFailed;
        return result;
    }

    result.outPath = packPath(packsDir, packName);
    if (!fs.writeFileAtomic(result.outPath.c_str(), builder.bytes())) {
        result.error = PackError::WriteFailed;
        result.outPath.clear();
        return result;
    }

    // **Verify what was written, not what was meant to be written.** Re-open
    // the file off the card, decode its terrain.png and build the atlas from
    // it. Only this makes it honest to then offer to delete the source jar --
    // a caller must not offer that on the strength of writeFileAtomic alone.
    AtlasImage atlas;
    const PackError verified = buildAtlas(fs, result.outPath, &atlas);
    if (verified != PackError::Ok) {
        result.error = verified;
        return result;
    }

    return result;
}

}  // namespace mc::texture
