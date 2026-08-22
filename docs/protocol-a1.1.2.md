# Minecraft Alpha 1.1.2 network protocol (protocol version 2)

Applies to clients **a1.1.0 – a1.1.2_01**, server **0.2.1**. Protocol version 2 was introduced
2010-09-10 and superseded by version 3 on 2010-10-31.

## Provenance and confidence

| Fact | Source | Confidence |
|---|---|---|
| Protocol version = 2 for a1.1.2 | wiki.vg protocol version table; minecraft.wiki | certain |
| Packet IDs and exact payload **sizes** | ViaLegacy `ClientboundPacketsa1_1_0` / `ServerboundPacketsa1_1_0` (a length table used by its frame splitter — it must be exact or the proxy desyncs) | certain |
| String and item-stack encoding | ViaLegacy `PreNettyTypes.readUtf` / `readItemStackb1_2` | certain |
| Which packets do **not** exist yet | wiki.vg Protocol History (2010-09-10 … 2010-12-01 entries) | certain |
| Field **names and order** within a packet | inferred from payload size + later-version layouts | **to be verified** |

> Field order is inferred. Before implementing the codec (milestone M5), pin it against a decompile
> of the real a1.1.2 jar (OrnitheMC feather + `gitcraft`) **and** a live capture against a real
> server. Record any correction here.

ViaLegacy is GPLv3. It is used here as documentation. Packet IDs and wire sizes are facts about a
2010 protocol, not copyrightable expression — but **do not copy its code**.

## Framing and data types

```
+--------+------------------+
| u8 id  | fields (varies)  |
+--------+------------------+
```

Big-endian. **No length prefix and no frame-level compression** — a reader must know each packet's
shape to find the next boundary. A malformed or unknown ID is unrecoverable; disconnect.

| Type | Encoding |
|---|---|
| `bool` | 1 byte, 0/1 |
| `i8/u8`, `i16`, `i32`, `i64` | big-endian |
| `f32`, `f64` | IEEE-754 big-endian |
| `string` | `u16 byteLength` + **Java modified UTF-8** bytes (`DataOutputStream.writeUTF`) |
| `itemstack` | `i16 id`; if `id >= 0` then `u8 count`, `i16 damage` |

**The string encoding is the classic trap.** Alpha uses modified UTF-8 with a *byte* length. The
switch to UCS-2 (`u16 charCount` + UTF-16BE) came later. Modified UTF-8 also differs from real
UTF-8: U+0000 is encoded as `0xC0 0x80`, and supplementary characters are encoded as surrogate
pairs. For a1.1.2 traffic (ASCII usernames and chat) plain UTF-8 is indistinguishable, but the
encoder must implement the modified form for correctness.

There is **no entity-metadata stream** in this era — that arrives with later protocols.

## Handshake and login

```
C -> S   0x02 Handshake   string username
S -> C   0x02 Handshake   string connectionHash    ; "-" means offline mode / no auth
C -> S   0x01 Login       i32 protocolVersion(=2), string username, string password
S -> C   0x01 Login       i32 entityId, string (unused), string (unused)
```

Mojang's session servers for this era are gone, so servers run offline: the hash is `-` and the
password field is ignored. Send an empty string for the password.

Note that the a1.1.2 Login packet carries **no map seed and no dimension** — those fields were added
in a1.2.0. A client that expects them will desync immediately.

## Keep-alive

`0x00` in both directions, **zero payload**. The payload `i32` was added much later. Send one every
second or so and treat a long silence as a dropped connection.

## Clientbound packets

| ID | Name | Payload |
|---|---|---|
| 0x00 | Keep Alive | *(none)* |
| 0x01 | Login | `i32 entityId`, `string`, `string` |
| 0x02 | Handshake | `string connectionHash` |
| 0x03 | Chat | `string` |
| 0x04 | Time Update | `i64 time` (ticks; 24000 per day) |
| 0x05 | Player Inventory | `i32 type`, `i16 count`, `itemstack[count]` |
| 0x06 | Spawn Position | `i32 x`, `i32 y`, `i32 z` |
| 0x0A | Player (on ground) | `bool onGround` |
| 0x0B | Player Position | `f64 x`, `f64 y`, `f64 stance`, `f64 z`, `bool onGround` |
| 0x0C | Player Look | `f32 yaw`, `f32 pitch`, `bool onGround` |
| 0x0D | Player Position & Look | `f64 x`, `f64 y`, `f64 stance`, `f64 z`, `f32 yaw`, `f32 pitch`, `bool onGround` |
| 0x10 | Holding Change | `i32 entityId`, `i16 itemId` |
| 0x11 | Add To Inventory | `i16 itemId`, `u8 count`, `i16 damage` |
| 0x12 | Animation | `i32 entityId`, `u8 animation` |
| 0x14 | Named Entity Spawn | `i32 entityId`, `string name`, `i32 x`, `i32 y`, `i32 z`, `u8 yaw`, `u8 pitch`, `i16 currentItem` |
| 0x15 | Pickup Spawn | `i32 entityId`, `i16 item`, `u8 count`, `i32 x`, `i32 y`, `i32 z`, `u8 yaw`, `u8 pitch`, `u8 roll` |
| 0x16 | Collect Item | `i32 collectedEntityId`, `i32 collectorEntityId` |
| 0x17 | Add Object/Vehicle | `i32 entityId`, `u8 type`, `i32 x`, `i32 y`, `i32 z` |
| 0x18 | Mob Spawn | `i32 entityId`, `u8 type`, `i32 x`, `i32 y`, `i32 z`, `u8 yaw`, `u8 pitch` |
| 0x1D | Destroy Entity | `i32 entityId` |
| 0x1E | Entity (no-op/keepalive) | `i32 entityId` |
| 0x1F | Entity Relative Move | `i32 entityId`, `i8 dx`, `i8 dy`, `i8 dz` |
| 0x20 | Entity Look | `i32 entityId`, `u8 yaw`, `u8 pitch` |
| 0x21 | Entity Look & Relative Move | `i32 entityId`, `i8 dx`, `i8 dy`, `i8 dz`, `u8 yaw`, `u8 pitch` |
| 0x22 | Entity Teleport | `i32 entityId`, `i32 x`, `i32 y`, `i32 z`, `u8 yaw`, `u8 pitch` |
| 0x32 | Pre-Chunk | `i32 chunkX`, `i32 chunkZ`, `bool mode` (1 = init, 0 = unload) |
| 0x33 | Map Chunk | `i32 x`, `i16 y`, `i32 z`, `u8 sizeX-1`, `u8 sizeY-1`, `u8 sizeZ-1`, `i32 compressedLength`, `u8[compressedLength]` |
| 0x34 | Multi Block Change | `i32 chunkX`, `i32 chunkZ`, `i16 n`, `i16[n] coords`, `u8[n] blockTypes`, `u8[n] metadata` |
| 0x35 | Block Change | `i32 x`, `u8 y`, `i32 z`, `u8 blockType`, `u8 metadata` |
| 0x3B | Complex Entity (tile entity) | `i32 x`, `i16 y`, `i32 z`, `u16 payloadLength`, `u8[payloadLength]` |
| 0xFF | Disconnect / Kick | `string reason` |

### 0x32 / 0x33 chunk streaming

A `0x33` is only valid after the matching `0x32` with `mode = 1` has allocated the column;
`mode = 0` unloads it. Positions are absolute block coordinates, not chunk coordinates.

The payload is a **raw zlib (deflate) stream**. Uncompressed it is, in order:

```
blocks      sizeX*sizeY*sizeZ bytes      (one block id per block)
metadata    ceil(n/2) bytes              (nibbles)
blockLight  ceil(n/2) bytes              (nibbles)
skyLight    ceil(n/2) bytes              (nibbles)
```
where `n = sizeX*sizeY*sizeZ` and index order is **YZX**, matching the on-disk chunk arrays:
`i = y + z*sizeY + x*sizeY*sizeZ`. A full column arrives as `16 x 128 x 16` (sizes are sent
minus one, so the wire carries `15, 127, 15`).

Multi-block-change coordinates pack into each `i16` as `x<<12 | z<<8 | y` relative to the chunk.

### 0x34 sizing note

The three arrays after the count are *separate* arrays, not interleaved records. Read all `n`
coordinates, then all `n` types, then all `n` metadata bytes.

## Serverbound packets

| ID | Name | Payload |
|---|---|---|
| 0x00 | Keep Alive | *(none)* |
| 0x01 | Login | `i32 protocolVersion` (2), `string username`, `string password` |
| 0x02 | Handshake | `string username` |
| 0x03 | Chat | `string` |
| 0x05 | Player Inventory | `i32 type`, `i16 count`, `itemstack[count]` |
| 0x0A | Player (on ground) | `bool onGround` |
| 0x0B | Player Position | `f64 x`, `f64 y`, `f64 stance`, `f64 z`, `bool onGround` |
| 0x0C | Player Look | `f32 yaw`, `f32 pitch`, `bool onGround` |
| 0x0D | Player Position & Look | `f64 x`, `f64 y`, `f64 stance`, `f64 z`, `f32 yaw`, `f32 pitch`, `bool onGround` |
| 0x0E | Player Digging | `u8 status`, `i32 x`, `u8 y`, `i32 z`, `u8 face` |
| 0x0F | Place / Use Item | `i16 itemId`, `i32 x`, `u8 y`, `i32 z`, `u8 direction` |
| 0x10 | Holding Change | `i32 unused`, `i16 itemId` |
| 0x12 | Arm Swing | `i32 entityId`, `u8 animation` |
| 0x15 | Pickup Spawn | *(as clientbound 0x15)* |
| 0x3B | Complex Entity | *(as clientbound 0x3B)* |
| 0xFF | Disconnect | `string reason` |

Note the **client→server position/look field order differs from the server→client one** in later
protocol versions (`stance` and `y` swap). For protocol 2 both directions are documented above with
the same order; this is one of the specific things to confirm against the decompiled client.

## Deliberately absent at protocol 2

Do not implement these, and do not expect a server to send them:

| Packet | Added in |
|---|---|
| 0x07 Use Entity | protocol 4 (2010-11-10) |
| 0x08 Update Health | protocol 5 (2010-11-24) |
| 0x09 Respawn | protocol 5 |
| 0x1C Entity Velocity | protocol 4 |
| 0x26 Entity Status | protocol 5 |
| 0x27 Attach Entity | protocol 4 |
| 0x3C Explosion | protocol 6 (2010-12-01) |
| 0x64–0x6B window/inventory family | Beta |

Two consequences for gameplay:

1. **There is no server-side health or damage in a1.1.2 multiplayer.** Nothing sends health, and
   there is no respawn packet. Damage on servers arrived with a1.2.x.
2. **There are no inventory windows.** Chests, furnaces and crafting tables are not negotiated over
   the wire; the server resyncs the whole player inventory with `0x05`. Anything resembling a
   container UI in multiplayer must be built on `0x05` plus `0x3B` tile-entity payloads.

Mobs *are* sent (`0x18`), but at protocol 2 vanilla's monsters were still described as experimental
and were only damaged by fire.

## Implementation notes

- Codec is **table-driven**: a `PacketTable` maps ID → an array of field descriptors, one table per
  direction per protocol version. No ID literals anywhere else. Adding a1.2.6 is a second table.
- Decompress `0x33` payloads on the worker thread, never on core0. See
  [3ds-performance.md](3ds-performance.md) on inflate cost.
- The receive path must handle partial reads: sockets deliver arbitrary fragments, and without a
  length prefix the parser has to be resumable. Buffer until a full packet's shape is satisfied.
- 3DS sockets come from the SOC service (`socInit` with a `memalign(0x1000, …)` buffer), are
  non-blocking, and are polled with `poll()`. There is no `epoll`.
- 3DS Wi-Fi supports WPA2-PSK (AES) at best — no WPA3. Worth stating in user-facing docs when a
  connection fails before any packet is exchanged.

## Testing

- **Replay harness** (host, `tests/`): feed a captured byte log of a real a1.1.2 session through the
  codec and assert every packet parses and re-serialises **byte-identically**. This is the only way
  to be confident about inferred field order.
- **Local server**: run the original `minecraft_server` 0.2.1 for deterministic tests.
- **Live**: a public Betacraft server for reality checks.
- **Bridge**: ViaProxy can translate a modern server down to protocol 2 for additional coverage.
