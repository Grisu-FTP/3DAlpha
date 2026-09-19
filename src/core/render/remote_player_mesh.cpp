// Other players as bipeds. See remote_player_mesh.hpp.

#include "core/render/remote_player_mesh.hpp"

#include "core/util/math_helper.hpp"

#include <cmath>
#include <string_view>

namespace mc::render {

namespace {

constexpr float kPi = 3.14159265358979323846f;

float interpolate(float previous, float current, float partial)
{
    // The short way round: a body whose yaw crossed north between two ticks
    // must not spin the long way through the frame between them.
    float delta = current - previous;
    while (delta < -180.0f) delta += 360.0f;
    while (delta >= 180.0f) delta -= 360.0f;
    return previous + delta * partial;
}

}  // namespace

Placement placeRemotePlayer(const net::RemotePlayer& player, double originX, double originY,
                            double originZ, float partial)
{
    // `180 - yaw`, the same turn the mob pass applies -- the model faces the
    // other way from the angle the wire carries. **`placeModel`, not
    // `placeAt`**: a body built out of `ModelPart`s hangs upside down from a
    // point above its feet until the renderer's flip and lift are applied.
    //
    // **Lower by an eighth when crouched**, as b1.2's `RenderPlayer` draws
    // one -- the crouch pose alone leaves the feet three pixels up -- and
    // tipped over while dying, as `dn` draws any living thing.
    const float yaw = interpolate(player.prevYaw, player.yaw, partial);
    const double drop = player.sneaking ? kSneakModelDrop : 0.0;
    return placeModel(player.renderX(partial) - originX,
                      player.renderY(partial) - originY - drop,
                      player.renderZ(partial) - originZ, (180.0f - yaw) * kPi / 180.0f,
                      deathFall(player.deathTime, partial));
}

int buildRemotePlayers(const net::RemoteEntities& entities, double originX, double originY,
                       double originZ, float partial, mesh::DetailVertex* out, int max)
{
    if (out == nullptr || max < kRemotePlayerVerticesEach) {
        return 0;
    }

    // The detail vertex is a short of sixteenths, so a body far enough away to
    // overflow it is drawn nowhere rather than somewhere wrong. The same bound
    // the mob pass keeps.
    constexpr double kUnits = double(mesh::kDetailUnitsPerBlock);
    constexpr double kLimit = 32000.0 / kUnits;

    int written = 0;
    for (int index = 0; index < entities.playerCount(); ++index) {
        const net::RemotePlayer& player = entities.player(index);
        if (!player.used) {
            continue;
        }
        if (written + kRemotePlayerVerticesEach > max) {
            break;
        }

        const double rx = player.renderX(partial) - originX;
        const double ry = player.renderY(partial) - originY;
        const double rz = player.renderZ(partial) - originZ;
        if (rx < -kLimit || rx > kLimit || ry < -kLimit || ry > kLimit || rz < -kLimit
            || rz > kLimit) {
            continue;
        }

        ModelPart parts[kBipedParts];
        bipedModel(parts);

        // The head turns against the body: the body is already turned by the
        // placement, so what is left for the head is the pitch alone. One yaw
        // is all protocol 2 carries for a player, so there is no look to
        // separate from the stance.
        const float pitch = interpolate(player.prevPitch, player.pitch, partial);
        const float amount =
            player.prevLimbAmount + (player.limbAmount - player.prevLimbAmount) * partial;
        posePlayer(parts, player.limbSwing, amount, float(player.ticksExisted) + partial, 0.0f,
                   pitch, player.swingProgress(partial), player.sneaking);

        const Placement place = placeRemotePlayer(player, originX, originY, originZ, partial);
        const int before = written;
        for (int part = 0; part < kBipedParts; ++part) {
            // **Not `EntitySkin::Player`**: that page is whatever skin this
            // console's owner picked for themselves, and other people are not
            // them. See the note on `EntitySkin::OtherPlayer`.
            written += buildBox(parts[part], place, texture::EntitySkin::OtherPlayer,
                                player.light, out + written, max - written);
        }

        // A dying body is red while it falls, as `dn` draws any living thing.
        if (player.deathTime > 0) {
            for (int v = before; v < written; ++v) {
                out[v].g = kHurtChannel;
                out[v].b = kHurtChannel;
            }
        }
    }
    return written;
}

int buildNameTags(const net::RemoteEntities& entities, const texture::FontImage& font,
                  const Billboard& camera, double originX, double originY, double originZ,
                  float partial, mesh::DetailVertex* out, int max)
{
    if (out == nullptr || font.empty() || max < 4) {
        return 0;
    }

    constexpr int kFontCell = texture::kFontCellPixels;
    constexpr int kFontCells = texture::kFontGlyphsPerEdge;
    // `0.016666668F * 1.6F` in `renderLivingLabel`, and then into the sign
    // pass's units rather than blocks.
    constexpr float kGlyph = 0.016666668f * 1.6f * float(kSignUnitsPerBlock);
    // **The player's own colour**, which is `playerColour` and therefore the
    // same colour their arrow is on everybody's map -- so a name and a marker
    // can be matched up without reading either. a1.1.2 has one colour for
    // every name because a1.1.2's names came from a server that had no idea
    // who else was in the room.
    //
    // Lit as if in daylight whatever it is: a1.1.2 draws the label with
    // lighting off, so a name in a cave is as readable as one in the open.
    constexpr u8 kFullLight = 0xF0;

    const auto toUnits = [](double blocks) {
        const double units = blocks * double(kSignUnitsPerBlock);
        return i16(units < -32000.0 ? -32000.0 : (units > 32000.0 ? 32000.0 : units));
    };

    int written = 0;
    for (int index = 0; index < entities.playerCount(); ++index) {
        const net::RemotePlayer& player = entities.player(index);
        if (!player.used || player.name[0] == '\0') {
            continue;
        }

        const double bx = player.renderX(partial) - originX;
        const double by = player.renderY(partial) - originY + double(kNameTagHeight);
        const double bz = player.renderZ(partial) - originZ;
        if (bx * bx + (by - double(kNameTagHeight)) * (by - double(kNameTagHeight)) + bz * bz
            > kNameTagDistanceSq) {
            continue;
        }

        u8 inkR = 255;
        u8 inkG = 255;
        u8 inkB = 255;
        net::playerColour(player.id, &inkR, &inkG, &inkB);

        const std::string_view name(player.name);
        const int width = texture::textWidth(font.widths, name);
        float penX = float(-width) * 0.5f;

        usize pos = 0;
        while (pos < name.size()) {
            const u32 code = texture::nextCodepoint(name, &pos);
            const int glyph = texture::fontGlyph(code);
            if (glyph < 0) {
                continue;
            }
            if (written + 4 > max) {
                return written;
            }

            const float cellX = float(glyph % kFontCells) * float(kFontCell);
            const float cellY = float(glyph / kFontCells) * float(kFontCell);

            // The quad in the camera's own plane, so the name faces whoever is
            // reading it however they stand. Y is up the screen, so the cell's
            // top is the larger offset.
            const float x0 = penX * kGlyph;
            const float x1 = (penX + float(kFontCell)) * kGlyph;
            const float y0 = -float(kFontCell) * kGlyph * 0.5f;
            const float y1 = float(kFontCell) * kGlyph * 0.5f;

            const float corner[4][2] = {{x0, y1}, {x1, y1}, {x1, y0}, {x0, y0}};
            const float uv[4][2] = {{cellX, cellY},
                                    {cellX + float(kFontCell), cellY},
                                    {cellX + float(kFontCell), cellY + float(kFontCell)},
                                    {cellX, cellY + float(kFontCell)}};

            for (int c = 0; c < 4; ++c) {
                // **Against the billboard's horizontal, not along it.** The
                // basis is `EffectRenderer`'s -- `(cos yaw, 0, sin yaw)` -- and
                // that vector points to the camera's *left*: at yaw 0 the
                // camera looks along +Z, whose right hand is -X, and the basis
                // gives +X. `renderParticle` knows it and pairs its low u with
                // the `-right` corners; a particle cannot tell either way, and
                // text can. Read left to right it made every name a mirror of
                // itself, which is what "the nametags are flipped" was.
                const float mx = -corner[c][0];
                const float my = corner[c][1];
                mesh::DetailVertex& v = out[written];
                v.x = toUnits(bx + double(mx * camera.rightX + my * camera.upX)
                                       / double(kSignUnitsPerBlock));
                v.y = toUnits(by + double(mx * camera.rightY + my * camera.upY)
                                       / double(kSignUnitsPerBlock));
                v.z = toUnits(bz + double(mx * camera.rightZ + my * camera.upZ)
                                       / double(kSignUnitsPerBlock));
                v.face = 0;
                v.u = i16(uv[c][0] * float(mesh::kUvUnitsPerAtlas) / float(texture::kFontEdge));
                v.v = i16(uv[c][1] * float(mesh::kUvUnitsPerAtlas) / float(texture::kFontEdge));
                v.r = inkR;
                v.g = inkG;
                v.b = inkB;
                v.light = kFullLight;
                ++written;
            }
            penX += float(font.widths[glyph]);
        }
    }
    return written;
}

}  // namespace mc::render
