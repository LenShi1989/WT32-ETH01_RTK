#include "RtkBase.h"
#include "Settings.h"
#include "JsonOut.h"

RtkBase rtkBase;

namespace {

// RTCM 位元寫入器 (MSB first)
struct BitWriter {
  uint8_t *buf;
  uint32_t pos = 0;
  explicit BitWriter(uint8_t *b) : buf(b) {}
  void put(uint64_t v, uint8_t len) {
    for (int i = len - 1; i >= 0; i--) {
      uint32_t byte = pos >> 3, bit = 7 - (pos & 7);
      if ((v >> i) & 1ULL) buf[byte] |= (1 << bit);
      else buf[byte] &= ~(1 << bit);
      pos++;
    }
  }
  void putSigned(int64_t v, uint8_t len) { put((uint64_t)v & ((1ULL << len) - 1), len); }
};

}  // namespace

// ---------------------------------------------------------------

void RtkBase::begin(OutputCb out) {
  _out = out;
  restart();
}

void RtkBase::restart() {
  _svN = 0;
  _mx = _my = _mz = _sx = _sy = _sz = 0;
  _svStartMs = 0;
  _corrValid = false;
  _corrN = 0;
  _hErrSq = _vErrSq = _hErrMax = 0;
  _baseFromSurvey = false;

  if (settings.baseMode == BASE_MODE_FIXED &&
      !(settings.fixedLat == 0 && settings.fixedLon == 0)) {
    Geo::Lla l{settings.fixedLat, settings.fixedLon, settings.fixedH};
    setBase(Geo::llaToEcef(l), false);
    Serial.printf("[RTK] 固定座標模式 %.9f, %.9f, %.4f\n", l.lat, l.lon, l.h);
  } else {
    _state = BASE_WAIT_FIX;
    Serial.println("[RTK] Survey-in 模式，等待 GNSS 定位");
  }
}

void RtkBase::setBase(const Geo::Ecef &e, bool fromSurvey) {
  _baseEcef = e;
  _baseLla = Geo::ecefToLla(e);
  _baseFromSurvey = fromSurvey;
  _state = BASE_READY;
  _last1005Ms = 0;  // 立即送出一次 1005
}

double RtkBase::svStd3d() const {
  if (_svN < 2) return NAN;
  return sqrt((_sx + _sy + _sz) / (double)(_svN - 1));
}

uint32_t RtkBase::svElapsed() const {
  return _svStartMs ? (millis() - _svStartMs) / 1000 : 0;
}

bool RtkBase::saveSurveyAsFixed() {
  if (_state != BASE_READY) return false;
  settings.baseMode = BASE_MODE_FIXED;
  settings.fixedLat = _baseLla.lat;
  settings.fixedLon = _baseLla.lon;
  settings.fixedH = _baseLla.h;
  settings.save();
  _baseFromSurvey = false;
  return true;
}

// ---------------------------------------------------------------
//  每筆 GGA 進來：Survey-in 累積 / 計算差分修正量
// ---------------------------------------------------------------
void RtkBase::handleGga(const GnssFix &fix) {
  if (!fix.valid) {
    _corrValid = false;
    return;
  }
  Geo::Ecef m = Geo::llaToEcef({fix.lat, fix.lon, fix.ellipsoidHeight()});

  if (_state == BASE_WAIT_FIX) {
    _state = BASE_SURVEYING;
    _svStartMs = millis();
    Serial.println("[RTK] 開始 Survey-in");
  }

  if (_state == BASE_SURVEYING) {
    if (fix.hdop > 4.0f) return;  // 幾何不佳的資料不納入
    // Welford 線上平均 / 變異數
    _svN++;
    double dx = m.x - _mx, dy = m.y - _my, dz = m.z - _mz;
    _mx += dx / _svN;
    _my += dy / _svN;
    _mz += dz / _svN;
    _sx += dx * (m.x - _mx);
    _sy += dy * (m.y - _my);
    _sz += dz * (m.z - _mz);

    double sd = svStd3d();
    if (svElapsed() >= settings.surveyMinSec && !isnan(sd) && sd <= settings.surveyAccM) {
      Geo::Ecef mean{_mx, _my, _mz};
      setBase(mean, true);
      Serial.printf("[RTK] Survey-in 完成：%u 筆, σ3D=%.3f m, %.9f, %.9f, %.3f\n",
                    _svN, sd, _baseLla.lat, _baseLla.lon, _baseLla.h);
    }
    return;
  }

  // ---- BASE_READY：位置域差分修正量 ----
  Geo::Ecef d{_baseEcef.x - m.x, _baseEcef.y - m.y, _baseEcef.z - m.z};
  _corr = Geo::ecefDeltaToEnu(d, _baseLla);
  _corrValid = true;
  double h2 = _corr.e * _corr.e + _corr.n * _corr.n;
  _hErrSq += h2;
  _vErrSq += _corr.u * _corr.u;
  _corrN++;
  if (sqrt(h2) > _hErrMax) _hErrMax = sqrt(h2);

  if (settings.outputDiffNmea) sendDiffNmea(fix);
}

// ---------------------------------------------------------------
//  接收機輸出的 RTCM3：統計 + 原封轉送
// ---------------------------------------------------------------
void RtkBase::countRtcm(uint16_t type, size_t len) {
  int freeIdx = -1;
  for (int i = 0; i < MAX_TYPES; i++) {
    if (_stats[i].type == type && _stats[i].count) {
      _stats[i].count++;
      _stats[i].bytes += len;
      _stats[i].lastMs = millis();
      return;
    }
    if (freeIdx < 0 && _stats[i].count == 0) freeIdx = i;
  }
  if (freeIdx >= 0) {
    _stats[freeIdx].type = type;
    _stats[freeIdx].count = 1;
    _stats[freeIdx].bytes = len;
    _stats[freeIdx].lastMs = millis();
  }
}

void RtkBase::handleRtcm(const uint8_t *frame, size_t len, uint16_t type) {
  countRtcm(type, len);

  if ((type == 1005 || type == 1006) && len >= 3 + 19 + 3) {
    const uint8_t *p = frame + 3;
    _rxStationId = (uint16_t)rtcmGetBits(p, 12, 12);
    _rxArp.x = rtcmGetBitsSigned(p, 34, 38) * 0.0001;
    _rxArp.y = rtcmGetBitsSigned(p, 74, 38) * 0.0001;
    _rxArp.z = rtcmGetBitsSigned(p, 114, 38) * 0.0001;
    _rxHas1005 = true;
    _rx1005Ms = millis();
  }

  if (_out) _out(frame, len);
  _rtcmBytesOut += len;
}

// ---------------------------------------------------------------
//  自行產生 RTCM 1005：基站天線參考點 ECEF
// ---------------------------------------------------------------
void RtkBase::send1005() {
  uint8_t f[3 + 19 + 3] = {0};
  BitWriter w(f + 3);
  w.put(1005, 12);                       // DF002 訊息編號
  w.put(settings.stationId & 0xFFF, 12); // DF003 基站 ID
  w.put(0, 6);                           // DF021 ITRF 年
  w.put(1, 1);                           // DF022 GPS
  w.put(1, 1);                           // DF023 GLONASS
  w.put(1, 1);                           // DF024 Galileo
  w.put(0, 1);                           // DF141 實體基站
  w.putSigned(llround(_baseEcef.x * 10000.0), 38);  // DF025
  w.put(1, 1);                           // DF142 單一接收機振盪器
  w.put(0, 1);                           // DF001 保留
  w.putSigned(llround(_baseEcef.y * 10000.0), 38);  // DF026
  w.put(0, 2);                           // DF364 1/4 周指標
  w.putSigned(llround(_baseEcef.z * 10000.0), 38);  // DF027

  f[0] = 0xD3;
  f[1] = 0;
  f[2] = 19;
  uint32_t crc = GnssParser::crc24q(f, 3 + 19);
  f[22] = (crc >> 16) & 0xFF;
  f[23] = (crc >> 8) & 0xFF;
  f[24] = crc & 0xFF;

  if (_out) _out(f, sizeof(f));
  _rtcmBytesOut += sizeof(f);
  _sent1005++;
}

// $PRTKD,UTC,dE,dN,dU,quality,sats*CS  (位置域差分修正量，單位 m)
void RtkBase::sendDiffNmea(const GnssFix &fix) {
  char body[96];
  snprintf(body, sizeof(body), "PRTKD,%s,%.3f,%.3f,%.3f,%u,%u",
           fix.utcTime, _corr.e, _corr.n, _corr.u, fix.quality, fix.sats);
  uint8_t cs = 0;
  for (const char *p = body; *p; p++) cs ^= (uint8_t)*p;
  char line[112];
  int n = snprintf(line, sizeof(line), "$%s*%02X\r\n", body, cs);
  if (_out && n > 0) _out((const uint8_t *)line, (size_t)n);
}

void RtkBase::loop() {
  if (_rxHas1005 && millis() - _rx1005Ms > 30000) _rxHas1005 = false;

  if (_state == BASE_READY && settings.inject1005 && !_rxHas1005) {
    uint32_t iv = (uint32_t)settings.rtcm1005Interval * 1000;
    if (_last1005Ms == 0 || millis() - _last1005Ms >= iv) {
      _last1005Ms = millis();
      send1005();
    }
  }
}

// ---------------------------------------------------------------
//  JSON
// ---------------------------------------------------------------
String RtkBase::json() const {
  static const char *stateName[] = {"wait_fix", "surveying", "ready"};

  JsonOut sv(320);
  Geo::Lla mean = _svN ? Geo::ecefToLla({_mx, _my, _mz}) : Geo::Lla();
  sv.add("samples", (unsigned long)_svN)
    .add("elapsed", (unsigned long)svElapsed())
    .add("minSec", (unsigned long)settings.surveyMinSec)
    .add("accLimit", (double)settings.surveyAccM, 2)
    .add("std3d", svStd3d(), 3)
    .add("meanLat", mean.lat, 9)
    .add("meanLon", mean.lon, 9)
    .add("meanH", mean.h, 3);

  bool ready = _state == BASE_READY;
  JsonOut base(320);
  base.add("valid", ready)
      .add("source", ready ? (_baseFromSurvey ? "survey" : "fixed") : "")
      .add("lat", _baseLla.lat, 9)
      .add("lon", _baseLla.lon, 9)
      .add("h", _baseLla.h, 4)
      .add("x", _baseEcef.x, 4)
      .add("y", _baseEcef.y, 4)
      .add("z", _baseEcef.z, 4);

  JsonOut corr(320);
  corr.add("valid", _corrValid)
      .add("e", _corr.e, 3)
      .add("n", _corr.n, 3)
      .add("u", _corr.u, 3)
      .add("h2d", sqrt(_corr.e * _corr.e + _corr.n * _corr.n), 3)
      .add("d3", sqrt(_corr.e * _corr.e + _corr.n * _corr.n + _corr.u * _corr.u), 3)
      .add("rmsH", _corrN ? sqrt(_hErrSq / _corrN) : NAN, 3)
      .add("rmsV", _corrN ? sqrt(_vErrSq / _corrN) : NAN, 3)
      .add("maxH", _hErrMax, 3)
      .add("epochs", (unsigned long)_corrN);

  JsonOut rtcm(320);
  Geo::Lla rxl = _rxHas1005 ? Geo::ecefToLla(_rxArp) : Geo::Lla();
  rtcm.add("bytesOut", (unsigned long)_rtcmBytesOut)
      .add("sent1005", (unsigned long)_sent1005)
      .add("inject1005", settings.inject1005)
      .add("rxHas1005", _rxHas1005)
      .add("rxStationId", (int)_rxStationId)
      .add("rxLat", rxl.lat, 9)
      .add("rxLon", rxl.lon, 9)
      .add("rxH", rxl.h, 4)
      .raw("types", rtcmStatsJson());

  JsonOut j(1800);
  j.add("state", stateName[_state])
   .add("mode", settings.baseMode == BASE_MODE_FIXED ? "fixed" : "survey")
   .raw("survey", sv.end())
   .raw("base", base.end())
   .raw("corr", corr.end())
   .raw("rtcm", rtcm.end());
  return j.end();
}

String RtkBase::rtcmStatsJson() const {
  String s = "[";
  bool first = true;
  uint32_t now = millis();
  for (int i = 0; i < MAX_TYPES; i++) {
    if (!_stats[i].count) continue;
    if (!first) s += ',';
    first = false;
    JsonOut e(96);
    e.add("type", (int)_stats[i].type)
     .add("count", (unsigned long)_stats[i].count)
     .add("bytes", (unsigned long)_stats[i].bytes)
     .add("age", (unsigned long)((now - _stats[i].lastMs) / 1000));
    s += e.end();
  }
  s += "]";
  return s;
}
