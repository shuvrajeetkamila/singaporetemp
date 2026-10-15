// Road-based agents: cars on vehicle roads (with a simple car-following rule) and
// pedestrians on footpaths. Portable C++, no Android or Vulkan dependencies.
#pragma once

#include <cstdint>
#include <vector>

#include "world.h"

struct Agent {
    uint32_t edge = 0;     // index into RoadGraph::edges
    float t = 0.0f;        // distance along the edge polyline, 0..length
    int8_t dir = 1;        // +1 travelling a->b, -1 travelling b->a
    float v = 0.0f;        // current speed, m/s
    float vmax = 0.0f;     // desired speed, m/s
    uint8_t kind = 0;      // 0 car, 1 pedestrian
    uint8_t tint = 0;      // colour variant
};

class TrafficSim {
public:
    // Spawns `cars` vehicles and `people` pedestrians on random edges of the graph.
    void init(const RoadGraph* graph, uint32_t seed, int cars, int people);
    void step(float dt);

    // Position (x east, north metres) and unit heading (dx, dz in north coordinates).
    void pose(const Agent& a, Vec2f& pos, Vec2f& heading) const;

    const std::vector<Agent>& agents() const { return agents_; }

private:
    uint32_t rand32();
    float randUnit();
    bool pickNextEdge(const Agent& a, uint32_t node, uint8_t mode, uint32_t& nextEdge);
    void placeAt(Agent& a, uint32_t node, uint32_t nextEdge, float overflow) const;
    void spawn(Agent& a, uint8_t kind);

    const RoadGraph* graph_ = nullptr;
    std::vector<Agent> agents_;
    std::vector<uint32_t> carEdges_, footEdges_;  // edges usable for spawning
    uint32_t rng_ = 1;
};
