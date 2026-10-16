// Host-side tests for the portable core (parsers, meshes, island layer, simulation, camera).
// Build and run from android-vk/ (one command):
//   g++ -std=c++17 -O2 -Wall -Wextra -Iapp/src/main/cpp tests/host_test.cpp app/src/main/cpp/world.cpp app/src/main/cpp/sim.cpp app/src/main/cpp/agents.cpp app/src/main/cpp/island.cpp app/src/main/cpp/diag.cpp app/src/main/cpp/camera.cpp -o host_test
//   ./host_test <dir containing terrain.bin, buildings.bin, roads.bin>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "agents.h"
#include "camera.h"
#include "diag.h"
#include "core_math.h"
#include "island.h"
#include "sim.h"
#include "world.h"

static int g_fail = 0;

#define CHECK(cond, ...)                         \
    do {                                         \
        if (!(cond)) {                           \
            std::printf("FAIL: " __VA_ARGS__);   \
            std::printf("\n");                   \
            ++g_fail;                            \
        }                                        \
    } while (0)

static std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static bool finite3(const float* p) {
    return std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]);
}

// Projected (x/z) area of near-horizontal triangles, i.e. roofs.
static double roofArea(const std::vector<Vertex>& mesh) {
    double area = 0.0;
    for (size_t i = 0; i + 2 < mesh.size(); i += 3) {
        const Vertex& a = mesh[i];
        const Vertex& b = mesh[i + 1];
        const Vertex& c = mesh[i + 2];
        Vec3 pa{a.pos[0], a.pos[1], a.pos[2]}, pb{b.pos[0], b.pos[1], b.pos[2]}, pc{c.pos[0], c.pos[1], c.pos[2]};
        Vec3 n = cross(pb - pa, pc - pa);
        float len = std::sqrt(dot(n, n));
        if (len < 1e-6f || std::fabs(n.y) < 0.99f * len) continue;
        area += 0.5 * std::fabs(n.y);
    }
    return area;
}

static void testConcaveRoof() {
    // L-shaped footprint, area = 3*1 + 1*1 = 4 m^2 (counter-clockwise).
    District d;
    Building b;
    b.base = 0.0f;
    b.height = 10.0f;
    b.ring = {{0, 0}, {3, 0}, {3, 1}, {1, 1}, {1, 2}, {0, 2}};
    d.buildings.push_back(b);
    std::vector<Vertex> mesh;
    buildStaticMesh(d, mesh);
    double a = roofArea(mesh);
    CHECK(std::fabs(a - 4.0) < 1e-4, "L-shaped roof area %.6f, expected 4", a);

    // Same footprint clockwise: must triangulate to the same area.
    District dcw;
    Building bcw = b;
    bcw.ring = {{0, 0}, {0, 2}, {1, 2}, {1, 1}, {3, 1}, {3, 0}};
    dcw.buildings.push_back(bcw);
    std::vector<Vertex> mesh2;
    buildStaticMesh(dcw, mesh2);
    CHECK(std::fabs(roofArea(mesh2) - 4.0) < 1e-4, "clockwise L roof area mismatch");
}

static void testMatrices() {
    Mat4 v = mat4LookAt({10, 5, 10}, {0, 0, 0}, {0, 1, 0});
    Vec3 eye{10, 5, 10};
    float x = v.m[0] * eye.x + v.m[4] * eye.y + v.m[8] * eye.z + v.m[12];
    float y = v.m[1] * eye.x + v.m[5] * eye.y + v.m[9] * eye.z + v.m[13];
    float z = v.m[2] * eye.x + v.m[6] * eye.y + v.m[10] * eye.z + v.m[14];
    CHECK(std::fabs(x) < 1e-4f && std::fabs(y) < 1e-4f && std::fabs(z) < 1e-4f, "lookAt eye not at origin");
    float tz = v.m[14];  // view-space z of the origin (the target)
    CHECK(tz < 0.0f, "lookAt target not in front");
}

static void testParserRejectsTruncated(const std::vector<uint8_t>& roads) {
    District d;
    std::string err;
    std::vector<uint8_t> cut(roads.begin(), roads.begin() + static_cast<long>(roads.size() / 2));
    CHECK(!parseRoads(cut, d, err), "truncated roads accepted");
    std::vector<uint8_t> bad = roads;
    bad[0] = 'X';
    CHECK(!parseRoads(bad, d, err), "bad magic accepted");
}

static void testLayoutParse() {
    std::string text = "33180.0 17010.0 2430.0 2100.0\n";
    std::vector<uint8_t> bytes(text.begin(), text.end());
    IslandLayout l;
    CHECK(parseLayout(bytes, l), "layout rejected");
    CHECK(l.dx == 33180.0f && l.dn == 17010.0f && l.sx == 2430.0f && l.sz == 2100.0f, "layout values wrong");
    std::string bad = "12 oops";
    std::vector<uint8_t> badBytes(bad.begin(), bad.end());
    CHECK(!parseLayout(badBytes, l), "bad layout accepted");
}

// Small synthetic island: 4x4 terrain at 60 m, two square buildings, one inside the pilot rectangle.
static void testIslandLayer() {
    District island;
    island.tw = 4;
    island.th = 4;
    island.cell = 60.0f;
    island.heights.assign(16, 3.0f);
    island.sizeX = 3 * 60.0f;
    island.sizeZ = 3 * 60.0f;
    Building inside;
    inside.base = 0.0f;
    inside.height = 12.0f;
    inside.ring = {{100, 100}, {120, 100}, {120, 120}, {100, 120}};
    Building outside = inside;
    outside.ring = {{300, 300}, {320, 300}, {320, 320}, {300, 320}};
    island.buildings = {inside, outside};

    MeshBlob all, cut;
    buildIslandMesh(island, Rect{1.0f, 1.0f, 0.0f, 0.0f}, all);  // empty rectangle: nothing excluded
    buildIslandMesh(island, Rect{90.0f, 90.0f, 130.0f, 130.0f}, cut);  // pilot rectangle over `inside`

    // Each building adds n roof vertices and 4n wall vertices (four per wall quad): 4 + 16 = 20 for n = 4.
    CHECK(all.vertices.size() - cut.vertices.size() == 20, "excluded building did not drop 20 vertices (%zu vs %zu)",
          all.vertices.size(), cut.vertices.size());
    CHECK(all.indices.size() % 3 == 0 && cut.indices.size() % 3 == 0, "index count not a multiple of 3");
    bool validAll = true, validCut = true;
    for (uint32_t i : all.indices) validAll = validAll && i < all.vertices.size();
    for (uint32_t i : cut.indices) validCut = validCut && i < cut.vertices.size();
    CHECK(validAll && validCut, "island index out of range");
    std::printf("island test: %zu vertices / %zu indices (all), %zu / %zu (pilot cut out)\n", all.vertices.size(),
                all.indices.size(), cut.vertices.size(), cut.indices.size());
}

// On-screen text: glyph coverage, scale, pixel placement, and clipping to the buffer.
static void testText() {
    for (char c = 'A'; c <= 'Z'; ++c) CHECK(hasGlyph(c), "no glyph for %c", c);
    for (char c = '0'; c <= '9'; ++c) CHECK(hasGlyph(c), "no glyph for %c", c);
    CHECK(hasGlyph('a'), "lowercase should map to capitals");
    CHECK(!hasGlyph('~'), "unexpected glyph for ~");

    // 40x40 buffer, one letter "I": margin 4, scale 4 (height limited). Top row is ".###." so the
    // middle column is inked and the left column is not.
    const int W = 40, H = 40;
    std::vector<uint32_t> px(static_cast<size_t>(W) * H, 0u);
    PixelBuffer b{px.data(), W, H, W};
    const uint32_t bg = packRgba(0, 0, 0), fg = packRgba(255, 255, 255);
    fillPixels(b, bg);
    const char* one[] = {"I"};
    const int scale = drawTextLines(b, one, 1, fg);
    CHECK(scale == 4, "scale %d, expected 4", scale);
    CHECK(px[static_cast<size_t>(4) * W + (4 + 2 * 4 + 1)] == fg, "middle of top bar not inked");
    CHECK(px[static_cast<size_t>(4) * W + (4 + 0 * 4 + 1)] == bg, "left of top bar should be blank");

    // Padding beyond the stride must never be written.
    const int SW = 30, SS = 40, SH = 20;
    std::vector<uint32_t> pad(static_cast<size_t>(SS) * SH, 0x12345678u);
    PixelBuffer pb{pad.data(), SW, SH, SS};
    fillPixels(pb, bg);
    const char* words[] = {"WWW", "MMM"};
    drawTextLines(pb, words, 2, fg);
    bool untouched = true;
    for (int y = 0; y < SH; ++y)
        for (int x = SW; x < SS; ++x)
            if (pad[static_cast<size_t>(y) * SS + x] != 0x12345678u) untouched = false;
    CHECK(untouched, "text wrote into stride padding");

    // A line too long for the width draws nothing (scale would be below 1).
    std::vector<uint32_t> small(100 * 20, 0u);
    PixelBuffer sb{small.data(), 100, 20, 100};
    fillPixels(sb, bg);
    std::string longLine(200, 'W');
    const char* longText[] = {longLine.c_str()};
    CHECK(drawTextLines(sb, longText, 1, fg) == 0, "too-long line should draw nothing");
    bool blank = true;
    for (uint32_t v : small) if (v != bg) blank = false;
    CHECK(blank, "too-long line changed pixels");
}

// Island cut-out at the pilot edge must not leave a gap; the sea plane must sit below sea level.
static void testSeamAndSea() {
    District d;
    d.tw = 5;
    d.th = 5;
    d.cell = 60.0f;
    d.sizeX = 240.0f;
    d.sizeZ = 240.0f;
    d.heights.assign(25, 0.0f);
    // Pilot rectangle not aligned to the 60 m grid: only the cell [60,120]^2 lies fully inside it.
    MeshBlob m;
    buildIslandMesh(d, Rect{30.0f, 30.0f, 150.0f, 150.0f}, m);
    const size_t terrainIdx = m.indices.size() - 6;  // last six indices are the sea plane
    CHECK(terrainIdx == 15 * 6, "terrain cells kept %zu, expected 15 (only one cell is fully inside)",
          terrainIdx / 6);

    // The sea plane is the last four vertices: 60 m below sea level, sea colour.
    const size_t n = m.vertices.size();
    CHECK(n >= 4, "no sea plane vertices");
    for (size_t k = n - 4; k < n && n >= 4; ++k) {
        CHECK(std::fabs(m.vertices[k].pos[1] + 60.0f) < 1e-3f, "sea plane not at -60 m");
        CHECK(m.vertices[k].rgba[2] > m.vertices[k].rgba[0], "sea plane not blue");
    }

    // Every vertex carries a unit normal (snorm8 length close to 127).
    for (const Vertex& v : m.vertices) {
        const float len = std::sqrt(static_cast<float>(v.nrm[0]) * v.nrm[0] + static_cast<float>(v.nrm[1]) * v.nrm[1] +
                                    static_cast<float>(v.nrm[2]) * v.nrm[2]);
        CHECK(std::fabs(len - 127.0f) < 3.0f, "vertex normal not unit (length %f)", static_cast<double>(len));
    }
    // The clear colour matches the shaded sea plane to within one 8-bit step, so there is no edge.
    float clear[3];
    seaShadedColour(clear);
    const Vertex& sv = m.vertices[n - 1];
    const float facing = 0.8f / std::sqrt(0.45f * 0.45f + 0.8f * 0.8f + 0.4f * 0.4f);
    const float light = 0.35f + 0.65f * facing;
    for (int k = 0; k < 3; ++k) {
        CHECK(std::fabs(clear[k] - sv.rgba[k] / 255.0f * light) < 1.0f / 255.0f,
              "clear colour does not match the shaded sea (channel %d)", k);
    }
}

// Camera: the ground under the pilot and island is found, and the eye is never below the ground.
static void testCameraGround() {
    District pilot;
    pilot.tw = 5;
    pilot.th = 5;
    pilot.cell = 30.0f;
    pilot.sizeX = 120.0f;
    pilot.sizeZ = 120.0f;
    pilot.heights.assign(25, 40.0f);  // a flat 40 m hill
    District island;
    island.tw = 5;
    island.th = 5;
    island.cell = 60.0f;
    island.sizeX = 240.0f;
    island.sizeZ = 240.0f;
    island.heights.assign(25, 0.0f);  // sea level

    SceneGround g;
    g.pilot = &pilot;
    g.pilotOffset[0] = 100.0f;
    g.pilotOffset[2] = -300.0f;  // pilot north 300..420 m
    g.island = &island;

    CHECK(std::fabs(groundHeight(g, 160.0f, -360.0f) - 40.0f) < 1e-3f, "pilot centre should be on the hill");
    CHECK(std::fabs(groundHeight(g, 10.0f, -10.0f)) < 1e-3f, "island point outside the pilot should be sea level");

    // Sweep targets, yaws, pitches and distances. The eye must end at least 6 m above the ground.
    int violations = 0, cases = 0;
    for (float tx = 0.0f; tx <= 240.0f; tx += 20.0f) {
        for (float tz = -240.0f; tz <= 0.0f; tz += 20.0f) {
            for (float yaw = 0.0f; yaw < 6.3f; yaw += 0.8f) {
                for (float pitch : {0.2f, 0.6f, 1.4f}) {
                    for (float dist : {60.0f, 300.0f}) {
                        const Vec3 target{tx, groundHeight(g, tx, tz), tz};
                        const float d = keepEyeAboveGround(g, target, yaw, pitch, dist, 6.0f);
                        const Vec3 eye = orbitEye(target, yaw, pitch, d);
                        if (eye.y < groundHeight(g, eye.x, eye.z) + 6.0f - 1e-3f) ++violations;
                        if (d < dist) ++violations;
                        ++cases;
                    }
                }
            }
        }
    }
    CHECK(violations == 0, "%d of %d camera cases ended below the ground", violations, cases);

    // The old failure: target at sea level inside the hill, low pitch. The camera must be pulled out.
    const Vec3 inHill{160.0f, 0.0f, -360.0f};
    const float d = keepEyeAboveGround(g, inHill, 0.6f, 0.2f, 60.0f, 6.0f);
    const Vec3 eye = orbitEye(inHill, 0.6f, 0.2f, d);
    const float ground = groundHeight(g, eye.x, eye.z);
    CHECK(d > 60.0f && eye.y >= ground + 6.0f - 1e-3f,
          "camera inside the hill was not pulled out (eye y %.1f, ground %.1f)", eye.y, ground);
}

int main(int argc, char** argv) {
    std::string dir = argc > 1 ? argv[1] : "app/src/main/assets/district";
    std::printf("data dir: %s\n", dir.c_str());

    testText();
    testCameraGround();
    testSeamAndSea();
    testConcaveRoof();
    testMatrices();
    testLayoutParse();
    testIslandLayer();

    std::vector<uint8_t> terrainBytes = readFile(dir + "/terrain.bin");
    std::vector<uint8_t> buildingBytes = readFile(dir + "/buildings.bin");
    std::vector<uint8_t> roadBytes = readFile(dir + "/roads.bin");
    CHECK(!terrainBytes.empty() && !buildingBytes.empty() && !roadBytes.empty(), "missing data files");
    if (g_fail) {
        std::printf("%d failure(s)\n", g_fail);
        return 1;
    }

    District d;
    std::string err;
    CHECK(parseTerrain(terrainBytes, d, err), "terrain: %s", err.c_str());
    CHECK(parseBuildings(buildingBytes, d, err), "buildings: %s", err.c_str());
    CHECK(parseRoads(roadBytes, d, err), "roads: %s", err.c_str());
    testParserRejectsTruncated(roadBytes);

    std::printf("terrain %ux%u cell %.1f m, size %.0f x %.0f m\n", d.tw, d.th, d.cell, d.sizeX, d.sizeZ);
    std::printf("buildings %zu, road nodes %zu, edges %zu\n", d.buildings.size(), d.roads.nodes.size(),
                d.roads.edges.size());
    CHECK(d.tw > 2 && d.th > 2, "terrain too small");
    CHECK(!d.buildings.empty(), "no buildings");
    CHECK(!d.roads.edges.empty(), "no roads");

    std::vector<Vertex> mesh;
    buildStaticMesh(d, mesh);
    std::printf("mesh vertices %zu (%zu triangles)\n", mesh.size(), mesh.size() / 3);
    CHECK(mesh.size() % 3 == 0, "vertex count not a multiple of 3");
    size_t badVerts = 0;
    for (const Vertex& v : mesh) {
        if (!finite3(v.pos)) ++badVerts;
    }
    CHECK(badVerts == 0, "%zu non-finite mesh vertices", badVerts);

    size_t vehicle = 0, foot = 0;
    for (const RoadEdge& e : d.roads.edges) {
        CHECK(e.length > 0.0f, "zero-length edge");
        (e.mode == 0 ? vehicle : foot)++;
    }
    std::printf("vehicle edges %zu, foot edges %zu\n", vehicle, foot);

    // Simulation: 10 simulated minutes at 30 Hz.
    TrafficSim sim;
    sim.init(&d.roads, 12345u, 150, 300);
    CHECK(sim.agents().size() == 450, "expected 450 agents, got %zu", sim.agents().size());
    const float dt = 1.0f / 30.0f;
    size_t maxViolations = 0;
    size_t samples = 0;
    double sumCarSpeed = 0.0;
    size_t carSamples = 0;
    float minX = 1e9f, maxX = -1e9f, minZ = 1e9f, maxZ = -1e9f;
    for (int s = 0; s < 30 * 600; ++s) {
        sim.step(dt);
        if (s % 30 != 0) continue;
        for (const Agent& a : sim.agents()) {
            ++samples;
            const RoadEdge& e = d.roads.edges[a.edge];
            if (!(a.t >= -1e-3f && a.t <= e.length + 1e-3f) || !std::isfinite(a.t) || !std::isfinite(a.v)) {
                ++maxViolations;
                continue;
            }
            if (a.v > a.vmax + 1e-3f) ++maxViolations;
            if (a.kind == 0) {
                sumCarSpeed += a.v;
                ++carSamples;
            }
            Vec2f p, h;
            sim.pose(a, p, h);
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(h.x) || !std::isfinite(h.y)) {
                ++maxViolations;
                continue;
            }
            float hl = std::sqrt(h.x * h.x + h.y * h.y);
            if (std::fabs(hl - 1.0f) > 1e-3f) ++maxViolations;
            minX = std::min(minX, p.x); maxX = std::max(maxX, p.x);
            minZ = std::min(minZ, p.y); maxZ = std::max(maxZ, p.y);
        }
    }
    std::printf("sim: %zu samples, %zu invariant violations\n", samples, maxViolations);
    std::printf("mean car speed %.2f m/s over %zu samples\n", carSamples ? sumCarSpeed / carSamples : 0.0,
                carSamples);
    std::printf("agent extent x [%.0f, %.0f] z [%.0f, %.0f] m\n", minX, maxX, minZ, maxZ);
    CHECK(maxViolations == 0, "%zu simulation invariant violations", maxViolations);
    CHECK(carSamples > 0 && sumCarSpeed / carSamples > 1.0, "cars are not moving");

    std::vector<Vertex> agentMesh;
    buildAgentMesh(d, sim, agentMesh);
    CHECK(agentMesh.size() == sim.agents().size() * 36, "agent mesh has %zu vertices, expected %zu",
          agentMesh.size(), sim.agents().size() * 36);
    size_t badAgentVerts = 0;
    for (const Vertex& v : agentMesh) {
        if (!finite3(v.pos)) ++badAgentVerts;
    }
    CHECK(badAgentVerts == 0, "%zu non-finite agent vertices", badAgentVerts);
    std::printf("agent mesh vertices %zu\n", agentMesh.size());

    // First-frame camera (same as the app): the district centre must be on screen and most of
    // the mesh must start inside the view.
    {
        const float yaw = 0.6f, pitch = 0.9f, dist = 2200.0f, aspect = 2.2f;
        const Vec3 target{d.sizeX * 0.5f, 0.0f, -d.sizeZ * 0.5f};
        const float cp = std::cos(pitch), sp = std::sin(pitch), sy = std::sin(yaw), cy = std::cos(yaw);
        const Vec3 eye = target + Vec3{cp * sy, sp, cp * cy} * dist;
        const Mat4 vp = mat4Mul(mat4Perspective(0.87266f, aspect, 2.0f, 40000.0f),
                                mat4LookAt(eye, target, Vec3{0.0f, 1.0f, 0.0f}));
        auto project = [&](const float* p, float* ndc) -> float {
            float x = p[0], y = p[1], z = p[2];
            float cx = vp.m[0] * x + vp.m[4] * y + vp.m[8] * z + vp.m[12];
            float cyy = vp.m[1] * x + vp.m[5] * y + vp.m[9] * z + vp.m[13];
            float cz = vp.m[2] * x + vp.m[6] * y + vp.m[10] * z + vp.m[14];
            float cw = vp.m[3] * x + vp.m[7] * y + vp.m[11] * z + vp.m[15];
            ndc[0] = cx / cw;
            ndc[1] = cyy / cw;
            ndc[2] = cz / cw;
            return cw;
        };
        float ndc[3];
        float wCenter = project(&target.x, ndc);
        std::printf("camera: target NDC (%.3f, %.3f) w %.1f\n", ndc[0], ndc[1], wCenter);
        CHECK(wCenter > 0.0f && std::fabs(ndc[0]) < 1.0f && std::fabs(ndc[1]) < 1.0f, "target not on screen");
        size_t inView = 0, total = 0;
        for (size_t i = 0; i < mesh.size(); i += 3) {
            ++total;
            float n2[3];
            float w = project(mesh[i].pos, n2);
            if (w > 0.0f && std::fabs(n2[0]) <= 1.0f && std::fabs(n2[1]) <= 1.0f && n2[2] >= 0.0f && n2[2] <= 1.0f)
                ++inView;
        }
        std::printf("first frame: %zu of %zu triangles start inside the view\n", inView, total);
        CHECK(inView > total / 10, "almost nothing in the first-frame view");
    }

    if (g_fail) {
        std::printf("%d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("ALL HOST TESTS PASSED\n");
    return 0;
}
