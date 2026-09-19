#pragma once

// The protocol-2 packet table: every packet a1.1.2 and server 0.2.1 register,
// its shape as a list of wire fields, and one parser and one encoder driven by
// that list.
//
// **Read off the jars rather than off a wiki.** `fn` in the client and `hp` in
// the server hold the id -> class map (33 entries each, identical), and each
// class's `a(DataInputStream)` and `a(DataOutputStream)` are the field order.
// docs/protocol-a1.1.2.md records the corrections that reading made to the
// inferred layouts -- the most consequential being that 0x0B/0x0D carry
// `x, y, stance, z` towards the server but `x, stance, y, z` away from it.
//
// **One table, not one per direction.** Protocol 2 has a single registry, and
// every class reads and writes the same bytes whichever end it is on, so a
// second table would only be a second copy that could disagree. Which packets
// a client may expect is a question for the session, not the codec.
//
// **No id literal outside this table.** Code that builds or dispatches on a
// packet names it through `packet::`, so adding a1.2.6 is a second table and a
// version-selected pointer, not a search through the session.
//
// A packet has no length prefix, so the parser has to know a packet's shape to
// find the next one. It is written to be called on a buffer that may end in the
// middle of a packet: `NeedMore` consumes nothing, and the caller tries again
// when more bytes arrive.

#include "core/util/types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace mc::net {

enum class FieldType : u8 {
    Bool,          // one byte, non-zero is true
    Byte,          // i8
    UByte,         // `read()`, 0..255
    Short,         // i16
    Int,           // i32
    Long,          // i64
    Float,         // f32
    Double,        // f64
    String,        // u16 length + modified UTF-8
    ItemStacks,    // i16 count, then per slot i16 id (-1 empty) [i8 count, i16 damage]
    IntBytes,      // i32 length + bytes (0x33)
    ShortBytes,    // i16 length + bytes (0x3B)
    BlockChanges,  // i16 n, n x i16 packed coordinates, n x u8 ids, n x u8 metadata (0x34)
};

inline constexpr int kMaxFields = 10;

struct PacketShape {
    const char* name = nullptr;
    u8 fieldCount = 0;
    FieldType fields[kMaxFields] = {};
};

namespace packet {
inline constexpr u8 KeepAlive = 0x00;
inline constexpr u8 Login = 0x01;
inline constexpr u8 Handshake = 0x02;
inline constexpr u8 Chat = 0x03;
inline constexpr u8 TimeUpdate = 0x04;
inline constexpr u8 PlayerInventory = 0x05;
inline constexpr u8 SpawnPosition = 0x06;
// **Not in a1.1.2's registry, and ours on purpose.** Use Entity arrives at
// protocol 4; at protocol 2 there is no way to tell a server you hit something,
// which is why vanilla's monsters "could only be damaged by fire" in the
// multiplayer of this era. Between two 3DAlpha consoles that is a technical
// hole rather than a design, so the id protocol 4 gave it carries the same
// three fields here -- and it is **never sent to a Java server**, which would
// find an id its table does not have and end the connection. See
// `NetPlay::allowExtensions`.
inline constexpr u8 UseEntity = 0x07;
// **Ours, as `UseEntity` is: the respawn**, protocol 5's, with no fields --
// `jk` in a1.2.6 and `kr` in b1.2, which `of.u()` sends when the death screen's
// button is pressed. Protocol 2 has
// no death on the wire, so a player that died on its own console stayed
// standing on everybody else's. Towards the host it says "I am alive again";
// the host answers the others with a fresh Named Entity Spawn, which is how a
// body that fell over and went is brought back.
inline constexpr u8 Respawn = 0x09;
inline constexpr u8 Flying = 0x0A;
inline constexpr u8 PlayerPosition = 0x0B;
inline constexpr u8 PlayerLook = 0x0C;
inline constexpr u8 PlayerPositionLook = 0x0D;
inline constexpr u8 BlockDig = 0x0E;
inline constexpr u8 Place = 0x0F;
inline constexpr u8 BlockItemSwitch = 0x10;
inline constexpr u8 AddToInventory = 0x11;
inline constexpr u8 ArmAnimation = 0x12;
// **Ours: the crouch**, b1.2's `on` at its own id and shape -- the entity and a
// byte, 1 when `of.W()` sees the sneak start and 2 when it ends. a1.1.2's
// `isSneaking` is a hard-wired false, so it never needed telling anybody; this
// port sneaks, and a crouch nobody else could see was a stance only its owner
// knew about. Client to server as b1.2 has it, and server to client the other
// way round, as `UseEntity` is: "this entity crouched" -- b1.2 says that with
// `0x28`'s metadata, which is a format of its own for one bit.
inline constexpr u8 EntityAction = 0x13;
inline constexpr u8 NamedEntitySpawn = 0x14;
inline constexpr u8 PickupSpawn = 0x15;
inline constexpr u8 Collect = 0x16;
inline constexpr u8 VehicleSpawn = 0x17;
inline constexpr u8 MobSpawn = 0x18;
inline constexpr u8 DestroyEntity = 0x1D;
inline constexpr u8 Entity = 0x1E;
inline constexpr u8 RelEntityMove = 0x1F;
inline constexpr u8 EntityLook = 0x20;
inline constexpr u8 RelEntityMoveLook = 0x21;
inline constexpr u8 EntityTeleport = 0x22;
// **Ours, as `UseEntity` is.** Entity Status arrives at protocol 5, and at
// protocol 2 nothing tells a client an animal was hit or died: a guest saw a
// cow it had just punched take no notice and then vanish. Between two 3DAlpha
// consoles the id protocol 5 gave it carries protocol 5's two fields -- the
// entity and 2 for a hit, 3 for a death -- and only a 3DAlpha host sends it, so
// a Java 0.2.1 server never reaches the row.
//
// **A guest sends it too, about itself**: 3 when its own player dies. Health
// lives on each console (see `net::IncomingHit`), so the console that died is
// the only one that knows, and the host passes it on.
inline constexpr u8 EntityStatus = 0x26;
inline constexpr u8 PreChunk = 0x32;
inline constexpr u8 MapChunk = 0x33;
inline constexpr u8 MultiBlockChange = 0x34;
inline constexpr u8 BlockChange = 0x35;
inline constexpr u8 ComplexEntity = 0x3B;
inline constexpr u8 KickDisconnect = 0xFF;
}  // namespace packet

// Null for an id the registry does not have, which ends the connection: with
// no length prefix there is no way to skip it.
const PacketShape* shapeOf(u8 id);

// `gp` on the server, `ev` on the client, as the wire carries it.
struct WireStack {
    i16 id = -1;
    i8 count = 0;
    i16 damage = 0;

    bool empty() const { return id < 0; }
};

// A decoded packet. Fields land in the order the shape lists them, sorted by
// kind: every integer-typed field (Bool through Long) in `ints`, every float
// in `reals`, every string in `strings`. So a Named Entity Spawn's
// `i32 id, string name, i32 x, i32 y, i32 z, i8 yaw, i8 pitch, i16 item` is
// `ints = {id, x, y, z, yaw, pitch, item}` and `strings = {name}`.
struct Packet {
    u8 id = 0;
    u8 intCount = 0;
    u8 realCount = 0;
    u8 stringCount = 0;
    i64 ints[kMaxFields] = {};
    double reals[kMaxFields] = {};
    std::string strings[2];
    std::vector<WireStack> stacks;  // ItemStacks
    std::vector<u8> bytes;          // IntBytes / ShortBytes
    std::vector<i16> changeCoords;  // BlockChanges
    std::vector<u8> changeIds;
    std::vector<u8> changeData;

    i64 integer(int n) const { return n < intCount ? ints[n] : 0; }
    double real(int n) const { return n < realCount ? reals[n] : 0.0; }
    const std::string& text(int n) const { return strings[n < 2 ? n : 1]; }

    // Empties the value lists but keeps their capacity, so a parser reusing
    // one Packet stops allocating once the buffers have grown.
    void reset(u8 packetId);
    void pushInt(i64 v) { ints[intCount++] = v; }
    void pushReal(double v) { reals[realCount++] = v; }
    void pushString(std::string_view v) { strings[stringCount++] = std::string(v); }
};

enum class ParseResult {
    Ok,         // `*out` holds a packet and `*consumed` bytes of it were used
    NeedMore,   // the buffer ends inside a packet; nothing was consumed
    Malformed,  // the bytes can never be a packet (bad string, negative length)
    UnknownId,  // the first byte is not in the registry
};

// A single compressed chunk payload, or a whole 0x34, larger than this is taken
// as a corrupt stream rather than allocated. A full column compresses to a few
// kilobytes and its worst case is under 82 KB.
inline constexpr usize kMaxPayloadBytes = 4u << 20;

ParseResult parsePacket(const u8* data, usize size, Packet* out, usize* consumed);

// Appends the packet's wire form, id byte first. False when it does not match
// its shape (a value list of the wrong length, or a string too long to send).
bool encodePacket(const Packet& packet, std::vector<u8>* out);

// ---- what the client sends -------------------------------------------------

Packet makeKeepAlive();
Packet makeHandshake(std::string_view username);
// `hp(name, "Password", 2)` -- the client sends that literal as the password,
// and offline servers never read it.
Packet makeLogin(std::string_view username, int protocolVersion);
Packet makeChat(std::string_view text);
Packet makeFlying(bool onGround);
// Towards the server the order is x, feet (`boundingBox.minY`), eye (`posY`),
// z. The server refuses a stance -- eye minus feet -- outside 0.1..1.65.
Packet makePosition(double x, double feetY, double eyeY, double z, bool onGround);
Packet makeLook(float yaw, float pitch, bool onGround);
Packet makePositionLook(double x, double feetY, double eyeY, double z, float yaw, float pitch,
                        bool onGround);
Packet makeDig(int status, i32 x, int y, i32 z, int face);
Packet makePlace(int itemId, i32 x, int y, i32 z, int face);
Packet makeHoldingChange(int itemId);
Packet makeArmSwing(i32 entityId);
// See `packet::UseEntity`. `leftClick` false is a right click, which nothing
// sends yet.
Packet makeUseEntity(i32 fromEntityId, i32 toEntityId, bool leftClick);

// See `packet::EntityStatus`. `status` is `entity::kStatusHurt` or
// `entity::kStatusDead`.
Packet makeEntityStatus(i32 entityId, int status);

// See `packet::EntityAction`: `kActionCrouch` or `kActionUncrouch`.
inline constexpr int kActionCrouch = 1;
inline constexpr int kActionUncrouch = 2;
Packet makeEntityAction(i32 entityId, int action);
// See `packet::Respawn`.
Packet makeRespawn();
Packet makeInventory(int type, const WireStack* stacks, int count);
// Position in 1/32 of a block and motion in 1/128 of a block per tick, the way
// `ha(dx)` packs a thrown item: **the last three bytes are its velocity**, not a
// rotation, which is how the server's `id.a(k)` reads them back.
Packet makePickupSpawn(i32 entityId, int item, int count, i32 x, i32 y, i32 z, int motionX,
                       int motionY, int motionZ);
Packet makeDisconnect(std::string_view reason);

}  // namespace mc::net
