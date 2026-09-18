// A scripted multiplayer session against a real server, as a check of the
// protocol code rather than a measurement.
//
// **The byte layouts were read off the jars, and reading can still be wrong.**
// The unit tests prove the parser and the serializer agree with each other; only
// a real 0.2.1 server can say they agree with it. So this logs in, keeps every
// column the server sends, answers the server's teleport the way `gy.a(eh)`
// does, says hello, digs out the block under its feet and puts it back -- and
// waits for the server's own Block Change echoes of both. Given a *copy* of the
// server's world saved after the session, it then compares every column it
// received with the one on disk, block for block. Non-zero exit when any step
// did not happen.
//
//     --join host[:port] [name] [seconds] [compare=<copy-of-server-world>]

#include "platform/host/join.hpp"

#include "core/entity/player_body.hpp"
#include "core/io/posix_file_system.hpp"
#include "core/net/chunk_payload.hpp"
#include "core/net/client_session.hpp"
#include "core/entity/mob.hpp"
#include "core/net/entities.hpp"
#include "core/tick/tick_world.hpp"
#include "core/net/player_sync.hpp"
#include "core/net/server_list.hpp"
#include "core/world/any_storage.hpp"
#include "core/world/chunk.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <thread>

using namespace mc;

namespace {

using Key = std::pair<i32, i32>;

struct World {
    std::map<Key, std::unique_ptr<world::ChunkColumn>> columns;

    static world::ChunkColumn* find(void* ctx, i32 cx, i32 cz)
    {
        auto& self = *static_cast<World*>(ctx);
        auto it = self.columns.find({cx, cz});
        return it == self.columns.end() ? nullptr : it->second.get();
    }

    world::ChunkColumn* at(i32 x, i32 z) { return find(this, x >> 4, z >> 4); }

    // **A tick world over the same columns**, which the entity code wants for
    // the two things it asks of a world: where to put a spawned item, and how
    // much light it is standing in. Nothing here ticks it.
    tick::TickWorld& ticking()
    {
        if (!world_) {
            tick::TickAccess access;
            access.ctx = this;
            access.column = &World::find;
            world_ = std::make_unique<tick::TickWorld>(access, 0);
        }
        return *world_;
    }

private:
    std::unique_ptr<tick::TickWorld> world_;
};

i64 nowMillis()
{
    return i64(std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
                   .count());
}

}  // namespace

int runJoin(int argc, char** argv)
{
    if (argc < 3) {
        std::printf("usage: --join host[:port] [name] [seconds] [compare=<world-copy>]\n");
        return 2;
    }

    std::string host;
    u16 port = 0;
    if (!net::parseAddress(argv[2], &host, &port)) {
        std::printf("not an address: %s\n", argv[2]);
        return 2;
    }
    const std::string name = net::usernameFrom(argc > 3 ? argv[3] : "");
    const int seconds = argc > 4 ? std::atoi(argv[4]) : 20;
    const char* compareDir = nullptr;
    for (int i = 3; i < argc; ++i) {
        if (std::strncmp(argv[i], "compare=", 8) == 0) {
            compareDir = argv[i] + 8;
        }
    }

    std::printf("joining    %s port %u as %s for %d s\n", host.c_str(), unsigned(port),
                name.c_str(), seconds);

    net::ClientSession session;
    if (!session.start(host, port, name)) {
        std::printf("could not start the session thread\n");
        return 1;
    }

    World world;
    std::vector<u8> scratch;
    // The other players and the items on the ground, exactly as the console
    // tracks them -- see core/net/entities.hpp.
    entity::ItemEntitySystem groundItems(4321);
    entity::MobSystem herd(4321);
    net::RemoteEntities entities;
    entities.bind(&groundItems);
    entities.bindMobs(&herd);
    int mostPlayers = 0;
    int mostItems = 0;
    int mostMobs = 0;
    std::string seenPlayer;

    net::MovementReporter movement;
    net::InventoryReporter inventoryReport;
    item::Inventory inventory;
    net::PlayerPose pose;

    bool loggedIn = false;
    bool placed = false;
    int teleports = 0;
    int columns = 0;
    int regions = 0;
    int unloads = 0;
    int blockChanges = 0;
    int chats = 0;
    int others = 0;
    i64 serverTime = -1;
    bool spawnKnown = false;
    i32 spawn[3] = {0, 0, 0};
    std::string closedTitle;

    // The dig: where, what was there, and which echoes came back.
    i32 digX = 0;
    int digY = 0;
    i32 digZ = 0;
    int dugBlock = -1;
    bool sawBroken = false;
    bool sawRestored = false;
    int ticksSinceTeleport = -1;

    const i64 deadline = nowMillis() + i64(seconds) * 1000;
    auto nextTick = std::chrono::steady_clock::now();

    while (nowMillis() < deadline) {
        net::ClientSession::Event event;
        while (session.poll(&event)) {
            switch (event.kind) {
            case net::ClientSession::Event::Kind::LoggedIn:
                loggedIn = true;
                std::printf("logged in  entity %d\n", event.entityId);
                break;
            case net::ClientSession::Event::Kind::Column:
                if (event.column != nullptr) {
                    const Key key{event.column->x, event.column->z};
                    world.columns[key] = std::move(event.column);
                    ++columns;
                }
                break;
            case net::ClientSession::Event::Kind::Region:
                net::applyMapChunk(event.region, &World::find, &world, &scratch);
                ++regions;
                break;
            case net::ClientSession::Event::Kind::Closed:
                closedTitle = event.title + ": " + event.detail;
                std::printf("closed     %s\n", closedTitle.c_str());
                break;
            case net::ClientSession::Event::Kind::Packet: {
                const net::Packet& p = event.packet;
                if (entities.apply(p, &world.ticking())) {
                    if (entities.playerCount() > mostPlayers) {
                        mostPlayers = entities.playerCount();
                        seenPlayer = entities.player(entities.playerCount() - 1).name;
                        std::printf("player     %s at %.1f %.1f %.1f\n", seenPlayer.c_str(),
                                    entities.player(entities.playerCount() - 1).x,
                                    entities.player(entities.playerCount() - 1).y,
                                    entities.player(entities.playerCount() - 1).z);
                    }
                    if (herd.count() > mostMobs) {
                        mostMobs = herd.count();
                        const entity::Mob& mob = herd[herd.count() - 1];
                        std::printf("mob        type %d at %.1f %.1f %.1f\n", int(mob.type),
                                    mob.body.x, mob.body.y, mob.body.z);
                    }
                    if (groundItems.count() > mostItems) {
                        mostItems = groundItems.count();
                        const entity::ItemEntity& item = groundItems[groundItems.count() - 1];
                        std::printf("item       %d x%d at %.1f %.1f %.1f\n", int(item.item),
                                    item.count, item.x, item.y, item.z);
                    }
                    break;
                }
                net::Teleport teleport;
                if (net::readTeleport(p, &teleport)) {
                    if (teleport.hasPosition) {
                        pose.x = teleport.x;
                        pose.eyeY = teleport.eyeY;
                        pose.feetY = teleport.eyeY - double(entity::kEyeHeight);
                        pose.z = teleport.z;
                    }
                    if (teleport.hasLook) {
                        pose.yaw = teleport.yaw;
                        pose.pitch = teleport.pitch;
                    }
                    pose.onGround = true;
                    // `gy.a(eh)` answers with a Position & Look of its own.
                    session.send(net::makePositionLook(pose.x, pose.feetY, pose.eyeY, pose.z,
                                                       pose.yaw, pose.pitch, false));
                    if (teleports++ == 0) {
                        std::printf("teleport   %.2f %.2f %.2f (feet %.2f)\n", pose.x, pose.eyeY,
                                    pose.z, pose.feetY);
                        ticksSinceTeleport = 0;
                    }
                    break;
                }
                switch (p.id) {
                case net::packet::SpawnPosition:
                    spawnKnown = true;
                    spawn[0] = i32(p.integer(0));
                    spawn[1] = i32(p.integer(1));
                    spawn[2] = i32(p.integer(2));
                    std::printf("spawn      %d %d %d\n", spawn[0], spawn[1], spawn[2]);
                    break;
                case net::packet::TimeUpdate:
                    if (serverTime < 0) {
                        std::printf("time       %lld\n", (long long)p.integer(0));
                    }
                    serverTime = p.integer(0);
                    break;
                case net::packet::Chat:
                    ++chats;
                    std::printf("chat       %s\n", p.text(0).c_str());
                    break;
                case net::packet::PreChunk:
                    if (p.integer(2) == 0) {
                        world.columns.erase({i32(p.integer(0)), i32(p.integer(1))});
                        ++unloads;
                    }
                    break;
                case net::packet::PlayerInventory:
                    net::applyInventory(p, &inventory);
                    std::printf("inventory  type %lld, %zu slots\n", (long long)p.integer(0),
                                p.stacks.size());
                    break;
                case net::packet::BlockChange: {
                    ++blockChanges;
                    const i32 x = i32(p.integer(0));
                    const int y = int(p.integer(1));
                    const i32 z = i32(p.integer(2));
                    if (world::ChunkColumn* c = world.at(x, z)) {
                        c->setBlock(x & 15, y, z & 15, u16(p.integer(3)));
                        c->setBlockData(x & 15, y, z & 15, u8(p.integer(4)));
                    }
                    if (dugBlock >= 0 && x == digX && y == digY && z == digZ) {
                        std::printf("echo       block %d at %d %d %d\n", int(p.integer(3)), x, y,
                                    z);
                        if (p.integer(3) == 0) sawBroken = true;
                        if (placed && p.integer(3) == dugBlock) sawRestored = true;
                    }
                    break;
                }
                default:
                    ++others;
                    break;
                }
                break;
            }
            }
        }

        if (!closedTitle.empty()) {
            break;
        }

        // One 20 Hz tick.
        entities.tick(&world.ticking());
        if (loggedIn && ticksSinceTeleport >= 0) {
            ++ticksSinceTeleport;
            session.send(movement.tick(pose));
            net::Packet inv[3];
            const int n = inventoryReport.tick(inventory, inv);
            for (int i = 0; i < n; ++i) {
                session.send(inv[i]);
            }

            const i32 fx = i32(std::floor(pose.x));
            const i32 fz = i32(std::floor(pose.z));
            if (ticksSinceTeleport == 20) {
                session.send(net::makeChat("3DAlpha host harness says hello"));
            }
            if (ticksSinceTeleport == 30) {
                // The server's eye height is its own `posY` plus 1.62, and 0.2.1's
                // `posY` already has the 1.62 in it, so the player arrives in the
                // air and an a1.1.2 client falls. With no body here, ground is
                // found in the columns the server sent: the nearest column whose
                // top is something a hand digs in under three seconds, with air
                // above it. The player stands on it and looks straight down,
                // because `id.a(hd)` ignores a dig its own 4-block raytrace from
                // the eye does not land on.
                int best = 1 << 30;
                for (int dz = -6; dz <= 6; ++dz) {
                    for (int dx = -6; dx <= 6; ++dx) {
                        world::ChunkColumn* c = world.at(fx + dx, fz + dz);
                        if (c == nullptr) continue;
                        int top = world::ChunkColumn::kHeight - 3;
                        while (top > 0 && c->block((fx + dx) & 15, top, (fz + dz) & 15) == 0) {
                            --top;
                        }
                        const u16 id = c->block((fx + dx) & 15, top, (fz + dz) & 15);
                        const bool diggable = id == 2 || id == 3 || id == 12 || id == 13;
                        if (diggable && dx * dx + dz * dz < best) {
                            best = dx * dx + dz * dz;
                            digX = fx + dx;
                            digY = top;
                            digZ = fz + dz;
                            dugBlock = id;
                        }
                    }
                }
                if (dugBlock > 0) {
                    pose.x = digX + 0.5;
                    pose.z = digZ + 0.5;
                    pose.feetY = digY + 1.0;
                    pose.eyeY = pose.feetY + double(entity::kEyeHeight);
                    pose.pitch = 90.0f;
                    pose.onGround = true;
                }
                std::printf("dig        block %d at %d %d %d\n", dugBlock, digX, digY, digZ);
            }
            if (ticksSinceTeleport == 40 && dugBlock > 0) {
                session.send(net::makeDig(0, digX, digY, digZ, 1));
            }
            if (ticksSinceTeleport > 40 && ticksSinceTeleport <= 100 && dugBlock >= 0) {
                session.send(net::makeDig(1, digX, digY, digZ, 1));
            }
            if (ticksSinceTeleport == 101 && dugBlock >= 0) {
                session.send(net::makeDig(3, digX, digY, digZ, 1));
                session.send(net::makeDig(2, 0, 0, 0, 0));
            }
            if (ticksSinceTeleport == 140 && dugBlock > 0) {
                // Against the top face of the block below the hole.
                session.send(net::makePlace(dugBlock, digX, digY - 1, digZ, 1));
                placed = true;
            }
        }

        nextTick += std::chrono::milliseconds(50);
        std::this_thread::sleep_until(nextTick);
    }

    session.stop("Quitting");

    std::printf("\ncolumns    %d received, %d regions, %d unloaded, %zu held\n", columns, regions,
                unloads, world.columns.size());
    std::printf("packets    %u in, %llu bytes in, %llu bytes out\n", session.packetsIn(),
                (unsigned long long)session.bytesIn(), (unsigned long long)session.bytesOut());
    std::printf("other      %d block changes, %d chat, %d teleports, %d others\n", blockChanges,
                chats, teleports, others);
    std::printf("entities   %d players, %d ground items, %d mobs (most at once), %u spawns not "
                "drawn\n",
                mostPlayers, mostItems, mostMobs, entities.unhandledSpawns());

    int failures = 0;
    const auto require = [&failures](bool ok, const char* what) {
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        failures += ok ? 0 : 1;
    };
    require(loggedIn, "logged in");
    require(spawnKnown, "spawn position received");
    require(teleports > 0, "server placed the player");
    require(columns > 0, "columns received");
    require(serverTime >= 0, "time received");
    require(chats > 0, "chat came back");
    // The dig starts at tick 30 and the place lands at tick 140, so a session
    // too short to reach them is not a failure of either.
    if (seconds >= 10) {
        require(dugBlock > 0, "found a block to dig");
        require(sawBroken, "server echoed the dig");
        require(sawRestored, "server echoed the place");
    }

    if (compareDir != nullptr) {
        io::PosixFileSystem fs;
        world::AnyStorage storage(fs);
        if (storage.open(compareDir, nowMillis()) != world::OpenResult::Ok) {
            std::printf("cannot open %s\n", compareDir);
            return 1;
        }
        int compared = 0;
        int blockDiffs = 0;
        int dataDiffs = 0;
        int lightDiffs = 0;
        // **The copy was saved before the session and the server kept ticking
        // through it**, so every column that arrived later carries world the
        // copy does not: water finding its level, leaves decaying, grass
        // spreading, cobblestone where lava met water. Air and fluid trading
        // places is counted apart, and what is left is bounded rather than
        // required to be zero.
        //
        // **The bound is loose on purpose, because the failure it is looking
        // for is not.** A wrong plane order, run length or nibble offset
        // disagrees with most of every column; measured against a real server
        // on 2026-09-13, a *correct* client differed on 0.11% of blocks on a
        // world generated seconds earlier, 0.047% after four minutes of
        // ticking and 0.0005% on one left running for ten -- so a world that
        // has had a few minutes to settle is the right thing to compare
        // against, and 0.5% is two orders of magnitude clear of a codec fault
        // either way.
        int unexplained = 0;
        const auto fluidOrAir = [](u16 id) { return id == 0 || (id >= 8 && id <= 11); };
        world::ChunkColumn disk;
        for (const auto& [key, column] : world.columns) {
            if (!storage.loadChunk(key.first, key.second, &disk)) {
                continue;
            }
            ++compared;
            for (int x = 0; x < 16; ++x) {
                for (int z = 0; z < 16; ++z) {
                    for (int y = 0; y < world::ChunkColumn::kHeight; ++y) {
                        const u16 got = column->block(x, y, z);
                        const u16 want = disk.block(x, y, z);
                        const bool blockDiff = got != want;
                        const bool dataDiff = column->blockData(x, y, z) != disk.blockData(x, y, z);
                        blockDiffs += blockDiff;
                        dataDiffs += dataDiff;
                        if ((blockDiff || dataDiff) && !(fluidOrAir(got) && fluidOrAir(want))) {
                            if (unexplained++ < 5) {
                                std::printf("differs    %d %d %d: got %u:%u, disk has %u:%u\n",
                                            key.first * 16 + x, y, key.second * 16 + z,
                                            unsigned(got), unsigned(column->blockData(x, y, z)),
                                            unsigned(want), unsigned(disk.blockData(x, y, z)));
                            }
                        }
                        lightDiffs += column->skyLight(x, y, z) != disk.skyLight(x, y, z)
                                      || column->blockLight(x, y, z) != disk.blockLight(x, y, z);
                    }
                }
            }
        }
        storage.close(nowMillis());
        std::printf("compare    %d columns: %d blocks, %d metadata, %d light values differ\n",
                    compared, blockDiffs, dataDiffs, lightDiffs);
        require(compared > 0, "columns found on disk");
        const long long blocksCompared = compared * 16LL * 16LL * world::ChunkColumn::kHeight;
        std::printf("           %d of them not fluid settling, of %lld blocks compared\n",
                    unexplained, blocksCompared);
        require(compared > 0 && unexplained * 200LL < blocksCompared,
                "blocks and metadata match the server's to within its own ticking");
        if (unexplained * 200LL >= blocksCompared) {
            std::printf("           (a world generated moments ago is still settling: let the "
                        "server\n           tick for a few minutes, save-all, copy, and run "
                        "again)\n");
        }
    }

    return failures == 0 ? 0 : 1;
}
