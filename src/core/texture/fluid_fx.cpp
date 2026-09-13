#include "core/texture/fluid_fx.hpp"

#include "core/block/block_def.hpp"
#include "core/block/registry.hpp"
#include "core/mesh/vertex.hpp"
#include "core/texture/atlas_image.hpp"
#include "core/util/java_cast.hpp"
#include "core/util/math_helper.hpp"

#include <cstring>

namespace mc::texture {

namespace {

// The constants, one line per class file, written as the literals the bytecode
// holds rather than folded together: these are float multiplies and doing the
// arithmetic here would be a different rounding.
//
// Water and flowing water differ in every one of them, which is why the two are
// not the same simulation with a flag on the scroll.
constexpr float kWaterBlurDivisor = 3.3f;
constexpr float kWaterFlowBlurDivisor = 3.2f;
constexpr float kWaterBubbleGain = 0.8f;
constexpr float kWaterRiseGain = 0.05f;
constexpr float kWaterRiseDecay = 0.1f;
constexpr float kWaterFlowRiseDecay = 0.3f;
constexpr double kWaterSpawnChance = 0.05;
constexpr double kWaterFlowSpawnChance = 0.2;
constexpr float kWaterSpawnHeight = 0.5f;

constexpr float kLavaBlurDivisor = 10.0f;
constexpr float kLavaBubbleGain = 0.8f;
constexpr float kLavaRiseGain = 0.01f;
constexpr float kLavaRiseDecay = 0.06f;
constexpr double kLavaSpawnChance = 0.005;
constexpr float kLavaSpawnHeight = 1.5f;

// `(float)(Math.sin(...) * 1.2)` in the two lava class files -- how far the
// sample square is displaced, in whole cells, before the nine-cell blur reads
// it. It is a sine of the *other* axis, so the displacement of a row and the
// displacement of a column are out of phase and the field turns.
constexpr float kLavaPi = 3.1415927f;
constexpr float kLavaSwirl = 1.2f;

// Every index in these four classes wraps rather than clamping: the field is a
// torus and an off-edge read comes back from the far side. `-1 & 15` is 15.
constexpr int wrap(int v) { return v & (kFluidEdge - 1); }

constexpr int at(int x, int y) { return wrap(x) + wrap(y) * kFluidEdge; }

// One 16 x 16 tile of RGBA into a decoded atlas, at `tile`.
void blitTile(u8* atlasRgba, int tile, const u8* texels)
{
    const int tileX = (tile % kAtlasTilesPerEdge) * kAtlasTilePixels;
    const int tileY = (tile / kAtlasTilesPerEdge) * kAtlasTilePixels;
    for (int row = 0; row < kAtlasTilePixels; ++row) {
        u8* out = atlasRgba + (usize(tileY + row) * kAtlasEdge + usize(tileX)) * 4;
        std::memcpy(out, texels + usize(row) * kFluidEdge * 4, kFluidEdge * 4);
    }
}

static_assert(kFluidEdge == kAtlasTilePixels,
              "a generated fluid is written straight into an atlas tile");

// Stated seeds rather than a clock, for the reason core/texture/texture_fx.cpp
// states one: there is no sequence to match, so the useful property is that
// this build draws the same water every time and a test can say so.
i64 fluidSeed(int which) { return 0x3DA1F1D0 + which; }

}  // namespace

FluidTexture::FluidTexture(FluidFx which, i64 seed) : which_(which), rng_(seed) {}

void FluidTexture::tick()
{
    // `k++` is the first statement of both flowing class files and of
    // `TextureWaterFX`; only the flowing two ever read it. `TextureLavaFX` does
    // not have the field at all, and incrementing it there would be harmless
    // and would still be a difference, so it does not.
    if (which_ != FluidFx::Lava) {
        ++step_;
    }

    if (which_ == FluidFx::Water || which_ == FluidFx::WaterFlow) {
        tickWater();
    } else {
        tickLava();
    }

    // The two fields swap rather than copy, which is the original's own move
    // and is why `heat_` is what the colour pass reads.
    for (int i = 0; i < kFluidTexels; ++i) {
        const float held = heat_[i];
        heat_[i] = next_[i];
        next_[i] = held;
    }
}

void FluidTexture::tickWater()
{
    const bool flow = which_ == FluidFx::WaterFlow;

    // **Two passes, not one.** The blur reads the bubble field as it stood at
    // the end of the last tick; lava's fuses the two loops and does not. That
    // is a real difference between the class files and not a tidying.
    for (int x = 0; x < kFluidEdge; ++x) {
        for (int y = 0; y < kFluidEdge; ++y) {
            float sum = 0.0f;
            if (flow) {
                // Three cells in y **ending at the cell itself** rather than
                // centred on it, which is the asymmetry that gives a flowing
                // face a direction to run in.
                for (int ny = y - 2; ny <= y; ++ny) {
                    sum += heat_[at(x, ny)];
                }
            } else {
                for (int nx = x - 1; nx <= x + 1; ++nx) {
                    sum += heat_[at(nx, y)];
                }
            }
            next_[at(x, y)] = sum / (flow ? kWaterFlowBlurDivisor : kWaterBlurDivisor)
                              + bubble_[at(x, y)] * kWaterBubbleGain;
        }
    }

    for (int x = 0; x < kFluidEdge; ++x) {
        for (int y = 0; y < kFluidEdge; ++y) {
            const int i = at(x, y);
            bubble_[i] += rise_[i] * kWaterRiseGain;
            if (bubble_[i] < 0.0f) {
                bubble_[i] = 0.0f;
            }
            rise_[i] -= flow ? kWaterFlowRiseDecay : kWaterRiseDecay;
            if (rng_.nextDouble() < (flow ? kWaterFlowSpawnChance : kWaterSpawnChance)) {
                rise_[i] = kWaterSpawnHeight;
            }
        }
    }
}

void FluidTexture::tickLava()
{
    // **One pass.** The bubble field is advanced inside the same loop that
    // reads it, so a cell's blur sees this tick's value wherever the wrap
    // brings it back round to a column already visited -- which is a quarter of
    // the reads and is the original's behaviour, not a bug being copied.
    for (int x = 0; x < kFluidEdge; ++x) {
        for (int y = 0; y < kFluidEdge; ++y) {
            float sum = 0.0f;
            const int offsetX = javaToInt(MathHelper::sin(float(y) * kLavaPi * 2.0f
                                                          / float(kFluidEdge))
                                          * kLavaSwirl);
            const int offsetY = javaToInt(MathHelper::sin(float(x) * kLavaPi * 2.0f
                                                          / float(kFluidEdge))
                                          * kLavaSwirl);
            for (int nx = x - 1; nx <= x + 1; ++nx) {
                for (int ny = y - 1; ny <= y + 1; ++ny) {
                    sum += heat_[at(nx + offsetX, ny + offsetY)];
                }
            }

            // The four bubble cells are the square **below and to the right**
            // of this one, not a neighbourhood centred on it.
            next_[at(x, y)] =
                sum / kLavaBlurDivisor
                + (bubble_[at(x, y)] + bubble_[at(x + 1, y)] + bubble_[at(x + 1, y + 1)]
                   + bubble_[at(x, y + 1)])
                      / 4.0f * kLavaBubbleGain;

            const int i = at(x, y);
            bubble_[i] += rise_[i] * kLavaRiseGain;
            if (bubble_[i] < 0.0f) {
                bubble_[i] = 0.0f;
            }
            rise_[i] -= kLavaRiseDecay;
            if (rng_.nextDouble() < kLavaSpawnChance) {
                rise_[i] = kLavaSpawnHeight;
            }
        }
    }
}

void FluidTexture::writeTo(u8* dst) const
{
    // **The scroll, and it is the whole of why a waterfall falls.** The flowing
    // pair read their field at an offset of whole rows -- one a tick for water
    // and one every three ticks for lava -- and wrap it, so the picture moves
    // while the simulation stays where it is. The still pair read it straight.
    u32 scroll = 0;
    if (which_ == FluidFx::WaterFlow) {
        scroll = step_ * u32(kFluidEdge);
    } else if (which_ == FluidFx::LavaFlow) {
        scroll = (step_ / 3u) * u32(kFluidEdge);
    }
    const bool hot = which_ == FluidFx::Lava || which_ == FluidFx::LavaFlow;

    for (int i = 0; i < kFluidTexels; ++i) {
        // Unsigned, so the subtraction wraps rather than going negative. The
        // original's `(i - k * 16) & 255` is a signed int doing the same thing,
        // and the mask is what makes the two agree.
        const int source = int((u32(i) - scroll) & u32(kFluidTexels - 1));

        // Lava doubles the heat before clamping and water does not.
        float heat = hot ? heat_[source] * 2.0f : heat_[source];
        if (heat > 1.0f) {
            heat = 1.0f;
        }
        if (heat < 0.0f) {
            heat = 0.0f;
        }

        int red = 0;
        int green = 0;
        int blue = 0;
        int alpha = 0;
        if (hot) {
            // Red is linear and starts high, green is the square of the heat,
            // blue is its fourth power over 128 -- so lava runs red, then
            // orange, then yellow, and the hottest cells alone go pale. Opaque.
            red = javaToInt(heat * 100.0f + 155.0f);
            green = javaToInt(heat * heat * 255.0f);
            blue = javaToInt(heat * heat * heat * heat * 128.0f);
            alpha = 255;
        } else {
            // Water keeps blue pinned at 255 and moves the other two a little,
            // so heat reads as foam rather than as a different colour, and it
            // is **translucent**: 146 at rest and 196 at the crest.
            const float f2 = heat * heat;
            red = javaToInt(32.0f + f2 * 32.0f);
            green = javaToInt(50.0f + f2 * 64.0f);
            blue = 255;
            alpha = javaToInt(146.0f + f2 * 50.0f);
        }

        dst[i * 4 + 0] = u8(red);
        dst[i * 4 + 1] = u8(green);
        dst[i * 4 + 2] = u8(blue);
        dst[i * 4 + 3] = u8(alpha);
    }
}

FluidTiles fluidTiles(bool hot)
{
    constexpr int kTileCount = kAtlasTilesPerEdge * kAtlasTilesPerEdge;

    for (int id = 0; id < mcver::kBlockTableSize; ++id) {
        const block::BlockDef& def = mcver::kBlocks[id];
        if (!def.known || def.render != block::RenderType::Fluid) {
            continue;
        }
        // Lava emits light and water does not, which is the only column in the
        // block table that tells the two apart without naming a block. Both of
        // a fluid's two ids -- the moving one and the still one -- carry the
        // same tiles, so the first match answers for the pair.
        if ((def.light > 0) != hot) {
            continue;
        }

        FluidTiles tiles;
        const int still = int(def.faces[mesh::kFacePosY]);
        const int flowing = int(def.faces[mesh::kFaceNegZ]);
        if (still < kTileCount) {
            tiles.still = still;
        }
        // `tileSize = 2` claims `flowing`, `flowing + 1` and the two under
        // them. The original writes all four without checking; this refuses a
        // flowing tile in the last column, where `flowing + 1` would be the
        // first tile of the next row and the block would be torn in half.
        if ((flowing % kAtlasTilesPerEdge) <= kAtlasTilesPerEdge - 2
            && flowing + kAtlasTilesPerEdge + 1 < kTileCount) {
            tiles.flowing = flowing;
        }
        return tiles;
    }
    return FluidTiles{};
}

FluidAnimation::FluidAnimation()
    : sims_{FluidTexture(FluidFx::Water, fluidSeed(0)),
            FluidTexture(FluidFx::WaterFlow, fluidSeed(1)),
            FluidTexture(FluidFx::Lava, fluidSeed(2)),
            FluidTexture(FluidFx::LavaFlow, fluidSeed(3))}
{
    addFluid(false, 0, 1);
    addFluid(true, 2, 3);

    for (int sim = 0; sim < kSimCount; ++sim) {
        bool used = false;
        for (int i = 0; i < writeCount_; ++i) {
            used = used || writes_[i].source == sim;
        }
        if (!used) {
            continue;
        }
        for (int i = 0; i < kFluidSettleTicks; ++i) {
            sims_[sim].tick();
        }
        sims_[sim].writeTo(texels_[sim]);
    }
}

void FluidAnimation::addFluid(bool hot, int stillSim, int flowSim)
{
    const FluidTiles tiles = fluidTiles(hot);
    if (tiles.still >= 0 && writeCount_ < kMaxWrites) {
        writes_[writeCount_++] = Write{tiles.still, 1, stillSim};
    }
    if (tiles.flowing < 0) {
        return;
    }
    // The 2 x 2 block, as its two rows: four tiles holding the same texels, and
    // a row of two is one copy on the console rather than two.
    for (int row = 0; row < 2 && writeCount_ < kMaxWrites; ++row) {
        writes_[writeCount_++] =
            Write{tiles.flowing + row * kAtlasTilesPerEdge, 2, flowSim};
    }
}

void FluidAnimation::tick(int ticks)
{
    if (ticks <= 0 || !present()) {
        return;
    }
    for (int sim = 0; sim < kSimCount; ++sim) {
        bool used = false;
        for (int i = 0; i < writeCount_; ++i) {
            used = used || writes_[i].source == sim;
        }
        if (!used) {
            continue;
        }
        for (int i = 0; i < ticks; ++i) {
            sims_[sim].tick();
        }
        sims_[sim].writeTo(texels_[sim]);
    }
}

void applyFluidTiles(u8* atlasRgba)
{
    struct Group {
        bool hot;
        FluidFx still;
        FluidFx flow;
        int stillSeed;
        int flowSeed;
    };
    // The seeds are the ones `FluidAnimation` uses, so the tile a pack load
    // bakes and the first tile the animation pushes are the same picture.
    constexpr Group kGroups[] = {
        {false, FluidFx::Water, FluidFx::WaterFlow, 0, 1},
        {true, FluidFx::Lava, FluidFx::LavaFlow, 2, 3},
    };

    for (const Group& group : kGroups) {
        const FluidTiles tiles = fluidTiles(group.hot);

        if (tiles.still >= 0) {
            FluidTexture sim(group.still, fluidSeed(group.stillSeed));
            for (int i = 0; i < kFluidSettleTicks; ++i) {
                sim.tick();
            }
            u8 texels[kFluidTexels * 4];
            sim.writeTo(texels);
            blitTile(atlasRgba, tiles.still, texels);
        }

        if (tiles.flowing >= 0) {
            FluidTexture sim(group.flow, fluidSeed(group.flowSeed));
            for (int i = 0; i < kFluidSettleTicks; ++i) {
                sim.tick();
            }
            u8 texels[kFluidTexels * 4];
            sim.writeTo(texels);
            const int block[4] = {tiles.flowing, tiles.flowing + 1,
                                  tiles.flowing + kAtlasTilesPerEdge,
                                  tiles.flowing + kAtlasTilesPerEdge + 1};
            for (int tile : block) {
                blitTile(atlasRgba, tile, texels);
            }
        }
    }
}

}  // namespace mc::texture
