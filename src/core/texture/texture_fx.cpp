#include "core/texture/texture_fx.hpp"

#include "core/block/block_def.hpp"
#include "core/block/registry.hpp"
#include "core/texture/atlas_image.hpp"
#include "core/texture/fluid_fx.hpp"

#include <cstring>

namespace mc::texture {

namespace {

// `int k = 18` before the neighbour loop, and six neighbours each bumping it,
// so the divisor is 24 -- and the seed cell's contribution is weighted by that
// initial 18 rather than by 1. Written as the two numbers the bytecode holds
// rather than as the 25.44f they multiply out to, because the multiplication
// is a float one and doing it here would be a different rounding.
constexpr int kSeedWeight = 18;
constexpr int kNeighbourCount = 6;
constexpr float kDecay = 1.06f;

// The colour ramp, in the order the class file applies it. Red rises fast and
// levels off, green is the square of the heat, blue is its **tenth** power --
// which is what keeps the white core to the very hottest texels -- and anything
// under half heat is fully transparent rather than dark.
constexpr float kRedScale = 155.0f;
constexpr float kRedFloor = 100.0f;
constexpr float kHeatGain = 1.8f;
constexpr float kOpaqueAbove = 0.5f;

}  // namespace

FlameTexture::FlameTexture(i64 seed) : rng_(seed) {}

void FlameTexture::tick()
{
    for (int x = 0; x < kFlameWidth; ++x) {
        for (int y = 0; y < kFlameHeight; ++y) {
            // **The cell below, weighted eighteen to one.** This single term is
            // the whole reason a flame rises: every cell is mostly a copy of
            // the one under it, and the six-neighbour sum around it is the
            // flicker. The modulo wraps the bottom row to the top, which the
            // original does and which never shows because the bottom four rows
            // are off the tile.
            int weight = kSeedWeight;
            float sum = current_[x + ((y + 1) % kFlameHeight) * kFlameWidth] * float(weight);

            for (int nx = x - 1; nx <= x + 1; ++nx) {
                for (int ny = y; ny <= y + 1; ++ny) {
                    // **No wrapping here, unlike the term above.** A neighbour
                    // off the left or right edge is skipped and still counts
                    // towards the divisor, which is what makes the two vertical
                    // edges of the tile cooler than its middle.
                    if (nx >= 0 && ny >= 0 && nx < kFlameWidth && ny < kFlameHeight) {
                        sum += current_[nx + ny * kFlameWidth];
                    }
                    ++weight;
                }
            }

            next_[x + y * kFlameWidth] = sum / (float(weight) * kDecay);

            if (y >= kFlameHeight - 1) {
                // The seed row: three randoms multiplied together, which is a
                // distribution heavily weighted towards nothing with an
                // occasional bright ember, plus a small floor so the fire never
                // goes out. The constants are the class file's doubles, which
                // are `0.1f` and `0.2f` widened and are not 0.1 and 0.2.
                const double ember = rng_.nextDouble() * rng_.nextDouble() * rng_.nextDouble() * 4.0;
                const double floorTerm = rng_.nextDouble() * 0.10000000149011612;
                next_[x + y * kFlameWidth] = float(ember + floorTerm + 0.20000000298023224);
            }
        }
    }

    // The two fields swap rather than copy, which is the original's own move
    // and is why `current_` is what the colour pass below reads.
    for (int i = 0; i < kFlameWidth * kFlameHeight; ++i) {
        const float held = current_[i];
        current_[i] = next_[i];
        next_[i] = held;
    }
}

void FlameTexture::writeTo(u8* dst) const
{
    for (int i = 0; i < kFlameWidth * kFlameVisibleRows; ++i) {
        float heat = current_[i] * kHeatGain;
        heat = heat > 1.0f ? 1.0f : (heat < 0.0f ? 0.0f : heat);

        const int red = int(heat * kRedScale + kRedFloor);
        const int green = int(heat * heat * 255.0f);
        // f^10, written as the nine multiplications the class file performs
        // rather than as a pow: pow would be a different number in the last
        // bits and this is a colour ramp with a very sharp knee.
        const float h2 = heat * heat;
        const float h4 = h2 * h2;
        const float h8 = h4 * h4;
        const int blue = int(h8 * h2 * 255.0f);
        const int alpha = heat < kOpaqueAbove ? 0 : 255;

        dst[i * 4 + 0] = u8(red);
        dst[i * 4 + 1] = u8(green);
        dst[i * 4 + 2] = u8(blue);
        dst[i * 4 + 3] = u8(alpha);
    }
}

namespace {

// A stated seed rather than a clock. See the header: there is no sequence to
// match, so the useful property is that this build draws the same flame every
// time and a test can say so.
i64 flameSeed(int which) { return 0x3DA1F17E + which; }

}  // namespace

int flameTile(int which)
{
    if (which < 0 || which >= kFlameCount) {
        return -1;
    }

    // **Asked of the render type, not of a block id.** Renderer code references
    // render types and never ids -- see CONTRIBUTING -- and this is the same
    // rule: the tile to patch is whatever the block that renders as fire says
    // its texture is.
    int firstTile = -1;
    for (int id = 0; id < mcver::kBlockTableSize; ++id) {
        const block::BlockDef& def = mcver::kBlocks[id];
        if (def.known && def.render == block::RenderType::Fire) {
            firstTile = int(def.texture);
            break;
        }
    }
    if (firstTile < 0) {
        return -1;
    }

    // One row of the atlas apart. The two are separate simulations with
    // separate noise, which is what makes a1.1.2's fire two alternating layers
    // rather than one doubled.
    const int tile = firstTile + which * kAtlasTilesPerEdge;
    return tile < kAtlasTilesPerEdge * kAtlasTilesPerEdge ? tile : -1;
}

FlameAnimation::FlameAnimation() : flames_{FlameTexture(flameSeed(0)), FlameTexture(flameSeed(1))}
{
    for (int which = 0; which < kFlameCount; ++which) {
        tile_[which] = flameTile(which);
        if (tile_[which] < 0) {
            continue;
        }
        for (int i = 0; i < kFlameSettleTicks; ++i) {
            flames_[which].tick();
        }
        flames_[which].writeTo(texels_[which]);
    }
}

void FlameAnimation::tick(int ticks)
{
    if (ticks <= 0 || !present()) {
        return;
    }
    for (int which = 0; which < kFlameCount; ++which) {
        if (tile_[which] < 0) {
            continue;
        }
        for (int i = 0; i < ticks; ++i) {
            flames_[which].tick();
        }
        flames_[which].writeTo(texels_[which]);
    }
}

void applyAnimatedTiles(std::vector<u8>* atlasRgba)
{
    if (atlasRgba == nullptr || atlasRgba->size() != kAtlasBytes) {
        return;
    }

    // **Water and lava first**, because they are the same kind of thing and
    // there are four more of them: see core/texture/fluid_fx.hpp. A pack's own
    // water tile is overwritten for the same stated reason its fire tile is --
    // the original registers the FX whatever terrain.png holds.
    applyFluidTiles(atlasRgba->data());

    for (int which = 0; which < kFlameCount; ++which) {
        const int tile = flameTile(which);
        if (tile < 0) {
            continue;
        }

        FlameTexture flame(flameSeed(which));
        for (int i = 0; i < kFlameSettleTicks; ++i) {
            flame.tick();
        }

        u8 texels[kFlameWidth * kFlameVisibleRows * 4];
        flame.writeTo(texels);

        const int tileX = (tile % kAtlasTilesPerEdge) * kAtlasTilePixels;
        const int tileY = (tile / kAtlasTilesPerEdge) * kAtlasTilePixels;
        for (int row = 0; row < kAtlasTilePixels; ++row) {
            u8* out = atlasRgba->data() + (usize(tileY + row) * kAtlasEdge + usize(tileX)) * 4;
            // The flame is 16 x 16 whatever the pack was; the atlas has already
            // been scaled to a 16-pixel tile grid by the time this runs, so the
            // two agree by construction. The static_assert next door says so.
            std::memcpy(out, texels + usize(row) * kFlameWidth * 4, kFlameWidth * 4);
        }
    }
}

static_assert(kFlameWidth == kAtlasTilePixels && kFlameVisibleRows == kAtlasTilePixels,
              "the generated flame is written straight into an atlas tile");

}  // namespace mc::texture
