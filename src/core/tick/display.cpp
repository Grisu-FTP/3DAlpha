// See display.hpp. Seven `randomDisplayTick` overrides and the loop that calls
// them, transcribed from the classes `ly`'s static initialiser names.

#include "core/tick/display.hpp"

#include "core/block/registry.hpp"
#include "core/entity/particle.hpp"
#include "core/tick/tick_world.hpp"

namespace mc::tick {
namespace {

using entity::ParticleKind;

int kindOf(ParticleKind kind) { return int(kind); }

// `cn.g(III)` -- `Block.isOpaqueCube()` at runtime, which is `BlockDef::opaque`
// and deliberately not `opaqueCube`; see the note on that column.
bool opaqueAt(const TickWorld& world, i32 x, int y, i32 z)
{
    return block::def(world.blockAt(x, y, z)).opaque;
}

// `mj.b` -- BlockTorch, and `bg` is the same method with one particle swapped.
// The five branches are the torch's five orientations; the fifth is the one
// standing on the floor.
//
// The two classes share this because they share the *geometry*: a redstone
// torch is a torch that glitters instead of burning, and it would be a second
// copy of the same five offsets otherwise.
void torchDisplay(const TickWorld& world, i32 x, int y, i32 z, ParticleKind kind,
                  bool alsoFlame, JavaRandom& rand)
{
    const int metadata = world.dataAt(x, y, z);
    const double baseX = double(x) + 0.5f;
    const double baseY = double(y) + 0.7f;
    const double baseZ = double(z) + 0.5f;
    // `0.27` out along the wall the torch is on, `0.22` up from the middle of
    // the block -- the tip of the stick, where a flame belongs.
    constexpr double kOut = 0.27000001072883606;
    constexpr double kUp = 0.2199999988079071;

    // **A redstone torch jitters and a burning one does not.** `bg` offsets all
    // three axes by a fifth of a block before it branches; `mj` uses the block
    // centre exactly.
    double px = baseX;
    double py = baseY;
    double pz = baseZ;
    if (kind == ParticleKind::Reddust) {
        px += double(rand.nextFloat() - 0.5f) * 0.2;
        py += double(rand.nextFloat() - 0.5f) * 0.2;
        pz += double(rand.nextFloat() - 0.5f) * 0.2;
    }

    switch (metadata) {
    case 1: px -= kOut; py += kUp; break;
    case 2: px += kOut; py += kUp; break;
    case 3: pz -= kOut; py += kUp; break;
    case 4: pz += kOut; py += kUp; break;
    default: break;  // 0 and 5: standing on the floor, dead centre
    }

    world.spawnParticle(kindOf(kind), px, py, pz);
    if (alsoFlame) {
        world.spawnParticle(kindOf(ParticleKind::Flame), px, py, pz);
    }
}

// `ku.b` -- BlockFurnace, lit only. **Four branches and no default**: a lit
// furnace whose metadata is not one of the four facings throws nothing at all,
// which is the jar's and not an omission here.
void furnaceDisplay(const TickWorld& world, i32 x, int y, i32 z, JavaRandom& rand)
{
    const int metadata = world.dataAt(x, y, z);
    const float centreX = float(x) + 0.5f;
    // Somewhere in the bottom three eighths of the block: the fire is in the
    // grate and not in the middle of the door.
    const float mouthY = float(y) + 0.0f + (rand.nextFloat() * 6.0f) / 16.0f;
    const float centreZ = float(z) + 0.5f;
    // Just proud of the face (0.52 of a block from the middle), and spread
    // across it by up to three tenths either way.
    constexpr float kProud = 0.52f;
    const float across = rand.nextFloat() * 0.6f - 0.3f;

    double px = 0.0;
    double pz = 0.0;
    switch (metadata) {
    case 4: px = double(centreX - kProud); pz = double(centreZ + across); break;
    case 5: px = double(centreX + kProud); pz = double(centreZ + across); break;
    case 2: px = double(centreX + across); pz = double(centreZ - kProud); break;
    case 3: px = double(centreX + across); pz = double(centreZ + kProud); break;
    default: return;
    }
    world.spawnParticle(kindOf(ParticleKind::Smoke), px, double(mouthY), pz);
    world.spawnParticle(kindOf(ParticleKind::Flame), px, double(mouthY), pz);
}

// `kf.b` -- BlockRedstoneWire. One mote, and only on a wire that is carrying
// something: the guard is the wire's own *power level* in its metadata, so an
// unpowered run is dark.
void wireDisplay(const TickWorld& world, i32 x, int y, i32 z, JavaRandom& rand)
{
    if (world.dataAt(x, y, z) <= 0) {
        return;
    }
    world.spawnParticle(kindOf(ParticleKind::Reddust),
                        double(x) + 0.5 + (double(rand.nextFloat()) - 0.5) * 0.2,
                        double(y) + 0.0625f,
                        double(z) + 0.5 + (double(rand.nextFloat()) - 0.5) * 0.2);
}

}  // namespace

// `ai.i` -- BlockOre's glitter. Six motes, one per face, each pulled just
// outside the block if that face is open. **Not guarded on the lit ore**: the
// guard is in the two callers, and one of them -- the glow -- sparkles a dull
// ore on its way to becoming a lit one.
void redstoneOreSparkle(const TickWorld& world, i32 x, int y, i32 z, JavaRandom& rand)
{
    // A sixteenth of a block proud of the face.
    constexpr double kProud = 0.0625;
    for (int face = 0; face < 6; ++face) {
        double px = double(x) + double(rand.nextFloat());
        double py = double(y) + double(rand.nextFloat());
        double pz = double(z) + double(rand.nextFloat());

        if (face == 0 && !opaqueAt(world, x, y + 1, z)) py = double(y + 1) + kProud;
        if (face == 1 && !opaqueAt(world, x, y - 1, z)) py = double(y + 0) - kProud;
        if (face == 2 && !opaqueAt(world, x, y, z + 1)) pz = double(z + 1) + kProud;
        if (face == 3 && !opaqueAt(world, x, y, z - 1)) pz = double(z + 0) - kProud;
        if (face == 4 && !opaqueAt(world, x + 1, y, z)) px = double(x + 1) + kProud;
        if (face == 5 && !opaqueAt(world, x - 1, y, z)) px = double(x + 0) - kProud;

        // **Only a mote that ended up outside the block is drawn**, which is
        // what makes the glitter sit on the open faces of an exposed ore and
        // not inside a buried one.
        //
        // Note the y test: it is `py < 0.0`, not `py < y`, which cannot be
        // right and is what the class file holds -- so a mote below a buried
        // ore's floor is refused only when the ore is at the bottom of the
        // world. Copied as written; see docs/status.md on being faithful to the
        // game rather than to its bugs.
        if (px < double(x) || px > double(x + 1) || py < 0.0 || py > double(y + 1)
            || pz < double(z) || pz > double(z + 1)) {
            world.spawnParticle(kindOf(ParticleKind::Reddust), px, py, pz);
        }
    }
}

namespace {

// `og.b` -- BlockFire. The crackle, then smoke off whichever neighbouring face
// has something burnable behind it.
void fireDisplay(const TickWorld& world, i32 x, int y, i32 z, JavaRandom& rand)
{
    if (rand.nextInt(24) == 0) {
        // **The volume is drawn before the pitch**, and the two are pulled out
        // into their own statements to say so: C++ leaves the order of two
        // arguments unspecified, and these are two draws off one generator.
        const float volume = 1.0f + rand.nextFloat();
        const float pitch = rand.nextFloat() * 0.7f + 0.3f;
        world.playSoundAt("fire.fire", double(x) + 0.5f, double(y) + 0.5f, double(z) + 0.5f,
                          volume, pitch);
    }

    // `Block.fire.canBlockCatchFire` -- the fire's own encourage table, not the
    // material's `canBurn`; see core/tick/fire.hpp on the three different
    // questions a1.1.2 asks about burning.
    const auto burnable = [&](i32 bx, int by, i32 bz) {
        return block::def(world.blockAt(bx, by, bz)).burnEncourage > 0;
    };

    // **Standing on something solid is the whole first branch, and it
    // returns**: a fire on a floor smokes upward only, however burnable its
    // walls are.
    if (opaqueAt(world, x, y - 1, z) || burnable(x, y - 1, z)) {
        for (int i = 0; i < 3; ++i) {
            world.spawnParticle(kindOf(ParticleKind::LargeSmoke),
                                double(x) + double(rand.nextFloat()),
                                double(y) + double(rand.nextFloat() * 0.5f + 0.5f),
                                double(z) + double(rand.nextFloat()));
        }
        return;
    }

    // Otherwise: two puffs off each burnable side, hugging that face at a
    // tenth of a block.
    constexpr float kHug = 0.1f;
    if (burnable(x - 1, y, z)) {
        for (int i = 0; i < 2; ++i) {
            world.spawnParticle(kindOf(ParticleKind::LargeSmoke),
                                double(x) + double(rand.nextFloat() * kHug),
                                double(y) + double(rand.nextFloat()),
                                double(z) + double(rand.nextFloat()));
        }
    }
    if (burnable(x + 1, y, z)) {
        for (int i = 0; i < 2; ++i) {
            world.spawnParticle(kindOf(ParticleKind::LargeSmoke),
                                double(x + 1) - double(rand.nextFloat() * kHug),
                                double(y) + double(rand.nextFloat()),
                                double(z) + double(rand.nextFloat()));
        }
    }
    if (burnable(x, y, z - 1)) {
        for (int i = 0; i < 2; ++i) {
            world.spawnParticle(kindOf(ParticleKind::LargeSmoke),
                                double(x) + double(rand.nextFloat()),
                                double(y) + double(rand.nextFloat()),
                                double(z) + double(rand.nextFloat() * kHug));
        }
    }
    if (burnable(x, y, z + 1)) {
        for (int i = 0; i < 2; ++i) {
            world.spawnParticle(kindOf(ParticleKind::LargeSmoke),
                                double(x) + double(rand.nextFloat()),
                                double(y) + double(rand.nextFloat()),
                                double(z + 1) - double(rand.nextFloat() * kHug));
        }
    }
    if (burnable(x, y + 1, z)) {
        for (int i = 0; i < 2; ++i) {
            world.spawnParticle(kindOf(ParticleKind::LargeSmoke),
                                double(x) + double(rand.nextFloat()),
                                double(y + 1) - double(rand.nextFloat() * kHug),
                                double(z) + double(rand.nextFloat()));
        }
    }
}

// `jp.b` -- BlockFluid, and the two halves have nothing to do with each other.
// Water trickles and makes no particle; lava pops and makes no sound.
void fluidDisplay(const TickWorld& world, i32 x, int y, i32 z, block::BlockId self,
                  JavaRandom& rand)
{
    const int metadata = world.dataAt(x, y, z);
    const bool lava = self == block::BlockId(mcver::Block::Lava)
                      || self == block::BlockId(mcver::Block::FlowingLava);

    if (!lava) {
        // **Flowing water only** -- `level > 0 && level < 8`, so a still source
        // is silent and only the stream off it trickles.
        if (rand.nextInt(64) == 0 && metadata > 0 && metadata < 8) {
            const float volume = rand.nextFloat() * 0.25f + 0.75f;
            const float pitch = rand.nextFloat() * 1.0f + 0.5f;
            world.playSoundAt("liquid.water", double(x) + 0.5f, double(y) + 0.5f,
                              double(z) + 0.5f, volume, pitch);
        }
        return;
    }

    // One pop in a hundred darts, and only from a surface with open air over
    // it: `getMaterial(above) == air && !isOpaqueCube(above)`.
    if (block::def(world.blockAt(x, y + 1, z)).material != 0) {
        return;
    }
    if (opaqueAt(world, x, y + 1, z) || rand.nextInt(100) != 0) {
        return;
    }
    // `this.maxY`, which is 1.0 for a fluid: the pop leaves the top of the
    // cell, not its middle.
    world.spawnParticle(kindOf(ParticleKind::Lava),
                        double(x) + double(rand.nextFloat()), double(y) + 1.0,
                        double(z) + double(rand.nextFloat()));
}

}  // namespace

void blockDisplayTick(const TickWorld& world, i32 x, int y, i32 z, block::BlockId self,
                      JavaRandom& rand)
{
    switch (self) {
    case block::BlockId(mcver::Block::Torch):
        torchDisplay(world, x, y, z, ParticleKind::Smoke, /*alsoFlame=*/true, rand);
        break;

    // **The lit one only.** `bg`'s `this.a` is a constructor argument and it is
    // true for 76 and false for 75.
    case block::BlockId(mcver::Block::RedstoneTorch):
        torchDisplay(world, x, y, z, ParticleKind::Reddust, /*alsoFlame=*/false, rand);
        break;

    case block::BlockId(mcver::Block::LitFurnace):
        furnaceDisplay(world, x, y, z, rand);
        break;

    case block::BlockId(mcver::Block::RedstoneWire):
        wireDisplay(world, x, y, z, rand);
        break;

    // **The lit one only**, and the guard lives here rather than inside the
    // sparkle because `ai.h` runs the same six motes off an ore that is still
    // dull. `ai`'s `this.a` is the constructor's flag: true for 74, false for
    // 73.
    case block::BlockId(mcver::Block::LitRedstoneOre):
        redstoneOreSparkle(world, x, y, z, rand);
        break;

    case block::BlockId(mcver::Block::Fire):
        fireDisplay(world, x, y, z, rand);
        break;

    case block::BlockId(mcver::Block::Water):
    case block::BlockId(mcver::Block::FlowingWater):
    case block::BlockId(mcver::Block::Lava):
    case block::BlockId(mcver::Block::FlowingLava):
        fluidDisplay(world, x, y, z, self, rand);
        break;

    default:
        // `ly.b` is empty, which is sixty-odd of the seventy blocks.
        break;
    }
}

void displayTick(TickWorld& world, i32 centreX, int centreY, i32 centreZ,
                 JavaRandom& rand)
{
    // Nothing to throw the particles into, so nothing to draw the darts for.
    // The world harness and the spawn measurements take this branch.
    if (!world.hasParticleSink()) {
        return;
    }

    JavaRandom& offsets = world.random();
    for (int i = 0; i < kDisplayTickDarts; ++i) {
        const i32 x = centreX + offsets.nextInt(kDisplayTickRadius)
                      - offsets.nextInt(kDisplayTickRadius);
        const int y = centreY + offsets.nextInt(kDisplayTickRadius)
                      - offsets.nextInt(kDisplayTickRadius);
        const i32 z = centreZ + offsets.nextInt(kDisplayTickRadius)
                      - offsets.nextInt(kDisplayTickRadius);
        const block::BlockId self = world.blockAt(x, y, z);
        // `if (id > 0)`, so air is refused before the switch and not inside it.
        if (self != block::kAir) {
            blockDisplayTick(world, x, y, z, self, rand);
        }
    }
}

}  // namespace mc::tick
