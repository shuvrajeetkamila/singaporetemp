// Host-side tests for the portable core (parsers, mesh, simulation).
// Build and run from android-vk/ (one command):
//   g++ -std=c++17 -O2 -Wall -Wextra -Iapp/src/main/cpp tests/host_test.cpp app/src/main/cpp/world.cpp app/src/main/cpp/sim.cpp app/src/main/cpp/agents.cpp -o host_test
//   ./host_test <dir containing terrain.bin, buildings.bin, roads.bin>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "agents.h"
#include "core_math.h"
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

// Area of triangles whose normal is near vertical, projected onto the x/z plane.
static double roofArea(const std::vector<Vertex>& mesh) {
    double area = 0.0;
    for (size_t i = 0; i + 2 < mesh.size(); i += 3) {
        if (std::fabs(mesh[i].nrm[1]) < 0.99f) continue;
        const Vertex& a = mesh[i];
        const Vertex& b = mesh[i + 1];
        const Vertex& c = mesh[i + 2];
        double ax = a.pos[0], az = a.pos[2], bx = b.pos[0], bz = b.pos[2], cx = c.pos[0], cz = c.pos[2];
        area += 0.5 * std::fabs((bx - ax) * (cz - az) - (cx - ax) * (bz - az));
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
    // Transform the eye point: view-space should be the origin.
    float x = v.m[0] * eye.x + v.m[4] * eye.y + v.m[8] * eye.z + v.m[12];
    float y = v.m[1] * eye.x + v.m[5] * eye.y + v.m[9] * eye.z + v.m[13];
    float z = v.m[2] * eye.x + v.m[6] * eye.y + v.m[10] * eye.z + v.m[14];
    CHECK(std::fabs(x) < 1e-4f && std::fabs(y) < 1e-4f && std::fabs(z) < 1e-4f, "lookAt eye not at origin");
    // Target should lie on the negative z axis in view space.
    float tz = v.m[2] * 0 + v.m[6] * 0 + v.m[10] * 0 + v.m[14];
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

int main(int argc, char** argv) {
    std::string dir = argc > 1 ? argv[1] : "app/src/main/assets/district";
    std::printf("data dir: %s\n", dir.c_str());

    testConcaveRoof();
    testMatrices();

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
        if (!finite3(v.pos) || !finite3(v.nrm) || !finite3(v.col)) ++badVerts;
    }
    CHECK(badVerts == 0, "%zu non-finite mesh vertices", badVerts);

    // Road edges: lengths positive, mode and class in range.
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

    // Agent boxes: 36 finite vertices per agent.
    std::vector<Vertex> agentMesh;
    buildAgentMesh(d, sim, agentMesh);
    CHECK(agentMesh.size() == sim.agents().size() * 36, "agent mesh has %zu vertices, expected %zu",
          agentMesh.size(), sim.agents().size() * 36);
    size_t badAgentVerts = 0;
    for (const Vertex& v : agentMesh) {
        if (!finite3(v.pos) || !finite3(v.nrm)) ++badAgentVerts;
    }
    CHECK(badAgentVerts == 0, "%zu non-finite agent vertices", badAgentVerts);
    std::printf("agent mesh vertices %zu\n", agentMesh.size());

    if (g_fail) {
        std::printf("%d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("ALL HOST TESTS PASSED\n");
    return 0;
}
