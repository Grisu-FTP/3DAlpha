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
//
// **Until their own skin arrives.** A peer running this code sends the skin it is
// wearing (`packet::PlayerSkin`, core/net/player_skin.hpp) and a body whose skin
// is in `RemoteEntities::skins()` is drawn from that page of the sheet instead;
// the pack's character stays the fallback for a player who sent none, which a
// Java server's players never do.

#include "core/mesh/vertex.hpp"
#include "core/render/particle_mesh.hpp"
#include "core/render/sign_mesh.hpp"
#include "core/texture/font.hpp"
#include "core/net/entities.hpp"
#include "core/render/box_model.hpp"
#include "core/render/held_item.hpp"
#include "core/render/player_model.hpp"
#include "core/util/types.hpp"

namespace mc::render {

// Seven boxes: the biped's six parts plus the hat layer.
inline constexpr int kRemotePlayerVerticesEach = kBipedParts * kBoxVertices;

// **Where a frame's players go.** A body is three draws -- the biped off the
// entity sheet, its armour off the armour sheet, and what it carries off the
// terrain or the items sheet -- so each pass has a buffer and a count, and
// `body` is the only one a caller must give. `held` is indexed by sheet: 0 for
// terrain, 1 for items.
struct PlayerPasses {
    mesh::DetailVertex* body = nullptr;
    int bodyMax = 0;
    int bodyCount = 0;
    mesh::DetailVertex* armour = nullptr;
    int armourMax = 0;
    int armourCount = 0;
    mesh::DetailVertex* held[2] = {nullptr, nullptr};
    int heldMax[2] = {0, 0};
    int heldCount[2] = {0, 0};
};

// Where one player's body stands this frame, facing where they are looking.
Placement placeRemotePlayer(const net::RemotePlayer& player, double originX, double originY,
                            double originZ, float partial);

// Appends every tracked player, nearest first, stopping when `max` is reached
// rather than overrunning it. Returns the vertices written.
int buildRemotePlayers(const net::RemoteEntities& entities, double originX, double originY,
                       double originZ, float partial, mesh::DetailVertex* out, int max);
// The same, into all three passes. Returns the body vertices written.
int buildRemotePlayers(const net::RemoteEntities& entities, double originX, double originY,
                       double originZ, float partial, PlayerPasses* passes);

// **This console's own player**, drawn in the third-person views with the skin
// they chose (`EntitySkin::Player`) rather than the visitors' character.
// `player` is a stand-in the caller fills from its own body -- feet, yaw,
// pitch, walk and swing -- since the biped only needs those. Returns the
// vertices written, `kRemotePlayerVerticesEach` at most.
int buildSelfPlayer(const net::RemotePlayer& player, double originX, double originY,
                    double originZ, float partial, PlayerPasses* passes);

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
