#include "sim.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr float kCarLength = 4.5f;
constexpr float kFollowRange = 20.0f;   // cars look this far ahead for a leader
constexpr float kAccel = 2.0f;          // m/s^2
constexpr float kBrake = 4.0f;          // m/s^2
}  // namespace

uint32_t TrafficSim::rand32() {
    uint32_t x = rng_;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_ = x;
    return x;
}

float TrafficSim::randUnit() {
    return static_cast<float>(rand32() >> 8) * (1.0f / 16777216.0f);
}

void TrafficSim::init(const RoadGraph* graph, uint32_t seed, int cars, int people) {
    graph_ = graph;
    rng_ = seed ? seed : 0x9E3779B9u;
    agents_.clear();
    carEdges_.clear();
    footEdges_.clear();
    if (!graph_) return;
    for (uint32_t i = 0; i < graph_->edges.size(); ++i) {
        const RoadEdge& e = graph_->edges[i];
        if (e.length < 2.0f) continue;
        if (e.mode == 0) carEdges_.push_back(i);
        else footEdges_.push_back(i);
    }
    agents_.reserve(static_cast<size_t>(std::max(0, cars) + std::max(0, people)));
    if (!carEdges_.empty()) {
        for (int i = 0; i < cars; ++i) {
            Agent a;
            spawn(a, 0);
            agents_.push_back(a);
        }
    }
    if (!footEdges_.empty()) {
        for (int i = 0; i < people; ++i) {
            Agent a;
            spawn(a, 1);
            agents_.push_back(a);
        }
    }
}

void TrafficSim::spawn(Agent& a, uint8_t kind) {
    const std::vector<uint32_t>& pool = (kind == 0) ? carEdges_ : footEdges_;
    a.kind = kind;
    a.edge = pool[rand32() % pool.size()];
    const RoadEdge& e = graph_->edges[a.edge];
    a.t = randUnit() * e.length;
    a.dir = (rand32() & 1u) ? 1 : -1;
    a.tint = static_cast<uint8_t>(rand32() % 6);
    if (kind == 0) {
        a.vmax = e.speed * (0.8f + 0.2f * randUnit());
        a.v = a.vmax * 0.5f;
    } else {
        a.vmax = e.speed * (0.85f + 0.3f * randUnit());  // walking speed around 1.3 m/s
        a.v = a.vmax;
    }
}

// Picks the next edge at `node` for agent `a`. Prefers not to turn back unless it is a dead end.
bool TrafficSim::pickNextEdge(const Agent& a, uint32_t node, uint8_t mode, uint32_t& nextEdge) {
    const std::vector<uint32_t>& options = graph_->adj[mode][node];
    if (options.empty()) return false;
    uint32_t candidates[64];
    size_t count = 0;
    for (uint32_t e : options) {
        if (e == a.edge) continue;
        if (count < 64) candidates[count++] = e;
    }
    if (count == 0) {
        nextEdge = a.edge;  // dead end: turn around
        return true;
    }
    nextEdge = candidates[rand32() % count];
    return true;
}

// Puts the agent on `nextEdge` starting at `node`, carrying over the distance it overshot by.
void TrafficSim::placeAt(Agent& a, uint32_t node, uint32_t nextEdge, float overflow) const {
    const RoadEdge& ne = graph_->edges[nextEdge];
    a.edge = nextEdge;
    if (ne.a == node) {
        a.dir = 1;
        a.t = overflow;
    } else {
        a.dir = -1;
        a.t = ne.length - overflow;
    }
    a.t = std::min(std::max(a.t, 0.0f), ne.length);
}

void TrafficSim::step(float dt) {
    if (!graph_ || agents_.empty() || !(dt > 0.0f)) return;
    const size_t n = agents_.size();

    // 1. Desired speeds. Cars keep a gap to the nearest car ahead on the same edge and direction.
    std::vector<float> vDes(n);
    for (size_t i = 0; i < n; ++i) {
        const Agent& a = agents_[i];
        vDes[i] = a.vmax;
        if (a.kind != 0) continue;
        float bestGap = kFollowRange + 1.0f;
        float leaderV = 0.0f;
        bool found = false;
        for (size_t j = 0; j < n; ++j) {
            if (j == i) continue;
            const Agent& b = agents_[j];
            if (b.kind != 0 || b.edge != a.edge || b.dir != a.dir) continue;
            float ahead = (a.dir > 0) ? (b.t - a.t) : (a.t - b.t);
            if (ahead <= 0.0f) continue;
            float gap = ahead - kCarLength;
            if (gap < bestGap) {
                bestGap = gap;
                leaderV = b.v;
                found = true;
            }
        }
        if (found && bestGap < kFollowRange) {
            if (bestGap <= 0.5f) {
                vDes[i] = 0.0f;
            } else {
                vDes[i] = std::min(vDes[i], std::max(0.0f, leaderV + (bestGap - 10.0f) * 0.5f));
            }
        }
    }

    // 2. Integrate speed and position, moving across junctions as needed.
    for (size_t i = 0; i < n; ++i) {
        Agent& a = agents_[i];
        if (a.v < vDes[i]) a.v = std::min(a.v + kAccel * dt, vDes[i]);
        else a.v = std::max(a.v - kBrake * dt, vDes[i]);
        a.v = std::max(a.v, 0.0f);

        a.t += a.dir * a.v * dt;
        const uint8_t mode = (a.kind == 0) ? 0 : 1;
        for (int hops = 0; hops < 4; ++hops) {
            const RoadEdge& e = graph_->edges[a.edge];
            bool pastEnd = (a.dir > 0) ? (a.t > e.length) : (a.t < 0.0f);
            if (!pastEnd) break;
            uint32_t node = (a.dir > 0) ? e.b : e.a;
            float overflow = (a.dir > 0) ? (a.t - e.length) : (-a.t);
            uint32_t next = a.edge;
            if (!pickNextEdge(a, node, mode, next)) {
                a.t = std::min(std::max(a.t, 0.0f), e.length);
                a.v = 0.0f;
                break;
            }
            placeAt(a, node, next, overflow);
        }
    }
}

void TrafficSim::pose(const Agent& a, Vec2f& pos, Vec2f& heading) const {
    const RoadEdge& e = graph_->edges[a.edge];
    const size_t np = e.pts.size();
    float t = std::min(std::max(a.t, 0.0f), e.length);
    size_t k = static_cast<size_t>(std::upper_bound(e.cum.begin(), e.cum.end(), t) - e.cum.begin());
    if (k == 0) k = 1;
    if (k >= np) k = np - 1;
    const Vec2f& p0 = e.pts[k - 1];
    const Vec2f& p1 = e.pts[k];
    float span = e.cum[k] - e.cum[k - 1];
    float f = span > 1e-6f ? (t - e.cum[k - 1]) / span : 0.0f;
    pos.x = p0.x + (p1.x - p0.x) * f;
    pos.y = p0.y + (p1.y - p0.y) * f;
    float dx = (p1.x - p0.x) * a.dir, dy = (p1.y - p0.y) * a.dir;
    float len = std::sqrt(dx * dx + dy * dy);
    if (len > 1e-6f) {
        heading.x = dx / len;
        heading.y = dy / len;
    } else {
        heading.x = 1.0f;
        heading.y = 0.0f;
    }
}
