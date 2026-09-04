#pragma once

// The player's body: a 0.6 x 1.8 box that falls, walks, steps up and refuses to
// go through things. This is a1.1.2's `Entity.moveEntity`, `EntityLiving`'s
// `moveEntityWithHeading` and `Entity.moveFlying`, transcribed from the class
// file rather than remembered -- see docs/physics-a1.1.2.md, which carries the
// derivation, the obfuscated names and every constant as it actually appears.
//
// **`y` here is the feet, and that is a deliberate departure from the original.**
// a1.1.2 keeps `posY` at the eye, 1.62 above the ground, because `yOffset` is
// 1.62 and `setPosition` puts the box at `posY - yOffset`. Storing the eye
// works, but it means every question anyone actually asks -- what am I standing
// on, does this fit, where do I draw the box -- carries a 1.62 correction, and
// getting one of them wrong is invisible until something falls through a floor.
// So the body stores the feet and hands out `eyeY()` for the two places that
// want the original's convention: the camera, and `level.dat`.
//
// The *format* is still exactly a1.1.2's. `eyeY()` is `posY`, and `posY` is
// what `Pos[1]` holds -- so a world saved by this code and a world saved by the
// real client mean the same thing by the same number.
//
// **Nothing here allocates**, and the collision query is a fixed-size buffer on
// the stack: this runs in the per-frame path, and the 3DS build is compiled
// with `-Werror=stack-usage=8192` against a 32 KB main thread.

#include "core/block/collision.hpp"
#include "core/util/aabb.hpp"
#include "core/util/types.hpp"

namespace mc::tick {
class TickWorld;
}

namespace mc::entity {

// Straight out of the class file. Several of these are a float widened to a
// double and are **not** the round number they look like -- writing 0.98 or
// 0.42 here instead would drift by about a part in 10^8 per tick, which is
// exactly the size of error that makes an oracle comparison fail confusingly.
// **These four are `float` because the jar's are**, and it matters. `yOffset`
// is `1.62f`, which widens to 1.6200000047683716 -- not to 1.62 -- and
// `setPosition` widens it exactly once, at the point of use. Declaring it a
// double here was wrong by 4.8e-9 and the oracle caught it on the very first
// tick. `width` and `height` have the same shape (0.60000002384185791 and
// 1.7999999523162842), and the half-width is halved **in float** before it is
// widened, so `double(kPlayerWidth) / 2.0` is not the same number as
// `double(kPlayerWidth / 2.0f)`.
inline constexpr float kPlayerWidth = 0.6f;
inline constexpr float kPlayerHeight = 1.8f;
inline constexpr float kEyeHeight = 1.62f;                 // Entity.yOffset
inline constexpr float kStepHeight = 0.5f;
inline constexpr double kGravity = 0.08;
inline constexpr double kVerticalDrag = 0.9800000190734863;   // (double)0.98f
inline constexpr double kJumpVelocity = 0.41999998688697815;  // (double)0.42f
inline constexpr float kAirFriction = 0.91f;
inline constexpr float kGroundFrictionBase = 0.54600006f;     // stored, not 0.6*0.91
inline constexpr float kAccelNormaliser = 0.16277136f;
inline constexpr float kGroundAcceleration = 0.1f;
inline constexpr float kAirAcceleration = 0.02f;
inline constexpr double kSneakProbe = 0.05;
inline constexpr float kYSizeDecay = 0.4f;
inline constexpr float kHeadingPi = 3.1415927f;               // the float literal in moveFlying

// **Creative flight, and neither of these numbers came out of a jar.**
// a1.1.2 has no Creative mode and no flight of any kind; see
// core/item/creative_palette.hpp for the whole of that argument. They are
// derived from two things this project already had rather than picked:
//
//   * The speeds are Spectator's, converted from frames to ticks. `flyCamera`
//     moves 12 blocks a second, or 40 held down, and at 20 Hz that is 0.6 and
//     2.0 blocks a tick. Creative flight that felt different from the free
//     flight next to it would be a second set of numbers to explain.
//   * Vertical is the same speed as horizontal, because there is no gravity to
//     make the two differ and a flight that rises slower than it flies reads as
//     broken rather than as heavy.
//
// A tick of flight is a *velocity*, not an acceleration: there is no drag term
// and motion is cleared at the end of the tick, so letting go stops you dead.
// That is Spectator's behaviour and it is the point -- flight here is a camera
// that collides, and a camera with momentum is a camera you fight.
inline constexpr double kFlightSpeed = 0.6;
inline constexpr double kFlightSprintSpeed = 2.0;

// What the player is asking for this tick. Angles are **degrees**, as the
// original stores them, because `moveFlying` multiplies by 3.1415927f/180 and
// converting elsewhere would round differently.
struct PlayerInput {
    float strafe = 0.0f;   // -1..1, positive is left in the original's sense
    float forward = 0.0f;  // -1..1
    float yawDegrees = 0.0f;
    bool jump = false;
    bool sneak = false;
};

// A floor to int that agrees with Java's, for the three helpers below. The
// physics itself goes through MathHelper::floorDouble, which is the original's
// own; this is here so the header needs no include of it.
inline i64 floorToInt(double v)
{
    const i64 truncated = i64(v);
    return v < double(truncated) ? truncated - 1 : truncated;
}

struct PlayerBody {
    // Feet. See the header note -- the original stores the eye here.
    double x = 0.0, y = 0.0, z = 0.0;

    // The original's `posY`, **stored rather than derived**. It has to be: the
    // original writes it during the move, using the `ySize` of that moment, and
    // then decays `ySize` by 0.4 before the call returns. Recomputing the eye
    // afterwards from the decayed value gives a different -- and visibly
    // wrong -- answer for the whole tick after a step up.
    double posY = 0.0;
    double motionX = 0.0, motionY = 0.0, motionZ = 0.0;

    // The camera-smoothing term the original carries so a step up does not
    // snap the view a whole half block. `moveEntity` adds 0.5 on a successful
    // step and multiplies by 0.4 at the end of every call.
    float ySize = 0.0f;

    float fallDistance = 0.0f;

    bool onGround = false;
    bool collidedHorizontally = false;
    bool collidedVertically = false;

    // Read by `move` for the ledge check, and set from the input each tick.
    bool sneaking = false;

    AABB box{};

    // Places the body with its feet at (x, y, z) and rebuilds the box.
    void setFeet(double fx, double fy, double fz);

    // Where the camera goes, and what `level.dat`'s `Pos[1]` holds -- the same
    // number, and the same one the original keeps in `posY`.
    double eyeY() const { return posY; }

    // Which column the body is standing in. **An arithmetic shift, not a
    // divide**: -1 / 16 is 0 and -1 >> 4 is -1, and the difference is a whole
    // chunk of wrong world. Same reasoning as core/tick/tick_world.cpp.
    i32 chunkX() const { return i32(floorToInt(x)) >> 4; }
    i32 chunkZ() const { return i32(floorToInt(z)) >> 4; }
    int sectionY() const { return int(floorToInt(y)) >> 4; }

    // `Entity.moveEntity`. Sweeps the box by (dx, dy, dz) against the world,
    // clipping each axis, then retries lifted by the step height if that
    // helped. Updates the collision flags, fall distance and motion.
    void move(const tick::TickWorld& world, double dx, double dy, double dz);

    // `EntityLiving.moveEntityWithHeading` for the on-land case, plus the jump.
    // One call is one 20 Hz tick, not one frame -- see stepTicks.
    void tick(const tick::TickWorld& world, const PlayerInput& input);

    // `Entity.moveFlying`: turns a stick heading into an acceleration.
    void applyHeading(float strafe, float forward, float yawDegrees, float acceleration);

    // `EntityLiving.jump`, which is one assignment and nothing else.
    void jump() { motionY = kJumpVelocity; }

    // **One tick of Creative flight, and it is ours.** Nothing above this line
    // is; everything here is. See the constants above.
    //
    // It is `flyCamera` with `move` under it: the stick heading goes through
    // the original's own `moveFlying` so a direction means the same thing
    // flying as walking, `ascend` and `descend` supply the vertical, and the
    // result is swept through the world by `moveEntity` -- so flight collides.
    // That is the whole difference from Spectator, which has no body to collide
    // with, and it is why this is a method on the body rather than a second
    // camera mover beside the first.
    //
    // `fallDistance` is cleared every tick: you cannot fall while flying, and
    // leaving it to accumulate would bank a fall for Survival to cash in the
    // moment flight was turned off over a canyon.
    void tickFlying(const tick::TickWorld& world, const PlayerInput& input, bool ascend,
                    bool descend, double speed);

    // Is the body overlapping anything solid where it stands?
    bool insideGround(const tick::TickWorld& world) const;

    // Raise the body until it is not inside anything, up to `maxBlocks`.
    // Returns how far it had to go.
    //
    // **This is a spawn fixer, not physics**, and it exists because `spawnY` in
    // level.dat is a block coordinate with no promise attached: putting the
    // feet at it can leave the body buried to the waist, and a buried body
    // cannot walk in any direction, which reads as the physics being broken
    // rather than the placement being wrong. The original does the same thing
    // when a player joins a world. It is a no-op when the spot is already
    // clear, which is the ordinary case for a world with a saved player.
    int liftOutOfGround(const tick::TickWorld& world, int maxBlocks = 8);
};

}  // namespace mc::entity
