#include "impl/worldgen/alpha_nobiome/big_tree.hpp"

#include "blocks.hpp"
#include "core/util/java_cast.hpp"
#include "core/util/java_random.hpp"
#include "impl/worldgen/alpha_nobiome/math_helper.hpp"
#include "impl/worldgen/alpha_nobiome/population_view.hpp"

#include <cmath>
#include <vector>

namespace mc::worldgen {

namespace {

constexpr u8 kAir = u8(mcver::Block::Air);
constexpr u8 kGrass = u8(mcver::Block::Grass);
constexpr u8 kDirt = u8(mcver::Block::Dirt);
constexpr u8 kWood = u8(mcver::Block::Log);
constexpr u8 kLeaves = u8(mcver::Block::Leaves);

// `ej.a`, the static byte table. It pairs each axis with the other two:
// for axis i, `kOther[i]` and `kOther[i + 3]` are the two axes that are not i.
// The original stores it as one six-entry array and indexes past the middle,
// which is why it is one array here too.
constexpr i8 kOther[6] = {2, 0, 0, 1, 2, 1};

i32 absOf(i32 value)
{
    return value < 0 ? -value : value;
}

float absOf(float value)
{
    return value < 0.0f ? -value : value;
}

// A planned leaf cluster: where it sits, and the y its branch leaves the
// trunk at. The original carries these as `int[4]` rows of one big array.
struct LeafNode {
    i32 x = 0;
    i32 y = 0;
    i32 z = 0;
    i32 branchBase = 0;
};

// The generator's whole state. A struct rather than free functions because
// the original is a class with fourteen fields and the methods read them
// freely -- flattening that into arguments would obscure which are constants
// and which the plan mutates.
struct BigTree {
    PopulationView& view;
    JavaRandom random;  // **private**, seeded from one draw of the shared one

    i32 originX = 0;
    i32 originY = 0;
    i32 originZ = 0;

    i32 height = 0;      // `e`
    i32 trunkHeight = 0; // `f`

    // The constructor's constants, none of which a1.1.2 ever changes except
    // `heightLimit` via setScale.
    double trunkHeightScale = 0.618;  // `g`
    double branchSlope = 0.381;       // `i`
    double branchDensity = 1.0;       // `j`
    double heightAttenuation = 1.0;   // `k`
    i32 trunkSize = 1;                // `l`
    i32 heightLimit = 12;             // `m`
    i32 leafDistanceLimit = 4;        // `n`

    std::vector<LeafNode> leafNodes;

    explicit BigTree(PopulationView& v, i64 seed) : view(v), random(seed) {}

    // `ej.a(I)F` -- the canopy's radius at a given absolute y, as a fraction
    // of the tree's own height. Returns -1.618 to mean "no leaves here".
    float layerSize(i32 y) const
    {
        if (double(y) < double(float(height)) * 0.3) {
            return -1.618f;
        }

        const float half = float(height) / 2.0f;
        const float fromMiddle = half - float(y);

        float size;
        if (fromMiddle == 0.0f) {
            size = half;
        } else if (absOf(fromMiddle) >= half) {
            size = 0.0f;
        } else {
            // pow(x, 2.0) is bit-identical to x * x -- see the header. The
            // intermediate is double and the result narrows to float, exactly
            // as the original does.
            const double a = double(absOf(half));
            const double b = double(absOf(fromMiddle));
            size = float(std::sqrt(a * a - b * b));
        }
        return size * 0.5f;
    }

    // `ej.b(I)F` -- the radius of one horizontal slice of a leaf cluster.
    float leafSize(i32 layer) const
    {
        if (layer < 0 || layer >= leafDistanceLimit) {
            return -1.0f;
        }
        return (layer == 0 || layer == leafDistanceLimit - 1) ? 2.0f : 3.0f;
    }

    // `ej.c(I)Z` -- whether a branch this far up the trunk is worth drawing.
    bool branchWorthDrawing(i32 rise) const
    {
        return double(rise) >= double(height) * 0.2;
    }

    // `ej.a([I[I)I` -- how far a straight line from `from` to `to` gets before
    // it hits something that is neither air nor leaves. Returns -1 when the
    // whole line is clear, which is the "yes" answer.
    i32 clearLineLength(const i32 from[3], const i32 to[3]) const
    {
        i32 delta[3] = {0, 0, 0};
        i8 longest = 0;

        for (i8 axis = 0; axis < 3; ++axis) {
            delta[axis] = to[axis] - from[axis];
            if (absOf(delta[axis]) > absOf(delta[longest])) {
                longest = axis;
            }
        }

        if (delta[longest] == 0) {
            return -1;
        }

        const i8 secondary = kOther[longest];
        const i8 tertiary = kOther[longest + 3];
        const i32 step = delta[longest] > 0 ? 1 : -1;

        const double slopeSecondary = double(delta[secondary]) / double(delta[longest]);
        const double slopeTertiary = double(delta[tertiary]) / double(delta[longest]);

        i32 at[3] = {0, 0, 0};
        i32 travelled = 0;
        const i32 limit = delta[longest] + step;

        for (; travelled != limit; travelled += step) {
            at[longest] = from[longest] + travelled;
            // **No rounding here**, unlike drawLine below, which adds 0.5
            // first -- so the line that is *checked* is not the line that gets
            // *drawn*. The asymmetry is the original's and it is transcribed
            // as found.
            //
            // **It is a known gap in the fixture**, stated here rather than
            // left implied: mutating this to round like drawLine passes the
            // whole suite. That is not for want of trying. Instrumenting the
            // twelve fixture cases, the two roundings visit a different cell
            // on **48 of 521 steps** -- so the difference is live, not
            // theoretical -- but on every one of those 48 the two cells agree
            // on whether the line is blocked, so the verdict never changes.
            //
            // Three obstruction schemes were built to force a disagreement and
            // none did. Anything close enough to the trunk to be hit by a
            // check line is also close enough for `chooseHeight` to clip the
            // tree short, and a short tree plants few clusters and walks few
            // lines: the obstructed cases run 5 to 25 steps against 125 on
            // clear ground. Closing this properly needs a hand-built scene
            // rather than a probed one, and is worth doing when something
            // else needs a scene of that shape.
            at[secondary] = javaToInt(double(from[secondary]) + double(travelled) * slopeSecondary);
            at[tertiary] = javaToInt(double(from[tertiary]) + double(travelled) * slopeTertiary);

            const u8 id = view.blockAt(at[0], at[1], at[2]);
            if (id != kAir && id != kLeaves) {
                break;
            }
        }

        return travelled == limit ? -1 : absOf(travelled);
    }

    // `ej.a([I[II)V` -- draw a line of one block id.
    void drawLine(const i32 from[3], const i32 to[3], u8 id)
    {
        i32 delta[3] = {0, 0, 0};
        i8 longest = 0;

        for (i8 axis = 0; axis < 3; ++axis) {
            delta[axis] = to[axis] - from[axis];
            if (absOf(delta[axis]) > absOf(delta[longest])) {
                longest = axis;
            }
        }

        if (delta[longest] == 0) {
            return;
        }

        const i8 secondary = kOther[longest];
        const i8 tertiary = kOther[longest + 3];
        const i32 step = delta[longest] > 0 ? 1 : -1;

        const double slopeSecondary = double(delta[secondary]) / double(delta[longest]);
        const double slopeTertiary = double(delta[tertiary]) / double(delta[longest]);

        i32 at[3] = {0, 0, 0};
        i32 travelled = 0;
        const i32 limit = delta[longest] + step;

        for (; travelled != limit; travelled += step) {
            // `eo.b(D)I` -- floor, not truncation, and the +0.5 makes it a
            // round-to-nearest. MathHelper::floorDouble is the same method the
            // cave carver uses.
            at[longest] = MathHelper::floorDouble(double(from[longest] + travelled) + 0.5);
            at[secondary] = MathHelper::floorDouble(
                double(from[secondary]) + double(travelled) * slopeSecondary + 0.5);
            at[tertiary] = MathHelper::floorDouble(
                double(from[tertiary]) + double(travelled) * slopeTertiary + 0.5);
            view.setBlock(at[0], at[1], at[2], id);
        }
    }

    // `ej.a(IIIFBI)V` -- one horizontal disc of leaves.
    void leafDisc(i32 cx, i32 cy, i32 cz, float radius, i8 axis, u8 id)
    {
        // The 0.618 is the original's own constant and not a rounding
        // convention -- though for the only two radii `leafSize` ever returns,
        // 2 and 3, any offset in [0, 1) gives the same span, so mutating it to
        // 0.5 is an equivalent program. Kept as written because the
        // equivalence is a property of those two radii, not of the code.
        const i32 span = javaToInt(double(radius) + 0.618);
        const i8 secondary = kOther[axis];
        const i8 tertiary = kOther[axis + 3];

        i32 centre[3] = {cx, cy, cz};
        i32 at[3] = {0, 0, 0};
        at[axis] = centre[axis];

        for (i32 a = -span; a <= span; ++a) {
            at[secondary] = centre[secondary] + a;
            for (i32 b = -span; b <= span; ++b) {
                // Both pow calls again, and again exactly a square. The 0.5
                // offsets make the disc a circle around block centres rather
                // than around a corner.
                const double da = double(absOf(a)) + 0.5;
                const double db = double(absOf(b)) + 0.5;
                // Strictly greater, and `>=` is an equivalent mutant here:
                // `da` and `db` are always half-integers, so the distance is
                // sqrt of a sum of two odd quarters, and no such value equals
                // 2 or 3 exactly. Checked over the whole reachable range.
                if (std::sqrt(da * da + db * db) > double(radius)) {
                    continue;
                }
                at[tertiary] = centre[tertiary] + b;
                const u8 existing = view.blockAt(at[0], at[1], at[2]);
                if (existing != kAir && existing != kLeaves) {
                    continue;
                }
                view.setBlock(at[0], at[1], at[2], id);
            }
        }
    }

    // `ej.a()V` -- plan every leaf cluster. This is where the shape is decided
    // and where the sin/cos pair lives.
    void planLeafNodes()
    {
        trunkHeight = javaToInt(double(height) * trunkHeightScale);
        if (trunkHeight >= height) {
            trunkHeight = height - 1;
        }

        const double scaled = heightAttenuation * double(height) / 13.0;
        i32 attempts = javaToInt(1.382 + scaled * scaled);
        if (attempts < 1) {
            attempts = 1;
        }

        leafNodes.clear();

        i32 y = originY + height - leafDistanceLimit;
        const i32 trunkTop = originY + trunkHeight;
        i32 rise = y - originY;

        // The first node is the top of the trunk itself.
        leafNodes.push_back(LeafNode{originX, y, originZ, trunkTop});
        --y;

        for (; rise >= 0; --y, --rise) {
            const float size = layerSize(rise);
            if (size < 0.0f) {
                continue;
            }

            for (i32 i = 0; i < attempts; ++i) {
                // **Two draws per attempt, always, before anything is
                // decided.** A cluster that turns out to be blocked has still
                // consumed them -- but this is the private Random, so the cost
                // never leaves the tree.
                const double reach =
                    branchDensity * double(size) * (double(random.nextFloat()) + 0.328);
                const double angle = double(random.nextFloat()) * 2.0 * 3.14159;

                // The one genuine transcendental pair. See the header for why
                // std::sin is used rather than a transcription of fdlibm.
                const i32 nx = javaToInt(reach * std::sin(angle) + double(originX) + 0.5);
                const i32 nz = javaToInt(reach * std::cos(angle) + double(originZ) + 0.5);

                const i32 from[3] = {nx, y, nz};
                const i32 to[3] = {nx, y + leafDistanceLimit, nz};
                if (clearLineLength(from, to) != -1) {
                    continue;
                }

                // Can a branch reach this cluster from the trunk? The branch
                // starts level with the trunk, drops by the horizontal
                // distance times the slope, and is clipped at the trunk top.
                const i32 trunkPoint[3] = {originX, originY, originZ};
                const double dx = double(absOf(originX - from[0]));
                const double dz = double(absOf(originZ - from[2]));
                const double horizontal = std::sqrt(dx * dx + dz * dz);
                const double drop = horizontal * branchSlope;

                i32 base[3] = {trunkPoint[0], trunkPoint[1], trunkPoint[2]};
                base[1] = (double(from[1]) - drop > double(trunkTop))
                              ? trunkTop
                              : javaToInt(double(from[1]) - drop);

                if (clearLineLength(base, from) == -1) {
                    leafNodes.push_back(LeafNode{nx, y, nz, base[1]});
                }
            }
        }
    }

    // `ej.a(III)V` -- the stack of discs that makes one cluster.
    void leafCluster(i32 cx, i32 cy, i32 cz)
    {
        for (i32 layer = cy; layer < cy + leafDistanceLimit; ++layer) {
            leafDisc(cx, layer, cz, leafSize(layer - cy), 1, kLeaves);
        }
    }

    // `ej.b()V`
    void growLeaves()
    {
        for (const LeafNode& node : leafNodes) {
            leafCluster(node.x, node.y, node.z);
        }
    }

    // `ej.c()V` -- the trunk. `trunkSize` is 1 in a1.1.2, so the four extra
    // columns are dead code; they are transcribed anyway because a version
    // manifest could set it and because leaving them out would misrepresent
    // the original.
    void growTrunk()
    {
        i32 from[3] = {originX, originY, originZ};
        i32 to[3] = {originX, originY + trunkHeight, originZ};

        drawLine(from, to, kWood);
        if (trunkSize == 2) {
            ++from[0];
            ++to[0];
            drawLine(from, to, kWood);
            ++from[2];
            ++to[2];
            drawLine(from, to, kWood);
            --from[0];
            --to[0];
            drawLine(from, to, kWood);
        }
    }

    // `ej.d()V` -- the branches, one per planned cluster that is high enough.
    void growBranches()
    {
        for (const LeafNode& node : leafNodes) {
            const i32 from[3] = {originX, node.branchBase, originZ};
            const i32 to[3] = {node.x, node.y, node.z};
            if (branchWorthDrawing(node.branchBase - originY)) {
                drawLine(from, to, kWood);
            }
        }
    }

    // `ej.e()Z` -- can a tree stand here at all, and how tall.
    bool chooseHeight()
    {
        const i32 base[3] = {originX, originY, originZ};
        const i32 top[3] = {originX, originY + height - 1, originZ};

        const u8 below = view.blockAt(originX, originY - 1, originZ);
        if (below != kGrass && below != kDirt) {
            return false;
        }

        const i32 clear = clearLineLength(base, top);
        if (clear == -1) {
            return true;
        }
        // Anything under six blocks of clearance is not worth a big tree.
        if (clear < 6) {
            return false;
        }
        height = clear;
        return true;
    }
};

}  // namespace

bool generateBigTree(PopulationView& view, JavaRandom& random, BigTreeState& state, i32 x, i32 y,
                     i32 z, double heightScale)
{
    // **The only draw from the shared stream.** Everything below runs on the
    // private Random seeded from it, so however the tree turns out, the chunk's
    // stream has advanced by exactly one nextLong.
    const i64 seed = random.nextLong();

    BigTree tree(view, seed);
    tree.originX = x;
    tree.originY = y;
    tree.originZ = z;

    // `ik.a(DDD)` -- setScale. The driver passes 1.0, which leaves everything
    // at its constructor default; the branch is written out so the parameter
    // is not silently ignored.
    tree.heightLimit = javaToInt(heightScale * 12.0);
    if (heightScale > 0.5) {
        tree.leafDistanceLimit = 5;
    }

    // `if (this.e == 0) this.e = 5 + this.b.nextInt(this.m)`. The guard is the
    // whole point: the driver holds one `ej` for the chunk, so this rolls for
    // the first big tree and every later one inherits it -- including the
    // shrink `chooseHeight` applies at a cramped site. See BigTreeState.
    //
    // Note the draw is from the tree's **private** Random, so skipping it on
    // later trees cannot move the chunk's shared stream. That is why the
    // symptom was one wrong tree rather than a wrong chunk, and why it took a
    // whole-pipeline comparison to find.
    tree.height = state.heightLimit;
    if (tree.height == 0) {
        tree.height = 5 + tree.random.nextInt(tree.heightLimit);
    }

    if (!tree.chooseHeight()) {
        state.heightLimit = tree.height;
        return false;
    }
    state.heightLimit = tree.height;

    tree.planLeafNodes();
    tree.growLeaves();
    tree.growTrunk();
    tree.growBranches();
    return true;
}

}  // namespace mc::worldgen
