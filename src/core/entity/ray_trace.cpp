// a1.1.2's block ray trace, transcribed. See ray_trace.hpp and
// docs/physics-a1.1.2.md.

#include "core/entity/ray_trace.hpp"

#include "core/block/collision.hpp"
#include "core/block/registry.hpp"
#include "core/tick/tick_world.hpp"
#include "core/util/math_helper.hpp"

#include <cmath>

namespace mc::entity {
namespace {

struct Vec3d {
    double x = 0.0, y = 0.0, z = 0.0;
};

bool isNaN(const Vec3d& v)
{
    return !(v.x == v.x) || !(v.y == v.y) || !(v.z == v.z);
}

// `Vec3D.getIntermediateWithXValue` and its two siblings: where the segment
// crosses a given plane, or nothing if it never does.
//
// **The epsilon is `1.0E-7f` widened, not `1.0E-7`.** It is stored in the class
// file as 1.0000000116860974E-7, and it guards a division that would otherwise
// produce an infinity for a ray parallel to the plane.
constexpr double kParallelEpsilon = 1.0000000116860974E-7;

bool intermediate(const Vec3d& from, const Vec3d& to, int axis, double plane, Vec3d* out)
{
    const double d[3] = {to.x - from.x, to.y - from.y, to.z - from.z};
    const double start[3] = {from.x, from.y, from.z};
    if (d[axis] * d[axis] < kParallelEpsilon) {
        return false;
    }
    const double t = (plane - start[axis]) / d[axis];
    if (t < 0.0 || t > 1.0) {
        return false;
    }
    out->x = from.x + d[0] * t;
    out->y = from.y + d[1] * t;
    out->z = from.z + d[2] * t;
    return true;
}

// `isVecInsideYZBounds` and friends: is the crossing point actually on the
// face, or did the ray pass the plane outside the box? Inclusive on both ends,
// as the original is.
bool onFace(const AABB& box, const Vec3d& p, int axis)
{
    const double lo[3] = {box.minX, box.minY, box.minZ};
    const double hi[3] = {box.maxX, box.maxY, box.maxZ};
    const double v[3] = {p.x, p.y, p.z};
    const int a = (axis + 1) % 3;
    const int b = (axis + 2) % 3;
    return v[a] >= lo[a] && v[a] <= hi[a] && v[b] >= lo[b] && v[b] <= hi[b];
}

// `Vec3D.distanceTo`. **The square root is not optional**, tempting as it is:
// comparing squared distances orders candidates the same way in arithmetic but
// not in floating point. Two faces of a block can be near enough that sqrt
// rounds both to the same double, and the original's `<` then keeps the
// earlier one while a comparison of squares keeps the later. That is one ray in
// 3,744 in the fixture, wrong in the last bit of the hit point -- which is
// exactly the sort of thing that only ever shows up as a face reported wrong at
// a grazing angle.
double distanceTo(const Vec3d& a, const Vec3d& b)
{
    const double dx = a.x - b.x;
    const double dy = a.y - b.y;
    const double dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// `Block.collisionRayTrace`: six plane crossings, keep the nearest that lands
// on the box. Ties go to the earlier candidate, which is what the original's
// strict `<` does and is why the order of the six below is part of the answer.
bool traceBox(const AABB& box, const Vec3d& from, const Vec3d& to, Vec3d* point,
              mesh::Face* face)
{
    // minX, maxX, minY, maxY, minZ, maxZ -- and the face each one is, in the
    // original's own numbering.
    const double planes[6] = {box.minX, box.maxX, box.minY, box.maxY, box.minZ, box.maxZ};
    const int axes[6] = {0, 0, 1, 1, 2, 2};
    const mesh::Face faces[6] = {mesh::kFaceNegX, mesh::kFacePosX, mesh::kFaceNegY,
                                 mesh::kFacePosY, mesh::kFaceNegZ, mesh::kFacePosZ};

    bool found = false;
    double best = 0.0;
    for (int i = 0; i < 6; ++i) {
        Vec3d candidate;
        if (!intermediate(from, to, axes[i], planes[i], &candidate)) {
            continue;
        }
        if (!onFace(box, candidate, axes[i])) {
            continue;
        }
        const double d = distanceTo(from, candidate);
        if (!found || d < best) {
            found = true;
            best = d;
            *point = candidate;
            *face = faces[i];
        }
    }
    return found;
}

}  // namespace

RayHit rayTrace(const tick::TickWorld& world, double eyeX, double eyeY, double eyeZ,
                double dirX, double dirY, double dirZ, double reach, bool hitLiquids)
{
    RayHit miss;

    Vec3d from{eyeX, eyeY, eyeZ};
    const Vec3d to{eyeX + dirX * reach, eyeY + dirY * reach, eyeZ + dirZ * reach};
    if (isNaN(from) || isNaN(to)) {
        return miss;
    }

    const i32 endX = MathHelper::floorDouble(to.x);
    const int endY = int(MathHelper::floorDouble(to.y));
    const i32 endZ = MathHelper::floorDouble(to.z);
    i32 x = MathHelper::floorDouble(from.x);
    int y = int(MathHelper::floorDouble(from.y));
    i32 z = MathHelper::floorDouble(from.z);

    for (int step = kRayStepLimit; step >= 0; --step) {
        if (isNaN(from)) {
            return miss;
        }
        // The far end reached without hitting anything.
        if (x == endX && y == endY && z == endZ) {
            return miss;
        }

        // The next cell wall on each axis, or 999 for an axis that is not
        // moving. 999 is the original's own sentinel and it works because a
        // world coordinate never reaches it in the cases that matter.
        constexpr double kNoWall = 999.0;
        double wallX = kNoWall;
        double wallY = kNoWall;
        double wallZ = kNoWall;
        if (endX > x) { wallX = double(x) + 1.0; }
        if (endX < x) { wallX = double(x) + 0.0; }
        if (endY > y) { wallY = double(y) + 1.0; }
        if (endY < y) { wallY = double(y) + 0.0; }
        if (endZ > z) { wallZ = double(z) + 1.0; }
        if (endZ < z) { wallZ = double(z) + 0.0; }

        double tX = kNoWall;
        double tY = kNoWall;
        double tZ = kNoWall;
        const double dx = to.x - from.x;
        const double dy = to.y - from.y;
        const double dz = to.z - from.z;
        if (wallX != kNoWall) { tX = (wallX - from.x) / dx; }
        if (wallY != kNoWall) { tY = (wallY - from.y) / dy; }
        if (wallZ != kNoWall) { tZ = (wallZ - from.z) / dz; }

        // Step to the nearest wall and remember which one it was.
        mesh::Face crossed = mesh::kFaceNegY;
        if (tX < tY && tX < tZ) {
            crossed = endX > x ? mesh::kFaceNegX : mesh::kFacePosX;
            from = Vec3d{wallX, from.y + dy * tX, from.z + dz * tX};
        } else if (tY < tZ) {
            crossed = endY > y ? mesh::kFaceNegY : mesh::kFacePosY;
            from = Vec3d{from.x + dx * tY, wallY, from.z + dz * tY};
        } else {
            crossed = endZ > z ? mesh::kFaceNegZ : mesh::kFacePosZ;
            from = Vec3d{from.x + dx * tZ, from.y + dy * tZ, wallZ};
        }

        // **The cell just entered, which needs a nudge on three of the six
        // faces.** Landing exactly on a wall floors into the cell on the far
        // side of it, so a step in the negative direction has to be pulled
        // back by one. The original does the same thing and for the same
        // reason.
        x = MathHelper::floorDouble(from.x);
        if (crossed == mesh::kFacePosX) { --x; }
        y = int(MathHelper::floorDouble(from.y));
        if (crossed == mesh::kFacePosY) { --y; }
        z = MathHelper::floorDouble(from.z);
        if (crossed == mesh::kFacePosZ) { --z; }

        const block::BlockId id = world.blockAt(x, y, z);
        if (id == block::kAir) {
            continue;
        }

        const u8 metadata = world.dataAt(x, y, z);
        if (!block::isTargetable(id, metadata, hitLiquids)) {
            continue;
        }

        // **In the block's own coordinates, then translated back** -- which is
        // what `Block.collisionRayTrace` does, and it is not the same as
        // intersecting the box where it stands. Doing it in world coordinates
        // agrees to about fifteen digits and then disagrees in the last two,
        // because `a + (b - a)*t` and `(a - o) + ((b - o) - (a - o))*t + o`
        // round differently. One ray in 3,744 of the fixture caught it.
        const AABB box = block::selectionBox(id, metadata);
        const Vec3d localFrom{from.x - double(x), from.y - double(y), from.z - double(z)};
        const Vec3d localTo{to.x - double(x), to.y - double(y), to.z - double(z)};

        Vec3d point;
        mesh::Face face = mesh::kFaceNegY;
        if (!traceBox(box, localFrom, localTo, &point, &face)) {
            // The ray crossed the cell but missed the shape inside it -- a
            // torch seen past its corner. Keep walking.
            continue;
        }

        RayHit result;
        result.hit = true;
        result.x = x;
        result.y = y;
        result.z = z;
        result.face = face;
        result.hitX = point.x + double(x);
        result.hitY = point.y + double(y);
        result.hitZ = point.z + double(z);
        return result;
    }

    return miss;
}

double entityPickReach(double eyeX, double eyeY, double eyeZ, const RayHit& blockHit)
{
    double reach = kBlockReach;
    if (blockHit.hit) {
        const double dx = blockHit.hitX - eyeX;
        const double dy = blockHit.hitY - eyeY;
        const double dz = blockHit.hitZ - eyeZ;
        reach = double(MathHelper::sqrtDouble(dx * dx + dy * dy + dz * dz));
    }
    return reach > kEntityReach ? kEntityReach : reach;
}

bool interceptDistance(const AABB& box, double fromX, double fromY, double fromZ, double toX,
                       double toY, double toZ, double* distance)
{
    const Vec3d from{fromX, fromY, fromZ};
    const Vec3d to{toX, toY, toZ};
    // The same six planes in the same order as `traceBox`, but **chosen on
    // squared distance**: `calculateIntercept` compares `squareDistanceTo`,
    // unlike `Block.collisionRayTrace`. Ties keep the earlier face.
    const double planes[6] = {box.minX, box.maxX, box.minY, box.maxY, box.minZ, box.maxZ};
    const int axes[6] = {0, 0, 1, 1, 2, 2};

    bool found = false;
    double best = 0.0;
    for (int i = 0; i < 6; ++i) {
        Vec3d candidate;
        if (!intermediate(from, to, axes[i], planes[i], &candidate)
            || !onFace(box, candidate, axes[i])) {
            continue;
        }
        const double dx = from.x - candidate.x;
        const double dy = from.y - candidate.y;
        const double dz = from.z - candidate.z;
        const double squared = dx * dx + dy * dy + dz * dz;
        if (!found || squared < best) {
            found = true;
            best = squared;
        }
    }
    if (found) {
        *distance = double(MathHelper::sqrtDouble(best));
    }
    return found;
}

}  // namespace mc::entity
