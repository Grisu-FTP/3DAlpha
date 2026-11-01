// The inventory's player as a software rasteriser. See player_doll.hpp.

#include "core/gui/player_doll.hpp"

#include "core/mesh/vertex.hpp"
#include "core/render/box_model.hpp"
#include "core/render/player_model.hpp"
#include "core/texture/entity_skins.hpp"

#include <algorithm>
#include <cmath>

namespace mc::gui {

namespace {

constexpr int kMaxVertices = render::kPlayerPreviewVertices + render::kArmourPreviewVertices;
constexpr int kMaxQuads = kMaxVertices / 4;
constexpr float kDegrees = 3.14159265358979f / 180.0f;

// `RenderHelper.enableStandardItemLighting`: 0.4 ambient and two 0.6 diffuse lights,
// in eye space (x right, y up, z toward the viewer). The two directions, normalised.
constexpr float kAmbient = 0.4f;
constexpr float kDiffuse = 0.6f;
constexpr float kLight0[3] = {0.2f / 1.2288f, 1.0f / 1.2288f, -0.7f / 1.2288f};
constexpr float kLight1[3] = {-0.2f / 1.1224f, 1.0f / 1.1224f, 0.5f / 1.1224f};

struct Quad {
    float x[4], y[4];  // pixels on the surface
    float u[4], v[4];  // texels in the sheet
    float depth;       // larger is nearer the viewer
    float shade;
    const u8* sheet;
    int edge;
};

float litBy(const float* n, const float* light)
{
    const float d = n[0] * light[0] + n[1] * light[1] + n[2] * light[2];
    return d > 0.0f ? d : 0.0f;
}

// One parallelogram, by its bounding box: orthographic, so a box's face stays affine
// and two edges from one corner are all the mapping there is.
void fillQuad(const Surface& surface, const Quad& q, int loX, int loY, int hiX, int hiY)
{
    const float ex = q.x[1] - q.x[0];
    const float ey = q.y[1] - q.y[0];
    const float fx = q.x[3] - q.x[0];
    const float fy = q.y[3] - q.y[0];
    const float det = ex * fy - ey * fx;
    if (std::fabs(det) < 0.001f) {
        return;
    }
    float minX = q.x[0], maxX = q.x[0], minY = q.y[0], maxY = q.y[0];
    for (int i = 1; i < 4; ++i) {
        minX = std::min(minX, q.x[i]);
        maxX = std::max(maxX, q.x[i]);
        minY = std::min(minY, q.y[i]);
        maxY = std::max(maxY, q.y[i]);
    }
    const int x0 = std::max(loX, int(std::floor(minX)));
    const int x1 = std::min(hiX, int(std::ceil(maxX)));
    const int y0 = std::max(loY, int(std::floor(minY)));
    const int y1 = std::min(hiY, int(std::ceil(maxY)));
    const int shade = int(q.shade * 256.0f);
    for (int py = y0; py < y1; ++py) {
        for (int px = x0; px < x1; ++px) {
            const float dx = float(px) + 0.5f - q.x[0];
            const float dy = float(py) + 0.5f - q.y[0];
            const float s = (dx * fy - dy * fx) / det;
            const float t = (ex * dy - ey * dx) / det;
            if (s < 0.0f || s >= 1.0f || t < 0.0f || t >= 1.0f) {
                continue;
            }
            const int tu = int(q.u[0] + s * (q.u[1] - q.u[0]) + t * (q.u[3] - q.u[0]));
            const int tv = int(q.v[0] + s * (q.v[1] - q.v[0]) + t * (q.v[3] - q.v[0]));
            if (tu < 0 || tv < 0 || tu >= q.edge || tv >= q.edge) {
                continue;
            }
            const u8* texel = q.sheet + (usize(tv) * usize(q.edge) + usize(tu)) * 4;
            // a1.1.2's `glAlphaFunc(GL_GREATER, 0.1)`: a hat's empty cells and an
            // armour page's holes are holes.
            if (texel[3] < 26) {
                continue;
            }
            *surface.at(px, py) = rgb565((int(texel[0]) * shade) >> 8, (int(texel[1]) * shade) >> 8,
                                         (int(texel[2]) * shade) >> 8);
        }
    }
}

}  // namespace

void drawPlayerDoll(const Surface& surface, int footX, int footY, float pixelsPerBlock,
                    int clipX, int clipY, int clipW, int clipH, const DollSheets& sheets,
                    const DollPose& pose)
{
    if (sheets.entity == nullptr || surface.pixels == nullptr) {
        return;
    }

    // Feet at y = 0 and the face toward the viewer: the model's own -z is its front
    // and its +y is down.
    render::Placement place;
    place.x = 0.0;
    place.y = 24.0 * render::kModelUnit;
    place.z = 0.0;
    place.ax[0] = render::kModelUnit;
    place.ay[1] = -render::kModelUnit;
    place.az[2] = -render::kModelUnit;

    render::ModelPart parts[render::kBipedParts];
    render::bipedModel(parts);
    render::posePlayer(parts, 0.0f, 0.0f, 0.0f, 0.0f, pose.headPitch);

    static mesh::DetailVertex vertices[kMaxVertices];
    int pageX = 0;
    int pageY = 0;
    texture::skinOrigin(texture::EntitySkin::Player, &pageX, &pageY);
    const int bodyCount =
        render::buildPlayerPreview(parts, place, pageX, pageY, vertices, kMaxVertices);
    int armourCount = 0;
    if (sheets.armour != nullptr) {
        armourCount = render::buildPlayerArmour(parts, pose.armour, place, vertices + bodyCount,
                                                kMaxVertices - bodyCount);
    }
    const int count = bodyCount + armourCount;

    const float cy = std::cos(pose.bodyYaw * kDegrees);
    const float sy = std::sin(pose.bodyYaw * kDegrees);
    const float toBlocks = 1.0f / float(mesh::kDetailUnitsPerBlock);
    constexpr float kTexelsPerUnit = float(texture::kEntitySheetWidth) / float(mesh::kUvUnitsPerAtlas);

    static Quad quads[kMaxQuads];
    int quadCount = 0;
    for (int part = 0; part + render::kBoxVertices <= count; part += render::kBoxVertices) {
        const mesh::DetailVertex* p = vertices + part;
        const bool worn = part >= bodyCount;
        // Turned into eye space. A box's outward side is the one facing away from its
        // own centre, which spares knowing the winding.
        float ex[render::kBoxVertices], ey[render::kBoxVertices], ez[render::kBoxVertices];
        float centre[3] = {0.0f, 0.0f, 0.0f};
        for (int i = 0; i < render::kBoxVertices; ++i) {
            const float mx = float(p[i].x) * toBlocks;
            const float my = float(p[i].y) * toBlocks;
            const float mz = float(p[i].z) * toBlocks;
            ex[i] = mx * cy + mz * sy;
            ey[i] = my;
            ez[i] = -mx * sy + mz * cy;
            centre[0] += ex[i];
            centre[1] += ey[i];
            centre[2] += ez[i];
        }
        for (float& c : centre) {
            c /= float(render::kBoxVertices);
        }
        for (int q = 0; q < render::kBoxVertices; q += 4) {
            const float e1[3] = {ex[q + 1] - ex[q], ey[q + 1] - ey[q], ez[q + 1] - ez[q]};
            const float e2[3] = {ex[q + 3] - ex[q], ey[q + 3] - ey[q], ez[q + 3] - ez[q]};
            float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                          e1[0] * e2[1] - e1[1] * e2[0]};
            const float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (length <= 0.0f) {
                continue;
            }
            for (float& c : n) {
                c /= length;
            }
            const float outward = n[0] * (ex[q] - centre[0]) + n[1] * (ey[q] - centre[1])
                                  + n[2] * (ez[q] - centre[2]);
            if (outward < 0.0f) {
                for (float& c : n) {
                    c = -c;
                }
            }
            // Facing away from the viewer: the box's nearer faces will cover it.
            if (n[2] <= 0.0f) {
                continue;
            }
            Quad& out = quads[quadCount++];
            out.depth = 0.0f;
            for (int c = 0; c < 4; ++c) {
                out.x[c] = float(footX) + ex[q + c] * pixelsPerBlock;
                out.y[c] = float(footY) - ey[q + c] * pixelsPerBlock;
                out.u[c] = float(p[q + c].u) * kTexelsPerUnit;
                out.v[c] = float(p[q + c].v) * kTexelsPerUnit;
                out.depth += ez[q + c] * 0.25f;
            }
            out.shade = std::min(1.0f, kAmbient + kDiffuse * litBy(n, kLight0)
                                           + kDiffuse * litBy(n, kLight1));
            out.sheet = worn ? sheets.armour : sheets.entity;
            out.edge = worn ? texture::kArmourSheetEdge : texture::kEntitySheetWidth;
        }
    }

    // Insertion sort, far first: at most ninety-odd faces, nearly in order already.
    static int order[kMaxQuads];
    for (int i = 0; i < quadCount; ++i) {
        int at = i;
        while (at > 0 && quads[order[at - 1]].depth > quads[i].depth) {
            order[at] = order[at - 1];
            --at;
        }
        order[at] = i;
    }

    const int loX = std::max(0, clipX);
    const int loY = std::max(0, clipY);
    const int hiX = std::min(surface.width, clipX + clipW);
    const int hiY = std::min(surface.height, clipY + clipH);
    for (int i = 0; i < quadCount; ++i) {
        fillQuad(surface, quads[order[i]], loX, loY, hiX, hiY);
    }
}

}  // namespace mc::gui
