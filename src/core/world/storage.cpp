#include "core/world/storage.hpp"

namespace mc::world {

const char* describeOpenResult(OpenResult result)
{
    switch (result) {
    case OpenResult::Ok:        return "ok";
    case OpenResult::NotAWorld: return "not a world folder";
    case OpenResult::Corrupt:   return "level.dat is unreadable";
    case OpenResult::Locked:    return "the world is open somewhere else";
    case OpenResult::IoError:   return "the card could not be read";
    }
    return "unknown";
}

}  // namespace mc::world
