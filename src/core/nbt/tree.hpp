#pragma once

// Generic whole-tree operations, built on Reader and Writer.
//
// copyValue() is the mechanism behind "loading a world never destroys what the
// client does not understand": the chunk loader copies tags it has no model for
// straight from the source buffer into the save buffer. It is also the engine
// of the round-trip test -- read a real world file, copy it through, and the
// result must dump identically.
//
// dump() renders a document as canonical text. Comparing two dumps is a
// semantic NBT diff, and it is far easier to read than a hex diff when a
// round trip does fail. It preserves file order rather than sorting keys,
// because our writer preserves order too; two files that differ only in member
// order are equal as NBT but will not compare equal here.

#include "core/nbt/nbt.hpp"
#include "core/nbt/writer.hpp"
#include "core/util/span.hpp"

#include <string>
#include <vector>

namespace mc::nbt {

// Copies one value of the given type from reader to writer. The reader must be
// positioned on the value (immediately after nextField reported it) and the
// writer inside the compound the value belongs to.
bool copyValue(Reader& reader, Writer& writer, TagType type, std::string_view name);

// Copies a whole document, root compound included. Byte-for-byte identity is
// not guaranteed and not wanted: the output is normalised NBT.
bool copyDocument(ConstByteSpan in, std::vector<u8>& out);

// Renders a document as indented text. Byte arrays print their length and a
// short prefix rather than 32 KB of digits.
bool dump(ConstByteSpan in, std::string& out);

}  // namespace mc::nbt
