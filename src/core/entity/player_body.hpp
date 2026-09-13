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
#include "core/entity/water_entry.hpp"
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
// **A ladder, and the two numbers it costs.** `ge.b(FF)`'s land branch asks
// `isOnLadder()` twice: once before the move, to clamp a fall to a slow slide,
// and once after, to turn "pressed into a wall" into "climbing". Both are
// doubles in the class file and neither is 0.15f or 0.2f widened -- they are
// written as double literals, so they are exact.
inline constexpr double kLadderSlide = -0.15;
inline constexpr double kLadderClimb = 0.2;

inline constexpr double kGravity = 0.08;
inline constexpr double kVerticalDrag = 0.9800000190734863;   // (double)0.98f
inline constexpr double kJumpVelocity = 0.41999998688697815;  // (double)0.42f
inline constexpr float kAirFriction = 0.91f;

inline constexpr float kGroundFrictionBase = 0.54600006f;     // stored, not 0.6*0.91
inline constexpr float kAccelNormaliser = 0.16277136f;
inline constexpr float kGroundAcceleration = 0.1f;
// 0.1f + 0.1f * 0.3f, computed the way Beta computes it rather than written as
// 0.13f: the sum of two floats is what the game holds, and 0.13f is a different
// number in the last bits. Not a1.1.2's -- see PlayerInput::sprint.
inline constexpr float kSprintAcceleration = kGroundAcceleration + kGroundAcceleration * 0.3f;
inline constexpr float kAirAcceleration = 0.02f;

// **What a rider accelerates at**, and it is `kAirAcceleration` rather than a
// number of its own: a rider is never `onGround` as far as `EntityLiving` is
// concerned -- it is standing in an entity, not on a block -- so
// `moveEntityWithHeading` takes its air branch.
//
// It matters more than it looks. A boat takes a fifth of the rider's motion and
// drags at 0.99, so its steady speed is about twenty times the rider's; at 0.02
// under 0.91 friction the rider settles near 0.22, and the boat is pinned at
// its own 0.4 cap whenever the stick is held. That is a boat at full speed,
// which is what a boat does.
inline constexpr float kRiderAcceleration = kAirAcceleration;
inline constexpr double kSneakProbe = 0.05;

// **A sneaking player moves at 30 %, and this one really is a1.1.2's.** The
// crouch is split across two classes in this jar and only one half of it
// survived: `Entity.isSneaking` is a hardcoded `false`, so the stance and the
// walk-back `kSneakProbe` feeds are dead -- but the *slowdown* never asks the
// entity anything. It is the tail of `gd.a(dm)`, which is
// `MovementInputFromOptions.updatePlayerMoveState` reading the sneak key
// straight out of its own array:
//
//     this.e = this.f[5];                            // sneak, from the keys
//     if (this.e) {
//         this.a = (float)((double)this.a * 0.3D);   // moveStrafe
//         this.b = (float)((double)this.b * 0.3D);   // moveForward
//     }
//
// b1.6.2, b1.8.1 and 1.8.9 all disassemble to that same guarded pair of
// multiplications, differing only in the obfuscated names, so unlike
// `kSneakEyeHeight` below this number needs no era label: it is period, and it
// was simply missing here.
//
// **It scales the stick, not the speed**, and those are not the same thing. The
// 0.3 lands before `moveFlying` turns the input into motion, so friction and
// whatever momentum the player already had dilute it -- a sneak is not a cap at
// 30 % of walking pace, it is a weaker push. Scaling the acceleration or the
// resulting velocity instead would both be wrong, and wrong differently.
inline constexpr double kSneakMoveScale = 0.3;

// **How far the camera drops when the player crouches, and a1.1.2 has no
// answer to give.** `Entity.isSneaking` is a hardcoded `false` in this jar --
// the walk-back `kSneakProbe` feeds is dead code there -- so there is nothing
// to transcribe for what a crouch *looks* like, only for what it does.
//
// The Betas do not settle it either, and it is worth saying which way they go
// so nobody re-derives it: b1.6.2 and b1.8.1 leave `yOffset` at 1.62 while
// sneaking and lower only the *model*, by 0.125, in `RenderPlayer` -- so their
// camera does not move at all. `EntityRenderer.orientCamera` there is
// `posY - (yOffset - 1.62)`, which is the same line this body's `renderEyeY`
// is, and a sneaking player feeds it the same 1.62.
//
// The eye drop is a release-era thing, and **0.08 is 1.8.9's**, read out of
// `EntityPlayer.getEyeHeight`:
//
//     float f = 1.62F;
//     if (isPlayerSleeping()) f = 0.2F;
//     if (isSneaking()) f -= 0.08F;
//
// -- so the sneaking eye is `1.62f - 0.08f`, which is exactly `1.54f`, and the
// drop is that subtraction done the way the jar does it rather than the 0.08
// it is written as. It is instant there, with no smoothing of its own, and it
// is instant here for the same reason: 0.08 of a block is a tenth of the half
// block `ySize` exists to glide over.
//
// **This is the camera and nothing else.** `yOffset` stays 1.62, so `eyeY()`
// -- the physics' `posY`, and what `Pos[1]` saves -- is untouched: a crouching
// player who saves and reloads comes back where they were, not 0.08 lower
// every time. Same separation 1.8.9 has, where `getEyeHeight` is a query and
// `posY` is the position.
inline constexpr float kSneakEyeHeight = kEyeHeight - 0.08f;   // 1.54f exactly
inline constexpr double kSneakEyeDrop = double(kEyeHeight) - double(kSneakEyeHeight);
inline constexpr float kYSizeDecay = 0.4f;
inline constexpr float kHeadingPi = 3.1415927f;               // the float literal in moveFlying

// **Swimming**, and every one of these did come out of the jar --
// `EntityLiving.moveEntityWithHeading` takes a liquid branch before it reaches
// land, and docs/physics-a1.1.2.md has the listing. The two branches are the
// same code with a different drag, which is why there is one function and two
// constants rather than two functions.
//
// Note what swimming does *not* consult: ground friction, the acceleration
// normaliser, slipperiness, the jump. A swimming player accelerates at a flat
// 0.02 whatever is under them, sinks at 0.02 a tick, and keeps 80 % of their
// motion in water or 50 % in lava.
inline constexpr float kSwimAcceleration = 0.02f;
inline constexpr double kSwimSink = 0.02;
inline constexpr double kWaterDrag = 0.800000011920929;  // (double)0.8f
inline constexpr double kLavaDrag = 0.5;
// **What holding jump does in a liquid, and it is not a jump.** `ge.j()` --
// `EntityLiving.onLivingUpdate` -- reaches `jump()` only when the player is in
// neither water nor lava; in either, it adds this flat amount to the motion
// every tick the button is held. That is the whole of swimming up, and the
// same number for both liquids. Held against the swim branch's 0.8 drag and
// 0.02 sink it settles at 0.06 a tick, which is a block and a fifth a second.
inline constexpr double kLiquidRise = 0.03999999910593033;
// How far above the eye the way out has to be clear before a swimmer pushing
// against a wall is lifted, and how hard they are lifted.
inline constexpr double kSwimLedgeReach = 0.6000000238418579;
inline constexpr double kSwimLedgeLift = 0.30000001192092896;

// **Creative flight, and neither of these numbers came out of a jar.**
// a1.1.2 has no Creative mode and no flight of any kind; see
// core/item/creative_palette.hpp for the whole of that argument. They are
// derived from two things this project already had rather than picked:
//
//   * The speed is Spectator's, converted from frames to ticks. `flyCamera`
//     moves 12 blocks a second, and at 20 Hz that is 0.6 blocks a tick.
//     Creative flight that felt different from the free flight next to it would
//     be a second number to explain.
//   * Vertical is the same speed as horizontal, because there is no gravity to
//     make the two differ and a flight that rises slower than it flies reads as
//     broken rather than as heavy.
//
// **There is one speed, and there used to be two.** Spectator's `flyCamera`
// has a held boost and Creative's flight copied it onto A -- which put a
// modifier on the one face button Creative had going spare, and A is now the
// drop. A held boost is also the wrong shape for this mode in a way it is not
// for Spectator: Spectator is a camera with nothing to hit, and Creative
// flight collides, places and breaks. Spectator keeps its boost; see
// `flyCamera` in platform/ctr/main.cpp.
//
// A tick of flight is a *velocity*, not an acceleration: there is no drag term
// and motion is cleared at the end of the tick, so letting go stops you dead.
// That is Spectator's behaviour and it is the point -- flight here is a camera
// that collides, and a camera with momentum is a camera you fight.
inline constexpr double kFlightSpeed = 0.6;

// What the player is asking for this tick. Angles are **degrees**, as the
// original stores them, because `moveFlying` multiplies by 3.1415927f/180 and
// converting elsewhere would round differently.
struct PlayerInput {
    float strafe = 0.0f;   // -1..1, positive is left in the original's sense
    float forward = 0.0f;  // -1..1
    float yawDegrees = 0.0f;
    bool jump = false;
    bool sneak = false;

    // **Sprint, and a1.1.2 has none.** `EntityPlayer` in this jar carries no
    // `sprinting` field, no `setSprinting`, and `moveEntityWithHeading` reads a
    // constant 0.1f where later versions read `landMovementFactor` -- so there
    // is nothing here to transcribe and nothing to check this against. It is in
    // for the same reason Creative is: the console editions sprint on a
    // double-tapped stick and a player arriving from one expects it.
    //
    // The multiplier is not invented, though. Beta's `EntityPlayer.onLivingUpdate`
    // does `landMovementFactor += landMovementFactor * 0.3F` while sprinting,
    // which turns 0.1 into 0.13 -- so sprint here is the same 30 % on the same
    // term, applied at the same point in the same tick. See
    // `kSprintAcceleration` and core/entity/sprint_gesture.hpp.
    bool sprint = false;
};

// The tail of `MovementInputFromOptions.updatePlayerMoveState`, run over an
// input that has been filled in and not yet read.
//
// **This is core and not the 3DS input code** for the reason
// core/entity/sprint_gesture.hpp is: it is game logic that happens to live in
// an input class upstream, and a host test can reach it here. It is just as
// deliberately *not* inside `PlayerBody::move` -- the jar scales the stick
// before `EntityLiving` is handed it, and the generated cases in
// tests/player_body_vectors.hpp drive `moveEntityWithHeading` directly, so a
// body that scaled its own input would disagree with its own oracle.
//
// The float-double-float round trip is the jar's `f2d; dmul; d2f` rather than
// decoration: `0.3f` and `(double)0.3` are different numbers, and for plenty of
// sticks the two orders land a ulp apart -- 0.7 comes out 0.20999999344348907
// here and 0.21000001 the float-only way.
inline void applySneakSlowdown(PlayerInput& input)
{
    if (!input.sneak) {
        return;
    }
    input.strafe = float(double(input.strafe) * kSneakMoveScale);
    input.forward = float(double(input.forward) * kSneakMoveScale);
}

// **The dimensions are fields, because this is `EntityLiving`'s body and not
// only the player's.** Everything below `move` is `kh.c(DDD)` and `ge.b(FF)`,
// which a pig runs exactly as a player does -- the same sweep, the same
// 0.5 step, the same ground friction, the same ladder. What differs between one
// living entity and the next is four numbers, so they are four fields with the
// player's values as the default rather than four constants baked into the
// code. See `LivingBody` below and core/entity/mob.hpp.
//
// `yOffset` is 1.62 for the player and **0 for every mob**: `Entity`'s
// constructor leaves it at zero and only `EntityPlayer` sets it, so a mob's
// `posY` is its feet and its own save file says so.

// A floor to int that agrees with Java's, for the three helpers below. The
// physics itself goes through MathHelper::floorDouble, which is the original's
// own; this is here so the header needs no include of it.
inline i64 floorToInt(double v)
{
    const i64 truncated = i64(v);
    return v < double(truncated) ? truncated - 1 : truncated;
}

struct PlayerBody {
    // **What kind of living thing this is**, as the four numbers that differ.
    // The defaults are `EntityPlayer`'s; a mob overwrites them once, at the
    // point it is spawned, and never again. Floats rather than doubles because
    // the original's are floats and `setPosition` halves the width **in float**
    // before widening it -- see the note on `kPlayerWidth`.
    float width = kPlayerWidth;
    float height = kPlayerHeight;
    float yOffset = kEyeHeight;
    float stepHeight = kStepHeight;

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

    // **The fall this body just landed from**, which is what `moveEntity`
    // hands to `ge.c(F)V` -- fall damage -- the moment `onGround` becomes true
    // with a distance banked: `if (fallDistance > 0) { c(fallDistance);
    // fallDistance = 0; }`.
    //
    // A field rather than a call for the reason `stepSoundDue` is one: `move`
    // takes a `const TickWorld&` and the body has no health. Whoever owns the
    // player's vitals reads it after the tick and zeroes it; nothing in the body
    // reads it back. Left at zero by every path that clears `fallDistance`
    // without landing -- water, ladders, flight -- because none of those hurt.
    float landedFall = 0.0f;

    bool onGround = false;
    bool collidedHorizontally = false;
    bool collidedVertically = false;

    // Read by `move` for the ledge check, and set from the input each tick.
    bool sneaking = false;

    // **Where the body was when this tick began**, which is `Entity.prevPosX`,
    // `prevPosY` and `prevPosZ` and is stored for exactly the original's
    // reason: the body moves 20 times a second and the screen is drawn 30 or 60
    // times a second, so the camera has to be told where the player is
    // *between* two ticks. `EntityRenderer.orientCamera` reads
    // `prevPos + (pos - prevPos) * partialTicks`, and `renderX` below is that
    // line.
    //
    // Without it the camera stands still for one or two frames and then jumps a
    // whole tick's travel, which at a walking pace is a fifth of a block --
    // visible as stepping rather than walking, and the reason this was added.
    //
    // `prevEyeY` follows `posY` rather than `y`, because `posY` is what the
    // camera wants and it carries the step-up smoothing (`ySize`) that makes a
    // half-block step glide instead of snap.
    double prevX = 0.0, prevEyeY = 0.0, prevZ = 0.0;

    AABB box{};

    // `ge.a(FF)` -- **setSize**, which is the one line every mob's constructor
    // runs and the player's never does. It does not move the body: the box is
    // rebuilt around the feet it already has, which is what `setPosition`
    // immediately afterwards would do anyway.
    void setSize(float w, float h, float offset = 0.0f, float step = kStepHeight);

    // Places the body with its feet at (x, y, z) and rebuilds the box.
    void setFeet(double fx, double fy, double fz);

    // Where the camera goes, and what `level.dat`'s `Pos[1]` holds -- the same
    // number, and the same one the original keeps in `posY`.
    double eyeY() const { return posY; }

    // The same three, `partial` of the way from the last tick to this one.
    // **This is what the camera reads and `eyeY()` is what the save file
    // reads**: an interpolated position is a picture of a moment between two
    // ticks and is not a state the world was ever in, so writing one into
    // `Pos` would round-trip a position the physics never produced.
    double renderX(float partial) const { return prevX + (x - prevX) * double(partial); }
    double renderEyeY(float partial) const
    {
        return prevEyeY + (posY - prevEyeY) * double(partial);
    }
    double renderZ(float partial) const { return prevZ + (z - prevZ) * double(partial); }

    // **Where the camera goes, which is not always where the eye is.** A
    // crouching player's view sits `kSneakEyeDrop` below `renderEyeY` and
    // their saved `Pos[1]` does not move at all -- see the note on that
    // constant for where the number comes from and why the two are separate.
    //
    // Whatever reads this must also aim with it: the crosshair is a ray out of
    // the camera, and a view that dropped while the reach did not would put the
    // outline off the block the player is looking at.
    double cameraEyeY(float partial) const
    {
        return renderEyeY(partial) - (sneaking ? kSneakEyeDrop : 0.0);
    }

    // Forgets where the body was, so the next frame draws it where it is. Every
    // teleport needs this -- a body moved without it is drawn sliding from the
    // old place to the new one over the following tick, which is the smearing
    // `setFeet` used to produce when the gamemode changed.
    void snapRenderPosition()
    {
        prevX = x;
        prevEyeY = posY;
        prevZ = z;
    }

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

    // ---- footsteps ----------------------------------------------------
    //
    // **`distanceWalkedModified` and `nextStepDistance`**, and they are
    // `Entity`'s rather than the player's: `moveEntity` banks the distance
    // actually covered and pays out one footstep per whole block of it.
    //
    // The counter starts at 1 and is *incremented* rather than reset, which is
    // the original's and is what makes a long fall across ground pay its steps
    // out one at a time instead of all at once.
    float distanceWalked = 0.0f;
    int nextStepDistance = 1;

    // **The block whose footstep this move earned**, or air for no footstep.
    //
    // Set by `move()` -- cleared at the top of every one, so it describes the
    // most recent move and nothing older -- and read by whoever owns a sound
    // engine. The body does not play it: `core/audio/block_sound.hpp` turns a
    // block into a key, a volume and a pitch, and the platform layer is what
    // has a listener to attenuate against. Keeping the two apart is what lets
    // the whole trigger be tested with no audio at all.
    //
    // Already resolved: snow lying on top has replaced the block underfoot,
    // and a liquid has become air, because both of those decisions need the
    // world and this is where the world is.
    block::BlockId stepSoundDue = block::kAir;

    // **The cell whose `Block.onEntityWalking` this move earned**, and whether
    // there was one. The same footstep `stepSoundDue` came from -- one `if` in
    // `moveEntity` pays out both -- but it is a separate field because the two
    // do not always agree: a liquid underfoot silences the sound and snow on
    // top substitutes its own, while the block that was *trodden on* is this
    // one either way.
    //
    // **Not applied here**, for the reason `tick::entityCollidedWithBlocks` is
    // not either: `move()` takes a `const TickWorld&` so that moving a body
    // cannot write blocks. Whoever owns the tick hands this to
    // `tick::entityWalkedOnBlock` straight after the move, which is the order
    // `moveEntity` runs them in.
    bool steppedOn = false;
    i32 stepBlockX = 0;
    int stepBlockY = 0;
    i32 stepBlockZ = 0;

    // **`kh.y()`'s water state**, which is `aV` and `c`. It is a field here
    // and not a local because the splash is an *edge*: it fires on the tick
    // water is first touched and never again until the body leaves it.
    WaterEntry water{};

    // **`Entity.onEntityUpdate`'s water branch**, which is a separate call and
    // deliberately not the first line of `tick()`.
    //
    // `tick()` is `ge.j()` and `ge.b(FF)` -- onLivingUpdate and
    // moveEntityWithHeading. `y()` is a different method that runs *before*
    // them, and the two callers reach it by different routes: a mob runs it
    // inside `MobSystem::updateCounters`, which is `ge.y()`, and the player
    // runs it from the frame loop just before ticking the body. Folding it
    // into `tick()` would run it twice for every animal.
    //
    // It clears `fallDistance`, as the branch does. It does not clear a fire
    // counter because a player body has none -- see `Mob::fire`, which does.
    //
    // The caller plays the splash: this returns the volume and
    // `entity::splashPitch` draws the pitch, for the reason `stepSoundDue`
    // gives above.
    WaterEntryResult updateWaterEntry(const tick::TickWorld& world);

    // The liquid half of `moveEntityWithHeading`, shared by water and lava.
    void swim(const tick::TickWorld& world, const PlayerInput& input, double drag);

    // `kh.b(DDD)Z` -- whether the box moved by this much would be clear of both
    // collision and liquid. Named for what it answers rather than for what the
    // jar calls it, which is `isOffsetPositionInLiquid` and is the opposite way
    // round.
    bool offsetPositionFree(const tick::TickWorld& world, double dx, double dy, double dz) const;

    // `EntityLiving.jump`, which is one assignment and nothing else.
    void jump() { motionY = kJumpVelocity; }

    // **One tick of riding something**, which is `Entity.updateRidden` for the
    // rider's half and is the first thing in this project that takes the body
    // off its own physics.
    //
    // The surprising part -- and the reason a boat responds to the movement
    // keys at all -- is that **`EntityLiving` never checks whether it is
    // riding**. There is no reference to `ridingEntity` anywhere in `ge`, so
    // `moveEntityWithHeading` keeps turning the stick into `motionX`/`motionZ`
    // exactly as it would on foot; what a vehicle reads is that motion, and
    // what gets overwritten is only the *position*. So this applies the heading
    // and the air friction and then puts the body where the seat says.
    //
    // `seatY` is the vehicle's `posY + getMountedYOffset()`; this adds the
    // body's own `yOffset` on top, which is the rider's business and not the
    // vehicle's.
    void tickRiding(const PlayerInput& input, double seatX, double seatY, double seatZ);

    // **The position half of `tickRiding`, for a vehicle that moves after its
    // rider does.** A boat and a minecart are ticked inside the body's own loop
    // because they read the rider's motion from that same tick; a pig is not --
    // it ignores its rider entirely and is ticked with the other animals, after
    // the body. Without this the rider would sit a tick behind the animal it is
    // on, which at a walking pace is a fifth of a block of the world sliding
    // under them.
    //
    // It does **not** snapshot the previous position: `tickRiding` already did
    // that this tick, and doing it twice would flatten the camera's
    // interpolation to nothing.
    void followSeat(double seatX, double seatY, double seatZ);

    // `kh.g_()` and `kh.G()`. Both shrink the body's box by 0.4 top and bottom
    // before asking, so a puddle at the ankles is not water to swim in.
    //
    // **Water asks a harder question than lava.** Being in water is "a cell of
    // the probe holds water whose *surface* reaches the top of it", which is
    // `handleMaterialAcceleration`; being in lava is the plain material test.
    // That difference is the jar's.
    //
    // **And water is not a question at all.** `kh.g_()` answers by walking
    // every water cell the probe touches, summing their flow vectors and
    // adding four thousandths of the normalised total to the motion -- so
    // asking it *is* being carried by the current, and there is no const
    // version of it to ask instead. That is why this one is a mutator and
    // `inLava` is not: lava has no vector in this version and carries nothing.
    // See core/block/fluid_flow.hpp.
    bool handleWaterMovement(const tick::TickWorld& world);
    bool inLava(const tick::TickWorld& world) const;

    // `ge.A()` -- **isOnLadder**, and it looks at two cells rather than one:
    //
    // ```
    // int i = floor(posX), j = floor(boundingBox.minY), k = floor(posZ);
    // return world.getBlockId(i, j, k) == ladder
    //     || world.getBlockId(i, j + 1, k) == ladder;
    // ```
    //
    // The second half is the one that matters to a player: a body is 1.8 tall
    // and the feet leave a ladder's bottom cell before the chest leaves the one
    // above, so without it the last block of every climb drops you.
    //
    // **It reads the box's bottom, not `posY`** -- `posY` is the eye. And it
    // asks for the ladder *block*, not for a shape or a material, which is why
    // standing on a rail or in a doorway is not climbing.
    bool onLadder(const tick::TickWorld& world) const;

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

// **The same struct under the name a mob calls it by.** `ge` -- EntityLiving --
// is what holds this physics in the jar, and `EntityPlayer` is one of its
// subclasses rather than the other way round; a pig and a player run the same
// `moveEntity` and the same `moveEntityWithHeading`, differing only in the four
// dimensions above and in who fills `PlayerInput`. An alias rather than a
// rename because this file's name and its 22-case oracle are the player's, and
// a mob reading `body.tick(world, input)` should not have to read "player" to
// find out what it is. See core/entity/mob.hpp.
using LivingBody = PlayerBody;

}  // namespace mc::entity
