// Java's DataOutputStream/DataInputStream primitives and modified UTF-8.
// See wire.hpp.

#include "core/net/wire.hpp"

#include <cstring>

namespace mc::net {

namespace {

constexpr u32 kReplacement = 0xFFFD;

}  // namespace

void ByteWriter::putI16(i16 v)
{
    const u16 u = u16(v);
    out_->push_back(u8(u >> 8));
    out_->push_back(u8(u));
}

void ByteWriter::putI32(i32 v)
{
    const u32 u = u32(v);
    for (int shift = 24; shift >= 0; shift -= 8) {
        out_->push_back(u8(u >> shift));
    }
}

void ByteWriter::putI64(i64 v)
{
    const u64 u = u64(v);
    for (int shift = 56; shift >= 0; shift -= 8) {
        out_->push_back(u8(u >> shift));
    }
}

void ByteWriter::putF32(float v)
{
    u32 bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    putI32(i32(bits));
}

void ByteWriter::putF64(double v)
{
    u64 bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    putI64(i64(bits));
}

void ByteWriter::putBytes(const u8* data, usize size)
{
    out_->insert(out_->end(), data, data + size);
}

bool ByteWriter::putString(std::string_view utf8)
{
    const usize start = out_->size();
    out_->push_back(0);
    out_->push_back(0);
    encodeModifiedUtf8(utf8, out_);
    const usize length = out_->size() - start - 2;
    if (length > 0xFFFF) {
        out_->resize(start);
        return false;
    }
    (*out_)[start] = u8(length >> 8);
    (*out_)[start + 1] = u8(length);
    return true;
}

bool ByteReader::getU8(u8* out)
{
    if (remaining() < 1) {
        return false;
    }
    *out = data_[pos_++];
    return true;
}

bool ByteReader::getI8(i8* out)
{
    u8 v = 0;
    if (!getU8(&v)) {
        return false;
    }
    *out = i8(v);
    return true;
}

bool ByteReader::getI16(i16* out)
{
    if (remaining() < 2) {
        return false;
    }
    *out = i16(u16((u16(data_[pos_]) << 8) | data_[pos_ + 1]));
    pos_ += 2;
    return true;
}

bool ByteReader::getI32(i32* out)
{
    if (remaining() < 4) {
        return false;
    }
    u32 v = 0;
    for (int i = 0; i < 4; ++i) {
        v = (v << 8) | data_[pos_ + usize(i)];
    }
    pos_ += 4;
    *out = i32(v);
    return true;
}

bool ByteReader::getI64(i64* out)
{
    if (remaining() < 8) {
        return false;
    }
    u64 v = 0;
    for (int i = 0; i < 8; ++i) {
        v = (v << 8) | data_[pos_ + usize(i)];
    }
    pos_ += 8;
    *out = i64(v);
    return true;
}

bool ByteReader::getF32(float* out)
{
    i32 bits = 0;
    if (!getI32(&bits)) {
        return false;
    }
    const u32 u = u32(bits);
    std::memcpy(out, &u, sizeof(u));
    return true;
}

bool ByteReader::getF64(double* out)
{
    i64 bits = 0;
    if (!getI64(&bits)) {
        return false;
    }
    const u64 u = u64(bits);
    std::memcpy(out, &u, sizeof(u));
    return true;
}

bool ByteReader::getBytes(usize size, const u8** out)
{
    if (remaining() < size) {
        return false;
    }
    *out = data_ + pos_;
    pos_ += size;
    return true;
}

bool ByteReader::getString(std::string* utf8, bool* malformed)
{
    *malformed = false;
    const usize start = pos_;
    i16 length = 0;
    if (!getI16(&length)) {
        return false;
    }
    const u8* bytes = nullptr;
    if (!getBytes(usize(u16(length)), &bytes)) {
        pos_ = start;
        return false;
    }
    if (!decodeModifiedUtf8(bytes, usize(u16(length)), utf8)) {
        *malformed = true;
        return false;
    }
    return true;
}

u32 nextUtf8(std::string_view text, usize* pos)
{
    const auto byteAt = [&](usize i) { return u8(text[i]); };
    const u8 lead = byteAt(*pos);
    if (lead < 0x80) {
        ++*pos;
        return lead;
    }

    int extra = 0;
    u32 cp = 0;
    u32 minimum = 0;
    if ((lead & 0xE0) == 0xC0) {
        extra = 1;
        cp = lead & 0x1F;
        minimum = 0x80;
    } else if ((lead & 0xF0) == 0xE0) {
        extra = 2;
        cp = lead & 0x0F;
        minimum = 0x800;
    } else if ((lead & 0xF8) == 0xF0) {
        extra = 3;
        cp = lead & 0x07;
        minimum = 0x10000;
    } else {
        ++*pos;
        return kReplacement;
    }

    if (*pos + usize(extra) >= text.size()) {
        ++*pos;
        return kReplacement;
    }
    for (int i = 1; i <= extra; ++i) {
        const u8 next = byteAt(*pos + usize(i));
        if ((next & 0xC0) != 0x80) {
            ++*pos;
            return kReplacement;
        }
        cp = (cp << 6) | (next & 0x3F);
    }
    if (cp < minimum || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        ++*pos;
        return kReplacement;
    }
    *pos += usize(extra) + 1;
    return cp;
}

void appendUtf8(u32 cp, std::string* out)
{
    if (cp < 0x80) {
        out->push_back(char(cp));
    } else if (cp < 0x800) {
        out->push_back(char(0xC0 | (cp >> 6)));
        out->push_back(char(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out->push_back(char(0xE0 | (cp >> 12)));
        out->push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back(char(0x80 | (cp & 0x3F)));
    } else {
        out->push_back(char(0xF0 | (cp >> 18)));
        out->push_back(char(0x80 | ((cp >> 12) & 0x3F)));
        out->push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back(char(0x80 | (cp & 0x3F)));
    }
}

void encodeModifiedUtf8(std::string_view utf8, std::vector<u8>* out)
{
    // One UTF-16 unit as `writeUTF` writes it: 0 and U+0080..U+07FF as two
    // bytes, the rest of the BMP -- surrogates included -- as three.
    const auto unit = [out](u32 u) {
        if (u != 0 && u < 0x80) {
            out->push_back(u8(u));
        } else if (u < 0x800) {
            out->push_back(u8(0xC0 | (u >> 6)));
            out->push_back(u8(0x80 | (u & 0x3F)));
        } else {
            out->push_back(u8(0xE0 | (u >> 12)));
            out->push_back(u8(0x80 | ((u >> 6) & 0x3F)));
            out->push_back(u8(0x80 | (u & 0x3F)));
        }
    };

    usize pos = 0;
    while (pos < utf8.size()) {
        const u32 cp = nextUtf8(utf8, &pos);
        if (cp >= 0x10000) {
            const u32 v = cp - 0x10000;
            unit(0xD800 | (v >> 10));
            unit(0xDC00 | (v & 0x3FF));
        } else {
            unit(cp);
        }
    }
}

bool decodeModifiedUtf8(const u8* data, usize size, std::string* utf8)
{
    utf8->clear();
    u32 pendingHigh = 0;

    const auto emit = [&](u32 unit) {
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            if (pendingHigh != 0) {
                appendUtf8(kReplacement, utf8);
            }
            pendingHigh = unit;
            return;
        }
        if (unit >= 0xDC00 && unit <= 0xDFFF) {
            if (pendingHigh != 0) {
                appendUtf8(0x10000 + ((pendingHigh - 0xD800) << 10) + (unit - 0xDC00), utf8);
                pendingHigh = 0;
            } else {
                appendUtf8(kReplacement, utf8);
            }
            return;
        }
        if (pendingHigh != 0) {
            appendUtf8(kReplacement, utf8);
            pendingHigh = 0;
        }
        appendUtf8(unit, utf8);
    };

    usize i = 0;
    while (i < size) {
        const u8 a = data[i];
        if (a < 0x80) {
            emit(a);
            i += 1;
        } else if ((a & 0xE0) == 0xC0) {
            if (i + 1 >= size || (data[i + 1] & 0xC0) != 0x80) {
                return false;
            }
            emit((u32(a & 0x1F) << 6) | (data[i + 1] & 0x3F));
            i += 2;
        } else if ((a & 0xF0) == 0xE0) {
            if (i + 2 >= size || (data[i + 1] & 0xC0) != 0x80 || (data[i + 2] & 0xC0) != 0x80) {
                return false;
            }
            emit((u32(a & 0x0F) << 12) | (u32(data[i + 1] & 0x3F) << 6) | (data[i + 2] & 0x3F));
            i += 3;
        } else {
            return false;
        }
    }
    if (pendingHigh != 0) {
        appendUtf8(kReplacement, utf8);
    }
    return true;
}

}  // namespace mc::net
