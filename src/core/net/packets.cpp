// The protocol-2 packet table and the codec it drives. See packets.hpp.

#include "core/net/packets.hpp"

#include "core/net/wire.hpp"

namespace mc::net {

namespace {

using F = FieldType;

struct Row {
    u8 id;
    PacketShape shape;
};

// Class names are the obfuscated ones in the client (`fn`'s map) and then the
// server (`hp`'s), so a row can be checked against either jar with javap.
constexpr Row kRows[] = {
    {packet::KeepAlive, {"KeepAlive gi/iz", 0, {}}},
    {packet::Login, {"Login hp/z", 3, {F::Int, F::String, F::String}}},
    {packet::Handshake, {"Handshake gt/e", 1, {F::String}}},
    {packet::Chat, {"Chat ij/ba", 1, {F::String}}},
    {packet::TimeUpdate, {"TimeUpdate du/fl", 1, {F::Long}}},
    {packet::PlayerInventory, {"PlayerInventory m/r", 2, {F::Int, F::ItemStacks}}},
    {packet::SpawnPosition, {"SpawnPosition ji/cb", 3, {F::Int, F::Int, F::Int}}},
    // Ours; see the note beside `packet::UseEntity`. The shape is protocol 4's
    // `Packet7UseEntity`, so if a1.2.6 is ever built the row moves rather than
    // changes.
    {packet::UseEntity, {"UseEntity (ours; protocol 4's 0x07)", 3, {F::Int, F::Int, F::Bool}}},
    // Ours; see the note beside `packet::Respawn`. Protocol 5's, which is empty.
    {packet::Respawn, {"Respawn (ours; protocol 5's 0x09)", 0, {}}},
    {packet::Flying, {"Flying eh/gf", 1, {F::Bool}}},
    {packet::PlayerPosition,
     {"PlayerPosition s/aa", 5, {F::Double, F::Double, F::Double, F::Double, F::Bool}}},
    {packet::PlayerLook, {"PlayerLook mh/fx", 3, {F::Float, F::Float, F::Bool}}},
    {packet::PlayerPositionLook,
     {"PlayerPositionLook ch/dq",
      7,
      {F::Double, F::Double, F::Double, F::Double, F::Float, F::Float, F::Bool}}},
    {packet::BlockDig, {"BlockDig fg/hd", 5, {F::UByte, F::Int, F::UByte, F::Int, F::UByte}}},
    {packet::Place, {"Place do/fe", 5, {F::Short, F::Int, F::UByte, F::Int, F::UByte}}},
    {packet::BlockItemSwitch, {"BlockItemSwitch dz/fv", 2, {F::Int, F::Short}}},
    {packet::AddToInventory, {"AddToInventory ld/en", 3, {F::Short, F::Byte, F::Short}}},
    {packet::ArmAnimation, {"ArmAnimation hf/o", 2, {F::Int, F::Byte}}},
    // Ours; see the note beside `packet::EntityAction`. b1.2's `on` -- a1.2.6
    // does not have it -- the entity and one byte.
    {packet::EntityAction, {"EntityAction (ours; b1.2's 0x13)", 2, {F::Int, F::Byte}}},
    {packet::NamedEntitySpawn,
     {"NamedEntitySpawn gp/c",
      8,
      {F::Int, F::String, F::Int, F::Int, F::Int, F::Byte, F::Byte, F::Short}}},
    {packet::PickupSpawn,
     {"PickupSpawn ha/k",
      9,
      {F::Int, F::Short, F::Byte, F::Int, F::Int, F::Int, F::Byte, F::Byte, F::Byte}}},
    {packet::Collect, {"Collect bm/ce", 2, {F::Int, F::Int}}},
    {packet::VehicleSpawn, {"VehicleSpawn kj/dl", 5, {F::Int, F::Byte, F::Int, F::Int, F::Int}}},
    {packet::MobSpawn,
     {"MobSpawn ez/gv", 7, {F::Int, F::Byte, F::Int, F::Int, F::Int, F::Byte, F::Byte}}},
    {packet::DestroyEntity, {"DestroyEntity ju/ct", 1, {F::Int}}},
    // Ours; see the note beside `packet::EntityStatus`. Protocol 5's
    // `Packet38EntityStatus`: the entity and one signed byte.
    {packet::EntityStatus, {"EntityStatus (ours; protocol 5's 0x26)", 2, {F::Int, F::Byte}}},
    {packet::Entity, {"Entity lq/ex", 1, {F::Int}}},
    {packet::RelEntityMove, {"RelEntityMove kp/dr", 4, {F::Int, F::Byte, F::Byte, F::Byte}}},
    {packet::EntityLook, {"EntityLook jx/cx", 3, {F::Int, F::Byte, F::Byte}}},
    {packet::RelEntityMoveLook,
     {"RelEntityMoveLook is/bg", 6, {F::Int, F::Byte, F::Byte, F::Byte, F::Byte, F::Byte}}},
    {packet::EntityTeleport,
     {"EntityTeleport jl/cf", 6, {F::Int, F::Int, F::Int, F::Int, F::Byte, F::Byte}}},
    {packet::PreChunk, {"PreChunk ka/da", 3, {F::Int, F::Int, F::Bool}}},
    // The three sizes are sent less one and read back plus one; the codec keeps
    // the wire value and chunk_payload adds the one.
    {packet::MapChunk,
     {"MapChunk bz/cz",
      7,
      {F::Int, F::Short, F::Int, F::UByte, F::UByte, F::UByte, F::IntBytes}}},
    {packet::MultiBlockChange,
     {"MultiBlockChange na/hh", 3, {F::Int, F::Int, F::BlockChanges}}},
    {packet::BlockChange,
     {"BlockChange li/et", 5, {F::Int, F::UByte, F::Int, F::UByte, F::UByte}}},
    {packet::ComplexEntity,
     {"ComplexEntity ny/ib", 4, {F::Int, F::Short, F::Int, F::ShortBytes}}},
    {packet::KickDisconnect, {"KickDisconnect oh/io", 1, {F::String}}},
};

}  // namespace

const PacketShape* shapeOf(u8 id)
{
    for (const Row& row : kRows) {
        if (row.id == id) {
            return &row.shape;
        }
    }
    return nullptr;
}

void Packet::reset(u8 packetId)
{
    id = packetId;
    intCount = 0;
    realCount = 0;
    stringCount = 0;
    strings[0].clear();
    strings[1].clear();
    stacks.clear();
    bytes.clear();
    changeCoords.clear();
    changeIds.clear();
    changeData.clear();
}

ParseResult parsePacket(const u8* data, usize size, Packet* out, usize* consumed)
{
    if (size == 0) {
        return ParseResult::NeedMore;
    }
    const PacketShape* shape = shapeOf(data[0]);
    if (shape == nullptr) {
        return ParseResult::UnknownId;
    }

    out->reset(data[0]);
    ByteReader in(data + 1, size - 1);

    for (int f = 0; f < shape->fieldCount; ++f) {
        switch (shape->fields[f]) {
        case F::Bool:
        case F::UByte: {
            u8 v = 0;
            if (!in.getU8(&v)) return ParseResult::NeedMore;
            out->pushInt(shape->fields[f] == F::Bool ? (v != 0 ? 1 : 0) : v);
            break;
        }
        case F::Byte: {
            i8 v = 0;
            if (!in.getI8(&v)) return ParseResult::NeedMore;
            out->pushInt(v);
            break;
        }
        case F::Short: {
            i16 v = 0;
            if (!in.getI16(&v)) return ParseResult::NeedMore;
            out->pushInt(v);
            break;
        }
        case F::Int: {
            i32 v = 0;
            if (!in.getI32(&v)) return ParseResult::NeedMore;
            out->pushInt(v);
            break;
        }
        case F::Long: {
            i64 v = 0;
            if (!in.getI64(&v)) return ParseResult::NeedMore;
            out->pushInt(v);
            break;
        }
        case F::Float: {
            float v = 0.0f;
            if (!in.getF32(&v)) return ParseResult::NeedMore;
            out->pushReal(double(v));
            break;
        }
        case F::Double: {
            double v = 0.0;
            if (!in.getF64(&v)) return ParseResult::NeedMore;
            out->pushReal(v);
            break;
        }
        case F::String: {
            bool malformed = false;
            std::string& s = out->strings[out->stringCount < 2 ? out->stringCount : 1];
            if (!in.getString(&s, &malformed)) {
                return malformed ? ParseResult::Malformed : ParseResult::NeedMore;
            }
            ++out->stringCount;
            break;
        }
        case F::ItemStacks: {
            i16 count = 0;
            if (!in.getI16(&count)) return ParseResult::NeedMore;
            if (count < 0) return ParseResult::Malformed;
            for (int i = 0; i < count; ++i) {
                WireStack stack;
                if (!in.getI16(&stack.id)) return ParseResult::NeedMore;
                if (stack.id >= 0) {
                    if (!in.getI8(&stack.count)) return ParseResult::NeedMore;
                    if (!in.getI16(&stack.damage)) return ParseResult::NeedMore;
                }
                out->stacks.push_back(stack);
            }
            break;
        }
        case F::IntBytes:
        case F::ShortBytes: {
            i64 length = 0;
            if (shape->fields[f] == F::IntBytes) {
                i32 v = 0;
                if (!in.getI32(&v)) return ParseResult::NeedMore;
                length = v;
            } else {
                i16 v = 0;
                if (!in.getI16(&v)) return ParseResult::NeedMore;
                length = v;
            }
            if (length < 0 || u64(length) > kMaxPayloadBytes) {
                return ParseResult::Malformed;
            }
            const u8* bytes = nullptr;
            if (!in.getBytes(usize(length), &bytes)) return ParseResult::NeedMore;
            out->bytes.assign(bytes, bytes + length);
            break;
        }
        case F::BlockChanges: {
            i16 n = 0;
            if (!in.getI16(&n)) return ParseResult::NeedMore;
            if (n < 0) return ParseResult::Malformed;
            const usize count = usize(n);
            if (in.remaining() < count * 4) return ParseResult::NeedMore;
            out->changeCoords.resize(count);
            for (usize i = 0; i < count; ++i) {
                in.getI16(&out->changeCoords[i]);
            }
            const u8* ids = nullptr;
            const u8* metadata = nullptr;
            in.getBytes(count, &ids);
            in.getBytes(count, &metadata);
            out->changeIds.assign(ids, ids + count);
            out->changeData.assign(metadata, metadata + count);
            break;
        }
        }
    }

    *consumed = 1 + in.consumed();
    return ParseResult::Ok;
}

bool encodePacket(const Packet& packet, std::vector<u8>* out)
{
    const PacketShape* shape = shapeOf(packet.id);
    if (shape == nullptr) {
        return false;
    }

    const usize start = out->size();
    ByteWriter w(out);
    w.putU8(packet.id);

    int ints = 0;
    int reals = 0;
    int strings = 0;
    const auto fail = [&]() {
        out->resize(start);
        return false;
    };

    for (int f = 0; f < shape->fieldCount; ++f) {
        const FieldType type = shape->fields[f];
        const bool integral = type == F::Bool || type == F::Byte || type == F::UByte
                              || type == F::Short || type == F::Int || type == F::Long;
        if (integral && ints >= packet.intCount) return fail();
        if ((type == F::Float || type == F::Double) && reals >= packet.realCount) return fail();
        if (type == F::String && strings >= packet.stringCount) return fail();

        switch (type) {
        case F::Bool:
            w.putU8(packet.ints[ints++] != 0 ? 1 : 0);
            break;
        case F::Byte:
        case F::UByte:
            w.putU8(u8(packet.ints[ints++]));
            break;
        case F::Short:
            w.putI16(i16(packet.ints[ints++]));
            break;
        case F::Int:
            w.putI32(i32(packet.ints[ints++]));
            break;
        case F::Long:
            w.putI64(packet.ints[ints++]);
            break;
        case F::Float:
            w.putF32(float(packet.reals[reals++]));
            break;
        case F::Double:
            w.putF64(packet.reals[reals++]);
            break;
        case F::String:
            if (!w.putString(packet.strings[strings++])) return fail();
            break;
        case F::ItemStacks:
            if (packet.stacks.size() > 0x7FFF) return fail();
            w.putI16(i16(packet.stacks.size()));
            for (const WireStack& stack : packet.stacks) {
                if (stack.id < 0) {
                    w.putI16(-1);
                } else {
                    w.putI16(stack.id);
                    w.putU8(u8(stack.count));
                    w.putI16(stack.damage);
                }
            }
            break;
        case F::IntBytes:
            w.putI32(i32(packet.bytes.size()));
            w.putBytes(packet.bytes.data(), packet.bytes.size());
            break;
        case F::ShortBytes:
            if (packet.bytes.size() > 0x7FFF) return fail();
            w.putI16(i16(packet.bytes.size()));
            w.putBytes(packet.bytes.data(), packet.bytes.size());
            break;
        case F::BlockChanges: {
            const usize n = packet.changeCoords.size();
            if (n > 0x7FFF || packet.changeIds.size() != n || packet.changeData.size() != n) {
                return fail();
            }
            w.putI16(i16(n));
            for (const i16 coord : packet.changeCoords) {
                w.putI16(coord);
            }
            w.putBytes(packet.changeIds.data(), n);
            w.putBytes(packet.changeData.data(), n);
            break;
        }
        }
    }
    if (ints != packet.intCount || reals != packet.realCount || strings != packet.stringCount) {
        return fail();
    }
    return true;
}

Packet makeKeepAlive()
{
    Packet p;
    p.reset(packet::KeepAlive);
    return p;
}

Packet makeHandshake(std::string_view username)
{
    Packet p;
    p.reset(packet::Handshake);
    p.pushString(username);
    return p;
}

Packet makeLogin(std::string_view username, int protocolVersion)
{
    Packet p;
    p.reset(packet::Login);
    p.pushInt(protocolVersion);
    p.pushString(username);
    p.pushString("Password");
    return p;
}

Packet makeChat(std::string_view text)
{
    Packet p;
    p.reset(packet::Chat);
    p.pushString(text);
    return p;
}

Packet makeFlying(bool onGround)
{
    Packet p;
    p.reset(packet::Flying);
    p.pushInt(onGround ? 1 : 0);
    return p;
}

Packet makePosition(double x, double feetY, double eyeY, double z, bool onGround)
{
    Packet p;
    p.reset(packet::PlayerPosition);
    p.pushReal(x);
    p.pushReal(feetY);
    p.pushReal(eyeY);
    p.pushReal(z);
    p.pushInt(onGround ? 1 : 0);
    return p;
}

Packet makeLook(float yaw, float pitch, bool onGround)
{
    Packet p;
    p.reset(packet::PlayerLook);
    p.pushReal(double(yaw));
    p.pushReal(double(pitch));
    p.pushInt(onGround ? 1 : 0);
    return p;
}

Packet makePositionLook(double x, double feetY, double eyeY, double z, float yaw, float pitch,
                        bool onGround)
{
    Packet p;
    p.reset(packet::PlayerPositionLook);
    p.pushReal(x);
    p.pushReal(feetY);
    p.pushReal(eyeY);
    p.pushReal(z);
    p.pushReal(double(yaw));
    p.pushReal(double(pitch));
    p.pushInt(onGround ? 1 : 0);
    return p;
}

Packet makeDig(int status, i32 x, int y, i32 z, int face)
{
    Packet p;
    p.reset(packet::BlockDig);
    p.pushInt(status);
    p.pushInt(x);
    p.pushInt(y);
    p.pushInt(z);
    p.pushInt(face);
    return p;
}

Packet makePlace(int itemId, i32 x, int y, i32 z, int face)
{
    Packet p;
    p.reset(packet::Place);
    p.pushInt(itemId);
    p.pushInt(x);
    p.pushInt(y);
    p.pushInt(z);
    p.pushInt(face);
    return p;
}

Packet makeUseItem(int itemId)
{
    return makePlace(itemId, -1, 255, -1, kUseItemFace);
}

bool isUseItem(const Packet& place)
{
    return place.id == packet::Place && place.integer(1) == -1 && place.integer(2) == 255
           && place.integer(3) == -1 && place.integer(4) == kUseItemFace;
}

Packet makeHoldingChange(int itemId)
{
    Packet p;
    p.reset(packet::BlockItemSwitch);
    p.pushInt(0);
    p.pushInt(itemId);
    return p;
}

Packet makeArmSwing(i32 entityId)
{
    Packet p;
    p.reset(packet::ArmAnimation);
    p.pushInt(entityId);
    p.pushInt(1);
    return p;
}

Packet makeUseEntity(i32 fromEntityId, i32 toEntityId, bool leftClick)
{
    Packet p;
    p.reset(packet::UseEntity);
    p.pushInt(fromEntityId);
    p.pushInt(toEntityId);
    p.pushInt(leftClick ? 1 : 0);
    return p;
}

Packet makeEntityStatus(i32 entityId, int status)
{
    Packet p;
    p.reset(packet::EntityStatus);
    p.pushInt(entityId);
    p.pushInt(status);
    return p;
}

Packet makeEntityAction(i32 entityId, int action)
{
    Packet p;
    p.reset(packet::EntityAction);
    p.pushInt(entityId);
    p.pushInt(action);
    return p;
}

Packet makeRespawn()
{
    Packet p;
    p.reset(packet::Respawn);
    return p;
}

Packet makeInventory(int type, const WireStack* stacks, int count)
{
    Packet p;
    p.reset(packet::PlayerInventory);
    p.pushInt(type);
    p.stacks.assign(stacks, stacks + count);
    return p;
}

Packet makePickupSpawn(i32 entityId, int item, int count, i32 x, i32 y, i32 z, int motionX,
                       int motionY, int motionZ)
{
    Packet p;
    p.reset(packet::PickupSpawn);
    p.pushInt(entityId);
    p.pushInt(item);
    p.pushInt(count);
    p.pushInt(x);
    p.pushInt(y);
    p.pushInt(z);
    p.pushInt(motionX);
    p.pushInt(motionY);
    p.pushInt(motionZ);
    return p;
}

Packet makeDisconnect(std::string_view reason)
{
    Packet p;
    p.reset(packet::KickDisconnect);
    p.pushString(reason);
    return p;
}

}  // namespace mc::net
