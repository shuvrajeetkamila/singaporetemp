// District data: parsing of the exported binary files and construction of the static mesh.
// Portable C++ (no Android or Vulkan dependencies) so it can be tested on a desktop.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct Vec2f {
    float x, y;
};

struct Vertex {
    float pos[3];
    float nrm[3];
    float col[3];
};

struct RoadEdge {
    uint32_t a = 0, b = 0;      // junction node indices (a = start of polyline, b = end)
    uint8_t mode = 0;           // 0 vehicle, 1 foot
    uint8_t cls = 0;            // see export: 0 motorway .. 5 minor road, 6 footway
    float speed = 0.0f;         // m/s
    std::vector<Vec2f> pts;     // polyline, metres relative to district origin
    std::vector<float> cum;     // cumulative length at each point
    float length = 0.0f;
};

struct RoadGraph {
    std::vector<Vec2f> nodes;
    std::vector<RoadEdge> edges;
    // adj[mode][node] = indices of edges of that mode touching the node
    std::vector<std::vector<uint32_t>> adj[2];
};

struct Building {
    float base = 0.0f;
    float height = 0.0f;
    std::vector<Vec2f> ring;    // exterior, metres relative to district origin
};

struct District {
    uint32_t tw = 0, th = 0;    // terrain grid size
    float cell = 30.0f;         // metres per terrain cell
    std::vector<float> heights; // row-major, row 0 = south
    std::vector<Building> buildings;
    RoadGraph roads;
    float sizeX = 0.0f, sizeZ = 0.0f;
};

// Parsers return false and set `err` on malformed input. They never read out of bounds.
bool parseTerrain(const std::vector<uint8_t>& bytes, District& d, std::string& err);
bool parseBuildings(const std::vector<uint8_t>& bytes, District& d, std::string& err);
bool parseRoads(const std::vector<uint8_t>& bytes, District& d, std::string& err);

// Height of the terrain at district coordinates (x east, north metres), bilinear.
float terrainHeightAt(const District& d, float x, float north);

// Builds a flat triangle list (3 vertices per triangle):
// terrain, building roofs and walls, roads and footpaths.
void buildStaticMesh(const District& d, std::vector<Vertex>& out);

// Scene coordinates: x = east, y = height, z = -north (both in metres).
