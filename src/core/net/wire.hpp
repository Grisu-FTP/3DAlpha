#pragma once

// Big-endian primitives and Java's modified UTF-8, which is everything a
// protocol-2 packet is built from.
//
// a1.1.2 writes packets through `DataOutputStream` and reads them back through
// `DataInputStream`, so the wire is Java's own: big-endian integers, IEEE-754
// floats in the same order, and strings as `writeUTF` -- a *byte* length and
// **modified** UTF-8. The modification is two rules and both are here: U+0000
// is the two bytes `C0 80`, and a character outside the BMP is its UTF-16
// surrogate pair with each half written as a three-byte sequence. Plain UTF-8
// reads the same for ASCII, which is exactly why it is the classic trap.
//
// The reader never throws and never reads past its span: every getter returns
// false when there are not enough bytes, which is how the packet parser tells
// "wait for more" from "this is broken".

#include "core/util/types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace mc::net {

class ByteWriter {
public:
    explicit ByteWriter(std::vector<u8>* out) : out_(out) {}

    void putU8(u8 v) { out_->push_back(v); }
    void putI16(i16 v);
    void putI32(i32 v);
    void putI64(i64 v);
    void putF32(float v);
    void putF64(double v);
    void putBytes(const u8* data, usize size);

    // `writeUTF`. False, with nothing written, when the encoded form is longer
    // than the u16 length prefix can say -- Java throws there.
    bool putString(std::string_view utf8);

private:
    std::vector<u8>* out_;
};

class ByteReader {
public:
    ByteReader(const u8* data, usize size) : data_(data), size_(size) {}

    bool getU8(u8* out);
    bool getI8(i8* out);
    bool getI16(i16* out);
    bool getI32(i32* out);
    bool getI64(i64* out);
    bool getF32(float* out);
    bool getF64(double* out);

    // Borrows `size` bytes out of the span without copying them.
    bool getBytes(usize size, const u8** out);

    // `readUTF`. `*malformed` is set when the bytes are there but are not
    // modified UTF-8 -- Java's UTFDataFormatException -- as against simply not
    // having arrived yet.
    bool getString(std::string* utf8, bool* malformed);

    usize consumed() const { return pos_; }
    usize remaining() const { return size_ - pos_; }

private:
    const u8* data_;
    usize size_;
    usize pos_ = 0;
};

// Appends the modified UTF-8 form of `utf8`. Invalid UTF-8 in the input becomes
// U+FFFD rather than failing: the text comes from a keyboard or a card, and a
// chat line with one bad byte in it should still be sent.
void encodeModifiedUtf8(std::string_view utf8, std::vector<u8>* out);

// Decodes modified UTF-8 into ordinary UTF-8. False on a sequence Java's
// `readUTF` would reject. A lone surrogate becomes U+FFFD; a proper pair
// becomes the four-byte character it stands for.
bool decodeModifiedUtf8(const u8* data, usize size, std::string* utf8);

// One code point off the front of a UTF-8 string, advancing `*pos`. Invalid
// bytes come back as U+FFFD and consume one byte.
u32 nextUtf8(std::string_view text, usize* pos);

// Appends `cp` as UTF-8.
void appendUtf8(u32 cp, std::string* out);

}  // namespace mc::net
