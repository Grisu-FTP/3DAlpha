#pragma once

// Who creates a background thread, and on which core.
//
// This is a platform seam and it exists because the portable API cannot express
// the one thing that matters on a New 3DS. `std::thread` cannot name a core, and
// on this toolchain it does not merely decline to: devkitARM's pthread shim
// hardcodes `threadCreate(..., 0x3F, 0, ...)`, so **every `std::thread` lands on
// core 0 at the bottom priority**. The ARM11 scheduler is strictly
// priority-ordered, so such a thread runs only in whatever is left of a frame
// after the main thread blocks -- which on a console holding 30 fps is a sliver.
//
// There are three background threads now and they want different things, which
// is why the seam carries a role:
//
//   * **Generation** is pure CPU for tens of milliseconds at a time. On a New
//     3DS it wants core 2, which is idle and which Luma's synthesised 3DSX
//     exheader already grants (`0xFF002109`, bit 13, "Access core2").
//   * **I/O** spends nearly all its life blocked in an IPC round trip to the FS
//     sysmodule. It wants core 0 at a priority just below the main thread's, so
//     it runs in exactly the slack the main thread leaves while waiting on
//     VBlank and is preempted the instant the main thread is ready. Giving it a
//     core of its own would waste one; giving it the main thread's priority
//     would cost frames.
//   * **Audio** is the awkward one: it is CPU-bound like Generation but has a
//     deadline like nothing else here, because a decoder that misses its buffer
//     is audible immediately. It takes core 2 on a New 3DS, sharing it with
//     Generation at the same priority, and on an Old 3DS it takes the I/O
//     policy -- core 0, just below the main thread -- because a 3DSX has no
//     other core to move it to. CONTRIBUTING's "no decompression on core 0" is
//     knowingly relaxed there.
//
//     **What makes it work on either console is buffer depth, not priority.**
//     A third of a second of decoded audio is queued ahead, so the decoder only
//     ever needs the slack the main thread already leaves at VBlank, and it
//     never has to win a scheduling race to stay ahead. That is why it does not
//     outrank Generation on a New 3DS even though it could ask to. See
//     platform/ctr/audio.hpp.
//
// `spawn` returns an opaque handle, or null if the thread could not be started
// -- in which case the caller falls back to doing the work inline, which is slow
// but not broken. `join` is handed that handle back, once, and must not return
// until the thread has finished. Set both or neither, before any world is
// opened. With neither set, `std::thread` is used and the role is ignored, which
// is what the host tests and harnesses run on.

namespace mc {

enum class WorkerRole {
    Generation,
    Io,
    Audio,
};

using WorkerSpawn = void* (*)(void (*entry)(void*), void* arg, WorkerRole role);
using WorkerJoin = void (*)(void* handle);

// Process-wide, because the platform's answer does not vary per world and
// threading a pair of function pointers through every constructor would put
// them in the signature of code that has no opinion about threads.
void setWorkerThreadOps(WorkerSpawn spawn, WorkerJoin join);

// Both null unless a platform set them.
WorkerSpawn workerSpawn();
WorkerJoin workerJoin();

}  // namespace mc
