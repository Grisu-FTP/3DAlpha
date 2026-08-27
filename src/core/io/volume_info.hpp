#pragma once

// How big a cluster is on the card, and how much of the card is left.
//
// A platform seam, and a seam rather than a FileSystem method for one reason:
// the answer comes from `FSUSER_GetSdmcArchiveResource()` on the 3DS, and
// **nothing in src/core/ may include <3ds.h>**. `PosixFileSystem` serves both
// targets from one body of code, so it cannot be where the console-only call
// goes. Same shape as core/util/worker.hpp, for the same reason.
//
// Two callers, both about honesty rather than correctness:
//
//   * **True on-disk size.** A world's bytes and the space it occupies are not
//     the same number and are not close. The Alpha format writes one file per
//     chunk column at a 2,917-byte median against a cluster that a real card
//     measured at 16 KB, so on-disk cost is roughly (chunks x clusterSize) and
//     the content is a fifth of it. Reporting the byte total alone would hide
//     the entire reason the packed format exists.
//   * **Refusing a conversion up front.** Converting builds the new
//     representation beside the old one, so the card has to hold both at once.
//     Finding that out at 80 % is the failure worth designing away.
//
// **Unknown is a normal answer**, not an error: the host has no SD card and a
// console that fails the call still has to be able to convert a world. A
// caller that cannot learn the cluster size reports bytes and skips the
// free-space refusal. It must never become a reason the work cannot run.

#include "core/util/types.hpp"

namespace mc::io {

struct VolumeInfo {
    // Bytes per allocation unit. A file occupies a whole number of these, so
    // rounding up to it is what turns a byte total into a footprint.
    u64 clusterSize = 0;
    u64 freeBytes = 0;
    u64 totalBytes = 0;
};

// False when the platform has no answer, leaving *out untouched.
using VolumeInfoQuery = bool (*)(const char* path, VolumeInfo* out);

// Process-wide. The platform shell installs it at startup; null clears it.
void setVolumeInfoQuery(VolumeInfoQuery query);

// False when no platform installed one, or when the installed one declined.
bool queryVolumeInfo(const char* path, VolumeInfo* out);

// Rounds `bytes` up to a whole number of clusters. With an unknown cluster
// size this is the identity, which is the honest answer -- a footprint nobody
// can measure is best reported as the content it holds rather than guessed at
// from an assumed 32 KB that a measured card has already contradicted.
u64 onDiskSize(u64 bytes, u64 clusterSize);

}  // namespace mc::io
