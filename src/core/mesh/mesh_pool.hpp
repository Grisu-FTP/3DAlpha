#pragma once

// Section meshing on a worker thread.
//
// `mesh::meshSection` is a pure function of a MeshScratch, so the only thing that
// ever had to stay on the main thread is reading the live columns. This pool
// splits the job exactly there: the caller fills a slot's scratch (main thread,
// the one that owns the columns and may write them), and the worker turns it into
// quads without touching a column, the renderer or GL. The caller collects
// finished slots later and uploads them itself.
//
// Nothing here waits on the worker: submit takes a free slot or says there is
// none, and poll hands back a finished slot or says there is none. The lock is
// held for a few instructions at a time.
//
// **Staleness is the caller's to judge.** A result carries the revision it was
// submitted with and the pool does not interpret it; the caller compares it with
// the section's current one when it collects. That is what keeps this class free
// of any idea what a block edit is.
//
// With no thread (none could be started, or the pool was built with zero
// workers) `submit` still queues and `runPending` meshes on the calling thread,
// which is how the host tests exercise it deterministically.

#include "core/mesh/mesher.hpp"
#include "core/mesh/scratch.hpp"
#include "core/util/types.hpp"
#include "core/util/worker.hpp"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace mc::mesh {

class MeshPool {
public:
    struct Result {
        i32 chunkX = 0;
        i32 chunkZ = 0;
        int sectionY = 0;
        u32 revision = 0;
        const MeshBuilder* builder = nullptr;  // valid until release(slot)
        int slot = -1;
    };

    ~MeshPool() { stop(); }

    // `slots` bounds the jobs in flight; each costs a scratch (23 KB) and a
    // builder. Call before any submit. `threaded` false leaves the pool to
    // runPending.
    void start(int slots, bool threaded, int quadReserve);
    void stop();

    bool threaded() const { return threaded_; }

    // Fills a free slot from `neighbourhood` and queues it. False when every
    // slot is busy, which is not an error: the section stays in the renderer's
    // queue and comes back next frame.
    bool submit(i32 chunkX, int sectionY, i32 chunkZ, u32 revision,
                const ColumnNeighbourhood& neighbourhood);

    // Next finished slot, or false. The builder stays valid, and the slot
    // reserved, until release().
    bool poll(Result* out);
    void release(int slot);

    // Synchronous mode: mesh everything queued, on this thread.
    void runPending();

    // Jobs submitted and not yet released.
    int inFlight() const;
    bool inFlight(i32 chunkX, int sectionY, i32 chunkZ) const;

    void setGreedy(bool on);
    void setCubeFormat(CubeFormat format);
    void setSnowHidesFloor(bool on);

private:
    enum class State : u8 { Free, Queued, Running, Done };

    struct Slot {
        MeshScratch scratch;
        MeshBuilder builder;
        State state = State::Free;
        i32 chunkX = 0;
        i32 chunkZ = 0;
        int sectionY = 0;
        u32 revision = 0;
        u32 order = 0;
    };

    static void entry(void* self);
    void loop();
    bool runOne();

    std::vector<std::unique_ptr<Slot>> slots_;
    mutable std::mutex lock_;
    std::condition_variable wake_;
    bool stopping_ = false;
    bool threaded_ = false;
    u32 nextOrder_ = 0;
    void* platformThread_ = nullptr;
    std::thread thread_;
    bool greedy_ = true;
    CubeFormat format_ = CubeFormat::Vertices;
    bool snowHidesFloor_ = true;
};

}  // namespace mc::mesh
