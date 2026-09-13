#include "core/tick/redstone.hpp"

#include "core/block/registry.hpp"
#include "core/tick/drop.hpp"
#include "core/tick/tick_world.hpp"

namespace mc::tick {

namespace {

using block::BlockId;
using block::TickBehaviour;

BlockId id(mcver::Block b) { return BlockId(b); }

bool isWire(BlockId b) { return block::def(b).tick == TickBehaviour::RedstoneWire; }

// `kf.b(Lnm;III)Z` (static) -- isPowerProviderOrWire.
bool powerProviderOrWire(const TickWorld& world, i32 x, int y, i32 z)
{
    const BlockId b = world.blockAt(x, y, z);
    if (isWire(b)) return true;
    if (b == block::kAir) return false;
    return canProvidePower(b);
}

// ---- the wire ---------------------------------------------------------

// `kf.g(Lcn;IIII)I` -- getMaxCurrentStrength, folded into a running maximum.
// A cell that is not wire leaves the running value alone, which is how -1 can
// be used as "tell me whether this is wire at all".
int maxCurrentStrength(const TickWorld& world, i32 x, int y, i32 z, int best, BlockId self)
{
    if (world.blockAt(x, y, z) != self) return best;
    const int md = int(world.dataAt(x, y, z));
    return md > best ? md : best;
}

// `kf.i(Lcn;III)V` -- notifyWireNeighborsOfNeighborChange. Seven notifications
// including the cell itself, and only if it really is wire.
void notifyAround(TickWorld& world, i32 x, int y, i32 z, BlockId self)
{
    if (world.blockAt(x, y, z) != self) return;
    world.notifyNeighbours(x, y, z, self);
    world.notifyNeighbours(x - 1, y, z, self);
    world.notifyNeighbours(x + 1, y, z, self);
    world.notifyNeighbours(x, y, z - 1, self);
    world.notifyNeighbours(x, y, z + 1, self);
    world.notifyNeighbours(x, y - 1, z, self);
    world.notifyNeighbours(x, y + 1, z, self);
}

// The four horizontal directions, in the wire's own order.
constexpr i32 kDx[4] = {-1, 1, 0, 0};
constexpr i32 kDz[4] = {0, 0, -1, 1};

// **The recursion is bounded and is not in the original.** a1.1.2 recurses
// through the wire net with no guard at all, which is safe there because a
// signal dies after fifteen blocks and so does the wave of changes. A 3DSX main
// thread gets 32 KB of stack that nothing in the binary can enlarge, so a net
// built to defeat that reasoning would take the process down rather than
// misbehave.
//
// **The bound used to be a per-entry depth of 64 and it did not work.** Every
// hop through `notifyNeighbours` re-entered `wirePropagate` with depth 0 -- see
// wireNeighbourChanged below -- so 64 bounded one straight run of wire and not
// the cascade, and the cascade is what overflows. The depth parameter is gone;
// what bounds this now is TickWorld's shared cascade budget, which every
// recursive path in the tick charges against. See
// TickWorld::kCascadeStackBudget.
// Declared in redstone.hpp; wirePropagateInner recurses through it so that the
// cascade budget is charged on every step rather than only on the way in.

// `kf.h(Lcn;III)V`.
void wirePropagateInner(TickWorld& world, i32 x, int y, i32 z, BlockId self)
{
    const int old = int(world.dataAt(x, y, z));
    int strength = 0;

    // The flag is off for exactly this query. Without it the wire counts
    // itself and its neighbouring wire as power sources and everything is 15.
    world.setWiresProvidePower(false);
    const bool powered = world.isIndirectlyPowered(x, y, z);
    world.setWiresProvidePower(true);

    if (powered) {
        strength = 15;
    } else {
        for (int d = 0; d < 4; ++d) {
            const i32 bx = x + kDx[d];
            const i32 bz = z + kDz[d];

            strength = maxCurrentStrength(world, bx, y, bz, strength, self);

            // Up over a solid neighbour, but only when this cell has open sky
            // above it; down off an open one. This pair is what lets wire run
            // up and down a staircase and refuse to climb under a ceiling.
            if (world.opaqueAt(bx, y, bz)) {
                if (!world.opaqueAt(x, y + 1, z)) {
                    strength = maxCurrentStrength(world, bx, y + 1, bz, strength, self);
                }
            } else {
                strength = maxCurrentStrength(world, bx, y - 1, bz, strength, self);
            }
        }
        strength = strength > 0 ? strength - 1 : 0;
    }

    if (old == strength) return;

    world.setDataRaw(x, y, z, u8(strength));

    // The original decrements again *after* the write, and the value the
    // propagation below compares against is that smaller one. Keeping the two
    // separate is not tidiness -- using the written value here re-walks the
    // whole net on every step.
    if (strength > 0) --strength;

    for (int d = 0; d < 4; ++d) {
        const i32 bx = x + kDx[d];
        const i32 bz = z + kDz[d];
        // The vertical neighbour to check is below an open block and above a
        // solid one, which is the same rule as the climb above, read the
        // other way.
        const int by = world.opaqueAt(bx, y, bz) ? y + 1 : y - 1;

        int c = maxCurrentStrength(world, bx, y, bz, -1, self);
        if (c >= 0 && c != strength) wirePropagate(world, bx, y, bz, self);

        c = maxCurrentStrength(world, bx, by, bz, -1, self);
        if (c >= 0 && c != strength) wirePropagate(world, bx, by, bz, self);
    }

    // Only the edges of the signal notify: a wire going from off to on, or on
    // to off. A wire merely changing strength does not wake what it touches,
    // which is why a long line of it is cheap.
    if (old == 0 || strength == 0) {
        world.notifyNeighbours(x, y, z, self);
        world.notifyNeighbours(x - 1, y, z, self);
        world.notifyNeighbours(x + 1, y, z, self);
        world.notifyNeighbours(x, y, z - 1, self);
        world.notifyNeighbours(x, y, z + 1, self);
        world.notifyNeighbours(x, y - 1, z, self);
        world.notifyNeighbours(x, y + 1, z, self);
    }
}

// The eight-neighbour sweep `onBlockAdded` and `onBlockRemoval` both run: the
// four horizontal neighbours, and then for each of them the cell one above if
// it is solid or one below if it is not.
void notifyWireNet(TickWorld& world, i32 x, int y, i32 z, BlockId self)
{
    for (int d = 0; d < 4; ++d) {
        notifyAround(world, x + kDx[d], y, z + kDz[d], self);
    }
    for (int d = 0; d < 4; ++d) {
        const i32 bx = x + kDx[d];
        const i32 bz = z + kDz[d];
        if (world.opaqueAt(bx, y, bz)) {
            notifyAround(world, bx, y + 1, bz, self);
        } else {
            notifyAround(world, bx, y - 1, bz, self);
        }
    }
}

// ---- the torch --------------------------------------------------------

// `ly.aQ` is 75, the dark torch, and `ly.aR` is 76, the lit one -- the same
// adjacent-pair arrangement the fluids use. The generated names say which is
// which, so nothing here has to remember that 76 is the bright one.
bool torchIsLit(BlockId b) { return b == id(mcver::Block::RedstoneTorch); }
BlockId torchLit() { return id(mcver::Block::RedstoneTorch); }
BlockId torchUnlit() { return id(mcver::Block::UnlitRedstoneTorch); }

// `bg.h(Lcn;III)Z` -- shouldBeOff: is the block this torch is attached to
// being powered? Metadata names the face, 1..5.
bool torchShouldBeOff(const TickWorld& world, i32 x, int y, i32 z)
{
    const int md = int(world.dataAt(x, y, z));
    if (md == 5) return world.indirectlyProvidesPowerTo(x, y - 1, z, 0);
    if (md == 3) return world.indirectlyProvidesPowerTo(x, y, z - 1, 2);
    if (md == 4) return world.indirectlyProvidesPowerTo(x, y, z + 1, 3);
    if (md == 1) return world.indirectlyProvidesPowerTo(x - 1, y, z, 4);
    if (md == 2) return world.indirectlyProvidesPowerTo(x + 1, y, z, 5);
    return false;
}

void notifySix(TickWorld& world, i32 x, int y, i32 z, BlockId self)
{
    world.notifyNeighbours(x, y - 1, z, self);
    world.notifyNeighbours(x, y + 1, z, self);
    world.notifyNeighbours(x - 1, y, z, self);
    world.notifyNeighbours(x + 1, y, z, self);
    world.notifyNeighbours(x, y, z - 1, self);
    world.notifyNeighbours(x, y, z + 1, self);
}

// ---- switches ---------------------------------------------------------

// The low three bits of a lever's or a button's metadata name the face it
// hangs on; bit 3 is "on". `wallOpaque` asks whether the named face is still
// something to hang from.
//
// **A face the original does not test answers "still supported", and 6 is one
// of them.** `no.a(Lcn;IIII)V` is a run of five `if (!isBlockNormalCube(..) &&
// meta == n)` lines, n from 1 to 5, each of which *sets* a drop flag; a
// metadata the chain never names leaves the flag alone and the block stays.
// A floor lever is 5 **or 6** -- `BlockLever.onBlockAdded` writes
// `5 + rand.nextInt(2)`, which is which way round the handle lies -- so half of
// all floor levers wear a face this method must not refuse. Answering false
// here deleted them: flicking one notifies its own cell, the notification came
// straight back here, and the lever removed itself mid-flick.
bool faceSupported(const TickWorld& world, i32 x, int y, i32 z, int face)
{
    switch (face) {
    case 1: return world.opaqueAt(x - 1, y, z);
    case 2: return world.opaqueAt(x + 1, y, z);
    case 3: return world.opaqueAt(x, y, z - 1);
    case 4: return world.opaqueAt(x, y, z + 1);
    case 5: return world.opaqueAt(x, y - 1, z);
    default:
        // 6 -- the other floor lever -- and the values nothing writes. The
        // original names none of them, and `switchHasSupport` above has already
        // insisted on *something* to hang from.
        return true;
    }
}

// `no.a(Lcn;III)Z` takes five faces and `hu.a(Lcn;III)Z` takes four -- a lever
// can sit on the floor and a button cannot.
bool switchHasSupport(const TickWorld& world, i32 x, int y, i32 z, bool allowFloor)
{
    if (world.opaqueAt(x - 1, y, z) || world.opaqueAt(x + 1, y, z) ||
        world.opaqueAt(x, y, z - 1) || world.opaqueAt(x, y, z + 1)) {
        return true;
    }
    return allowFloor && world.opaqueAt(x, y - 1, z);
}

// The face-to-side map both share: the side a switch hands direct power to is
// the one opposite the face it hangs on.
// The notify a switch does after it flips: itself, and then whatever its
// metadata says it is stuck to. Six lines that appear three times in the class
// files -- a button's press, a button's release and a lever's flick -- and are
// one function here.
//
// The face is read back out of the world rather than passed in, which is safe
// because every caller writes the low three bits back unchanged: only bit 3,
// the on/off bit, ever moves.
void notifyAttached(TickWorld& world, i32 x, int y, i32 z, BlockId self)
{
    world.notifyNeighbours(x, y, z, self);
    switch (int(world.dataAt(x, y, z)) & 7) {
    case 1: world.notifyNeighbours(x - 1, y, z, self); break;
    case 2: world.notifyNeighbours(x + 1, y, z, self); break;
    case 3: world.notifyNeighbours(x, y, z - 1, self); break;
    case 4: world.notifyNeighbours(x, y, z + 1, self); break;
    default: world.notifyNeighbours(x, y - 1, z, self); break;
    }
}

bool switchPowersSide(int face, int side)
{
    if (face == 5 && side == 1) return true;
    if (face == 4 && side == 2) return true;
    if (face == 3 && side == 3) return true;
    if (face == 2 && side == 4) return true;
    if (face == 1 && side == 5) return true;
    return false;
}

// ---- the door ---------------------------------------------------------

// `fw.a(Lcn;IIIZ)V` -- onPoweredBlockChange. Bit 3 of the metadata is "this is
// the upper half"; bit 2 is "open". The upper half never decides anything --
// it delegates down -- and the lower half writes both.
void doorSetOpen(TickWorld& world, i32 x, int y, i32 z, BlockId self, bool open)
{
    const int md = int(world.dataAt(x, y, z));
    if ((md & 8) != 0) {
        if (world.blockAt(x, y - 1, z) == self) doorSetOpen(world, x, y - 1, z, self, open);
        return;
    }
    if (((md & 4) > 0) == open) return;

    if (world.blockAt(x, y + 1, z) == self) {
        world.setDataRaw(x, y + 1, z, u8((md ^ 4) + 8));
    }
    world.setDataRaw(x, y, z, u8(md ^ 4));

    // `random.door_open` or `random.door_close` at volume 1, pitched
    // `rand.nextFloat() * 0.1F + 0.9F` -- **out of the world's own Random**,
    // which is why this is here and not in the caller: the draw shifts the
    // stream every later random tick reads, so a door that is opened is a
    // different world from a door that is not.
    const float pitch = world.random().nextFloat() * 0.1f + 0.9f;
    world.playSoundAt(open ? "random.door_open" : "random.door_close", double(x) + 0.5,
                      double(y) + 0.5, double(z) + 0.5, 1.0f, pitch);
}

}  // namespace

// `ly.d()Z` -- canProvidePower, false on the base class. Only a wire, a torch,
// a lever, a button and a pressure plate answer true, and it is a property of
// the class rather than a table, so it is derived from the behaviour column.
bool canProvidePower(BlockId b)
{
    switch (block::def(b).tick) {
    case TickBehaviour::RedstoneWire:
    case TickBehaviour::RedstoneTorch:
    case TickBehaviour::Lever:
    case TickBehaviour::Button:
    case TickBehaviour::PressurePlateAll:
    case TickBehaviour::PressurePlateMobs:
        return true;
    default:
        return false;
    }
}

bool providesPowerTo(const TickWorld& world, i32 x, int y, i32 z, int side, BlockId self)
{
    switch (block::def(self).tick) {
    case TickBehaviour::RedstoneWire:
        // `kf.c`: a wire hands out direct power exactly where it hands out
        // indirect power, when it is allowed to at all.
        if (!world.wiresProvidePower()) return false;
        return indirectlyProvidesPowerTo(world, x, y, z, side, self);
    case TickBehaviour::RedstoneTorch:
        // `bg.c`: a torch's *direct* power goes only into the block above it,
        // which is asked as side 0 from up there.
        if (side != 0) return false;
        return indirectlyProvidesPowerTo(world, x, y, z, side, self);
    case TickBehaviour::Lever:
    case TickBehaviour::Button: {
        // `no.c` and `hu.c`, which are the same method twice.
        const int md = int(world.dataAt(x, y, z));
        if ((md & 8) == 0) return false;
        return switchPowersSide(md & 7, side);
    }
    case TickBehaviour::PressurePlateAll:
    case TickBehaviour::PressurePlateMobs:
        // `al.c`: a plate under load powers straight up and nothing else.
        if (world.dataAt(x, y, z) == 0) return false;
        return side == 1;
    default:
        return false;  // `ly.c` is false for everything else
    }
}

bool indirectlyProvidesPowerTo(const TickWorld& world, i32 x, int y, i32 z, int side,
                               BlockId self)
{
    switch (block::def(self).tick) {
    case TickBehaviour::RedstoneTorch: {
        // `bg.b`: a lit torch powers every side except the one it hangs by.
        if (!torchIsLit(self)) return false;
        const int md = int(world.dataAt(x, y, z));
        if (md == 5 && side == 1) return false;
        if (md == 3 && side == 3) return false;
        if (md == 4 && side == 2) return false;
        if (md == 1 && side == 5) return false;
        if (md == 2 && side == 4) return false;
        return true;
    }
    case TickBehaviour::RedstoneWire: {
        // `kf.b(Lnm;IIII)Z`. A wire powers straight up unconditionally, and
        // sideways only along the direction it is actually running -- which is
        // worked out here from what is around it, because a1.1.2 stores no
        // shape in the metadata.
        if (!world.wiresProvidePower()) return false;
        if (world.dataAt(x, y, z) == 0) return false;
        if (side == 1) return true;

        bool west = powerProviderOrWire(world, x - 1, y, z) ||
                    (!world.opaqueAt(x - 1, y, z) &&
                     powerProviderOrWire(world, x - 1, y - 1, z));
        bool east = powerProviderOrWire(world, x + 1, y, z) ||
                    (!world.opaqueAt(x + 1, y, z) &&
                     powerProviderOrWire(world, x + 1, y - 1, z));
        bool north = powerProviderOrWire(world, x, y, z - 1) ||
                     (!world.opaqueAt(x, y, z - 1) &&
                      powerProviderOrWire(world, x, y - 1, z - 1));
        bool south = powerProviderOrWire(world, x, y, z + 1) ||
                     (!world.opaqueAt(x, y, z + 1) &&
                      powerProviderOrWire(world, x, y - 1, z + 1));

        // A wire climbing a solid neighbour also counts as connected that way,
        // but only when nothing is sitting on top of this cell.
        if (!world.opaqueAt(x, y + 1, z)) {
            if (world.opaqueAt(x - 1, y, z) &&
                powerProviderOrWire(world, x - 1, y + 1, z)) {
                west = true;
            }
            if (world.opaqueAt(x + 1, y, z) &&
                powerProviderOrWire(world, x + 1, y + 1, z)) {
                east = true;
            }
            if (world.opaqueAt(x, y, z - 1) &&
                powerProviderOrWire(world, x, y + 1, z - 1)) {
                north = true;
            }
            if (world.opaqueAt(x, y, z + 1) &&
                powerProviderOrWire(world, x, y + 1, z + 1)) {
                south = true;
            }
        }

        // Wire connected to nothing is a dot, and a dot powers all four sides.
        if (!north && !east && !west && !south && side >= 2 && side <= 5) return true;
        // Otherwise it powers only along its own run, and only when that run
        // is straight: a corner powers neither of its ends.
        if (side == 2 && north && !west && !east) return true;
        if (side == 3 && south && !west && !east) return true;
        if (side == 4 && west && !north && !south) return true;
        if (side == 5 && east && !north && !south) return true;
        return false;
    }
    case TickBehaviour::Lever:
    case TickBehaviour::Button:
        // `no.b` and `hu.b`: a switch that is on powers every side around it,
        // and the face it hangs on is irrelevant to the indirect answer.
        return (world.dataAt(x, y, z) & 8) != 0;
    case TickBehaviour::PressurePlateAll:
    case TickBehaviour::PressurePlateMobs:
        return world.dataAt(x, y, z) > 0;
    default:
        return false;  // `ly.b(Lnm;IIII)Z` is false for everything else
    }
}

void wirePropagate(TickWorld& world, i32 x, int y, i32 z, BlockId self)
{
    // The shared budget, not a local depth: a wire net reached through a chain
    // of neighbour notifications has to be charged for the stack those hops are
    // already holding, and a per-entry counter cannot see them. This is the one
    // gate -- wirePropagateInner recurses back through here.
    if (!world.enterCascade(TickWorld::kWireLevelBytes)) {
        world.noteWireRefused();
        return;
    }
    wirePropagateInner(world, x, y, z, self);
    world.leaveCascade(TickWorld::kWireLevelBytes);
}

void wireNeighbourChanged(TickWorld& world, i32 x, int y, i32 z, BlockId self)
{
    // `kf.a(Lcn;IIII)V`: a wire needs an opaque cube under it, and leaves a
    // pile of redstone behind when it has not got one.
    if (!world.opaqueAt(x, y - 1, z)) {
        dropBlockAsItem(world, x, y, z, self, world.dataAt(x, y, z));
        world.setBlockWithNotify(x, y, z, block::kAir);
        return;
    }
    wirePropagate(world, x, y, z, self);
}

void wirePlaced(TickWorld& world, i32 x, int y, i32 z, BlockId self)
{
    wirePropagate(world, x, y, z, self);
    world.notifyNeighbours(x, y + 1, z, self);
    world.notifyNeighbours(x, y - 1, z, self);
    notifyWireNet(world, x, y, z, self);
}

void wireRemoved(TickWorld& world, i32 x, int y, i32 z, BlockId self)
{
    world.notifyNeighbours(x, y + 1, z, self);
    world.notifyNeighbours(x, y - 1, z, self);
    wirePropagate(world, x, y, z, self);
    notifyWireNet(world, x, y, z, self);
}

void redstoneTorchTick(TickWorld& world, i32 x, int y, i32 z, BlockId self, JavaRandom& rand)
{
    (void) rand;
    const bool shouldBeOff = torchShouldBeOff(world, x, y, z);
    const u8 md = world.dataAt(x, y, z);

    if (torchIsLit(self)) {
        if (!shouldBeOff) return;
        world.setBlockAndDataRaw(x, y, z, torchUnlit(), md);
        // `bg.a(Lcn;IIIZ)Z` with `true`: record the toggle, and if this
        // position has flipped eight times inside 100 ticks the torch has
        // burnt out and stays dark. That is a1.1.2's oscillator brake.
        (void) world.noteTorchToggle(x, y, z);
        notifySix(world, x, y, z, torchUnlit());
        return;
    }

    if (shouldBeOff) return;
    // Recording is *not* done on the way back on: the original passes false,
    // so relighting only reads the count.
    if (world.torchToggleCount(x, y, z) >= 8) return;
    world.setBlockAndDataRaw(x, y, z, torchLit(), md);
    notifySix(world, x, y, z, torchLit());
}

void redstoneTorchNeighbourChanged(TickWorld& world, i32 x, int y, i32 z, BlockId self)
{
    world.scheduleBlockUpdate(x, y, z, self);
}

void redstoneTorchPlaced(TickWorld& world, i32 x, int y, i32 z, BlockId self)
{
    if (!torchIsLit(self)) return;
    notifySix(world, x, y, z, self);
}

void redstoneTorchRemoved(TickWorld& world, i32 x, int y, i32 z, BlockId self)
{
    if (!torchIsLit(self)) return;
    notifySix(world, x, y, z, self);
}

void redstoneOreTick(TickWorld& world, i32 x, int y, i32 z, BlockId self)
{
    // `ai.a(...)`: only the lit one does anything, and all it does is stop
    // being lit. What lights it is a player walking on or hitting it, which
    // needs a player.
    if (self != id(mcver::Block::LitRedstoneOre)) return;
    world.setBlockWithNotify(x, y, z, id(mcver::Block::RedstoneOre));
}

void buttonTick(TickWorld& world, i32 x, int y, i32 z, BlockId self)
{
    // `hu.a(Lcn;IIILjava/util/Random;)V`. Twenty ticks after it was pressed --
    // its tick rate -- the button lets itself back out and tells both itself
    // and the wall it is on.
    const int md = int(world.dataAt(x, y, z));
    if ((md & 8) == 0) return;

    world.setDataRaw(x, y, z, u8(md & 7));
    notifyAttached(world, x, y, z, self);
    // `random.click` at 0.3, **pitched 0.5 coming out** against the 0.6 it went
    // in at -- the two-note click of a button, and the only thing that says it
    // has popped back up.
    world.playSoundAt("random.click", double(x) + 0.5, double(y) + 0.5, double(z) + 0.5,
                      0.3f, 0.5f);
}

namespace {

// `js` -- which entities this plate answers to. The jar passes it to `al`'s
// constructor; the table carries it as the behaviour, for the reason
// core/block/block_def.hpp gives.
EntityFilter plateFilter(BlockId self)
{
    return block::def(self).tick == TickBehaviour::PressurePlateMobs ? EntityFilter::Mobs
                                                                    : EntityFilter::Everything;
}

}  // namespace

void pressurePlateSense(TickWorld& world, i32 x, int y, i32 z, BlockId self)
{
    // `boolean flag = world.getBlockMetadata(i,j,k) == 1` -- and it is `== 1`
    // rather than `!= 0`, which matters nowhere in this version because the
    // only two values ever written are 0 and 1, and is transcribed anyway.
    const bool wasArmed = world.dataAt(x, y, z) == 1;

    // `float f = 0.125F`, and a quarter of a block tall. **The arithmetic is
    // the class file's, floats and all**: `(float)i + f` promoted to double,
    // not `(double)i + 0.125`. The two disagree once the coordinate is large
    // enough that a float cannot hold it exactly, which this project reaches
    // -- a1.1.2's world runs to +-32,000,000 and a float loses the eighth at
    // about 2^21.
    constexpr float kInset = 0.125f;
    const AABB box{
        double(float(x) + kInset),        double(y),           double(float(z) + kInset),
        double(float(x + 1) - kInset),    double(y) + 0.25,    double(float(z + 1) - kInset),
    };

    // `list.size() > 0`. The list itself is never read, so the seam is a
    // predicate; see TickWorld::anyEntityIn.
    const bool armed = world.anyEntityIn(box, plateFilter(self));

    if (armed && !wasArmed) {
        // `cn.b(IIII)V` is setBlockMetadata and notifies **nothing** -- the
        // two `notifyBlocksOfNeighborChange` calls below are the whole of the
        // notification, and they are the plate's own. Using the notifying
        // write here would send a third round and change the order a circuit
        // settles in.
        world.setDataRaw(x, y, z, 1);
        world.notifyNeighbours(x, y, z, self);
        world.notifyNeighbours(x, y - 1, z, self);
        // `markBlocksDirty`, then `random.click` at 0.3/0.6. The redraw is
        // `setDataRaw`'s `changed` callback. **The height is `j + 0.1`**, not
        // `j + 0.5`: a plate is a quarter of a block tall and the original
        // plays from just above the floor.
        world.playSoundAt("random.click", double(x) + 0.5, double(y) + 0.1, double(z) + 0.5,
                          0.3f, 0.6f);
    }
    if (!armed && wasArmed) {
        world.setDataRaw(x, y, z, 0);
        world.notifyNeighbours(x, y, z, self);
        world.notifyNeighbours(x, y - 1, z, self);
        // `random.click` again, at 0.3/**0.5** -- a lower pitch on the way up
        // than on the way down, which is the two-note click of a plate.
        world.playSoundAt("random.click", double(x) + 0.5, double(y) + 0.1, double(z) + 0.5,
                          0.3f, 0.5f);
    }

    // **Only while something is on it.** A plate that has just been stepped
    // off does not re-schedule, so the chain ends here rather than ticking for
    // ever; the twenty-tick delay is the block's own tickRate.
    if (armed) {
        world.scheduleBlockUpdate(x, y, z, self);
    }
}

void pressurePlateTick(TickWorld& world, i32 x, int y, i32 z, BlockId self)
{
    // `if (world.getBlockMetadata(i,j,k) == 0) return;` -- a plate that is
    // already up has nothing to do on a timer, because nothing but contact can
    // put it down.
    if (world.dataAt(x, y, z) == 0) {
        return;
    }
    pressurePlateSense(world, x, y, z, self);
}

void pressurePlateCollided(TickWorld& world, i32 x, int y, i32 z, BlockId self)
{
    // `if (world.getBlockMetadata(i,j,k) == 1) return;` -- the mirror of the
    // above. A plate already down is left to its timer, which is what stops
    // every entity in the box re-notifying its neighbours every tick.
    if (world.dataAt(x, y, z) == 1) {
        return;
    }
    pressurePlateSense(world, x, y, z, self);
}

// **All three of these drop themselves before they go**, which is the whole of
// `b_(Lcn;IIII)V` on the end of each of `al.a`, `no.a` and `hu.a`. It reads as
// a small thing and it is the difference between knocking the wall out from
// behind a lever and getting the lever back, and knocking it out and having
// deleted a lever.
void switchNeighbourChanged(TickWorld& world, i32 x, int y, i32 z, BlockId self)
{
    const TickBehaviour behaviour = block::def(self).tick;

    if (behaviour == TickBehaviour::PressurePlateAll
        || behaviour == TickBehaviour::PressurePlateMobs) {
        // `al.a(Lcn;IIII)V`: a plate wants an opaque cube directly under it and
        // has no face to name.
        if (world.opaqueAt(x, y - 1, z)) return;
        dropBlockAsItem(world, x, y, z, self, world.dataAt(x, y, z));
        world.setBlockWithNotify(x, y, z, block::kAir);
        return;
    }

    const bool allowFloor = behaviour == TickBehaviour::Lever;
    if (!switchHasSupport(world, x, y, z, allowFloor)) {
        dropBlockAsItem(world, x, y, z, self, world.dataAt(x, y, z));
        world.setBlockWithNotify(x, y, z, block::kAir);
        return;
    }

    // Attached *somewhere* is not enough: the face the metadata names is the
    // one that has to still be there.
    const int face = int(world.dataAt(x, y, z)) & 7;
    if (faceSupported(world, x, y, z, face)) return;
    dropBlockAsItem(world, x, y, z, self, world.dataAt(x, y, z));
    world.setBlockWithNotify(x, y, z, block::kAir);
}

void doorNeighbourChanged(TickWorld& world, i32 x, int y, i32 z, BlockId self,
                          BlockId fromId)
{
    const int md = int(world.dataAt(x, y, z));

    if ((md & 8) != 0) {
        // The upper half. It only checks that the lower half is still there,
        // and hands any power question down.
        if (world.blockAt(x, y - 1, z) != self) {
            world.setBlockWithNotify(x, y, z, block::kAir);
        }
        if (fromId != block::kAir && canProvidePower(fromId)) {
            doorNeighbourChanged(world, x, y - 1, z, self, fromId);
        }
        return;
    }

    bool broken = false;
    if (world.blockAt(x, y + 1, z) != self) {
        world.setBlockWithNotify(x, y, z, block::kAir);
        broken = true;
    }
    if (!world.opaqueAt(x, y - 1, z)) {
        world.setBlockWithNotify(x, y, z, block::kAir);
        broken = true;
        if (world.blockAt(x, y + 1, z) == self) {
            world.setBlockWithNotify(x, y + 1, z, block::kAir);
        }
    }
    // `if (flag) dropBlockAsItem(world, i, j, k, l)` -- **the metadata is the
    // lower half's**, which is what makes the drop table's `itemByMetadata` row
    // give a door item here and nothing at all for the upper half. A door
    // therefore comes back as one item however it was knocked down.
    if (broken) {
        dropBlockAsItem(world, x, y, z, self, u8(md));
        return;
    }

    // **Only a power source wakes a door.** A door does not re-read the world
    // every time any neighbour changes; it looks only when the block that
    // changed was something that can provide power, which is what keeps a
    // doorway in a wall from costing anything.
    if (fromId == block::kAir || !canProvidePower(fromId)) return;

    const bool powered = world.isIndirectlyPowered(x, y, z) ||
                         world.isIndirectlyPowered(x, y + 1, z);
    doorSetOpen(world, x, y, z, self, powered);
}

// ---- what a hand does to them -----------------------------------------
//
// The four `blockActivated` overrides that are mechanisms rather than screens.
// Dispatched by tick::blockActivated; see core/tick/behaviour.hpp for the
// order a right-click goes in and for the ones that are missing.

bool doorActivated(TickWorld& world, i32 x, int y, i32 z, BlockId self)
{
    // `fw.a(Lcn;IIILdm;)Z`. **An iron door eats the click and does nothing** --
    // its first line is `if (material == Material.iron) return true`, which is
    // the whole of why an iron door needs redstone. Naming a block to get at a
    // material is the same move `PlayerBody` makes for water and lava: the test
    // in the class file is on the material, so the material is what is
    // compared, and the id is only how it is found.
    constexpr u8 kIronMaterial = mcver::kBlocks[int(mcver::Block::IronDoor)].material;
    if (block::def(self).material == kIronMaterial) return true;

    const int md = int(world.dataAt(x, y, z));
    if ((md & 8) != 0) {
        // The upper half hands the click down and answers for both.
        if (world.blockAt(x, y - 1, z) == self) {
            doorActivated(world, x, y - 1, z, self);
        }
        return true;
    }

    // `meta ^ 4` on this half and `(meta ^ 4) + 8` on the one above, which is
    // exactly what the redstone path already does once it has decided which
    // way the door should be -- so the flip is that function with the state
    // negated rather than a second copy of it.
    doorSetOpen(world, x, y, z, self, (md & 4) == 0);
    return true;
}

void leverPlaced(TickWorld& world, i32 x, int y, i32 z, BlockId self)
{
    // `no.e(Lcn;III)V` -- **BlockLever.onBlockAdded**, and it is the method
    // that gives a lever an orientation when the struck face did not.
    //
    // The original's order is onBlockAdded first, picking the first opaque
    // neighbour it finds, and then `onBlockPlaced(side)` overwriting that with
    // the struck face. This port writes the struck face's answer up front, out
    // of the generated placement table -- so this runs *last* where the
    // original ran it first, and it must only supply what a static table
    // cannot. There are exactly two such things:
    //
    //   * **The underside.** `onBlockPlaced` names sides 1 to 5 and leaves side
    //     0 alone, so the table's row for it is 0 -- and 0 is not an
    //     orientation. Whatever wall is beside the lever is the answer, which
    //     is the chain below.
    //   * **`5 + rand.nextInt(2)`**, which is which way round a floor lever's
    //     handle lies. Both `onBlockAdded` and `onBlockPlaced` roll it and a
    //     `(id, face)` table can carry neither, so the roll happens here. It is
    //     `World.rand` in the original and `world.random()` here, which is the
    //     same source; the random tick's positions come out of the update LCG
    //     and are not disturbed by it.
    //
    // The second is not cosmetic, which is worth saying because it looks it:
    // `no.c(Lcn;IIII)Z` names orientations 1 to 5 and **not 6**, so a floor
    // lever lying the second way round hands no *direct* power to the block it
    // stands on. That is a1.1.2's own quirk, and pinning the roll would have
    // made every floor lever in the game wear it.
    const int md = int(world.dataAt(x, y, z));
    const int orientation = md & 7;

    if (orientation == 5 || orientation == 6) {
        world.setDataRaw(x, y, z, u8((md & 8) + 5 + world.random().nextInt(2)));
        return;
    }
    if (orientation != 0) return;

    int face = 0;
    if (world.opaqueAt(x - 1, y, z)) {
        face = 1;
    } else if (world.opaqueAt(x + 1, y, z)) {
        face = 2;
    } else if (world.opaqueAt(x, y, z - 1)) {
        face = 3;
    } else if (world.opaqueAt(x, y, z + 1)) {
        face = 4;
    } else if (world.opaqueAt(x, y - 1, z)) {
        face = 5 + world.random().nextInt(2);
    }
    if (face == 0) return;

    world.setDataRaw(x, y, z, u8((md & 8) + face));
    (void) self;
}

bool leverActivated(TickWorld& world, i32 x, int y, i32 z, BlockId self)
{
    // `no.a(Lcn;IIILdm;)Z`. `8 - (meta & 8)` is the original's way of writing
    // "toggle bit 3", and the metadata is written **without** notifying, then
    // the notifications are sent by hand -- so a lever wakes its own cell and
    // the block it hangs on, and nothing else.
    const int md = int(world.dataAt(x, y, z));
    const int on = 8 - (md & 8);
    world.setDataRaw(x, y, z, u8((md & 7) + on));
    notifyAttached(world, x, y, z, self);
    // `random.click` at 0.3, pitched 0.6 when it has just gone on and 0.5 when
    // it has gone off -- so a lever says which way it went without being looked
    // at.
    world.playSoundAt("random.click", double(x) + 0.5, double(y) + 0.5, double(z) + 0.5,
                      0.3f, on > 0 ? 0.6f : 0.5f);
    return true;
}

bool buttonActivated(TickWorld& world, i32 x, int y, i32 z, BlockId self)
{
    // `hu.a(Lcn;IIILdm;)Z`, and the interesting line is the early return: a
    // button that is already in **consumes the click and does nothing**, so
    // pressing it again does not extend it. The twenty ticks it stays down are
    // scheduled here and spent in `buttonTick`.
    const int md = int(world.dataAt(x, y, z));
    const int on = 8 - (md & 8);
    if (on == 0) return true;

    world.setDataRaw(x, y, z, u8((md & 7) + on));
    notifyAttached(world, x, y, z, self);
    world.scheduleBlockUpdate(x, y, z, self);
    world.playSoundAt("random.click", double(x) + 0.5, double(y) + 0.5, double(z) + 0.5,
                      0.3f, 0.6f);
    return true;
}

void redstoneOreActivated(TickWorld& world, i32 x, int y, i32 z, BlockId self)
{
    // `ai.h(Lcn;III)V`, reached from a `blockActivated` that then returns
    // **false** -- so touching redstone ore lights it and the click carries on
    // to the item in the hand. It is the one activation that does not consume.
    if (self != id(mcver::Block::RedstoneOre)) return;
    world.setBlockWithNotify(x, y, z, id(mcver::Block::LitRedstoneOre));
}


void tntNeighbourChanged(TickWorld& world, i32 x, int y, i32 z, BlockId fromId)
{
    if (fromId == block::kAir || !canProvidePower(fromId)) return;
    if (!world.isIndirectlyPowered(x, y, z)) return;

    // `onBlockDestroyedByPlayer` first, then the air -- see redstone.hpp.
    tntDestroyedByPlayer(world, x, y, z);
    world.setBlockWithNotify(x, y, z, block::kAir);
}

}  // namespace mc::tick
