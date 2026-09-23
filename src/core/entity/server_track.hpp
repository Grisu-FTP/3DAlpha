#pragma once

// Where a body the server owns should be drawn this tick: the last place the
// server said, plus a guess at how far it has gone since, taken back when the
// guess turns out wrong.
//
// **This replaces a1.1.2's three-tick walk, on purpose.** `setPositionAndRotation2`
// is handed a target and closes a third of the gap a tick, so a body is always
// two or three ticks behind the last packet, and when a packet is late it
// stops dead and then lurches. That is the jar's lag, not its design (a1.1.2
// ran on a LAN or a wired connection; this runs over a shared radio and the
// internet), so what happens between packets here is ours.
//
// **Three parts, one tick at a time:**
//
// - **Velocity**, measured once a tick from how far the server's position moved
//   since the last tick that brought one. Several packets in one tick are one
//   sample, so a burst after a stall reads as the walk it was rather than as a
//   sprint.
// - **Prediction**: the body is aimed at the server's position carried forward
//   by that velocity, for as long as a gap is no longer than this entity's
//   packets usually are apart (`horizon`). A late packet is bridged, not
//   stalled on.
// - **Revert**: a gap longer than that is the prediction proven false -- a
//   server sends nothing for an entity that stopped, so silence *is* the
//   answer -- and the aim goes back to the last confirmed place. A packet
//   that disagrees with the guess is corrected the same way.
//
// The body never jumps to the aim: it closes half the gap a tick, which is the
// interpolation, unless the gap is a teleport's.

#include "core/util/types.hpp"

namespace mc::entity {

struct ServerTrack {
    // Half the gap a tick. Faster than a1.1.2's third, because the aim is now
    // ahead of the server rather than on it: with one tick of lead, a body fed
    // a packet a tick sits on the newest one instead of two ticks behind it.
    static constexpr double kBlend = 0.5;
    // Farther than this in one update is a teleport, and the body is put there.
    static constexpr double kSnapDistance = 4.0;
    // Nothing on foot or in the air in a1.1.2 covers more than this in a tick;
    // a faster "velocity" is two updates that were not one movement.
    static constexpr double kMaxSpeed = 2.0;
    // The longest gap a prediction is carried across, however slow the link.
    static constexpr int kMaxHorizon = 8;

    // The server's position, as confirmed. Relative moves are added to this and
    // never to the body -- `gy` accumulates into `kh.bd/be/bf` the same way.
    double x = 0.0, y = 0.0, z = 0.0;
    // Blocks a tick, as last measured.
    double vx = 0.0, vy = 0.0, vz = 0.0;

    // Where the last tick's sample was taken, and how long ago.
    double sampleX = 0.0, sampleY = 0.0, sampleZ = 0.0;
    int sinceSample = 0;
    // A moving average of ticks between updates: what "late" means for this one.
    float interval = 1.0f;
    bool updated = false;
    bool teleported = false;
    // Predictions taken back since this entity was seen. For the debug page and
    // the tests; nothing reads it to decide anything.
    u16 reverts = 0;

    // A spawn: here, at rest, nothing owed.
    void place(double px, double py, double pz);

    // A position from the server, absolute. Relative moves add to `x/y/z` first.
    void receive(double px, double py, double pz);

    // How many ticks a gap may run before the guess is abandoned.
    int horizon() const;

    // True while the aim is ahead of the confirmed position.
    bool predicting() const;

    // One tick: measures, aims, and moves the body (`bx/by/bz`, the feet) half
    // way to the aim. Returns true when the body was put straight there.
    bool step(double* bx, double* by, double* bz);
};

}  // namespace mc::entity
