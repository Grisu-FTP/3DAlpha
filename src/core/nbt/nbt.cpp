#include "core/nbt/nbt.hpp"

#include <cstring>

namespace mc::nbt {

const char* tagTypeName(TagType type)
{
    switch (type) {
        case TagType::End: return "End";
        case TagType::Byte: return "Byte";
        case TagType::Short: return "Short";
        case TagType::Int: return "Int";
        case TagType::Long: return "Long";
        case TagType::Float: return "Float";
        case TagType::Double: return "Double";
        case TagType::ByteArray: return "ByteArray";
        case TagType::String: return "String";
        case TagType::List: return "List";
        case TagType::Compound: return "Compound";
        case TagType::IntArray: return "IntArray";
        case TagType::LongArray: return "LongArray";
    }
    return "?";
}

bool Reader::need(usize bytes)
{
    if (failed_ || bytes > data_.size() - pos_) {
        failed_ = true;
        return false;
    }
    return true;
}

u8 Reader::u8At()
{
    if (!need(1)) {
        return 0;
    }
    return data_[pos_++];
}

u16 Reader::u16At()
{
    if (!need(2)) {
        return 0;
    }
    const u16 v = static_cast<u16>((static_cast<u16>(data_[pos_]) << 8) | data_[pos_ + 1]);
    pos_ += 2;
    return v;
}

u32 Reader::u32At()
{
    if (!need(4)) {
        return 0;
    }
    const u32 v = (static_cast<u32>(data_[pos_]) << 24) | (static_cast<u32>(data_[pos_ + 1]) << 16) |
                  (static_cast<u32>(data_[pos_ + 2]) << 8) | static_cast<u32>(data_[pos_ + 3]);
    pos_ += 4;
    return v;
}

u64 Reader::u64At()
{
    const u64 hi = u32At();
    const u64 lo = u32At();
    return (hi << 32) | lo;
}

ConstByteSpan Reader::rangeSince(usize start) const
{
    if (failed_ || start > pos_) {
        return {};
    }
    return data_.subspan(start, pos_ - start);
}

bool Reader::enterRoot(std::string_view* nameOut)
{
    const TagType type = static_cast<TagType>(u8At());
    if (failed_ || type != TagType::Compound) {
        failed_ = true;
        return false;
    }
    const std::string_view name = string();
    if (nameOut != nullptr) {
        *nameOut = name;
    }
    return ok();
}

bool Reader::nextField(TagType* type, std::string_view* name)
{
    const TagType t = static_cast<TagType>(u8At());
    if (failed_) {
        return false;
    }
    if (t == TagType::End) {
        return false;
    }
    if (t > TagType::LongArray) {
        failed_ = true;
        return false;
    }
    const std::string_view n = string();
    if (failed_) {
        return false;
    }
    if (type != nullptr) {
        *type = t;
    }
    if (name != nullptr) {
        *name = n;
    }
    return true;
}

bool Reader::skipToEnd()
{
    TagType type;
    while (nextField(&type, nullptr)) {
        skipValue(type);
    }
    return ok();
}

i8 Reader::byteValue()
{
    return static_cast<i8>(u8At());
}

i16 Reader::shortValue()
{
    return static_cast<i16>(u16At());
}

i32 Reader::intValue()
{
    return static_cast<i32>(u32At());
}

i64 Reader::longValue()
{
    return static_cast<i64>(u64At());
}

float Reader::floatValue()
{
    const u32 bits = u32At();
    float out;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

double Reader::doubleValue()
{
    const u64 bits = u64At();
    double out;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

ConstByteSpan Reader::byteArray()
{
    const i32 length = intValue();
    if (length < 0) {
        failed_ = true;
        return {};
    }
    const usize count = static_cast<usize>(length);
    if (!need(count)) {
        return {};
    }
    const ConstByteSpan out = data_.subspan(pos_, count);
    pos_ += count;
    return out;
}

std::string_view Reader::string()
{
    const usize length = u16At();
    if (!need(length)) {
        return {};
    }
    const std::string_view out(reinterpret_cast<const char*>(data_.data() + pos_), length);
    pos_ += length;
    return out;
}

Span<const i32> Reader::intArray()
{
    // Alpha files never contain this tag. It is here so the reader can walk a
    // later format's file without a second parser; the elements stay big-endian
    // in the buffer, so this only validates and measures the run.
    const i32 length = intValue();
    if (length < 0 || static_cast<usize>(length) > (data_.size() - pos_) / 4) {
        failed_ = true;
        return {};
    }
    const usize bytes = static_cast<usize>(length) * 4;
    const auto* base = reinterpret_cast<const i32*>(data_.data() + pos_);
    pos_ += bytes;
    return Span<const i32>(base, static_cast<usize>(length));
}

bool Reader::enterList(TagType* elemType, i32* count)
{
    const TagType t = static_cast<TagType>(u8At());
    const i32 n = intValue();
    if (failed_ || t > TagType::LongArray || n < 0) {
        failed_ = true;
        return false;
    }
    // A TAG_End element type is how an empty list is written; some writers pair
    // it with a non-zero count, which would be unreadable. Reject that rather
    // than looping on a type we cannot size.
    if (t == TagType::End && n != 0) {
        failed_ = true;
        return false;
    }
    if (elemType != nullptr) {
        *elemType = t;
    }
    if (count != nullptr) {
        *count = n;
    }
    return true;
}

bool Reader::skipValue(TagType type)
{
    return skipValueDepth(type, 0);
}

bool Reader::skipValueDepth(TagType type, int depth)
{
    if (depth > kMaxDepth) {
        failed_ = true;
        return false;
    }

    switch (type) {
        case TagType::End:
            return ok();
        case TagType::Byte:
            need(1) && (pos_ += 1, true);
            return ok();
        case TagType::Short:
            need(2) && (pos_ += 2, true);
            return ok();
        case TagType::Int:
        case TagType::Float:
            need(4) && (pos_ += 4, true);
            return ok();
        case TagType::Long:
        case TagType::Double:
            need(8) && (pos_ += 8, true);
            return ok();
        case TagType::ByteArray:
            byteArray();
            return ok();
        case TagType::String:
            string();
            return ok();
        case TagType::IntArray:
            intArray();
            return ok();
        case TagType::LongArray: {
            const i32 length = intValue();
            if (length < 0 || static_cast<usize>(length) > (data_.size() - pos_) / 8) {
                failed_ = true;
                return false;
            }
            pos_ += static_cast<usize>(length) * 8;
            return ok();
        }
        case TagType::List: {
            TagType elem;
            i32 count;
            if (!enterList(&elem, &count)) {
                return false;
            }
            for (i32 i = 0; i < count; ++i) {
                if (!skipValueDepth(elem, depth + 1)) {
                    return false;
                }
            }
            return ok();
        }
        case TagType::Compound: {
            TagType member;
            while (nextField(&member, nullptr)) {
                if (!skipValueDepth(member, depth + 1)) {
                    return false;
                }
            }
            return ok();
        }
    }

    failed_ = true;
    return false;
}

}  // namespace mc::nbt
