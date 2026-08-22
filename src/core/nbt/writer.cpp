#include "core/nbt/writer.hpp"

#include <cstring>

namespace mc::nbt {

bool Writer::startListElement()
{
    if (stack_.empty() || stack_.back().type != TagType::List) {
        failed_ = true;
        return false;
    }
    stack_.back().count++;
    return true;
}

void Writer::u8At(u8 value)
{
    out_.push_back(value);
}

void Writer::u16At(u16 value)
{
    out_.push_back(static_cast<u8>(value >> 8));
    out_.push_back(static_cast<u8>(value));
}

void Writer::u32At(u32 value)
{
    out_.push_back(static_cast<u8>(value >> 24));
    out_.push_back(static_cast<u8>(value >> 16));
    out_.push_back(static_cast<u8>(value >> 8));
    out_.push_back(static_cast<u8>(value));
}

void Writer::u64At(u64 value)
{
    u32At(static_cast<u32>(value >> 32));
    u32At(static_cast<u32>(value));
}

void Writer::rawString(std::string_view value)
{
    if (value.size() > 0xFFFF) {
        failed_ = true;
        return;
    }
    u16At(static_cast<u16>(value.size()));
    out_.insert(out_.end(), value.begin(), value.end());
}

void Writer::tag(TagType type, std::string_view name)
{
    // A named tag is only legal directly inside a compound. Emitting one inside
    // a list would produce a file that no reader can parse, so refuse instead.
    if (stack_.empty() || stack_.back().type != TagType::Compound) {
        failed_ = true;
        return;
    }
    u8At(static_cast<u8>(type));
    rawString(name);
}

void Writer::beginRoot(std::string_view name)
{
    if (!stack_.empty()) {
        failed_ = true;
        return;
    }
    u8At(static_cast<u8>(TagType::Compound));
    rawString(name);
    stack_.push_back({TagType::Compound, 0, 0});
}

void Writer::endRoot()
{
    endCompound();
    if (!stack_.empty()) {
        failed_ = true;
    }
}

void Writer::writeByte(std::string_view name, i8 value)
{
    tag(TagType::Byte, name);
    u8At(static_cast<u8>(value));
}

void Writer::writeShort(std::string_view name, i16 value)
{
    tag(TagType::Short, name);
    u16At(static_cast<u16>(value));
}

void Writer::writeInt(std::string_view name, i32 value)
{
    tag(TagType::Int, name);
    u32At(static_cast<u32>(value));
}

void Writer::writeLong(std::string_view name, i64 value)
{
    tag(TagType::Long, name);
    u64At(static_cast<u64>(value));
}

void Writer::writeFloat(std::string_view name, float value)
{
    tag(TagType::Float, name);
    u32 bits;
    std::memcpy(&bits, &value, sizeof(bits));
    u32At(bits);
}

void Writer::writeDouble(std::string_view name, double value)
{
    tag(TagType::Double, name);
    u64 bits;
    std::memcpy(&bits, &value, sizeof(bits));
    u64At(bits);
}

void Writer::writeByteArray(std::string_view name, ConstByteSpan value)
{
    tag(TagType::ByteArray, name);
    u32At(static_cast<u32>(value.size()));
    out_.insert(out_.end(), value.begin(), value.end());
}

void Writer::writeString(std::string_view name, std::string_view value)
{
    tag(TagType::String, name);
    rawString(value);
}

void Writer::writeIntArray(std::string_view name, Span<const i32> value)
{
    tag(TagType::IntArray, name);
    u32At(static_cast<u32>(value.size()));
    for (usize i = 0; i < value.size(); ++i) {
        u32At(static_cast<u32>(value[i]));
    }
}

void Writer::writeRaw(std::string_view name, TagType type, ConstByteSpan payload)
{
    tag(type, name);
    out_.insert(out_.end(), payload.begin(), payload.end());
}

void Writer::beginCompound(std::string_view name)
{
    tag(TagType::Compound, name);
    stack_.push_back({TagType::Compound, 0, 0});
}

void Writer::endCompound()
{
    if (stack_.empty() || stack_.back().type != TagType::Compound) {
        failed_ = true;
        return;
    }
    u8At(static_cast<u8>(TagType::End));
    stack_.pop_back();
}

void Writer::beginList(std::string_view name, TagType elemType)
{
    tag(TagType::List, name);
    u8At(static_cast<u8>(elemType));
    const usize countOffset = out_.size();
    u32At(0);  // patched by endList
    stack_.push_back({TagType::List, countOffset, 0});
}

void Writer::endList()
{
    if (stack_.empty() || stack_.back().type != TagType::List) {
        failed_ = true;
        return;
    }
    const Frame frame = stack_.back();
    stack_.pop_back();

    // An empty list is written with element type Byte, not TAG_End. That is
    // not a choice: Java's NBTTagList.write() sets tagType from the first
    // element, and falls back to 1 when there is none, so every empty list the
    // original game ever wrote carries a 1. Verified against a real a1.1.2
    // level.dat, whose empty Inventory reads back as Byte. Readers ignore the
    // type when the count is zero, so this costs nothing and keeps our output
    // byte-identical to the original's.
    if (frame.count == 0) {
        out_[frame.countOffset - 1] = static_cast<u8>(TagType::Byte);
    }

    const u32 count = static_cast<u32>(frame.count);
    out_[frame.countOffset + 0] = static_cast<u8>(count >> 24);
    out_[frame.countOffset + 1] = static_cast<u8>(count >> 16);
    out_[frame.countOffset + 2] = static_cast<u8>(count >> 8);
    out_[frame.countOffset + 3] = static_cast<u8>(count);
}

void Writer::beginListElementCompound()
{
    if (!startListElement()) {
        return;
    }
    stack_.push_back({TagType::Compound, 0, 0});
}

void Writer::beginListElementList(TagType elemType)
{
    if (!startListElement()) {
        return;
    }
    u8At(static_cast<u8>(elemType));
    const usize countOffset = out_.size();
    u32At(0);  // patched by endList
    stack_.push_back({TagType::List, countOffset, 0});
}

void Writer::listByteArray(ConstByteSpan value)
{
    if (!startListElement()) {
        return;
    }
    u32At(static_cast<u32>(value.size()));
    out_.insert(out_.end(), value.begin(), value.end());
}

void Writer::listByte(i8 value)
{
    if (!startListElement()) {
        return;
    }
    u8At(static_cast<u8>(value));
}

void Writer::listShort(i16 value)
{
    if (!startListElement()) {
        return;
    }
    u16At(static_cast<u16>(value));
}

void Writer::listInt(i32 value)
{
    if (!startListElement()) {
        return;
    }
    u32At(static_cast<u32>(value));
}

void Writer::listLong(i64 value)
{
    if (!startListElement()) {
        return;
    }
    u64At(static_cast<u64>(value));
}

void Writer::listFloat(float value)
{
    if (!startListElement()) {
        return;
    }
    u32 bits;
    std::memcpy(&bits, &value, sizeof(bits));
    u32At(bits);
}

void Writer::listDouble(double value)
{
    if (!startListElement()) {
        return;
    }
    u64 bits;
    std::memcpy(&bits, &value, sizeof(bits));
    u64At(bits);
}

void Writer::listString(std::string_view value)
{
    if (!startListElement()) {
        return;
    }
    rawString(value);
}

void Writer::listRaw(ConstByteSpan payload)
{
    if (!startListElement()) {
        return;
    }
    out_.insert(out_.end(), payload.begin(), payload.end());
}

}  // namespace mc::nbt
