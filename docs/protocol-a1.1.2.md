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
| Field **names and order** within a packet | `javap -c` of every packet class in the a1.1.2 client jar (registry `fn`) and the 0.2.1 server jar (registry `hp`) | **verified 2026-09-13** |
| The codec against a real server | `3dalpha --join` against server 0.2.1: login, 270+ columns compared block for block with the server's saved world, a dig and a place echoed back | **verified 2026-09-13** |

The table in `src/core/net/packets.cpp` is the authority now; each row names its client and server
class, so any line here can be re-checked with `javap -c` against either jar. Corrections the jars
made to what was inferred here are marked **Corrected** below.

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
password field is ignored. **Corrected:** the a1.1.2 client sends the literal string `Password`
(`hp(name, "Password", 2)` in `gy.a(gt)`), and so does 3DAlpha. Server 0.2.1 answers Login with
entity id **0** and two empty strings, then Spawn Position, the player's Position & Look, Time, and
the three inventory arrays.

**The first Position & Look puts the player 1.62 blocks up.** The server builds it as
`dq(x, posY + 1.62, posY, z)` and the a1.1.2 client takes the *second* field as its own `posY` --
so a client starts a block and a half above where the server has it and falls the moment it ticks.
Nothing corrects it; it is the game.

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
| 0x0B | Player Position | `f64 x`, `f64 stance`, `f64 y`, `f64 z`, `bool onGround` |
| 0x0C | Player Look | `f32 yaw`, `f32 pitch`, `bool onGround` |
| 0x0D | Player Position & Look | `f64 x`, `f64 stance`, `f64 y`, `f64 z`, `f32 yaw`, `f32 pitch`, `bool onGround` — **Corrected:** from the server the *eye* is second and the feet third, and the client reads the second field as `posY` and ignores the third (`gy.a(eh)`) |
| 0x10 | Holding Change | `i32 entityId`, `i16 itemId` |
| 0x11 | Add To Inventory | `i16 itemId`, `u8 count`, `i16 damage` |
| 0x12 | Animation | `i32 entityId`, `u8 animation` |
| 0x14 | Named Entity Spawn | `i32 entityId`, `string name`, `i32 x`, `i32 y`, `i32 z`, `u8 yaw`, `u8 pitch`, `i16 currentItem` |
| 0x15 | Pickup Spawn | `i32 entityId`, `i16 item`, `i8 count`, `i32 x`, `i32 y`, `i32 z`, `i8 motionX`, `i8 motionY`, `i8 motionZ` — **Corrected:** the last three are velocity in 1/128 of a block per tick (`ha(dx)`), not a rotation; positions are `floor(v × 32)` |
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
| 0x35 | Block Change | `i32 x`, `u8 y`, `i32 z`, `u8 blockType`, `u8 metadata` — all three single bytes are read with `read()`, so unsigned |
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
| 0x0B | Player Position | `f64 x`, `f64 y` (feet), `f64 stance` (eye), `f64 z`, `bool onGround` |
| 0x0C | Player Look | `f32 yaw`, `f32 pitch`, `bool onGround` |
| 0x0D | Player Position & Look | `f64 x`, `f64 y`, `f64 stance`, `f64 z`, `f32 yaw`, `f32 pitch`, `bool onGround` |
| 0x0E | Player Digging | `u8 status`, `i32 x`, `u8 y`, `i32 z`, `u8 face` — status 0 starts, 1 every tick while digging, 3 broken, 2 stopped (with zero coordinates) |
| 0x0F | Place / Use Item | `i16 itemId`, `i32 x`, `u8 y`, `i32 z`, `u8 direction` |
| 0x10 | Holding Change | `i32 unused`, `i16 itemId` |
| 0x12 | Arm Swing | `i32 entityId`, `u8 animation` |
| 0x15 | Pickup Spawn | *(as clientbound 0x15)* |
| 0x3B | Complex Entity | *(as clientbound 0x3B)* |
| 0xFF | Disconnect | `string reason` |

**Confirmed in protocol 2: the order already differs by direction.** Towards the server it is x,
feet, eye, z; away from it x, eye, feet, z. Both come from the jars, and the server checks it --
`id.a(gf)` kicks for "Illegal stance" when eye − feet leaves 0.1–1.65. The server also ignores every
move until the client has echoed its Position & Look back exactly, which is what `gy.a(eh)` does.

**What else the server checks, which a client has to satisfy:**

- Digging: statuses 0 and 1 are ignored unless the server's own 4-block raytrace from the player's
  eye along its look hits that block and face. The server counts the status-1 packets against the
  block's strength and breaks the block itself; the client's 3 makes it echo the result back.
- Spawn protection: within 16 blocks of the spawn point a non-op's dig and place are refused and the
  block is echoed back unchanged. `op <name>` on the server console lifts it.
- A place is answered with a Block Change for the target whether or not it worked.
- The inventory belongs to the client: every 20 ticks, if it changed, the client sends all three
  arrays (-1 main of 37, -2 crafting of 4, -3 armour of 4) and the server adopts them.

## Deliberately absent at protocol 2

Do not implement these, and do not expect a server to send them:

| Packet | Added in |
|---|---|
| 0x07 Use Entity | protocol 4 (2010-11-10) |
| 0x08 Update Health | protocol 5 (2010-11-24) |
| 0x09 Respawn | protocol 5 (a 3DAlpha host takes it from a guest; see the end of this file) |
| 0x1C Entity Velocity | protocol 4 |
| 0x26 Entity Status | protocol 5 (a 3DAlpha host sends it anyway; see the end of this file) |
| 0x13 Entity Action | after a1.2.6, by b1.2 (between two 3DAlpha consoles only; see the end of this file) |
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
- **Resolving a name is four attempts, and the last one is ours.** SOC's `gethostbyname` is not
  dependable: the first hardware session past the loopback connected to every numeric address and
  to no name at all. `TcpSocket::connect` now tries `inet_aton`, then `getaddrinfo`, then
  `gethostbyname`, then an A query to the console's own name servers — read out of
  `SOCU_GetNetworkOpt(SOL_CONFIG, NETOPT_DNS_TABLE)`, with the default gateway from
  `NETOPT_ROUTING_TABLE` appended. On a home network that last one *is* the router. See
  [core/net/dns.hpp](../src/core/net/dns.hpp).

## Entities

Positions on the wire are `floor(v * 32)`; a relative move is the same units in a byte; a rotation
is a byte of a whole turn (`byte * 360 / 256`); a Pickup Spawn's last three bytes are velocity in
1/128 of a block per tick. A player's `y` is the server's `posY`, which is the **feet** -- the bottom of the box. The eye is
that plus 1.62 and only ever appears as the stance field of a position packet.

`EntityOtherPlayerMP` does not jump to a position it is sent: `setPositionAndRotation2` is given
three ticks (`otherPlayerMPPosRotationIncrements`) and covers a third of what is left on each, which
is what makes another player walk rather than stutter. Nothing on the wire says whether they are
walking, so the limbs are driven by how far the body actually moved between ticks. A relative move
accumulates against the **last position the server sent**, not against where the body has got to.

**Sheep, cow and chicken are all EntityList id 91 in a1.1.2** -- read off `ew.<clinit>` in the
client jar, where `a(Class, String, int)` is called with 91 three times (`bo` Sheep, `am` Cow, `mz`
Chicken). A Mob Spawn therefore cannot say which of the three it is, and since `ew`'s id map keeps
the *last* registration, **a real a1.1.2 client draws a chicken for every sheep and cow it meets in
multiplayer**. The ids that are unambiguous: 50 creeper, 51 skeleton, 52 spider, 53 giant, 54
zombie, 55 slime, 90 pig. Vehicles (`0x17`) carry a different, smaller set of type numbers.

A mob a client is sent is **entirely the server's**: its AI, its physics and its despawn all happen
there, and a client that also ran them would be a second mind arguing with the first. The client
moves it only when a packet says to, and drives the legs from the ground actually covered.

## Multiplayer client behaviour this port follows

Read off `gy` (NetClientHandler), `gs` (WorldClient), `la` (EntityClientPlayerMP) and `nj`
(PlayerControllerMP):

- Every tick sends exactly one of 0x0D / 0x0B / 0x0C / 0x0A, chosen by comparing the player with the
  values *last sent* rather than with last tick (`la.J()`). Something goes every tick, which is also
  what keeps the server's own read timeout from firing.
- **The client world does not tick**: `gs.a(boolean)` is `return false` and `gs.g()` only advances
  time and the revert list. Every block change a client sees arrives in a packet.
- The client's own block writes are provisional for **80 ticks** (`lc`); a Block Change, Multi Block
  Change or Map Chunk covering the block confirms them, and anything unconfirmed is put back.
- A thrown item is never simulated locally: `la.a(dx)` sends it as 0x15 and lets it go.
- Block drops do not happen on the client at all; the server spawns them and sends them.

## What a 3DAlpha host adds to protocol 2, and why

Everything above is read off the two jars. This section is **not**: it is what one 3DAlpha console
sends another over local wireless (`core/net/world_server.hpp`), and nothing here ever goes to a
Java server -- a real 0.2.1 has no length prefixes to skip an unknown id with, so one packet it did
not expect would end the connection.

| Addition | What | Why it is not in the jar |
| --- | --- | --- |
| `0x07` Use Entity, client to server | `i32 fromEntityId`, `i32 toEntityId`, `bool leftClick` -- protocol 4's `Packet7UseEntity`, shape unchanged | Protocol 2 has **no way to tell a server you hit something**, which is why vanilla's monsters of this era "were only damaged by fire". Between two consoles running this port that is a hole in the wire rather than a decision about the game, so the id protocol 4 gave it is used at its own shape. Gated behind `NetPlay::allowExtensions`, off by default. |
| `0x07` Use Entity, **server to client** | The same three fields, read the other way round: "`fromEntityId` hit you" | Protocol 4 has no such direction either. It is what makes players able to hit each other at all, and it carries no damage number on purpose -- see below. |
| `0x26` Entity Status, server to client | `i32 entityId`, `i8 status` -- protocol 5's `Packet38EntityStatus`; `2` a hit, `3` a death | Protocol 2 **never tells a client an animal was hit or died**, so a guest's punch drew no red tint, played no noise and flailed no legs, and the corpse vanished without falling over. The host watches `Mob::hurtSerial` and the health in `WorldServer::syncMobs` and sends both as they happen; the guest runs later versions' `handleHealthUpdate` in `MobSystem::statusFromServer`. |
| `0x13` Entity Action, both directions | `i32 entityId`, `i8 action` -- b1.2's `on`; `1` crouched, `2` stood up | a1.1.2's `isSneaking` is a hard-wired false, so nothing had to say it; this port sneaks, and nobody else saw it. Client to server it is b1.2's own, sent by `of.W()` on a change (`net::StanceReporter`). Server to client it is read the other way round, as `0x07` is: "this entity crouched" -- b1.2 says that with `0x28`'s metadata, a format of its own for one bit. Drawn with `cr`'s sneak branch, which a1.1.2 carries as dead code, lowered 0.125 as b1.2's `RenderPlayer` does. |
| `0x26` Entity Status, **client to server** | The same two fields, about the sender's own entity; only `3` | Health is each console's own (see below), so the console that died is the only one that knows. The host passes it on, and the others run `ge.y()`'s death: `random.hurt`, twenty ticks of falling over, the puff -- `dm` overrides none of it. |
| `0x09` Respawn, client to server | No fields -- protocol 5's, empty in a1.2.6 (`jk`) and b1.2 (`kr`), which sends it from `of.u()` | "Alive again". The host answers the others with a fresh `0x14` where the player now stands, which brings back the body that fell over and went. The host's own stance goes out through `WorldServer::setHostStance`, and a console that joins is told how everyone already stands. |
| Mob type `92` sheep, `93` cow | Two values in `0x18`'s type byte | `a(Class, String, int)` registers `bo`, `am` and `mz` **all as 91** and the map keeps the last, so a real a1.1.2 client draws a chicken for every sheep and cow it meets. That is reproduced for a real server and is a plain bug between two copies of this port, which knows what it spawned. `91` still means what the jar says it means. |
| Mob type `94`, `95`, `96` -- slime of size 1, 2 and 4 | Three more values in the same byte | `0x18` carries a type and nothing else, and a slime's size is not a type: `ma`'s constructor draws `1 << nextInt(3)` and `gy` never overwrites it, so against a real server the two ends disagree about how big every slime is, how much standing on it costs (`0.6 * size` reach, `size` damage) and how far it hops (`moveForward = size`). `55` keeps meaning exactly that, and a client that is sent it draws a size of its own as the jar does. Three ids because `1 << nextInt(3)` never produces a 3. |
| `0x0F` Place at `(-1, 255, -1)` facing `255`, client to server | A right click in the air -- the shape later protocols gave "use item"; `net::makeUseItem` | a1.1.2's `Minecraft.clickMouse` calls `ev.a(cn, dm)` (useItemRightClick) directly and never the controller -- `nj` does not override it -- so **a bow shot never reaches the server**: the arrow lives in the shooter's world alone and nobody else sees it. The host fires it from the guest's eye along their look, as theirs (`HostPlay::useItem`); the guest spends the arrow and plays `random.bow` itself. Gated behind `NetPlay::allowExtensions`, so against a Java server the arrow is still fired locally, as the jar does. |
| Object type `60` in `0x17`, then `0x22` every tick it moves, `0x1D` when it goes | An arrow; the number later versions gave `EntityArrow` | `gy.a(kj)` knows 1 (boat) and 10-12 (carts). The guest's copy is `Arrow::remote`: drawn where each teleport says, one tick late for interpolation, and never simulated -- the host alone decides what an arrow hit. `WorldServer::syncArrows`. A guest standing on their own stuck, still arrow is sent `0x16` and `0x11` for one arrow, as for an item (`kg.b(dm)`'s rule, shooter by entity id). |
| `0x07` Use Entity from an **arrow's** id, server to client | "That arrow hit you" | An arrow the host runs reaches a guest's box (`ArrowTargets::remotePlayers`). The guest finds the id in its arrow pool rather than among the players and takes `kArrowDamage` as `DamageSource::Arrow`, knocked back from the arrow -- what the host's own player takes (`WorldServer::arrowStruck`). |

Two things a host deliberately does **not** add. Health and damage stay on each console, because
a1.1.2 has no health packet and both ends run the same `PlayerVitals` -- inventing one would be
inventing a1.2.x. And how the world is played (gamemode, difficulty) does not travel in this stream
at all: it is not a1.1.2's idea, so it rides in the link's own `Welcome` and `Rules` messages
instead (`core/net/link.hpp`).

**That is why a blow carries no number.** A hit between two players says only who swung and at
whom; the console being hit works out what it cost, from the attacker's held item, with the same
`ItemDef::damageVsEntity` lookup the attacker's own console would have used, and applies it through
its own vitals -- `DamageSource::Other`, because `dm.a(Lkh;I)Z` scales by difficulty for a `dq` or
a `kg` and a player is neither. The knockback needs no field either: the attacker's position is
already on this client, in `RemoteEntities`. An animal is the opposite case and stays that way --
the host owns it, so the host resolves the hit and sends where it ended up.

Entity ids are allotted rather than merely counted. `1` upward belongs to the players, in the
session's own player order, and everything else starts at `kFirstFreeEntityId`. Nothing on the wire
needs to say what colour anybody is: both ends reach the same answer out of the id they already
have, which is what makes a name and a map marker agree across two consoles that never discussed
it. See `net::playerColour`.

## Testing

- **Unit tests** (`tests/net_*_test.cpp`): hand-written wire bytes checked against the jars, a
  byte-at-a-time parse of every packet the client sends, region layouts across chunk borders with
  odd heights, the revert list, and a fake server on a loopback socket.
- **Local server**: `./build-host/3dalpha --join 127.0.0.1:<port> Harness 14 compare=<copy of the
  server's world>` against the original 0.2.1 jar in `srv/`. Run it outside the repo with
  `online-mode=false`, `op` the harness name (0.2.1 refuses a non-op's dig and place within 16
  blocks of spawn), and keep its stdin open -- `nogui` stops at EOF, so give it a fifo opened
  read-write: `java -jar a0.2.1.jar nogui 0<>console.fifo`.

  **Compare against a world that has settled.** The copy is taken before the session and the server
  goes on ticking through it, so the columns that arrive carry changes the copy does not have.
  Measured 2026-09-13, every scripted step passing each time: a world generated seconds earlier
  differed on 0.11% of blocks, the same world four minutes later on 0.047%, and one left running
  for ten minutes on 0.0005%. A codec fault -- a wrong plane order, run length or nibble offset --
  disagrees with most of every column instead, which is why the harness bounds this at 0.5% rather
  than at zero.
- **Live**: a public Betacraft server for reality checks.
- **Bridge**: ViaProxy can translate a modern server down to protocol 2 for additional coverage.
