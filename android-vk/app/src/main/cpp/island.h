// Whole-island base-detail layer: terrain at 60 m and extruded building footprints.
// The pilot district (full detail) is cut out of this layer.
#pragma once

#include <cstdint>
#include <vector>

#include "world.h"

// Axis-aligned rectangle in the island frame (metres). z is north.
struct Rect {
    float x0, z0, x1, z1;
};

// Where the pilot district sits inside the island frame.
struct IslandLayout {
    float dx = 0.0f;  // pilot origin minus island origin, east (m)
    float dn = 0.0f;  // pilot origin minus island origin, north (m)
    float sx = 0.0f;  // pilot size east (m)
    float sz = 0.0f;  // pilot size north (m)
};

// Parses the text file "dx dn sx sz" written by the packing step.
bool parseLayout(const std::vector<uint8_t>& text, IslandLayout& out);

// Builds an indexed mesh for the island terrain and buildings, skipping anything inside `exclude`.
void buildIslandMesh(const District& island, const Rect& exclude, MeshBlob& out);

// The sea plane's colour as the shader draws it (flat up normal, same light as mesh.frag).
// Used as the clear colour so the sea has no visible edge against the background.
void seaShadedColour(float out[3]);
