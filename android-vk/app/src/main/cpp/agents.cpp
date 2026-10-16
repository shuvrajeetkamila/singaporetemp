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

Vertex makeVertex(Vec3 p, const float* col, Vec3 n) {
    Vertex v;
    v.pos[0] = p.x;
    v.pos[1] = p.y;
    v.pos[2] = p.z;
    for (int k = 0; k < 3; ++k) {
        float c = col[k] < 0.0f ? 0.0f : (col[k] > 1.0f ? 1.0f : col[k]);
        v.rgba[k] = static_cast<uint8_t>(c * 255.0f + 0.5f);
    }
    v.rgba[3] = 255;
    setNormal(v, n.x, n.y, n.z);
    return v;
}

// Box centred at `base` + up * h/2 in the agent's own frame: half extents hl (fwd), hw (side),
// h/2 (up). Six quads, two triangles each. Positions are in the pilot frame.
void emitBox(Vec3 base, Vec3 fwd, Vec3 side, Vec3 up, float hl, float hw, float h, const float* col,
             std::vector<Vertex>& out) {
    const Vec3 c = base + up * (h * 0.5f);
    const Vec3 ext[3] = {fwd * hl, side * hw, up * (h * 0.5f)};
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
            const Vec3 n = cross(q[1] - q[0], q[2] - q[0]);  // flat face
            out.push_back(makeVertex(q[0], col, n));
            out.push_back(makeVertex(q[1], col, n));
            out.push_back(makeVertex(q[2], col, n));
            out.push_back(makeVertex(q[0], col, n));
            out.push_back(makeVertex(q[2], col, n));
            out.push_back(makeVertex(q[3], col, n));
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
            emitBox(base, fwd, side, up, 2.25f, 0.9f, 1.5f, kCarColours[a.tint % 4], out);
        } else {
            emitBox(base, fwd, side, up, 0.25f, 0.25f, 1.7f, kPersonColour, out);
        }
    }
}
