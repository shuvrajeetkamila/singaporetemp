#include "geo.h"

#include <cmath>

namespace {

// SVY21 (EPSG:3414) parameters: GRS80 ellipsoid, central meridian 103 50 E, origin latitude 1 22 N.
constexpr double kA = 6378137.0;
constexpr double kF = 1.0 / 298.257223563;
constexpr double kK0 = 1.0;
constexpr double kLat0 = 1.0 + 22.0 / 60.0;          // degrees
constexpr double kLon0 = 103.0 + 50.0 / 60.0;        // degrees
constexpr double kFalseE = 28001.642;
constexpr double kFalseN = 38744.572;
constexpr double kDeg = 3.14159265358979323846 / 180.0;

}  // namespace

LatLon svy21ToWgs84(double easting, double northing) {
    const double e2 = kF * (2.0 - kF);
    const double ep2 = e2 / (1.0 - e2);
    const double e4 = e2 * e2, e6 = e4 * e2;

    // Meridian arc length from the equator to the origin latitude.
    const double phi0 = kLat0 * kDeg;
    const double m0 = kA * ((1.0 - e2 / 4.0 - 3.0 * e4 / 64.0 - 5.0 * e6 / 256.0) * phi0 -
                            (3.0 * e2 / 8.0 + 3.0 * e4 / 32.0 + 45.0 * e6 / 1024.0) * std::sin(2.0 * phi0) +
                            (15.0 * e4 / 256.0 + 45.0 * e6 / 1024.0) * std::sin(4.0 * phi0) -
                            (35.0 * e6 / 3072.0) * std::sin(6.0 * phi0));
    const double m = m0 + (northing - kFalseN) / kK0;
    const double mu = m / (kA * (1.0 - e2 / 4.0 - 3.0 * e4 / 64.0 - 5.0 * e6 / 256.0));
    const double e1 = (1.0 - std::sqrt(1.0 - e2)) / (1.0 + std::sqrt(1.0 - e2));
    const double phi1 = mu + (3.0 * e1 / 2.0 - 27.0 * e1 * e1 * e1 / 32.0) * std::sin(2.0 * mu) +
                        (21.0 * e1 * e1 / 16.0 - 55.0 * e1 * e1 * e1 * e1 / 32.0) * std::sin(4.0 * mu) +
                        (151.0 * e1 * e1 * e1 / 96.0) * std::sin(6.0 * mu) +
                        (1097.0 * e1 * e1 * e1 * e1 / 512.0) * std::sin(8.0 * mu);

    const double s1 = std::sin(phi1), c1 = std::cos(phi1), t1 = std::tan(phi1);
    const double n1 = kA / std::sqrt(1.0 - e2 * s1 * s1);
    const double r1 = kA * (1.0 - e2) / std::pow(1.0 - e2 * s1 * s1, 1.5);
    const double tt = t1 * t1;
    const double cc = ep2 * c1 * c1;
    const double d = (easting - kFalseE) / (n1 * kK0);

    const double lat = phi1 - (n1 * t1 / r1) *
                                  (d * d / 2.0 -
                                   (5.0 + 3.0 * tt + 10.0 * cc - 4.0 * cc * cc - 9.0 * ep2) * std::pow(d, 4) / 24.0 +
                                   (61.0 + 90.0 * tt + 298.0 * cc + 45.0 * tt * tt - 252.0 * ep2 - 3.0 * cc * cc) *
                                       std::pow(d, 6) / 720.0);
    const double lon = kLon0 * kDeg +
                       (d - (1.0 + 2.0 * tt + cc) * std::pow(d, 3) / 6.0 +
                        (5.0 - 2.0 * cc + 28.0 * tt - 3.0 * cc * cc + 8.0 * ep2 + 24.0 * tt * tt) * std::pow(d, 5) / 120.0) /
                           c1;
    LatLon out;
    out.lat = lat / kDeg;
    out.lon = lon / kDeg;
    return out;
}

void sceneToSvy21(float x, float z, double islandE, double islandN, double& e, double& n) {
    e = islandE + static_cast<double>(x);
    n = islandN - static_cast<double>(z);  // scene z = -north
}
