#pragma once

// The Alpha level.dat layout <-> LevelData.
//
//   Compound ""
//     Compound "Data"
//       Long   LastPlayed, SizeOnDisk, RandomSeed, Time
//       Int    SpawnX, SpawnY, SpawnZ
//       Compound "Player"
//         Int            Dimension, Score
//         List<Double>   Pos[3], Motion[3]
//         List<Float>    Rotation[2]
//         Byte           OnGround
//         Float          FallDistance
//         Short          Health, AttackTime, HurtTime, DeathTime, Air, Fire
//         List<Compound> Inventory  [{ Byte Slot, Short id, Byte Count, Short Damage }]
//
// Like the chunk codec these take and produce *decompressed* NBT; level.dat is
// gzipped on disk.
//
// Types are checked, not guessed. A tag we model appearing with the wrong type
// means this is not an a1.1.2 level.dat, and loading it as one would write a
// mangled file back. Tags we do not model are preserved verbatim, and a missing
// one falls back to its default -- except Player, whose absence is meaningful
// and is preserved as absence.
//
// A caution for anyone comparing output against a real save: the original game
// stores compounds in a HashMap, so its tag order is hash order and not
// insertion order. Round-trip fidelity here is semantic, never byte-for-byte,
// against files this project did not write. See docs/world-format.md.

#include "core/util/span.hpp"
#include "core/world/level_data.hpp"

#include <vector>

namespace mc::alpha {

// *out is written only on success.
bool decodeLevelDat(ConstByteSpan nbt, world::LevelData* out);

bool encodeLevelDat(const world::LevelData& level, std::vector<u8>* out);

}  // namespace mc::alpha
