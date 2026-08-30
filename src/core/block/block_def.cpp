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

const char* tickBehaviourName(TickBehaviour behaviour)
{
    switch (behaviour) {
    case TickBehaviour::None:          return "none";
    case TickBehaviour::Grass:         return "grass";
    case TickBehaviour::Sapling:       return "sapling";
    case TickBehaviour::Leaves:        return "leaves";
    case TickBehaviour::Plant:         return "plant";
    case TickBehaviour::Mushroom:      return "mushroom";
    case TickBehaviour::Crops:         return "crops";
    case TickBehaviour::Farmland:      return "farmland";
    case TickBehaviour::Reed:          return "reed";
    case TickBehaviour::Cactus:        return "cactus";
    case TickBehaviour::FluidFlowing:  return "fluid_flowing";
    case TickBehaviour::FluidStill:    return "fluid_still";
    case TickBehaviour::Falling:       return "falling";
    case TickBehaviour::Fire:          return "fire";
    case TickBehaviour::Ice:           return "ice";
    case TickBehaviour::SnowLayer:     return "snow_layer";
    case TickBehaviour::SnowBlock:     return "snow_block";
    case TickBehaviour::Torch:         return "torch";
    case TickBehaviour::RedstoneTorch: return "redstone_torch";
    case TickBehaviour::RedstoneWire:  return "redstone_wire";
    case TickBehaviour::RedstoneOre:   return "redstone_ore";
    case TickBehaviour::Button:        return "button";
    case TickBehaviour::PressurePlate: return "pressure_plate";
    case TickBehaviour::Lever:         return "lever";
    case TickBehaviour::Door:          return "door";
    case TickBehaviour::Rail:          return "rail";
    case TickBehaviour::Ladder:        return "ladder";
    case TickBehaviour::Sign:          return "sign";
    case TickBehaviour::Tnt:           return "tnt";
    case TickBehaviour::Sponge:        return "sponge";
    case TickBehaviour::Stairs:        return "stairs";
    case TickBehaviour::Count:         break;
    }
    return "unknown";
}

}  // namespace mc::block
