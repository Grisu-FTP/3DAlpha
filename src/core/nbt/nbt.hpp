#pragma once

// NBT reader.
//
// Cursor-based and zero-copy: it walks a decompressed buffer in place and
// never allocates. Byte arrays and strings are returned as views into that
// buffer, so the buffer must outlive them. This matters -- an Alpha chunk
// column decompresses to 80 KB, and building a DOM for every one of them would
// cost more than the block data itself.
//
// Errors are sticky. Every accessor is a no-op once the reader has failed, so
// parsing code reads linearly and checks ok() once at the end instead of
// testing every call. The parser is fully bounds-checked: it reads world files
// off an SD card and chunk payloads off the network, neither of which is
// trusted, so malformed input must produce a clean failure and never UB.
//
// Nesting uses the C++ call stack: after nextField() reports Compound, call
// nextField() again to iterate its members; it returns false when it consumes
// that compound's TAG_End, leaving the cursor on the parent's next member.
//
//   Reader r(data);
//   std::string_view rootName;
//   if (!r.enterRoot(&rootName)) return false;
//   TagType type; std::string_view name;
//   while (r.nextField(&type, &name)) {
//       if (name == "Blocks" && type == TagType::ByteArray) blocks = r.byteArray();
//       else                                                r.skipValue(type);
//   }
//   return r.ok();
//
// IMPORTANT: a nested loop must drain its compound completely. Breaking out
// early leaves the cursor inside the child and desynchronises the parent loop.
// Use skipToEnd() to bail out of a compound you have stopped caring about.

#include "core/util/span.hpp"
#include "core/util/types.hpp"

#include <string_view>

namespace mc::nbt {

enum class TagType : u8 {
    End = 0,
    Byte = 1,
    Short = 2,
    Int = 3,
    Long = 4,
    Float = 5,
    Double = 6,
    ByteArray = 7,
    String = 8,
    List = 9,
    Compound = 10,
    // Not present in the Alpha-era spec. Accepted (and skippable) anyway so the
    // reader stays usable for later world formats without a second parser.
    IntArray = 11,
    LongArray = 12,
};

const char* tagTypeName(TagType type);

// Guards against a hostile file nesting compounds deeply enough to overflow the
// stack in skipValue(). Real files nest fewer than ten deep.
inline constexpr int kMaxDepth = 64;

class Reader {
public:
    explicit Reader(ConstByteSpan data) : data_(data) {}

    // Reads the root tag header. NBT files always have a single root compound;
    // anything else is a malformed file.
    bool enterRoot(std::string_view* nameOut = nullptr);

    // Advances to the next member of the compound currently being iterated.
    // Returns false when the compound ends (consuming its TAG_End) or on error.
    bool nextField(TagType* type, std::string_view* name);

    // Skips every remaining member of the current compound, including TAG_End.
    bool skipToEnd();

    // Value accessors. Each is valid exactly once, immediately after
    // nextField() reported the matching type, and advances the cursor past the
    // value. Calling the wrong one desynchronises the stream, so callers must
    // check the reported type first.
    i8 byteValue();
    i16 shortValue();
    i32 intValue();
    i64 longValue();
    float floatValue();
    double doubleValue();
    ConstByteSpan byteArray();
    // Raw Java modified-UTF-8 bytes. Decoding is the strings slot's job; key
    // comparisons work directly because modified UTF-8 is ASCII-compatible.
    std::string_view string();
    Span<const i32> intArray();

    // After nextField() reports List. Elements follow immediately and are read
    // (or skipped) one at a time, each as a bare value of *elemType*.
    bool enterList(TagType* elemType, i32* count);

    // Consumes one value of the given type, whatever it is.
    bool skipValue(TagType type);

    usize offset() const { return pos_; }

    // The bytes between a previously recorded offset() and the cursor. Pair it
    // with skipValue() to capture a value verbatim without modelling it -- how
    // the chunk loader carries tags it does not understand through to the save.
    ConstByteSpan rangeSince(usize start) const;

    bool ok() const { return !failed_; }
    // Marks the stream malformed. For callers that detect a semantic problem
    // (a wrong array length, a missing required tag) and want one error path.
    void fail() { failed_ = true; }

private:
    bool need(usize bytes);
    u8 u8At();
    u16 u16At();
    u32 u32At();
    u64 u64At();
    bool skipValueDepth(TagType type, int depth);

    ConstByteSpan data_;
    usize pos_ = 0;
    bool failed_ = false;
};

// Checks the type of the tag nextField() just reported, marking the document
// malformed if it is not what the decoder expects.
//
// This has to be an error rather than something to preserve. A decoder that
// models a name writes that name back unconditionally, so carrying a wrongly
// typed copy through as an unknown tag would emit two tags with the same name.
// It is also the right answer on the merits: `Health` as an Int is not an
// a1.1.2 level.dat, and loading it as one would write a mangled file back over
// somebody's world.
inline bool expectType(Reader& reader, TagType actual, TagType want)
{
    if (actual != want) {
        reader.fail();
        return false;
    }
    return true;
}

}  // namespace mc::nbt
