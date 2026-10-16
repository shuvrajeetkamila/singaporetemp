// Map coordinates for the on-screen readout: SVY21 (EPSG:3414) to WGS84 latitude and longitude.
// Portable (no Android or Vulkan).
#pragma once

struct LatLon {
    double lat = 0.0;  // degrees north
    double lon = 0.0;  // degrees east
};

// SVY21 easting and northing (metres) to WGS84 degrees (transverse Mercator inverse, GRS80).
LatLon svy21ToWgs84(double easting, double northing);

// SVY21 position of a scene point. Scene frame: x east, z = -north, metres from the island origin.
// `islandE` and `islandN` are the SVY21 coordinates of that origin.
void sceneToSvy21(float x, float z, double islandE, double islandN, double& e, double& n);
