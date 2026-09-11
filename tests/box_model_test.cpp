// The box model -- `ip.addBox` and `ll`'s UV constructor.
//
// This is the piece docs/entity-render-a1.1.2.md singles out as worth testing,
// and the reason is that its failure mode is not a crash: a box whose UVs are
// transposed, or whose depth and width are swapped, is a boat with the wrong
// planks on it, and no screenshot tells you which of the six faces went wrong.
// Twenty-four vertices out of seven numbers is a pure function, so it can be
// checked exactly.

#include "core/render/box_model.hpp"
#include "core/texture/entity_skins.hpp"
#include "framework.hpp"

#include <cmath>

using namespace mc;
using mc::render::ModelPart;
using mc::render::Placement;
using mc::render::buildBox;
using mc::render::kBoxVertices;
using mc::render::kModelUnit;
using mc::texture::EntitySkin;

namespace {

constexpr double kUnits = double(mesh::kDetailUnitsPerBlock);

struct Built {
    mesh::DetailVertex v[kBoxVertices];
    int count = 0;
};

Built build(const ModelPart& part, const Placement& place, EntitySkin skin = EntitySkin::Boat)
{
    Built out;
    out.count = buildBox(part, place, skin, 0xF0, out.v, kBoxVertices);
    return out;
}

// The axis-aligned bounds of what came out, in blocks.
void bounds(const Built& b, double* lo, double* hi)
{
    for (int a = 0; a < 3; ++a) {
        lo[a] = 1e30;
        hi[a] = -1e30;
    }
    for (int i = 0; i < b.count; ++i) {
        const double p[3] = {double(b.v[i].x) / kUnits, double(b.v[i].y) / kUnits,
                             double(b.v[i].z) / kUnits};
        for (int a = 0; a < 3; ++a) {
            if (p[a] < lo[a]) lo[a] = p[a];
            if (p[a] > hi[a]) hi[a] = p[a];
        }
    }
}

bool near(double a, double b, double tol = 0.003)
{
    return std::fabs(a - b) <= tol;
}

}  // namespace

TEST(a_box_is_six_quads_and_never_fewer)
{
    ModelPart part;
    part.w = 4;
    part.h = 4;
    part.d = 4;
    const Built b = build(part, Placement{});
    // A box model has no neighbours to hide behind, so no face is ever culled
    // at build time the way a chunk face is.
    CHECK_EQ(b.count, 24);
}

TEST(a_box_that_does_not_fit_writes_nothing)
{
    ModelPart part;
    part.w = part.h = part.d = 4;
    mesh::DetailVertex out[8];
    CHECK_EQ(buildBox(part, Placement{}, EntitySkin::Boat, 0, out, 8), 0);
}

TEST(model_units_are_a_sixteenth_of_a_block)
{
    // A 16 x 16 x 16 box is one block across, which is the whole meaning of
    // `kModelUnit` and the one number a wrong scale shows up in immediately.
    ModelPart part;
    part.w = part.h = part.d = 16;
    const Built b = build(part, Placement{});

    double lo[3], hi[3];
    bounds(b, lo, hi);
    for (int a = 0; a < 3; ++a) {
        CHECK(near(hi[a] - lo[a], 1.0));
        CHECK(near(lo[a], 0.0));
    }
}

TEST(the_box_is_placed_relative_to_its_rotation_point)
{
    // `addBox`'s x/y/z are relative to `setRotationPoint`, not to the model
    // origin -- so moving the pivot moves the box and nothing else.
    ModelPart part;
    part.w = part.h = part.d = 16;
    part.pivotX = 16.0f;
    part.pivotY = -32.0f;
    part.pivotZ = 8.0f;
    const Built b = build(part, Placement{});

    double lo[3], hi[3];
    bounds(b, lo, hi);
    CHECK(near(lo[0], 1.0));
    CHECK(near(lo[1], -2.0));
    CHECK(near(lo[2], 0.5));
}

TEST(grow_expands_every_face_and_does_not_move_the_box)
{
    ModelPart plain;
    plain.w = plain.h = plain.d = 8;
    ModelPart grown = plain;
    grown.grow = 1.0f;

    double lo0[3], hi0[3], lo1[3], hi1[3];
    bounds(build(plain, Placement{}), lo0, hi0);
    bounds(build(grown, Placement{}), lo1, hi1);

    for (int a = 0; a < 3; ++a) {
        // One model unit out on each side, which is a sixteenth of a block.
        CHECK(near(lo1[a], lo0[a] - kModelUnit));
        CHECK(near(hi1[a], hi0[a] + kModelUnit));
        // ...and the centre has not moved, which is the half a naive "scale"
        // reading of the argument would get wrong.
        CHECK(near((lo1[a] + hi1[a]) / 2.0, (lo0[a] + hi0[a]) / 2.0));
    }
}

TEST(the_placement_translates_and_turns_the_whole_model)
{
    ModelPart part;
    part.w = part.h = part.d = 16;

    // A quarter turn about Y takes the box's +x extent onto -z, which is the
    // one rotation every one of these entities uses.
    const Placement turned = render::placeAt(10.0, 20.0, 30.0, 3.14159265f / 2.0f);
    const Built b = build(part, turned);

    double lo[3], hi[3];
    bounds(b, lo, hi);
    // Still one block on a side however it is turned.
    CHECK(near(hi[0] - lo[0], 1.0));
    CHECK(near(hi[2] - lo[2], 1.0));
    // The box spans 0..1 in model x and z, so after a quarter turn about the
    // origin it spans 0..1 in world x and -1..0 in world z, offset by the
    // placement.
    CHECK(near(lo[0], 10.0));
    CHECK(near(lo[1], 20.0));
    CHECK(near(hi[2], 30.0));
    CHECK(near(lo[2], 29.0));
}

TEST(a_part_angle_turns_the_box_about_its_rotation_point)
{
    // The pivot is the fixed point: a box rotated about it keeps its distance
    // from it, which a rotation applied after the translation would not.
    ModelPart part;
    part.x = 0.0f;
    part.y = -1.0f;
    part.z = -1.0f;
    part.w = 16;
    part.h = 2;
    part.d = 2;
    part.pivotX = 0.0f;
    part.pivotY = 32.0f;
    part.pivotZ = 0.0f;
    part.angleZ = 3.14159265f / 2.0f;

    const Built b = build(part, Placement{});
    double lo[3], hi[3];
    bounds(b, lo, hi);

    // A bar sixteen units long on x, turned a quarter turn about z, is sixteen
    // units long on y instead -- and still hangs off the pivot at y = 2 blocks.
    CHECK(near(hi[1] - lo[1], 1.0, 0.01));
    CHECK(near(hi[0] - lo[0], 2.0 * kModelUnit, 0.01));
    CHECK(near(lo[1], 2.0, 0.01));
}

TEST(mirror_flips_the_box_across_its_own_x)
{
    ModelPart plain;
    plain.x = 2.0f;
    plain.w = 8;
    plain.h = 4;
    plain.d = 4;
    ModelPart mirrored = plain;
    mirrored.mirror = true;

    // The *shape* is unchanged -- the two x extents are swapped, not moved --
    // so this is a texture flip and not a translation. That is the property
    // that lets one page skin a left and a right paddle.
    double lo0[3], hi0[3], lo1[3], hi1[3];
    bounds(build(plain, Placement{}), lo0, hi0);
    bounds(build(mirrored, Placement{}), lo1, hi1);
    for (int a = 0; a < 3; ++a) {
        CHECK(near(lo0[a], lo1[a]));
        CHECK(near(hi0[a], hi1[a]));
    }

    // ...and the UVs are not the same, or it would be a flip that flipped
    // nothing.
    const Built a = build(plain, Placement{});
    const Built m = build(mirrored, Placement{});
    bool differs = false;
    for (int i = 0; i < kBoxVertices; ++i) {
        differs = differs || a.v[i].u != m.v[i].u || a.v[i].v != m.v[i].v
                  || a.v[i].x != m.v[i].x;
    }
    CHECK(differs);
}

TEST(every_uv_lands_inside_the_skins_own_page)
{
    // The unwrap is d, w, d, w across and d then h down, so a box needs
    // 2(w + d) by (h + d) texels of its 64 x 32 page. A box that fits must
    // produce UVs inside the page it was given -- and, just as importantly,
    // inside the *right* page of the shared sheet.
    ModelPart part;
    part.w = 20;
    part.h = 6;
    part.d = 4;
    part.texU = 0;
    part.texV = 0;

    for (int s = 0; s < texture::kEntitySkinCount; ++s) {
        const EntitySkin skin = EntitySkin(s);
        int ox = 0, oy = 0;
        texture::skinOrigin(skin, &ox, &oy);
        const Built b = build(part, Placement{}, skin);

        for (int i = 0; i < b.count; ++i) {
            const double u = double(b.v[i].u) * texture::kEntitySheetWidth
                             / double(mesh::kUvUnitsPerAtlas);
            const double v = double(b.v[i].v) * texture::kEntitySheetHeight
                             / double(mesh::kUvUnitsPerAtlas);
            // Inside the page, in sheet texels.
            CHECK(u >= double(ox) - 0.01);
            CHECK(v >= double(oy) - 0.01);
            CHECK(u <= double(ox) + 2.0 * (part.w + part.d) + 0.01);
            CHECK(v <= double(oy) + double(part.h + part.d) + 0.01);
        }
    }
}

TEST(the_six_faces_take_six_different_parts_of_the_page)
{
    // The classic cuboid unwrap: no two faces share texels. A transposed w and
    // d still fits the page and still draws -- it just draws the wrong planks
    // on the wrong side -- so what is checkable is that the six rectangles are
    // distinct and that each is the size its face is.
    ModelPart part;
    part.w = 20;
    part.h = 6;
    part.d = 4;
    const Built b = build(part, Placement{});

    // Each quad's u/v rectangle, in page texels.
    double minU[6], maxU[6], minV[6], maxV[6];
    for (int q = 0; q < 6; ++q) {
        minU[q] = minV[q] = 1e30;
        maxU[q] = maxV[q] = -1e30;
        for (int c = 0; c < 4; ++c) {
            const mesh::DetailVertex& v = b.v[q * 4 + c];
            const double u = double(v.u) * texture::kEntitySheetWidth
                             / double(mesh::kUvUnitsPerAtlas);
            const double t = double(v.v) * texture::kEntitySheetHeight
                             / double(mesh::kUvUnitsPerAtlas);
            if (u < minU[q]) minU[q] = u;
            if (u > maxU[q]) maxU[q] = u;
            if (t < minV[q]) minV[q] = t;
            if (t > maxV[q]) maxV[q] = t;
        }
    }

    // The two x faces are d wide and h tall; the two y faces are w by d; the
    // two z faces are w by h. That mapping is the unwrap, and swapping any two
    // of the three sizes breaks it.
    const double expectU[6] = {double(part.d), double(part.d), double(part.w),
                               double(part.w), double(part.w), double(part.w)};
    const double expectV[6] = {double(part.h), double(part.h), double(part.d),
                               double(part.d), double(part.h), double(part.h)};
    for (int q = 0; q < 6; ++q) {
        CHECK(near(maxU[q] - minU[q], expectU[q], 0.3));
        CHECK(near(maxV[q] - minV[q], expectV[q], 0.3));
    }

    // Distinct: no two quads have the same rectangle.
    for (int a = 0; a < 6; ++a) {
        for (int c = a + 1; c < 6; ++c) {
            CHECK(!(near(minU[a], minU[c], 0.05) && near(minV[a], minV[c], 0.05)
                    && near(maxU[a], maxU[c], 0.05) && near(maxV[a], maxV[c], 0.05)));
        }
    }
}

TEST(the_uv_inset_is_a_tenth_of_a_texel_and_pulls_inwards)
{
    // `ll` subtracts 0.0015625F from one u edge and adds it to the other, which
    // is 0.1/64. It is the original's own bleed guard and is why nothing here
    // adds the mesher's `kUvInset` on top of it -- two insets would shrink a
    // 4-texel face by a tenth of its width.
    ModelPart part;
    part.w = 16;
    part.h = 16;
    part.d = 16;
    part.texU = 0;
    part.texV = 0;
    const Built b = build(part, Placement{}, EntitySkin::Boat);

    double lowest = 1e30;
    for (int i = 0; i < b.count; ++i) {
        const double u = double(b.v[i].u) * texture::kEntitySheetWidth
                         / double(mesh::kUvUnitsPerAtlas);
        if (u < lowest) lowest = u;
    }
    // The -X face starts at page u 0, pulled in by a tenth of a texel.
    CHECK(near(lowest, 0.1, 0.03));
}

TEST(the_sheet_pages_do_not_overlap)
{
    // Four fixed slots in a 128 x 64 sheet. The offsets are compiled into every
    // model's UVs, so two that overlapped would put a minecart's iron on a
    // boat.
    int x[texture::kEntitySkinCount], y[texture::kEntitySkinCount];
    for (int i = 0; i < texture::kEntitySkinCount; ++i) {
        texture::skinOrigin(texture::EntitySkin(i), &x[i], &y[i]);
        CHECK(x[i] >= 0);
        CHECK(y[i] >= 0);
        CHECK(x[i] + texture::kSkinPageWidth <= texture::kEntitySheetWidth);
        CHECK(y[i] + texture::kSkinPageHeight <= texture::kEntitySheetHeight);
    }
    for (int a = 0; a < texture::kEntitySkinCount; ++a) {
        for (int b = a + 1; b < texture::kEntitySkinCount; ++b) {
            const bool apart = x[a] + texture::kSkinPageWidth <= x[b]
                               || x[b] + texture::kSkinPageWidth <= x[a]
                               || y[a] + texture::kSkinPageHeight <= y[b]
                               || y[b] + texture::kSkinPageHeight <= y[a];
            CHECK(apart);
        }
    }
}
