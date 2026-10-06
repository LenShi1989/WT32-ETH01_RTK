// =============================================================
//  GNSS 資料串流解析：NMEA-0183 與 RTCM3 混合串流分框
// =============================================================
#pragma once

#include <Arduino.h>

struct GnssFix {
  // GGA
  bool     valid = false;
  uint8_t  quality = 0;      // 0 無定位, 1 GPS, 2 DGPS, 4 RTK Fixed, 5 RTK Float, 6 推估
  uint8_t  sats = 0;
  float    hdop = 99.9f;
  double   lat = 0, lon = 0; // 度
  double   altMsl = 0;       // 海拔高 (m)
  double   geoidSep = 0;     // 大地起伏 (m)
  char     utcTime[12] = "";
  uint32_t lastGgaMs = 0;
  // RMC
  char     utcDate[8] = "";
  float    speedKn = 0;
  float    course = 0;
  // GSA
  uint8_t  fixType = 0;      // 1 無, 2 2D, 3 3D
  float    pdop = 99.9f, vdop = 99.9f;
  // GSV (各系統可見衛星)
  uint8_t  svGps = 0, svGlo = 0, svGal = 0, svBds = 0, svQzss = 0, svOther = 0;

  double ellipsoidHeight() const { return altMsl + geoidSep; }
};

class GnssParser {
public:
  typedef void (*NmeaCb)(const char *line);
  typedef void (*RtcmCb)(const uint8_t *frame, size_t len, uint16_t msgType);

  void onNmea(NmeaCb cb) { _nmeaCb = cb; }
  void onRtcm(RtcmCb cb) { _rtcmCb = cb; }

  void feed(uint8_t b);
  void feed(const uint8_t *buf, size_t len) { for (size_t i = 0; i < len; i++) feed(buf[i]); }

  const GnssFix &fix() const { return _fix; }

  uint32_t nmeaCount = 0, nmeaErrors = 0;
  uint32_t rtcmCount = 0, rtcmErrors = 0;
  uint32_t bytesIn = 0;

  static uint32_t crc24q(const uint8_t *data, size_t len);

private:
  enum State { IDLE, NMEA, RTCM_LEN1, RTCM_LEN2, RTCM_BODY };
  State _state = IDLE;

  char     _nmea[128];
  size_t   _nmeaLen = 0;

  uint8_t  _rtcm[1029];   // 3 header + 1023 payload + 3 CRC
  size_t   _rtcmLen = 0;
  size_t   _rtcmNeed = 0;

  GnssFix  _fix;
  NmeaCb   _nmeaCb = nullptr;
  RtcmCb   _rtcmCb = nullptr;

  void handleNmea();
  void handleRtcm();
  void parseGGA(char **f, int n);
  void parseRMC(char **f, int n);
  void parseGSA(char **f, int n);
  void parseGSV(const char *talker, char **f, int n);
};

// 依 RTCM 位元流讀取 (MSB first)
uint64_t rtcmGetBits(const uint8_t *buf, uint32_t pos, uint8_t len);
int64_t  rtcmGetBitsSigned(const uint8_t *buf, uint32_t pos, uint8_t len);
