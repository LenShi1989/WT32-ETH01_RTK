#include "GnssParser.h"
#include <stdlib.h>
#include <string.h>

// ---------------- RTCM 工具 ----------------

uint32_t GnssParser::crc24q(const uint8_t *data, size_t len) {
  uint32_t crc = 0;
  for (size_t i = 0; i < len; i++) {
    crc ^= (uint32_t)data[i] << 16;
    for (int b = 0; b < 8; b++) {
      crc <<= 1;
      if (crc & 0x1000000) crc ^= 0x1864CFB;
    }
  }
  return crc & 0xFFFFFF;
}

uint64_t rtcmGetBits(const uint8_t *buf, uint32_t pos, uint8_t len) {
  uint64_t v = 0;
  for (uint32_t i = pos; i < pos + len; i++) {
    v = (v << 1) | ((buf[i >> 3] >> (7 - (i & 7))) & 1u);
  }
  return v;
}

int64_t rtcmGetBitsSigned(const uint8_t *buf, uint32_t pos, uint8_t len) {
  uint64_t v = rtcmGetBits(buf, pos, len);
  if (len < 64 && (v & (1ULL << (len - 1)))) v |= ~((1ULL << len) - 1);
  return (int64_t)v;
}

// ---------------- 串流狀態機 ----------------

void GnssParser::feed(uint8_t b) {
  bytesIn++;
  switch (_state) {
    case IDLE:
      if (b == '$') {
        _nmea[0] = '$';
        _nmeaLen = 1;
        _state = NMEA;
      } else if (b == 0xD3) {
        _rtcm[0] = b;
        _rtcmLen = 1;
        _state = RTCM_LEN1;
      }
      break;

    case NMEA:
      if (b == '\r' || b == '\n') {
        _nmea[_nmeaLen] = 0;
        handleNmea();
        _state = IDLE;
      } else if (b == '$') {          // 前一句不完整，重新開始
        nmeaErrors++;
        _nmeaLen = 1;
      } else if (b < 0x20 || b > 0x7E || _nmeaLen >= sizeof(_nmea) - 1) {
        nmeaErrors++;
        _state = IDLE;
        if (b == 0xD3) { _rtcm[0] = b; _rtcmLen = 1; _state = RTCM_LEN1; }
      } else {
        _nmea[_nmeaLen++] = (char)b;
      }
      break;

    case RTCM_LEN1:
      if ((b & 0xFC) != 0) {          // 前 6 bit 保留位必須為 0
        _state = IDLE;
        if (b == '$') { _nmea[0] = '$'; _nmeaLen = 1; _state = NMEA; }
        break;
      }
      _rtcm[_rtcmLen++] = b;
      _state = RTCM_LEN2;
      break;

    case RTCM_LEN2: {
      _rtcm[_rtcmLen++] = b;
      size_t payload = ((size_t)(_rtcm[1] & 0x03) << 8) | b;
      _rtcmNeed = 3 + payload + 3;
      _state = RTCM_BODY;
      break;
    }

    case RTCM_BODY:
      _rtcm[_rtcmLen++] = b;
      if (_rtcmLen >= _rtcmNeed) {
        handleRtcm();
        _state = IDLE;
      }
      break;
  }
}

void GnssParser::handleRtcm() {
  size_t n = _rtcmLen;
  uint32_t crc = crc24q(_rtcm, n - 3);
  uint32_t got = ((uint32_t)_rtcm[n - 3] << 16) | ((uint32_t)_rtcm[n - 2] << 8) | _rtcm[n - 1];
  if (crc != got) {
    rtcmErrors++;
    return;
  }
  rtcmCount++;
  uint16_t type = n >= 8 ? (uint16_t)rtcmGetBits(_rtcm + 3, 0, 12) : 0;
  if (_rtcmCb) _rtcmCb(_rtcm, n, type);
}

// ---------------- NMEA ----------------

static int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

static double nmeaToDeg(const char *s, const char *hemi) {
  if (!s || !*s) return 0;
  double v = atof(s);
  int deg = (int)(v / 100);
  double d = deg + (v - deg * 100) / 60.0;
  if (hemi && (*hemi == 'S' || *hemi == 'W')) d = -d;
  return d;
}

static void copyField(char *dst, size_t size, const char *src) {
  strncpy(dst, src, size - 1);
  dst[size - 1] = 0;
}

const char *GnssParser::sysName(uint8_t sys) {
  static const char *names[SYS_COUNT] = {"GPS", "GLONASS", "Galileo", "BeiDou", "QZSS", "SBAS", "其他"};
  return sys < SYS_COUNT ? names[sys] : "?";
}

void GnssParser::handleNmea() {
  // 驗證 checksum：$ 與 * 之間所有字元 XOR
  char *star = strrchr(_nmea, '*');
  if (!star || star - _nmea < 6 || strlen(star) < 3) { nmeaErrors++; return; }
  uint8_t cs = 0;
  for (char *p = _nmea + 1; p < star; p++) cs ^= (uint8_t)*p;
  int h = hexVal(star[1]), l = hexVal(star[2]);
  if (h < 0 || l < 0 || cs != (uint8_t)((h << 4) | l)) { nmeaErrors++; return; }
  nmeaCount++;
  lastNmeaMs = millis();

  // 分割欄位 (在副本上操作)
  char work[128];
  size_t len = star - _nmea;
  memcpy(work, _nmea, len);
  work[len] = 0;

  char *f[32];
  int n = 0;
  char *p = work;
  f[n++] = p;
  while (*p && n < 32) {
    if (*p == ',') { *p = 0; f[n++] = p + 1; }
    p++;
  }

  const char *id = f[0] + 1;  // 例如 "GNGGA"
  if (strlen(id) != 5) { if (_nmeaCb) _nmeaCb(_nmea); return; }  // $PAIR / $PMTK 等專有語句只送監看
  char talker[3] = {id[0], id[1], 0};
  const char *type = id + 2;

  if (!strcmp(type, "GGA")) parseGGA(f, n);
  else if (!strcmp(type, "RMC")) parseRMC(f, n);
  else if (!strcmp(type, "GSA")) parseGSA(talker, f, n);
  else if (!strcmp(type, "GSV")) parseGSV(talker, f, n);
  else if (!strcmp(type, "GLL")) parseGLL(f, n);
  else if (!strcmp(type, "VTG")) parseVTG(f, n);
  else if (!strcmp(type, "ZDA")) parseZDA(f, n);

  if (_nmeaCb) _nmeaCb(_nmea);  // 欄位解析完成後再通知
}

// $GNGGA,hhmmss.sss,ddmm.mmmm,a,dddmm.mmmm,a,x,xx,x.x,x.x,M,x.x,M,x.x,xxxx*hh
//   1 UTC  2,3 緯度  4,5 經度  6 品質  7 衛星數  8 HDOP  9 海拔  11 大地起伏  13 差分齡期  14 差分站 ID
void GnssParser::parseGGA(char **f, int n) {
  if (n < 15) return;
  _epoch++;
  copyField(_fix.utcTime, sizeof(_fix.utcTime), f[1]);
  _fix.quality = (uint8_t)atoi(f[6]);
  _fix.sats = (uint8_t)atoi(f[7]);
  _fix.hdop = *f[8] ? atof(f[8]) : 99.9f;
  _fix.valid = _fix.quality > 0 && *f[2] && *f[4];
  if (_fix.valid) {
    _fix.lat = nmeaToDeg(f[2], f[3]);
    _fix.lon = nmeaToDeg(f[4], f[5]);
    _fix.altMsl = atof(f[9]);
    _fix.geoidSep = atof(f[11]);
  }
  _fix.dgpsAge = *f[13] ? atof(f[13]) : -1;
  _fix.dgpsStation = (*f[13] && *f[14]) ? (int16_t)atoi(f[14]) : -1;
  _fix.lastGgaMs = millis();
}

// $GNGLL,ddmm.mmmm,a,dddmm.mmmm,a,hhmmss.sss,A,a*hh
//   6 狀態 A/V  7 模式 N/A/D/E
void GnssParser::parseGLL(char **f, int n) {
  if (n >= 8 && *f[7]) _fix.mode = *f[7];
}

// $GNRMC,hhmmss.sss,A,ddmm.mmmm,a,dddmm.mmmm,a,x.x,x.x,ddmmyy,,,a*hh
//   2 狀態  7 速度 (節)  8 航向  9 日期  12 模式
void GnssParser::parseRMC(char **f, int n) {
  if (n < 10) return;
  _fix.rmcStatus = *f[2] ? *f[2] : 'V';
  copyField(_fix.utcDate, sizeof(_fix.utcDate), f[9]);
  _fix.speedKn = atof(f[7]);
  _fix.course = atof(f[8]);
  if (n >= 13 && *f[12]) _fix.mode = *f[12];
}

// $GNVTG,x.x,T,,M,x.x,N,x.x,K,a*hh
//   1 真航向  5 速度 (節)  7 速度 (km/h)  9 模式
void GnssParser::parseVTG(char **f, int n) {
  if (n < 8) return;
  if (*f[1]) _fix.course = atof(f[1]);
  if (*f[5]) _fix.speedKn = atof(f[5]);
  if (*f[7]) _fix.speedKmh = atof(f[7]);
}

// $GNZDA,hhmmss.sss,dd,mm,yyyy,zz,zz*hh
void GnssParser::parseZDA(char **f, int n) {
  if (n < 5 || !*f[4]) return;
  _fix.zdaDay = (uint8_t)atoi(f[2]);
  _fix.zdaMonth = (uint8_t)atoi(f[3]);
  _fix.zdaYear = (uint16_t)atoi(f[4]);
}

// $GNGSA,A,x,xx,xx,xx,xx,xx,xx,xx,xx,xx,xx,xx,xx,x.x,x.x,x.x,x*hh
//   1 模式 M/A  2 定位型態 1/2/3  3~14 使用中衛星 PRN  15 PDOP  16 HDOP  17 VDOP  18 GNSS System ID
//   多系統定位時每個系統各輸出一句 GNGSA
void GnssParser::parseGSA(const char *talker, char **f, int n) {
  if (n < 18) return;
  _fix.fixType = (uint8_t)atoi(f[2]);
  _fix.pdop = *f[15] ? atof(f[15]) : 99.9f;
  _fix.vdop = *f[17] ? atof(f[17]) : 99.9f;

  int group;
  if (n >= 19 && *f[18]) group = atoi(f[18]);
  else if (!strcmp(talker, "GP")) group = 1;
  else if (!strcmp(talker, "GL")) group = 2;
  else if (!strcmp(talker, "GA")) group = 3;
  else if (!strcmp(talker, "GB") || !strcmp(talker, "BD")) group = 4;
  else if (!strcmp(talker, "GQ")) group = 5;
  else {                       // 舊式 $GNGSA 無 System ID：以 PRN 範圍判斷
    int prn = atoi(f[3]);
    group = (prn >= 65 && prn <= 96) ? 2 : 1;
  }
  if (group < 1 || group >= GSA_GROUPS) return;

  if (_usedEpoch[group] != _epoch) {   // 新的一輪：清除該系統舊清單
    _usedEpoch[group] = _epoch;
    _usedN[group] = 0;
  }
  for (int i = 3; i <= 14; i++) {
    if (!*f[i] || _usedN[group] >= GSA_MAX) continue;
    _used[group][_usedN[group]++] = (uint16_t)atoi(f[i]);
  }
}

bool GnssParser::satUsed(const GnssSat &s) const {
  int groups[2] = {0, 0};
  switch (s.sys) {
    case SYS_GPS: case SYS_SBAS: groups[0] = 1; break;
    case SYS_QZSS: groups[0] = 1; groups[1] = 5; break;
    case SYS_GLO: groups[0] = 2; break;
    case SYS_GAL: groups[0] = 3; break;
    case SYS_BDS: groups[0] = 4; break;
    default: return false;
  }
  for (int g : groups) {
    if (!g || _epoch - _usedEpoch[g] > 1) continue;  // 太舊的 GSA 不採用
    for (int i = 0; i < _usedN[g]; i++)
      if (_used[g][i] == s.prn) return true;
  }
  return false;
}

// $GPGSV,x,x,xx,xx,xx,xxx,xx,...,x*hh
//   1 總句數  2 句序  3 可見衛星總數  之後每 4 欄一顆衛星 (PRN, 仰角, 方位角, C/N0)，每句最多 4 顆
//   最後可能多一欄 Signal ID (1 = L1 C/A)
void GnssParser::parseGSV(const char *talker, char **f, int n) {
  if (n < 4) return;

  uint8_t baseSys;
  if (!strcmp(talker, "GP")) baseSys = SYS_GPS;
  else if (!strcmp(talker, "GL")) baseSys = SYS_GLO;
  else if (!strcmp(talker, "GA")) baseSys = SYS_GAL;
  else if (!strcmp(talker, "GB") || !strcmp(talker, "BD")) baseSys = SYS_BDS;
  else if (!strcmp(talker, "GQ")) baseSys = SYS_QZSS;
  else baseSys = SYS_OTHER;

  if (atoi(f[2]) == 1) {  // 每組第一句的「可見衛星總數」
    uint8_t total = (uint8_t)atoi(f[3]);
    switch (baseSys) {
      case SYS_GPS:  _fix.svGps = total; break;
      case SYS_GLO:  _fix.svGlo = total; break;
      case SYS_GAL:  _fix.svGal = total; break;
      case SYS_BDS:  _fix.svBds = total; break;
      case SYS_QZSS: _fix.svQzss = total; break;
      default:       _fix.svOther = total; break;
    }
  }

  int groups = (n - 4) / 4;
  for (int g = 0; g < groups && g < 4; g++) {
    char **s = f + 4 + g * 4;
    if (!*s[0]) continue;
    uint16_t prn = (uint16_t)atoi(s[0]);
    uint8_t sys = baseSys;
    if (baseSys == SYS_GPS) {   // $GPGSV 內也會帶 SBAS (33~64) / QZSS (193~)
      if (prn >= 33 && prn <= 64) sys = SYS_SBAS;
      else if (prn >= 193 && prn <= 202) sys = SYS_QZSS;
    }
    upsertSat(sys, prn, *s[1] ? atoi(s[1]) : -1, *s[2] ? atoi(s[2]) : -1, *s[3] ? atoi(s[3]) : -1);
  }
}

void GnssParser::upsertSat(uint8_t sys, uint16_t prn, int elev, int az, int snr) {
  int idx = -1, oldest = 0;
  for (int i = 0; i < _satN; i++) {
    if (_sats[i].sys == sys && _sats[i].prn == prn) { idx = i; break; }
    if (_sats[i].seenMs < _sats[oldest].seenMs) oldest = i;
  }
  if (idx < 0) idx = _satN < MAX_SATS ? _satN++ : oldest;  // 表滿：覆蓋最久沒出現的衛星
  GnssSat &s = _sats[idx];
  s.sys = sys;
  s.prn = prn;
  s.elev = (int8_t)constrain(elev, -1, 90);
  s.az = (int16_t)constrain(az, -1, 359);
  s.snr = (int8_t)constrain(snr, -1, 99);
  s.seenMs = millis();
}
