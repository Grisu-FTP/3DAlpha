#include "core/world/world_format.hpp"

#include "core/world/format/manifest.hpp"

#include <string>

namespace mc::world {

namespace {

std::string join(std::string_view dir, const char* name)
{
    std::string path(dir);
    if (!path.empty() && path.back() != '/') {
        path += '/';
    }
    path += name;
    return path;
}

}  // namespace

WorldFormat detectFormat(io::FileSystem& fs, std::string_view worldDir)
{
    if (fs.exists(join(worldDir, format::kManifestName).c_str())) {
        return WorldFormat::Packed;
    }
    if (fs.exists(join(worldDir, "level.dat").c_str())) {
        return WorldFormat::Folder;
    }
    return WorldFormat::Unknown;
}

const char* formatName(WorldFormat format)
{
    switch (format) {
    case WorldFormat::Folder:
        return "Folder";
    case WorldFormat::Packed:
        return "Packed";
    case WorldFormat::Unknown:
        break;
    }
    return "-";
}

}  // namespace mc::world
