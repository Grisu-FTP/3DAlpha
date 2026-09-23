// A server-owned body's aim, between one packet and the next: measured,
// carried forward, and taken back. See server_track.hpp.

#include "core/entity/server_track.hpp"

#include <cmath>

namespace mc::entity {

namespace {

double clampSpeed(double v)
{
    return v > ServerTrack::kMaxSpeed    ? ServerTrack::kMaxSpeed
           : v < -ServerTrack::kMaxSpeed ? -ServerTrack::kMaxSpeed
                                         : v;
}

double lengthSquared(double dx, double dy, double dz) { return dx * dx + dy * dy + dz * dz; }

}  // namespace

void ServerTrack::place(double px, double py, double pz)
{
    *this = ServerTrack{};
    x = sampleX = px;
    y = sampleY = py;
    z = sampleZ = pz;
}

void ServerTrack::receive(double px, double py, double pz)
{
    constexpr double kSnapSquared = kSnapDistance * kSnapDistance;
    if (lengthSquared(px - x, py - y, pz - z) > kSnapSquared) {
        teleported = true;
    }
    x = px;
    y = py;
    z = pz;
    updated = true;
}

int ServerTrack::horizon() const
{
    const int ticks = int(interval + 0.5f) + 1;
    return ticks < 2 ? 2 : ticks > kMaxHorizon ? kMaxHorizon : ticks;
}

bool ServerTrack::predicting() const
{
    return sinceSample < horizon() && (vx != 0.0 || vy != 0.0 || vz != 0.0);
}

bool ServerTrack::step(double* bx, double* by, double* bz)
{
    ++sinceSample;

    if (teleported) {
        // Nothing to measure across a teleport: the body goes, at rest.
        teleported = false;
        updated = false;
        vx = vy = vz = 0.0;
        sampleX = x;
        sampleY = y;
        sampleZ = z;
        sinceSample = 0;
        *bx = x;
        *by = y;
        *bz = z;
        return true;
    }

    if (updated) {
        // **A gap longer than the horizon was a rest**, not a slow walk, so the
        // distance is divided by the usual interval and not by the whole of it
        // -- otherwise the first step after standing still would read as a
        // crawl and the body would trail it.
        const int usual = int(interval + 0.5f) < 1 ? 1 : int(interval + 0.5f);
        const double ticks = double(sinceSample <= horizon() ? sinceSample : usual);
        vx = clampSpeed((x - sampleX) / ticks);
        vy = clampSpeed((y - sampleY) / ticks);
        vz = clampSpeed((z - sampleZ) / ticks);
        const float seen = float(sinceSample < 4 ? sinceSample : 4);
        interval += (seen - interval) * 0.25f;
        sampleX = x;
        sampleY = y;
        sampleZ = z;
        sinceSample = 0;
        updated = false;
    }

    double aimX = x;
    double aimY = y;
    double aimZ = z;
    if (sinceSample < horizon()) {
        // One tick of lead as well as the gap, so the half-way body lands on
        // the confirmed position while packets flow rather than a tick behind.
        const double lead = double(sinceSample + 1);
        aimX += vx * lead;
        aimY += vy * lead;
        aimZ += vz * lead;
    } else if (vx != 0.0 || vy != 0.0 || vz != 0.0) {
        // **The guess was wrong**: the silence has lasted longer than this
        // entity's silences do, which is a server that has stopped reporting a
        // body that stopped. Aim back at what was confirmed.
        vx = vy = vz = 0.0;
        ++reverts;
    }

    constexpr double kSnapSquared = kSnapDistance * kSnapDistance;
    if (lengthSquared(aimX - *bx, aimY - *by, aimZ - *bz) > kSnapSquared) {
        *bx = aimX;
        *by = aimY;
        *bz = aimZ;
        return true;
    }
    *bx += (aimX - *bx) * kBlend;
    *by += (aimY - *by) * kBlend;
    *bz += (aimZ - *bz) * kBlend;
    // Close enough is there: a body that has arrived stops exactly, rather
    // than creeping for ever toward a point it can never reach by halves.
    constexpr double kArrived = 1.0 / 4096.0;
    if (std::fabs(aimX - *bx) < kArrived && std::fabs(aimY - *by) < kArrived
        && std::fabs(aimZ - *bz) < kArrived) {
        *bx = aimX;
        *by = aimY;
        *bz = aimZ;
    }
    return false;
}

}  // namespace mc::entity
