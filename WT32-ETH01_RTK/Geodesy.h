// =============================================================
//  WGS-84 座標轉換：LLA <-> ECEF、ECEF 差量 -> ENU
// =============================================================
#pragma once

#include <math.h>

namespace Geo {

constexpr double WGS84_A  = 6378137.0;
constexpr double WGS84_F  = 1.0 / 298.257223563;
constexpr double WGS84_E2 = WGS84_F * (2.0 - WGS84_F);
constexpr double DEG2RAD  = M_PI / 180.0;
constexpr double RAD2DEG  = 180.0 / M_PI;

struct Ecef { double x = 0, y = 0, z = 0; };
struct Lla  { double lat = 0, lon = 0, h = 0; };  // 度、度、橢球高 (m)
struct Enu  { double e = 0, n = 0, u = 0; };

inline Ecef llaToEcef(const Lla &p) {
  double lat = p.lat * DEG2RAD, lon = p.lon * DEG2RAD;
  double sl = sin(lat), cl = cos(lat);
  double N = WGS84_A / sqrt(1.0 - WGS84_E2 * sl * sl);
  Ecef r;
  r.x = (N + p.h) * cl * cos(lon);
  r.y = (N + p.h) * cl * sin(lon);
  r.z = (N * (1.0 - WGS84_E2) + p.h) * sl;
  return r;
}

inline Lla ecefToLla(const Ecef &c) {
  Lla r;
  double p = sqrt(c.x * c.x + c.y * c.y);
  r.lon = atan2(c.y, c.x);
  double lat = atan2(c.z, p * (1.0 - WGS84_E2));
  double N = WGS84_A, h = 0;
  for (int i = 0; i < 6; i++) {
    double sl = sin(lat);
    N = WGS84_A / sqrt(1.0 - WGS84_E2 * sl * sl);
    h = p / cos(lat) - N;
    lat = atan2(c.z, p * (1.0 - WGS84_E2 * N / (N + h)));
  }
  r.lat = lat * RAD2DEG;
  r.lon = r.lon * RAD2DEG;
  r.h = h;
  return r;
}

// d = (target - ref) 的 ECEF 差量轉為以 refLla 為原點的區域 ENU
inline Enu ecefDeltaToEnu(const Ecef &d, const Lla &refLla) {
  double lat = refLla.lat * DEG2RAD, lon = refLla.lon * DEG2RAD;
  double sl = sin(lat), cl = cos(lat), so = sin(lon), co = cos(lon);
  Enu r;
  r.e = -so * d.x + co * d.y;
  r.n = -sl * co * d.x - sl * so * d.y + cl * d.z;
  r.u =  cl * co * d.x + cl * so * d.y + sl * d.z;
  return r;
}

}  // namespace Geo
