// On-screen text for the orbit camera readout, drawn as screen-space quads. Portable (no Android or Vulkan).
#pragma once

#include <cstddef>
#include <vector>

#include "world.h"

// Hard cap on HUD vertices per frame (the renderer's HUD buffer has the same size).
constexpr size_t kMaxHudVertices = 65536;

// Lines of 5x7 text (unknown characters are blank) at the top left of a w x h viewport, on a dark panel.
// Vertices are in normalised device coordinates with z = 0.5 and are meant to be drawn with an identity
// view-projection. Returns the number of vertices written.
size_t buildHudText(const char* const* lines, int count, int w, int h, std::vector<Vertex>& out);
