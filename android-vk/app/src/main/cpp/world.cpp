#include "world.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core_math.h"

namespace {

// Bounds-checked little-endian reader. Any read past the end sets ok = false.
struct Reader {
    const uint8_t* p;
    size_t n;
    size_t off = 0;
    bool ok = true;

    bool need(size_t k) {
        if (!ok || k > n - off) {  // n - off never underflows: off <= n is maintained
            ok = false;
            return false;
        }
        return true;
    }
    uint32_t u32() {
        if (!need(4)) return 0;
        uint32_t v;
        std::memcpy(&v, p + off, 4);
        off += 4;
        return v;
    }
    uint16_t u16() {
        if (!need(2)) return 0;
        uint16_t v;
        std::memcpy(&v, p + off, 2);
        off += 2;
        return v;
    }
    uint8_t u8() {
        if (!need(1)) return 0;
        return p[off++];
    }
    float f32() {
        if (!need(4)) return 0.0f;
        float v;
        std::memcpy(&v, p + off, 4);
        off += 4;
        return v;
    }
    bool magic(const char* m) {
        if (!need(4)) return false;
        bool same = std::memcmp(p + off, m, 4) == 0;
        off += 4;
        return same;
    }
};

const float kRoadWidth[7] = {24.0f, 20.0f, 16.0f, 14.0f, 12.0f, 9.0f, 2.5f};
const float kDefaultSpeed[7] = {22.2f, 19.4f, 16.7f, 13.9f, 11.1f, 8.3f, 1.3f};

void heightColor(float h, float* c) {
    if (h < 0.5f) { c[0] = 0.14f; c[1] = 0.40f; c[2] = 0.62f; return; }   // sea / water
    if (h < 5.0f) { c[0] = 0.62f; c[1] = 0.74f; c[2] = 0.46f; return; }   // coastal lowland
    if (h < 30.0f) { c[0] = 0.45f; c[1] = 0.66f; c[2] = 0.36f; return; }  // lowland
    if (h < 80.0f) { c[0] = 0.56f; c[1] = 0.52f; c[2] = 0.36f; return; }  // hills
    c[0] = 0.60f; c[1] = 0.52f; c[2] = 0.46f;                             // higher ground
}

uint8_t toByte(float v) {
    float c = std::min(std::max(v, 0.0f), 1.0f);
    return static_cast<uint8_t>(c * 255.0f + 0.5f);
}

Vertex makeVertex(Vec3 p, const float* col) {
    Vertex v;
    v.pos[0] = p.x;
    v.pos[1] = p.y;
    v.pos[2] = p.z;
    v.rgba[0] = toByte(col[0]);
    v.rgba[1] = toByte(col[1]);
    v.rgba[2] = toByte(col[2]);
    v.rgba[3] = 255;
    setNormal(v, 0.0f, 1.0f, 0.0f);
    return v;
}

// Emit one triangle (non-indexed). Each vertex gets the triangle's face normal.
void pushTriangle(std::vector<Vertex>& out, Vec3 a, Vec3 b, Vec3 c, const float* col) {
    const Vec3 n = cross(b - a, c - a);
    for (const Vec3 p : {a, b, c}) {
        Vertex v = makeVertex(p, col);
        setNormal(v, n.x, n.y, n.z);
        out.push_back(v);
    }
}

// Scene mapping from district metres: x = east, y = height, z = -north.
Vec3 scenePoint(float x, float north, float h) { return {x, h, -north}; }

float cross2(Vec2f o, Vec2f a, Vec2f b) {
    return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
}

bool pointInTriangle(Vec2f p, Vec2f a, Vec2f b, Vec2f c) {
    float d1 = cross2(a, b, p), d2 = cross2(b, c, p), d3 = cross2(c, a, p);
    bool hasNeg = d1 < 0 || d2 < 0 || d3 < 0;
    bool hasPos = d1 > 0 || d2 > 0 || d3 > 0;
    return !(hasNeg && hasPos);
}

// Ear-clipping triangulation of a simple polygon. Falls back to a fan for anything it can't clip.
void triangulateRing(const std::vector<Vec2f>& ring, std::vector<uint32_t>& tris) {
    size_t n = ring.size();
    if (n < 3) return;
    std::vector<uint32_t> idx(n);
    for (size_t i = 0; i < n; ++i) idx[i] = static_cast<uint32_t>(i);

    // Make the polygon counter-clockwise for the ear test.
    double area2 = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const Vec2f& p = ring[i];
        const Vec2f& q = ring[(i + 1) % n];
        area2 += static_cast<double>(p.x) * q.y - static_cast<double>(q.x) * p.y;
    }
    if (area2 < 0) std::reverse(idx.begin(), idx.end());

    int guard = 0;
    while (idx.size() > 3 && guard++ < 100000) {
        bool clipped = false;
        size_t m = idx.size();
        for (size_t i = 0; i < m; ++i) {
            uint32_t ia = idx[(i + m - 1) % m], ib = idx[i], ic = idx[(i + 1) % m];
            Vec2f a = ring[ia], b = ring[ib], c = ring[ic];
            if (cross2(a, b, c) <= 1e-9f) continue;  // reflex or degenerate corner
            bool contains = false;
            for (size_t k = 0; k < m && !contains; ++k) {
                uint32_t ik = idx[k];
                if (ik == ia || ik == ib || ik == ic) continue;
                if (pointInTriangle(ring[ik], a, b, c)) contains = true;
            }
            if (contains) continue;
            tris.push_back(ia);
            tris.push_back(ib);
            tris.push_back(ic);
            idx.erase(idx.begin() + static_cast<long>(i));
            clipped = true;
            break;
        }
        if (!clipped) break;
    }
    if (idx.size() >= 3) {  // fallback: fan the remainder
        for (size_t k = 1; k + 1 < idx.size(); ++k) {
            tris.push_back(idx[0]);
            tris.push_back(idx[k]);
            tris.push_back(idx[k + 1]);
        }
    }
}

}  // namespace

bool parseTerrain(const std::vector<uint8_t>& bytes, District& d, std::string& err) {
    Reader r{bytes.data(), bytes.size()};
    if (!r.magic("SGT1")) { err = "terrain: bad magic"; return false; }
    uint32_t w = r.u32(), h = r.u32();
    float cell = r.f32();
    if (!r.ok || w < 2 || h < 2 || w > 20000 || h > 20000 || !(cell > 0.0f)) {
        err = "terrain: bad header";
        return false;
    }
    size_t count = static_cast<size_t>(w) * h;
    if (bytes.size() - r.off != count * 4) { err = "terrain: size mismatch"; return false; }
    d.tw = w;
    d.th = h;
    d.cell = cell;
    d.heights.resize(count);
    for (size_t i = 0; i < count; ++i) {
        float v = r.f32();
        d.heights[i] = std::isfinite(v) ? v : 0.0f;
    }
    d.sizeX = (w - 1) * cell;
    d.sizeZ = (h - 1) * cell;
    return r.ok;
}

bool parseBuildings(const std::vector<uint8_t>& bytes, District& d, std::string& err) {
    Reader r{bytes.data(), bytes.size()};
    if (!r.magic("SGB1")) { err = "buildings: bad magic"; return false; }
    uint32_t count = r.u32();
    if (!r.ok) { err = "buildings: truncated header"; return false; }
    d.buildings.clear();
    d.buildings.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        Building b;
        b.base = r.f32();
        b.height = r.f32();
        uint16_t n = r.u16();
        if (!r.ok || n < 3) { err = "buildings: bad record"; return false; }
        b.ring.resize(n);
        for (uint16_t k = 0; k < n; ++k) {
            b.ring[k].x = r.f32();
            b.ring[k].y = r.f32();
        }
        if (!r.ok) { err = "buildings: truncated record"; return false; }
        if (!std::isfinite(b.base) || !std::isfinite(b.height)) { err = "buildings: non-finite value"; return false; }
        d.buildings.push_back(std::move(b));
    }
    return true;
}

bool parseRoads(const std::vector<uint8_t>& bytes, District& d, std::string& err) {
    Reader r{bytes.data(), bytes.size()};
    if (!r.magic("SGR1")) { err = "roads: bad magic"; return false; }
    uint32_t nNodes = r.u32(), nEdges = r.u32();
    if (!r.ok || nNodes > 5000000 || nEdges > 5000000) { err = "roads: bad header"; return false; }
    if (bytes.size() - r.off < static_cast<size_t>(nNodes) * 8) { err = "roads: truncated nodes"; return false; }
    RoadGraph& g = d.roads;
    g.nodes.resize(nNodes);
    for (uint32_t i = 0; i < nNodes; ++i) {
        g.nodes[i].x = r.f32();
        g.nodes[i].y = r.f32();
    }
    g.adj[0].assign(nNodes, {});
    g.adj[1].assign(nNodes, {});
    g.edges.clear();
    g.edges.reserve(nEdges);
    for (uint32_t e = 0; e < nEdges; ++e) {
        RoadEdge edge;
        edge.a = r.u32();
        edge.b = r.u32();
        edge.mode = r.u8();
        edge.cls = r.u8();
        uint16_t npts = r.u16();
        edge.speed = r.f32();
        if (!r.ok || edge.a >= nNodes || edge.b >= nNodes || edge.mode > 1 || edge.cls > 6 || npts < 2) {
            err = "roads: bad edge";
            return false;
        }
        if (bytes.size() - r.off < static_cast<size_t>(npts) * 8) { err = "roads: truncated edge"; return false; }
        edge.pts.resize(npts);
        edge.cum.resize(npts);
        for (uint16_t k = 0; k < npts; ++k) {
            edge.pts[k].x = r.f32();
            edge.pts[k].y = r.f32();
        }
        if (!(edge.speed > 0.5f && edge.speed < 60.0f)) edge.speed = kDefaultSpeed[edge.cls];
        edge.cum[0] = 0.0f;
        for (uint16_t k = 1; k < npts; ++k) {
            float dx = edge.pts[k].x - edge.pts[k - 1].x, dy = edge.pts[k].y - edge.pts[k - 1].y;
            edge.cum[k] = edge.cum[k - 1] + std::sqrt(dx * dx + dy * dy);
        }
        edge.length = edge.cum[npts - 1];
        uint32_t idx = static_cast<uint32_t>(g.edges.size());
        g.adj[edge.mode][edge.a].push_back(idx);
        if (edge.b != edge.a) g.adj[edge.mode][edge.b].push_back(idx);
        g.edges.push_back(std::move(edge));
    }
    return r.ok;
}

float terrainHeightAt(const District& d, float x, float north) {
    if (d.tw < 2 || d.th < 2) return 0.0f;
    float fx = std::min(std::max(x / d.cell, 0.0f), static_cast<float>(d.tw - 1));
    float fy = std::min(std::max(north / d.cell, 0.0f), static_cast<float>(d.th - 1));
    uint32_t i0 = static_cast<uint32_t>(std::floor(fx)), j0 = static_cast<uint32_t>(std::floor(fy));
    uint32_t i1 = std::min(i0 + 1, d.tw - 1), j1 = std::min(j0 + 1, d.th - 1);
    float tx = fx - i0, ty = fy - j0;
    float h00 = d.heights[j0 * d.tw + i0], h10 = d.heights[j0 * d.tw + i1];
    float h01 = d.heights[j1 * d.tw + i0], h11 = d.heights[j1 * d.tw + i1];
    float a = h00 + (h10 - h00) * tx;
    float b = h01 + (h11 - h01) * tx;
    return a + (b - a) * ty;
}

void buildStaticMesh(const District& d, std::vector<Vertex>& out) {
    out.clear();

    // Terrain: two triangles per cell, coloured by height.
    if (d.tw >= 2 && d.th >= 2) {
        for (uint32_t j = 0; j + 1 < d.th; ++j) {
            for (uint32_t i = 0; i + 1 < d.tw; ++i) {
                float x0 = i * d.cell, x1 = (i + 1) * d.cell;
                float n0 = j * d.cell, n1 = (j + 1) * d.cell;
                float h00 = d.heights[j * d.tw + i], h10 = d.heights[j * d.tw + i + 1];
                float h01 = d.heights[(j + 1) * d.tw + i], h11 = d.heights[(j + 1) * d.tw + i + 1];
                Vec3 p00 = scenePoint(x0, n0, h00), p10 = scenePoint(x1, n0, h10);
                Vec3 p01 = scenePoint(x0, n1, h01), p11 = scenePoint(x1, n1, h11);
                float col[3];
                heightColor((h00 + h10 + h01 + h11) * 0.25f, col);
                pushTriangle(out, p00, p10, p01, col);
                pushTriangle(out, p10, p11, p01, col);
            }
        }
    }

    // Buildings: roof triangulation and walls.
    const float roofCol[3] = {0.86f, 0.84f, 0.80f};
    const float wallCol[3] = {0.78f, 0.75f, 0.68f};
    for (const Building& b : d.buildings) {
        size_t n = b.ring.size();
        if (n < 3) continue;
        float top = b.base + std::max(b.height, 1.0f);
        std::vector<uint32_t> tris;
        triangulateRing(b.ring, tris);
        for (size_t t = 0; t + 2 < tris.size(); t += 3) {
            const Vec2f& p0 = b.ring[tris[t]];
            const Vec2f& p1 = b.ring[tris[t + 1]];
            const Vec2f& p2 = b.ring[tris[t + 2]];
            pushTriangle(out, scenePoint(p0.x, p0.y, top), scenePoint(p1.x, p1.y, top),
                         scenePoint(p2.x, p2.y, top), roofCol);
        }
        for (size_t k = 0; k < n; ++k) {
            const Vec2f& a = b.ring[k];
            const Vec2f& c = b.ring[(k + 1) % n];
            Vec3 ab = scenePoint(a.x, a.y, b.base), at = scenePoint(a.x, a.y, top);
            Vec3 cb = scenePoint(c.x, c.y, b.base), ct = scenePoint(c.x, c.y, top);
            pushTriangle(out, ab, cb, ct, wallCol);
            pushTriangle(out, ab, ct, at, wallCol);
        }
    }

    // Roads and footpaths: a flat strip per segment, lifted slightly above the terrain.
    const RoadGraph& g = d.roads;
    const float lift = 0.5f;
    for (const RoadEdge& e : g.edges) {
        float col[3];
        if (e.mode == 1) { col[0] = 0.78f; col[1] = 0.74f; col[2] = 0.62f; }
        else { col[0] = 0.24f; col[1] = 0.24f; col[2] = 0.27f; }
        float hw = kRoadWidth[e.cls] * 0.5f;
        for (size_t k = 0; k + 1 < e.pts.size(); ++k) {
            Vec2f p0 = e.pts[k], p1 = e.pts[k + 1];
            float dx = p1.x - p0.x, dy = p1.y - p0.y;
            float len = std::sqrt(dx * dx + dy * dy);
            if (len < 1e-3f) continue;
            float px = -dy / len * hw, py = dx / len * hw;
            Vec2f a{p0.x + px, p0.y + py}, bq{p0.x - px, p0.y - py};
            Vec2f c{p1.x + px, p1.y + py}, dq{p1.x - px, p1.y - py};
            Vec3 A = scenePoint(a.x, a.y, terrainHeightAt(d, a.x, a.y) + lift);
            Vec3 B = scenePoint(bq.x, bq.y, terrainHeightAt(d, bq.x, bq.y) + lift);
            Vec3 C = scenePoint(c.x, c.y, terrainHeightAt(d, c.x, c.y) + lift);
            Vec3 D = scenePoint(dq.x, dq.y, terrainHeightAt(d, dq.x, dq.y) + lift);
            pushTriangle(out, A, B, C, col);
            pushTriangle(out, B, D, C, col);
        }
    }
}

void terrainColour(float heightM, float* rgb) { heightColor(heightM, rgb); }

void triangulatePolygon(const std::vector<Vec2f>& ring, std::vector<uint32_t>& tris) {
    triangulateRing(ring, tris);
}
