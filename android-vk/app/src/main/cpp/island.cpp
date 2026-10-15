#include "island.h"

#include <cstdio>
#include <string>

namespace {

bool inside(const Rect& r, float x, float z) {
    return x >= r.x0 && x <= r.x1 && z >= r.z0 && z <= r.z1;
}

// True if the whole cell [x0,x1]x[z0,z1] lies inside the rectangle.
bool containsCell(const Rect& r, float x0, float z0, float x1, float z1) {
    return x0 >= r.x0 && x1 <= r.x1 && z0 >= r.z0 && z1 <= r.z1;
}

Vertex vertexAt(float x, float north, float h, const float* rgb) {
    Vertex v;
    v.pos[0] = x;
    v.pos[1] = h;
    v.pos[2] = -north;  // scene z = -north
    for (int k = 0; k < 3; ++k) {
        float c = rgb[k] < 0.0f ? 0.0f : (rgb[k] > 1.0f ? 1.0f : rgb[k]);
        v.rgba[k] = static_cast<uint8_t>(c * 255.0f + 0.5f);
    }
    v.rgba[3] = 255;
    return v;
}

}  // namespace

bool parseLayout(const std::vector<uint8_t>& text, IslandLayout& out) {
    std::string s(text.begin(), text.end());
    float dx = 0, dn = 0, sx = 0, sz = 0;
    if (std::sscanf(s.c_str(), "%f %f %f %f", &dx, &dn, &sx, &sz) != 4) return false;
    out.dx = dx;
    out.dn = dn;
    out.sx = sx;
    out.sz = sz;
    return sx > 0.0f && sz > 0.0f;
}

void buildIslandMesh(const District& island, const Rect& exclude, MeshBlob& out) {
    out.vertices.clear();
    out.indices.clear();
    out.offset[0] = out.offset[1] = out.offset[2] = 0.0f;

    // Terrain: one vertex per grid point, two triangles per kept cell.
    const uint32_t tw = island.tw, th = island.th;
    const float cell = island.cell;
    if (tw >= 2 && th >= 2) {
        std::vector<uint32_t> gridIndex(static_cast<size_t>(tw) * th);
        out.vertices.reserve(gridIndex.size() + static_cast<size_t>(tw) * th * 2);
        for (uint32_t j = 0; j < th; ++j) {
            for (uint32_t i = 0; i < tw; ++i) {
                const float h = island.heights[static_cast<size_t>(j) * tw + i];
                float rgb[3];
                terrainColour(h, rgb);
                gridIndex[static_cast<size_t>(j) * tw + i] = static_cast<uint32_t>(out.vertices.size());
                out.vertices.push_back(vertexAt(i * cell, j * cell, h, rgb));
            }
        }
        for (uint32_t j = 0; j + 1 < th; ++j) {
            for (uint32_t i = 0; i + 1 < tw; ++i) {
                // Removed only when fully inside the pilot: a cell that straddles the pilot edge is kept,
                // so the island surface never leaves a gap next to the pilot.
                if (containsCell(exclude, i * cell, j * cell, (i + 1) * cell, (j + 1) * cell)) continue;
                const uint32_t a = gridIndex[static_cast<size_t>(j) * tw + i];
                const uint32_t b = gridIndex[static_cast<size_t>(j) * tw + i + 1];
                const uint32_t c = gridIndex[static_cast<size_t>(j + 1) * tw + i];
                const uint32_t d = gridIndex[static_cast<size_t>(j + 1) * tw + i + 1];
                out.indices.insert(out.indices.end(), {a, c, b, b, c, d});
            }
        }
    }

    // Sea plane: a large flat surface 60 m below sea level, under the island and beyond its edges,
    // so the island sits in the sea instead of on a floating tile.
    {
        constexpr float kApron = 2000000.0f, kSeaY = -60.0f;
        float seaRgb[3];
        terrainColour(0.0f, seaRgb);
        const float x0 = -kApron, x1 = island.sizeX + kApron;
        const float z0 = -island.sizeZ - kApron, z1 = kApron;
        const uint32_t s0 = static_cast<uint32_t>(out.vertices.size());
        Vertex corners[4];
        corners[0] = vertexAt(x0, -z0, kSeaY, seaRgb);  // placed via north = -z in vertexAt
        corners[1] = vertexAt(x1, -z0, kSeaY, seaRgb);
        corners[2] = vertexAt(x1, -z1, kSeaY, seaRgb);
        corners[3] = vertexAt(x0, -z1, kSeaY, seaRgb);
        for (const Vertex& v : corners) out.vertices.push_back(v);
        out.indices.insert(out.indices.end(), {s0, s0 + 1, s0 + 2, s0, s0 + 2, s0 + 3});
    }

    // Buildings: roof (triangulated), walls as quads. Walls get their own vertices so they
    // can have a different colour from the roof.
    const float roofRgb[3] = {0.86f, 0.84f, 0.80f};
    const float wallRgb[3] = {0.78f, 0.75f, 0.68f};
    std::vector<uint32_t> tris;
    for (const Building& b : island.buildings) {
        const size_t n = b.ring.size();
        if (n < 3) continue;
        double sx = 0.0, sz = 0.0;
        for (const Vec2f& p : b.ring) {
            sx += p.x;
            sz += p.y;
        }
        const float cx = static_cast<float>(sx / n), cz = static_cast<float>(sz / n);
        if (inside(exclude, cx, cz)) continue;

        const float top = b.base + (b.height > 1.0f ? b.height : 1.0f);
        const uint32_t roofBase = static_cast<uint32_t>(out.vertices.size());
        for (const Vec2f& p : b.ring) out.vertices.push_back(vertexAt(p.x, p.y, top, roofRgb));
        const uint32_t wallTopBase = static_cast<uint32_t>(out.vertices.size());
        for (const Vec2f& p : b.ring) out.vertices.push_back(vertexAt(p.x, p.y, top, wallRgb));
        const uint32_t wallBottomBase = static_cast<uint32_t>(out.vertices.size());
        for (const Vec2f& p : b.ring) out.vertices.push_back(vertexAt(p.x, p.y, b.base, wallRgb));

        tris.clear();
        triangulatePolygon(b.ring, tris);
        for (uint32_t t : tris) out.indices.push_back(roofBase + t);

        for (size_t k = 0; k < n; ++k) {
            const uint32_t c = static_cast<uint32_t>((k + 1) % n);
            const uint32_t a = static_cast<uint32_t>(k);
            const uint32_t bA = wallBottomBase + a, bC = wallBottomBase + c;
            const uint32_t tA = wallTopBase + a, tC = wallTopBase + c;
            out.indices.insert(out.indices.end(), {bA, bC, tC, bA, tC, tA});
        }
    }
}
