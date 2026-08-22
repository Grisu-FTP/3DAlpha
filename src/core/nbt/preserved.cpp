#include "core/nbt/preserved.hpp"

namespace mc::nbt {

bool PreservedTags::capture(Reader& reader, std::string_view name, TagType type)
{
    const usize start = reader.offset();
    if (!reader.skipValue(type)) {
        return false;
    }

    const ConstByteSpan payload = reader.rangeSince(start);
    if (payload.empty() && type != TagType::End) {
        // rangeSince returns nothing only when the reader has failed; an empty
        // payload for a real tag would silently drop it.
        return false;
    }

    PreservedTag tag;
    tag.name.assign(name.data(), name.size());
    tag.type = type;
    tag.payload.assign(payload.begin(), payload.end());
    tags_.push_back(std::move(tag));
    return true;
}

void PreservedTags::writeTo(Writer& writer) const
{
    for (const PreservedTag& tag : tags_) {
        writer.writeRaw(tag.name, tag.type, tag.payload);
    }
}

const PreservedTag* PreservedTags::find(std::string_view name) const
{
    for (const PreservedTag& tag : tags_) {
        if (tag.name == name) {
            return &tag;
        }
    }
    return nullptr;
}

usize PreservedTags::memoryUsage() const
{
    usize bytes = tags_.capacity() * sizeof(PreservedTag);
    for (const PreservedTag& tag : tags_) {
        bytes += tag.name.capacity() + tag.payload.capacity();
    }
    return bytes;
}

}  // namespace mc::nbt
