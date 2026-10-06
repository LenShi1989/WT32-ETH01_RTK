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

void GnssParser::handleNmea() {
  // 驗證 checksum
  char *star = strrchr(_nmea, '*');
  if (!star || star - _nmea < 6 || strlen(star) < 3) { nmeaErrors++; return; }
  uint8_t cs = 0;
  for (char *p = _nmea + 1; p < star; p++) cs ^= (uint8_t)*p;
  int h = hexVal(star[1]), l = hexVal(star[2]);
  if (h < 0 || l < 0 || cs != (uint8_t)((h << 4) | l)) { nmeaErrors++; return; }
  nmeaCount++;

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
  if (strlen(id) < 5) { if (_nmeaCb) _nmeaCb(_nmea); return; }
  char talker[3] = {id[0], id[1], 0};
  const char *type = id + 2;

  if (!strcmp(type, "GGA")) parseGGA(f, n);
  else if (!strcmp(type, "RMC")) parseRMC(f, n);
  else if (!strcmp(type, "GSA")) parseGSA(f, n);
  else if (!strcmp(type, "GSV")) parseGSV(talker, f, n);

  if (_nmeaCb) _nmeaCb(_nmea);  // 欄位解析完成後再通知
}

void GnssParser::parseGGA(char **f, int n) {
  if (n < 15) return;
  strncpy(_fix.utcTime, f[1], sizeof(_fix.utcTime) - 1);
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
  _fix.lastGgaMs = millis();
}

void GnssParser::parseRMC(char **f, int n) {
  if (n < 10) return;
  strncpy(_fix.utcDate, f[9], sizeof(_fix.utcDate) - 1);
  _fix.speedKn = atof(f[7]);
  _fix.course = atof(f[8]);
}

void GnssParser::parseGSA(char **f, int n) {
  if (n < 18) return;
  _fix.fixType = (uint8_t)atoi(f[2]);
  _fix.pdop = *f[15] ? atof(f[15]) : 99.9f;
  _fix.vdop = *f[17] ? atof(f[17]) : 99.9f;
}

void GnssParser::parseGSV(const char *talker, char **f, int n) {
  if (n < 4 || atoi(f[2]) != 1) return;  // 只看每組第一句的「可見衛星總數」
  uint8_t total = (uint8_t)atoi(f[3]);
  if (!strcmp(talker, "GP")) _fix.svGps = total;
  else if (!strcmp(talker, "GL")) _fix.svGlo = total;
  else if (!strcmp(talker, "GA")) _fix.svGal = total;
  else if (!strcmp(talker, "GB") || !strcmp(talker, "BD")) _fix.svBds = total;
  else if (!strcmp(talker, "GQ")) _fix.svQzss = total;
  else _fix.svOther = total;
}
