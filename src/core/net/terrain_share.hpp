#pragma once

// Terrain generated on one console for another's world.
//
// **Why this is worth a protocol.** Generating a column is the most expensive
// thing either console does, and it splits cleanly in two. Terrain, the
// surface pass and caves are a pure function of the seed and the chunk's
// coordinates -- `caves.hpp`'s carver reads a 17x17 neighbourhood *into* the
// column it is making and never writes out of it -- so a column produced on
// another 3DS is byte-identical to one produced here and may arrive in any
// order without changing the world. Population is the opposite: it writes into
// its neighbours, so its order *is* the world (status.md §0g), and it stays on
// the console that owns the world.
//
// Measured on the host with `--generate`, over three seeds: terrain and caves
// **~1400 us a column**, population **~400 us a pass**. So the half that can be
// handed out is the expensive half, by roughly eight to one, and the half that
// must not be handed out is the cheap one.
//
// **Nothing here blocks.** The host asks for columns it expects to want; an
// answer that arrives in time saves it the work, and one that arrives late is
// dropped because the generator has already made it. There is no waiting, no
// timeout in the critical path, and no way for a slow or vanished guest to
// stall the world -- the worst case is exactly the cost of no session at all.
//
// **The host trusts what it is sent.** A column handed over is installed and
// eventually saved; nothing verifies it against local generation, because
// verifying it would mean generating it, which is the work being avoided. See
// the handshake note on `generatorId` below for the one check that is cheap --
// it catches the accident (a console on a different build) and not the lie.

#include "core/net/link.hpp"
#include "core/util/types.hpp"
#include "version_slots.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace mc::net::link {

// One column of terrain, as `mcver::WorldGen::generateColumn` fills it.
inline constexpr usize kTerrainBytes = usize(mcver::kWorldGenChunkBlocks);

// A column crosses the link deflated and cut into messages, because one
// datagram holds `kMaxMessage`. Sixty-four parts is far more than a terrain
// column has ever needed; it is here so a malformed count cannot make the
// receiver allocate.
inline constexpr int kMaxTerrainParts = 64;

// What a host will hold at once, and therefore what the scheme costs it:
// each waiting column is `kTerrainBytes` raw.
inline constexpr int kDefaultTerrainCapacity = 8;

// The flags in `GeneratorId::options`, in one place so both ends pack them the
// same way.
u8 packOptions(const mcver::WorldGenOptions& options);
mcver::WorldGenOptions unpackOptions(u8 flags);

// This build's generator name, for `GeneratorId::version`.
const char* generatorVersion();

// ---------------------------------------------------------------------------

// **The host's side.** Columns it would like somebody else to make, the
// answers that came back, and the callback the generator reaches them through.
//
// **Two threads touch this**, which is the whole reason it holds a lock: the
// link is pumped on the main thread and the generator runs on the chunk
// worker. The lock is held for a move and a comparison and never spans a
// generation, a decompression or a send.
class TerrainPool {
public:
    explicit TerrainPool(int capacity = kDefaultTerrainCapacity);
    ~TerrainPool();

    TerrainPool(const TerrainPool&) = delete;
    TerrainPool& operator=(const TerrainPool&) = delete;

    // ---- the link's thread ----

    // Ask for a column, if it is not already asked for or already here. False
    // when the pool is full, which is the back-pressure: the host stops asking
    // for more until the generator has taken some.
    bool want(i32 x, i32 z);

    // The next request to put on the wire. False when there is none.
    bool nextRequest(i32* x, i32* z);

    // One `TerrainPart` body. True when it completed a column.
    bool onPart(const u8* body, usize size);

    // The guest carrying our outstanding requests has gone. Anything it owed
    // is dropped so the slots can be asked for again.
    void abandon();

    // ---- the generator's thread ----

    // `mcver::ChunkGenerator::Store::supplyTerrain`, with the pool as context.
    static bool supply(void* context, i32 chunkX, i32 chunkZ, u8* blocks);

    // ---- either ----
    int waiting() const;
    u32 used() const { return used_.load(); }
    u32 late() const { return late_.load(); }

private:
    struct Slot {
        i32 x = 0;
        i32 z = 0;
        bool asked = false;     // a request is out for it
        bool sent = false;      // ...and has been put on the wire
        bool ready = false;     // the bytes are here
        std::vector<u8> blocks;
    };

    Slot* find(i32 x, i32 z);

    mutable std::mutex lock_;
    std::vector<Slot> slots_;

    // Reassembly. One guest sends its parts down one ordered channel, so there
    // is never more than one column part-way in.
    std::vector<u8> incoming_;
    i32 incomingX_ = 0;
    i32 incomingZ_ = 0;
    int incomingNext_ = 0;
    int incomingCount_ = 0;

    std::atomic<u32> used_{0};
    std::atomic<u32> late_{0};
};

// **The guest's side.** Answers requests by generating terrain for the host's
// seed, one column per call, and hands back the messages to send.
//
// `generate` is the expensive call and the caller chooses where it happens:
// in a lobby that is the main thread, which is idle; in a world it belongs on
// the chunk worker.
class TerrainResponder {
public:
    TerrainResponder();
    ~TerrainResponder();

    TerrainResponder(const TerrainResponder&) = delete;
    TerrainResponder& operator=(const TerrainResponder&) = delete;

    // The world to generate for. Until this is called the responder refuses
    // every request, which is what a guest whose build does not match does for
    // the rest of the session.
    void open(const GeneratorId& id);
    bool ready() const { return provider_ != nullptr; }

    // A `TerrainRequest` body. False when it is malformed or the queue is
    // full, which simply drops it -- the host will ask again or make it
    // itself.
    bool onRequest(const u8* body, usize size);

    // Generates at most one queued column and fills `parts` with the messages
    // that carry it. False when nothing was waiting.
    bool generateOne(std::vector<std::vector<u8>>* parts);

    int queued() const { return int(queue_.size()); }
    u32 answered() const { return answered_; }

private:
    struct Ask {
        i32 x = 0;
        i32 z = 0;
    };

    std::unique_ptr<mcver::WorldGen> provider_;
    GeneratorId id_;
    std::vector<Ask> queue_;
    std::vector<u8> blocks_;
    std::vector<u8> packed_;
    u32 answered_ = 0;
};

// The two bodies, built and read in one place like every other message.
void encodeTerrainRequest(i32 x, i32 z, std::vector<u8>* out);
bool decodeTerrainRequest(const u8* body, usize size, i32* x, i32* z);

}  // namespace mc::net::link
