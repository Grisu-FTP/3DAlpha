#include "core/tick/redstone.hpp"

#include "core/block/registry.hpp"
#include "core/tick/tick_world.hpp"

namespace mc::tick {

namespace {

using block::BlockId;
using block::TickBehaviour;

BlockId id(mcver::Block b) { return BlockId(b); }

bool isWire(BlockId b) { return block::def(b).tick == TickBehaviour::RedstoneWire; }

// `ly.d()Z` -- canProvidePower, false on the base class. Only a wire, a
// torch, a lever, a button and a pressure plate answer true, and it is a
// property of the class rather than a table, so it is derived from the
// behaviour column here.
bool canProvidePower(BlockId b)
{
    switch (block::def(b).tick) {
    case TickBehaviour::RedstoneWire:
    case TickBehaviour::RedstoneTorch:
    case TickBehaviour::Lever:
    case TickBehaviour::Button:
    case TickBehaviour::PressurePlate:
        return true;
    default:
        return false;
    }
}

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
bool faceSupported(const TickWorld& world, i32 x, int y, i32 z, int face)
{
    switch (face) {
    case 1: return world.opaqueAt(x - 1, y, z);
    case 2: return world.opaqueAt(x + 1, y, z);
    case 3: return world.opaqueAt(x, y, z - 1);
    case 4: return world.opaqueAt(x, y, z + 1);
    case 5: return world.opaqueAt(x, y - 1, z);
    default: return false;
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
    // The original also plays random.door_open or random.door_close here.
}

}  // namespace

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
    case TickBehaviour::PressurePlate:
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
    case TickBehaviour::PressurePlate:
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
    // `kf.a(Lcn;IIII)V`: a wire needs an opaque cube under it.
    if (!world.opaqueAt(x, y - 1, z)) {
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

    const int face = md & 7;
    world.setDataRaw(x, y, z, u8(face));
    world.notifyNeighbours(x, y, z, self);

    switch (face) {
    case 1: world.notifyNeighbours(x - 1, y, z, self); break;
    case 2: world.notifyNeighbours(x + 1, y, z, self); break;
    case 3: world.notifyNeighbours(x, y, z - 1, self); break;
    case 4: world.notifyNeighbours(x, y, z + 1, self); break;
    default: world.notifyNeighbours(x, y - 1, z, self); break;
    }
    // The original also plays random.click here.
}

void switchNeighbourChanged(TickWorld& world, i32 x, int y, i32 z, BlockId self)
{
    const TickBehaviour behaviour = block::def(self).tick;

    if (behaviour == TickBehaviour::PressurePlate) {
        // `al.a(Lcn;IIII)V`: a plate wants an opaque cube directly under it and
        // has no face to name.
        if (world.opaqueAt(x, y - 1, z)) return;
        world.setBlockWithNotify(x, y, z, block::kAir);
        return;
    }

    const bool allowFloor = behaviour == TickBehaviour::Lever;
    if (!switchHasSupport(world, x, y, z, allowFloor)) {
        world.setBlockWithNotify(x, y, z, block::kAir);
        return;
    }

    // Attached *somewhere* is not enough: the face the metadata names is the
    // one that has to still be there.
    const int face = int(world.dataAt(x, y, z)) & 7;
    if (faceSupported(world, x, y, z, face)) return;
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
    if (broken) return;

    // **Only a power source wakes a door.** A door does not re-read the world
    // every time any neighbour changes; it looks only when the block that
    // changed was something that can provide power, which is what keeps a
    // doorway in a wall from costing anything.
    if (fromId == block::kAir || !canProvidePower(fromId)) return;

    const bool powered = world.isIndirectlyPowered(x, y, z) ||
                         world.isIndirectlyPowered(x, y + 1, z);
    doorSetOpen(world, x, y, z, self, powered);
}

}  // namespace mc::tick
