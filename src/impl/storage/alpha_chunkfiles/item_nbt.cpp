#include "impl/storage/alpha_chunkfiles/item_nbt.hpp"

namespace mc::alpha {

bool decodeItemStack(nbt::Reader& r, item::ItemStack* stack)
{
    nbt::TagType type;
    std::string_view name;

    while (r.nextField(&type, &name)) {
        if (name == "Slot") {
            if (!nbt::expectType(r, type, nbt::TagType::Byte)) return false;
            stack->slot = r.byteValue();
        } else if (name == "id") {
            if (!nbt::expectType(r, type, nbt::TagType::Short)) return false;
            stack->id = r.shortValue();
        } else if (name == "Count") {
            if (!nbt::expectType(r, type, nbt::TagType::Byte)) return false;
            stack->count = r.byteValue();
        } else if (name == "Damage") {
            if (!nbt::expectType(r, type, nbt::TagType::Short)) return false;
            stack->damage = r.shortValue();
        } else if (!stack->preserved.capture(r, name, type)) {
            return false;
        }
    }
    return r.ok();
}

void encodeItemStack(nbt::Writer& w, const item::ItemStack& stack)
{
    w.writeByte("Slot", stack.slot);
    w.writeShort("id", stack.id);
    w.writeByte("Count", stack.count);
    w.writeShort("Damage", stack.damage);
    stack.preserved.writeTo(w);
}

}  // namespace mc::alpha
