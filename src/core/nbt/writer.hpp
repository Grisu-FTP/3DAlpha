#pragma once

// NBT writer.
//
// Appends big-endian NBT into a caller-owned byte vector. Like the reader it
// carries a sticky error flag, so a long sequence of writes is checked once at
// the end.
//
// Nesting is explicit and balanced: beginCompound/endCompound and
// beginList/endList. Lists do not need their length up front -- the count field
// is patched when the list closes -- because entity lists are built while
// iterating live objects, and forcing a pre-count would mean walking them twice.
//
//   Writer w(out);
//   w.beginRoot("");
//     w.beginCompound("Level");
//       w.writeInt("xPos", x);
//       w.writeByteArray("Blocks", blocks);
//       w.beginList("Entities", TagType::Compound);
//         for (auto& e : entities) { w.beginListElementCompound(); ...; w.endCompound(); }
//       w.endList();
//     w.endCompound();
//   w.endRoot();

#include "core/nbt/nbt.hpp"
#include "core/util/span.hpp"
#include "core/util/types.hpp"

#include <string_view>
#include <vector>

namespace mc::nbt {

class Writer {
public:
    explicit Writer(std::vector<u8>& out) : out_(out) {}

    // NBT files carry a single root compound, conventionally with an empty
    // name (Alpha's chunk files and level.dat both use "").
    void beginRoot(std::string_view name = {});
    void endRoot();

    void writeByte(std::string_view name, i8 value);
    void writeShort(std::string_view name, i16 value);
    void writeInt(std::string_view name, i32 value);
    void writeLong(std::string_view name, i64 value);
    void writeFloat(std::string_view name, float value);
    void writeDouble(std::string_view name, double value);
    void writeByteArray(std::string_view name, ConstByteSpan value);
    void writeString(std::string_view name, std::string_view value);
    void writeIntArray(std::string_view name, Span<const i32> value);

    void beginCompound(std::string_view name);
    void endCompound();

    // If the list ends up empty, endList() rewrites elemType to Byte, because
    // that is what the original game emits for an empty list regardless of what
    // it was going to hold. See the note in endList().
    void beginList(std::string_view name, TagType elemType);
    void endList();

    // Inside a list, elements are bare values with no type byte and no name.
    // A compound element still needs its TAG_End, so it pairs with
    // endCompound(); the other element writers below emit the payload directly.
    void beginListElementCompound();
    // Lists of lists do not occur in Alpha files, but they are legal NBT and
    // the generic tree copier has to be able to reproduce anything it reads.
    void beginListElementList(TagType elemType);
    void listByte(i8 value);
    void listShort(i16 value);
    void listInt(i32 value);
    void listLong(i64 value);
    void listFloat(float value);
    void listDouble(double value);
    void listString(std::string_view value);
    void listByteArray(ConstByteSpan value);
    void listRaw(ConstByteSpan payload);

    // Splices an already-serialised value in verbatim. This is how tags the
    // client does not model are preserved across a load/save round trip:
    // the loader records their byte range and the saver writes it back
    // untouched, so saving a world never destroys data a newer game version
    // put there.
    void writeRaw(std::string_view name, TagType type, ConstByteSpan payload);

    bool ok() const { return !failed_; }

private:
    void tag(TagType type, std::string_view name);
    void rawString(std::string_view value);
    // True when the cursor is directly inside a list, and counts the element.
    // Unbalanced nesting is a bug in calling code rather than bad input, so it
    // only needs to be caught, not recovered from.
    bool startListElement();
    void u8At(u8 value);
    void u16At(u16 value);
    void u32At(u32 value);
    void u64At(u64 value);

    struct Frame {
        TagType type;      // Compound or List
        usize countOffset; // lists only: where the i32 element count sits
        i32 count;         // lists only
    };

    std::vector<u8>& out_;
    std::vector<Frame> stack_;
    bool failed_ = false;
};

}  // namespace mc::nbt
