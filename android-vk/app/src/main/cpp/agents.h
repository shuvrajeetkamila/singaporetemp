// Turns simulated agents into simple coloured boxes (cars and pedestrians).
#pragma once

#include <vector>

#include "sim.h"
#include "world.h"

// Appends 36 vertices per agent. Cars are 4.5 x 1.8 x 1.5 m, pedestrians 0.5 x 0.5 x 1.7 m.
void buildAgentMesh(const District& d, const TrafficSim& sim, std::vector<Vertex>& out);
