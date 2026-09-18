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

const char* shapeName(Shape shape)
{
    switch (shape) {
    case Shape::None:     return "none";
    case Shape::FullCube: return "full_cube";
    case Shape::Slab:     return "slab";
    case Shape::Stairs:   return "stairs";
    case Shape::Door:     return "door";
    case Shape::Ladder:   return "ladder";
    case Shape::Fence:    return "fence";
    case Shape::Cactus:   return "cactus";
    case Shape::Count:    break;
    }
    return "unknown";
}

const char* contactName(Contact contact)
{
    switch (contact) {
    case Contact::Hurt:          return "hurt";
    case Contact::PressurePlate: return "pressure_plate";
    case Contact::None:          return "none";
    case Contact::Count:         break;
    }
    return "unknown";
}

const char* worldTextureName(WorldTexture texture)
{
    switch (texture) {
    case WorldTexture::None:  return "none";
    case WorldTexture::Chest: return "chest";
    case WorldTexture::Count: break;
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
    case TickBehaviour::PressurePlateAll:  return "pressure_plate_all";
    case TickBehaviour::PressurePlateMobs: return "pressure_plate_mobs";
    case TickBehaviour::Lever:         return "lever";
    case TickBehaviour::Door:          return "door";
    case TickBehaviour::Rail:          return "rail";
    case TickBehaviour::Ladder:        return "ladder";
    case TickBehaviour::SignPost:      return "sign_post";
    case TickBehaviour::SignWall:      return "sign_wall";
    case TickBehaviour::Tnt:           return "tnt";
    case TickBehaviour::Sponge:        return "sponge";
    case TickBehaviour::Stairs:        return "stairs";
    case TickBehaviour::Slab:          return "slab";
    case TickBehaviour::Furnace:       return "furnace";
    case TickBehaviour::MobSpawner:    return "mob_spawner";
    case TickBehaviour::Chest:         return "chest";
    case TickBehaviour::Workbench:     return "workbench";
    case TickBehaviour::Jukebox:       return "jukebox";
    case TickBehaviour::Count:         break;
    }
    return "unknown";
}

const char* sideRuleName(SideRule rule)
{
    switch (rule) {
    case SideRule::OwnKind:     return "own_kind";
    case SideRule::OwnMaterial: return "own_material";
    case SideRule::Slab:        return "slab";
    default:                    return "none";
    }
}

bool tileEntityBearing(TickBehaviour behaviour)
{
    switch (behaviour) {
    case TickBehaviour::Chest:
    case TickBehaviour::Furnace:
    case TickBehaviour::SignPost:
    case TickBehaviour::SignWall:
    case TickBehaviour::MobSpawner:
        return true;
    default:
        return false;
    }
}

}  // namespace mc::block
