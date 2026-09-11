// `mk` -- RailLogic -- transcribed. See rail.hpp for what it is for.

#include "core/tick/rail.hpp"

#include "core/block/registry.hpp"
#include "core/tick/behaviour.hpp"
#include "core/tick/drop.hpp"
#include "core/tick/redstone.hpp"
#include "core/tick/tick_world.hpp"

namespace mc::tick {
namespace {

using block::BlockId;

BlockId railId() { return BlockId(mcver::Block::Rail); }

// `mt` -- ChunkPosition, which is three ints and an equals this never uses.
struct RailPos {
    i32 x = 0;
    int y = 0;
    i32 z = 0;
};

// `mk`. **The list holds at most two**, and that is a property rather than a
// guess: `setBasicRail` writes exactly two or none, `refreshConnectedTracks`
// only ever removes, and `connectTo` is reached only through `canConnectFrom`,
// which refuses at two. Three slots so that a transcription slip overruns a
// bound rather than the stack.
class RailLogic {
public:
    // Default-constructed is a scratch frame `logicAt` fills in, so that
    // looking for a neighbour costs no world reads when there is none there.
    RailLogic() = default;

    RailLogic(TickWorld& world, i32 x, int y, i32 z) { reset(world, x, y, z); }

    void reset(TickWorld& world, i32 x, int y, i32 z)
    {
        world_ = &world;
        x_ = x;
        y_ = y;
        z_ = z;
        shape_ = int(world.dataAt(x, y, z));
        setBasicRail();
    }

    int shape() const { return shape_; }

    // `mk.c()I` -- getNumberOfAdjacentTracks, and it is the only method here
    // that looks at the world without building anything.
    int adjacentTrackCount() const
    {
        int n = 0;
        if (railAt(x_, y_, z_ - 1)) ++n;
        if (railAt(x_, y_, z_ + 1)) ++n;
        if (railAt(x_ - 1, y_, z_)) ++n;
        if (railAt(x_ + 1, y_, z_)) ++n;
        return n;
    }

    // `mk.a(Z)V` -- refreshTrackShape, the whole of the block's behaviour.
    void refreshTrackShape(bool powered);

private:
    // `mk.a(III)Z` -- isRailAt: this cell, one above, or one below.
    bool railAt(i32 x, int y, i32 z) const
    {
        const BlockId rail = railId();
        return world_->blockAt(x, y, z) == rail || world_->blockAt(x, y + 1, z) == rail
               || world_->blockAt(x, y - 1, z) == rail;
    }

    // `mk.a()V` -- setBasicRail: the ten shapes, as the two cells each one
    // joins. **Metadata outside 0..9 leaves the list empty**, which is what
    // makes the 15 a fresh rail is written with mean "no prior shape".
    void setBasicRail();

    // `mk.a(Lmt;)Lmk;` -- getRailLogic: a RailLogic for the rail at that
    // position, at the position itself, one above, or one below, in that
    // order. `false` when there is no rail at any of the three.
    bool logicAt(const RailPos& at, RailLogic* out) const
    {
        const BlockId rail = railId();
        if (world_->blockAt(at.x, at.y, at.z) == rail) {
            out->reset(*world_, at.x, at.y, at.z);
            return true;
        }
        if (world_->blockAt(at.x, at.y + 1, at.z) == rail) {
            out->reset(*world_, at.x, at.y + 1, at.z);
            return true;
        }
        if (world_->blockAt(at.x, at.y - 1, at.z) == rail) {
            out->reset(*world_, at.x, at.y - 1, at.z);
            return true;
        }
        return false;
    }

    // `mk.b(Lmk;)Z` -- isConnectedToRail, and `mk.b(III)Z`, which is the same
    // test written against loose coordinates. **Both compare x and z only**;
    // the height is not part of it, which is how a track joins the ramp above
    // or below it.
    bool listHolds(i32 x, i32 z) const
    {
        for (int i = 0; i < count_; ++i) {
            if (tracks_[i].x == x && tracks_[i].z == z) {
                return true;
            }
        }
        return false;
    }

    // `mk.b()V` -- refreshConnectedTracks. Every remembered position that no
    // longer holds a rail pointing back here is dropped; every one that does is
    // rewritten to the height the rail was actually found at.
    void refreshConnectedTracks()
    {
        for (int i = 0; i < count_;) {
            RailLogic other;
            if (!logicAt(tracks_[i], &other) || !other.listHolds(x_, z_)) {
                removeAt(i);
                continue;
            }
            tracks_[i] = RailPos{other.x_, other.y_, other.z_};
            ++i;
        }
    }

    // `mk.c(Lmk;)Z` -- canConnectFrom. **The tail of this method is dead**: the
    // class file compares `other.y` and the first remembered height against
    // this rail's own and returns true either way, so the whole of it is "yes
    // unless I am already full". Transcribed as what it computes rather than
    // as what it looks like it meant to.
    bool canConnectFrom(const RailLogic& other) const
    {
        if (listHolds(other.x_, other.z_)) return true;
        return count_ != 2;
    }

    // `mk.c(III)Z` -- the four-way question `refreshTrackShape` asks.
    bool canConnectTo(i32 x, int y, i32 z) const
    {
        RailLogic other;
        if (!logicAt(RailPos{x, y, z}, &other)) {
            return false;
        }
        other.refreshConnectedTracks();
        return other.canConnectFrom(*this);
    }

    // `mk.d(Lmk;)V` -- connectTo: remember the other rail, then re-derive this
    // rail's shape **from the remembered list rather than from the world**, and
    // write it. That last part is what joins the two ends of a new track: the
    // neighbour is re-pointed at the rail that just arrived.
    void connectTo(const RailLogic& other);

    // The shape table both `refreshTrackShape` and `connectTo` run, given the
    // four "is there something to join this way" answers. -1 means "nothing
    // decided", which the caller then resolves differently in each.
    static int shapeFromSides(bool negZ, bool posZ, bool negX, bool posX);

    // The slope pass, which both methods also share: a flat shape becomes an
    // ascending one when there is a rail diagonally above the end it points at.
    void applySlopes(int* shape) const;

    void add(i32 x, int y, i32 z)
    {
        if (count_ < int(sizeof(tracks_) / sizeof(tracks_[0]))) {
            tracks_[count_++] = RailPos{x, y, z};
        }
    }

    void removeAt(int index)
    {
        for (int i = index; i + 1 < count_; ++i) {
            tracks_[i] = tracks_[i + 1];
        }
        --count_;
    }

    TickWorld* world_ = nullptr;
    i32 x_ = 0;
    int y_ = 0;
    i32 z_ = 0;
    int shape_ = 0;
    RailPos tracks_[3];
    int count_ = 0;
};

void RailLogic::setBasicRail()
{
    count_ = 0;
    switch (shape_) {
    case 0:
        add(x_, y_, z_ - 1);
        add(x_, y_, z_ + 1);
        break;
    case 1:
        add(x_ - 1, y_, z_);
        add(x_ + 1, y_, z_);
        break;
    case 2:
        add(x_ - 1, y_, z_);
        add(x_ + 1, y_ + 1, z_);
        break;
    case 3:
        add(x_ - 1, y_ + 1, z_);
        add(x_ + 1, y_, z_);
        break;
    case 4:
        add(x_, y_ + 1, z_ - 1);
        add(x_, y_, z_ + 1);
        break;
    case 5:
        add(x_, y_, z_ - 1);
        add(x_, y_ + 1, z_ + 1);
        break;
    case 6:
        add(x_ + 1, y_, z_);
        add(x_, y_, z_ + 1);
        break;
    case 7:
        add(x_ - 1, y_, z_);
        add(x_, y_, z_ + 1);
        break;
    case 8:
        add(x_ - 1, y_, z_);
        add(x_, y_, z_ - 1);
        break;
    case 9:
        add(x_ + 1, y_, z_);
        add(x_, y_, z_ - 1);
        break;
    default:
        break;
    }
}

int RailLogic::shapeFromSides(bool negZ, bool posZ, bool negX, bool posX)
{
    int shape = -1;
    if ((negZ || posZ) && !negX && !posX) shape = 0;
    if ((negX || posX) && !negZ && !posZ) shape = 1;
    if (posZ && posX && !negZ && !negX) shape = 6;
    if (posZ && negX && !negZ && !posX) shape = 7;
    if (negZ && negX && !posZ && !posX) shape = 8;
    if (negZ && posX && !posZ && !negX) shape = 9;
    return shape;
}

void RailLogic::applySlopes(int* shape) const
{
    const BlockId rail = railId();
    if (*shape == 0) {
        if (world_->blockAt(x_, y_ + 1, z_ - 1) == rail) *shape = 4;
        if (world_->blockAt(x_, y_ + 1, z_ + 1) == rail) *shape = 5;
    }
    if (*shape == 1) {
        if (world_->blockAt(x_ + 1, y_ + 1, z_) == rail) *shape = 2;
        if (world_->blockAt(x_ - 1, y_ + 1, z_) == rail) *shape = 3;
    }
    if (*shape < 0) *shape = 0;
}

void RailLogic::refreshTrackShape(bool powered)
{
    const bool negZ = canConnectTo(x_, y_, z_ - 1);
    const bool posZ = canConnectTo(x_, y_, z_ + 1);
    const bool negX = canConnectTo(x_ - 1, y_, z_);
    const bool posX = canConnectTo(x_ + 1, y_, z_);

    int shape = shapeFromSides(negZ, posZ, negX, posX);

    // **Nothing unambiguous, so take the best available**, and this is the one
    // place power changes anything: the four corner cases are tried in one
    // order when the rail is getting power and in the reverse order when it is
    // not, so the *last* one that matches wins and a junction points a
    // different way. That is a1.1.2's rail switch.
    if (shape == -1) {
        if (negZ || posZ) shape = 0;
        if (negX || posX) shape = 1;
        if (powered) {
            if (posZ && posX) shape = 6;
            if (negX && posZ) shape = 7;
            if (posX && negZ) shape = 9;
            if (negZ && negX) shape = 8;
        } else {
            if (negZ && negX) shape = 8;
            if (posX && negZ) shape = 9;
            if (negX && posZ) shape = 7;
            if (posZ && posX) shape = 6;
        }
    }

    applySlopes(&shape);

    shape_ = shape;
    setBasicRail();
    // `cn.b(IIII)V` -- setBlockMetadata, which notifies nothing. The rail's own
    // redraw comes from the write; the neighbours are re-pointed below by name
    // rather than by a notification round, which is why a track laid across a
    // junction settles in one pass instead of ringing.
    world_->setDataRaw(x_, y_, z_, u8(shape));

    for (int i = 0; i < count_; ++i) {
        RailLogic other;
        if (!logicAt(tracks_[i], &other)) {
            continue;
        }
        other.refreshConnectedTracks();
        if (other.canConnectFrom(*this)) {
            other.connectTo(*this);
        }
    }
}

void RailLogic::connectTo(const RailLogic& other)
{
    add(other.x_, other.y_, other.z_);

    // **These four read the remembered list, not the world.** That is the whole
    // difference between this method and `refreshTrackShape`: a rail being
    // connected *to* shapes itself around the track it already knows about,
    // which is what stops the two ends of a new join disagreeing.
    const bool negZ = listHolds(x_, z_ - 1);
    const bool posZ = listHolds(x_, z_ + 1);
    const bool negX = listHolds(x_ - 1, z_);
    const bool posX = listHolds(x_ + 1, z_);

    int shape = shapeFromSides(negZ, posZ, negX, posX);
    applySlopes(&shape);
    world_->setDataRaw(x_, y_, z_, u8(shape));
}

}  // namespace

void railPlaced(TickWorld& world, i32 x, int y, i32 z)
{
    // `world.setBlockMetadata(i, j, k, 15)` -- see rail.hpp. It is a value the
    // game never draws and never leaves behind: the refresh below overwrites it
    // on the same call, and until it does it means "I have no prior shape".
    world.setDataRaw(x, y, z, 15);

    RailLogic logic(world, x, y, z);
    logic.refreshTrackShape(world.isIndirectlyPowered(x, y, z));
}

void railNeighbourChanged(TickWorld& world, i32 x, int y, i32 z, BlockId self, BlockId fromId)
{
    const int metadata = int(world.dataAt(x, y, z));

    // A rail needs an opaque cube under it; an ascending one *also* needs one
    // under the neighbour it climbs towards, which is the block the slope
    // leans on. 2 climbs to +x, 3 to -x, 4 to -z, 5 to +z -- the same mapping
    // the mesher tilts by.
    bool drop = !world.opaqueAt(x, y - 1, z);
    if (metadata == 2 && !world.opaqueAt(x + 1, y, z)) drop = true;
    if (metadata == 3 && !world.opaqueAt(x - 1, y, z)) drop = true;
    if (metadata == 4 && !world.opaqueAt(x, y, z - 1)) drop = true;
    if (metadata == 5 && !world.opaqueAt(x, y, z + 1)) drop = true;

    if (drop) {
        dropBlockAsItem(world, x, y, z, self, u8(metadata));
        world.setBlockWithNotify(x, y, z, block::kAir);
        return;
    }

    // **Only a source of power re-points a junction, and only a three-way one.**
    // `l > 0 && Block.blocksList[l].canProvidePower()` and
    // `getNumberOfAdjacentTracks() == 3`, both of which are cheap enough that
    // the ordinary case -- a dirt block changing next to a straight track --
    // costs one table lookup and stops.
    if (fromId == block::kAir || !canProvidePower(fromId)) {
        return;
    }
    RailLogic probe(world, x, y, z);
    if (probe.adjacentTrackCount() != 3) {
        return;
    }
    RailLogic logic(world, x, y, z);
    logic.refreshTrackShape(world.isIndirectlyPowered(x, y, z));
}

}  // namespace mc::tick
