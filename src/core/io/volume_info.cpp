#include "core/io/volume_info.hpp"

namespace mc::io {
namespace {

VolumeInfoQuery gQuery = nullptr;

}  // namespace

void setVolumeInfoQuery(VolumeInfoQuery query)
{
    gQuery = query;
}

bool queryVolumeInfo(const char* path, VolumeInfo* out)
{
    if (gQuery == nullptr || out == nullptr) {
        return false;
    }
    return gQuery(path, out);
}

u64 onDiskSize(u64 bytes, u64 clusterSize)
{
    if (clusterSize == 0) {
        return bytes;
    }
    // Zero stays zero: a zero-length file allocates no clusters on FAT, its
    // directory entry just names cluster 0. It is the directory entry itself
    // that is not free, and that is not what this counts.
    return ((bytes + clusterSize - 1) / clusterSize) * clusterSize;
}

}  // namespace mc::io
