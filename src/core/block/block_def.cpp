#include "core/block/block_def.hpp"

namespace mc::block {

const char* renderTypeName(RenderType type)
{
    switch (type) {
    case RenderType::None:         return "none";
    case RenderType::Cube:         return "cube";
    case RenderType::Cross:        return "cross";
    case RenderType::Torch:        return "torch";
    case RenderType::Fire:         return "fire";
    case RenderType::Fluid:        return "fluid";
    case RenderType::RedstoneWire: return "redstone_wire";
    case RenderType::Crops:        return "crops";
    case RenderType::Door:         return "door";
    case RenderType::Ladder:       return "ladder";
    case RenderType::Rail:         return "rail";
    case RenderType::Stairs:       return "stairs";
    case RenderType::Fence:        return "fence";
    case RenderType::Lever:        return "lever";
    case RenderType::Cactus:       return "cactus";
    case RenderType::Count:        break;
    }
    return "unknown";
}

}  // namespace mc::block
