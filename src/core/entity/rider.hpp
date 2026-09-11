#pragma once

// **The seam between a vehicle and whatever is sitting in it.**
//
// A boat and a minecart both carry the player, and both read the *rider's*
// motion rather than the player's input: `EntityBoat.onUpdate` is
// `motionX += riddenByEntity.motionX * 0.2` and nothing else. That is the whole
// of a1.1.2's steering -- `EntityLiving` has no idea it is riding anything, so
// `moveEntityWithHeading` keeps turning the movement keys into motion exactly
// as it would on foot, and only the rider's *position* is overwritten.
//
// So this file exists to keep `core/entity/boat.hpp` from including
// `player_body.hpp`. A vehicle is handed two numbers and hands back three; what
// is riding it could be anything.
//
// It also carries the three numbers `attackEntityFrom` uses, for the same
// reason: `dc.a(Lkh;I)Z` and `oc.a(Lkh;I)Z` are the same four lines with a
// different list of drops at the end, and neither header should include the
// other to say so.

#include "core/util/types.hpp"

namespace mc::entity {

// `damage += i * 10`, `if (damage > 40) ...` and `timeSinceHit = 10` -- the
// scale an `attackEntityFrom` argument is multiplied by, the counter it has to
// pass to break the vehicle, and how long the renderer rocks a struck one for.
//
// **Five bare-handed hits break either of them**, because
// `InventoryPlayer.getDamageVsEntity` answers 1 for an empty hand and
// `onUpdate` sheds one point of damage a tick in between.
inline constexpr int kVehicleDamageScale = 10;
inline constexpr int kVehicleBreakDamage = 40;
inline constexpr int kVehicleHitTime = 10;

struct VehicleRider {
    // Whether anything is aboard at all.
    bool present = false;

    // `riddenByEntity.motionX` and `.motionZ`. The vehicle takes a fifth of
    // each per tick.
    double motionX = 0.0;
    double motionZ = 0.0;
};

// Where a vehicle puts its rider back, in world coordinates. The y is already
// `vehicle.posY + getMountedYOffset()`; what the rider adds on top of that is
// its own `yOffset`, which is the rider's business and not the vehicle's.
struct RiderSeat {
    bool valid = false;
    double x = 0.0, y = 0.0, z = 0.0;
    // The vehicle's heading, so a rider that wants to face along it can.
    float yaw = 0.0f;
};

}  // namespace mc::entity
