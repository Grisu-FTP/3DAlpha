#pragma once

// Other players, drawn with the same biped the zombie and the skeleton are
// drawn with, because in a1.1.2 it is the same model: `cb` (ModelBiped) is
// what `RenderPlayer` poses, and `render/player_model.hpp` already holds it for
// the inventory's preview and for the mob pass.
//
// **They ride the mob pass's buffer and its draw call.** A remote player is a
// handful of boxes off the entity sheet, exactly like a mob, so appending to
// the vertices the mobs just wrote costs no second bind and no second draw --
// the same arrangement the spawner miniatures already use. See
// ctr::Renderer::drawMobs.
//
// **Everyone wears the texture pack's own character**, `EntitySkin::OtherPlayer`
// rather than `EntitySkin::Player`. `0x14` carries a name, and the skin behind
// that name lived on a Mojang service that has been gone for years -- so one
// skin for everyone is the only option, and the honest one to pick is the
// pack's default rather than the skin this console's owner chose for
// themselves, which is theirs.

#include "core/mesh/vertex.hpp"
#include "core/render/particle_mesh.hpp"
#include "core/render/sign_mesh.hpp"
#include "core/texture/font.hpp"
#include "core/net/entities.hpp"
#include "core/render/box_model.hpp"
#include "core/render/player_model.hpp"
#include "core/util/types.hpp"

namespace mc::render {

// Seven boxes: the biped's six parts plus the hat layer.
inline constexpr int kRemotePlayerVerticesEach = kBipedParts * kBoxVertices;

// Where one player's body stands this frame, facing where they are looking.
Placement placeRemotePlayer(const net::RemotePlayer& player, double originX, double originY,
                            double originZ, float partial);

// Appends every tracked player, nearest first, stopping when `max` is reached
// rather than overrunning it. Returns the vertices written.
int buildRemotePlayers(const net::RemoteEntities& entities, double originX, double originY,
                       double originZ, float partial, mesh::DetailVertex* out, int max);

// **The name over each head**, which is the one thing the wire says about
// another player that a body cannot show. `renderLivingLabel` turns the label
// to face the camera, puts it half a block above the head and scales it by
// 1/60 of a block per pixel; this does the same, without the translucent plate
// behind it -- that wants a pass with no texture, and this one is drawn with
// the font.
//
// **In the sign pass's units and on the sign pass's buffer**: text in the world
// is already drawn there with the font bound, at `kSignUnitsPerBlock`, so a
// nametag costs no bind and no draw call of its own. Nothing is drawn past
// `kNameTagDistanceSq`, because a name at that range is a pixel of noise.
inline constexpr double kNameTagDistanceSq = 1024.0;
inline constexpr float kNameTagHeight = 2.3f;

int buildNameTags(const net::RemoteEntities& entities, const texture::FontImage& font,
                  const Billboard& camera, double originX, double originY, double originZ,
                  float partial, mesh::DetailVertex* out, int max);

}  // namespace mc::render
