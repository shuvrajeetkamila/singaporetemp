#include "agents.h"

#include <cmath>

#include "core_math.h"

namespace {

const float kCarColours[4][3] = {
    {0.85f, 0.12f, 0.12f},  // red
    {0.12f, 0.35f, 0.85f},  // blue
    {0.92f, 0.92f, 0.92f},  // white
    {0.20f, 0.70f, 0.35f},  // green
};
const float kPersonColour[3] = {0.95f, 0.45f, 0.15f};
const float kAgentLift = 0.6f;  // agents sit slightly above the road surface

Vertex makeVertex(Vec3 p, Vec3 n, const float* col) {
    Vertex v;
    v.pos[0] = p.x; v.pos[1] = p.y; v.pos[2] = p.z;
    v.nrm[0] = n.x; v.nrm[1] = n.y; v.nrm[2] = n.z;
    v.col[0] = col[0]; v.col[1] = col[1]; v.col[2] = col[2];
    return v;
}

// Axis-aligned (in the agent's own frame) box centred at `base` + up * h/2.
// Half extents: hl along fwd, hw along side, h/2 along up. Six quads, two triangles each.
void emitBox(Vec3 base, Vec3 fwd, Vec3 side, Vec3 up, float hl, float hw, float h, const float* col,
             std::vector<Vertex>& out) {
    const Vec3 c = base + up * (h * 0.5f);
    const Vec3 ext[3] = {fwd * hl, side * hw, up * (h * 0.5f)};
    const Vec3 dir[3] = {fwd, side, up};
    const int corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    for (int k = 0; k < 3; ++k) {
        const int i = (k + 1) % 3, j = (k + 2) % 3;
        for (int s = -1; s <= 1; s += 2) {
            Vec3 q[4];
            for (int m = 0; m < 4; ++m) {
                float v[3];
                v[k] = static_cast<float>(s);
                v[i] = static_cast<float>(corners[m][0]);
                v[j] = static_cast<float>(corners[m][1]);
                q[m] = c + ext[0] * v[0] + ext[1] * v[1] + ext[2] * v[2];
            }
            Vec3 n = dir[k] * static_cast<float>(s);
            out.push_back(makeVertex(q[0], n, col));
            out.push_back(makeVertex(q[1], n, col));
            out.push_back(makeVertex(q[2], n, col));
            out.push_back(makeVertex(q[0], n, col));
            out.push_back(makeVertex(q[2], n, col));
            out.push_back(makeVertex(q[3], n, col));
        }
    }
}

}  // namespace

void buildAgentMesh(const District& d, const TrafficSim& sim, std::vector<Vertex>& out) {
    out.clear();
    const Vec3 up{0.0f, 1.0f, 0.0f};
    for (const Agent& a : sim.agents()) {
        Vec2f pos{0.0f, 0.0f}, heading{1.0f, 0.0f};
        sim.pose(a, pos, heading);
        // Scene: x east, z = -north. Heading in scene space:
        Vec3 fwd = normalize(Vec3{heading.x, 0.0f, -heading.y});
        Vec3 side = normalize(cross(fwd, up));
        Vec3 base{pos.x, terrainHeightAt(d, pos.x, pos.y) + kAgentLift, -pos.y};
        if (a.kind == 0) {
            const float* col = kCarColours[a.tint % 4];
            emitBox(base, fwd, side, up, 2.25f, 0.9f, 1.5f, col, out);
        } else {
            emitBox(base, fwd, side, up, 0.25f, 0.25f, 1.7f, kPersonColour, out);
        }
    }
}
