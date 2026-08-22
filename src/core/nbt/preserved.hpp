#pragma once

// Tags this build reads but does not model, carried verbatim across a save.
//
// Real worlds are not written only by the client that reads them. Servers and
// third-party tools of the era added their own tags, and later game versions
// added more; a chunk loaded and saved by us must come back with all of them
// intact. Losing one is silent world corruption that only shows up when the
// world is opened somewhere else.
//
// It is also how unfinished subsystems stay honest. Entities and TileEntities
// are captured here until there is an entity system to decode them into, so
// round-trip safety does not have to wait for M3.
//
// Usage is the default branch of a read loop:
//
//   while (r.nextField(&type, &name)) {
//       if (name == "xPos" && type == TagType::Int) x = r.intValue();
//       else if (!preserved.capture(r, name, type)) return false;
//   }
//
// and one call on the way out:
//
//   preserved.writeTo(w);

#include "core/nbt/nbt.hpp"
#include "core/nbt/writer.hpp"
#include "core/util/types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace mc::nbt {

struct PreservedTag {
    std::string name;
    TagType type;
    std::vector<u8> payload;  // the value only, with no type byte and no name
};

class PreservedTags {
public:
    // Copies the value the reader is positioned on and advances past it.
    // Returns false if the value could not be skipped, i.e. the document is
    // malformed -- in which case the reader has already failed too.
    bool capture(Reader& reader, std::string_view name, TagType type);

    void writeTo(Writer& writer) const;

    bool empty() const { return tags_.empty(); }
    usize size() const { return tags_.size(); }
    const std::vector<PreservedTag>& tags() const { return tags_; }
    // Non-null when a tag of that name was captured. Lets a subsystem claim a
    // tag later without a second parse.
    const PreservedTag* find(std::string_view name) const;

    void clear() { tags_.clear(); }

    usize memoryUsage() const;

private:
    std::vector<PreservedTag> tags_;
};

}  // namespace mc::nbt
