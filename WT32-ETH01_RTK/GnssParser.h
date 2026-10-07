// =============================================================
//  GNSS 資料串流解析：NMEA-0183 與 RTCM3 混合串流分框
//
//  NMEA 語句依 YIC GT-504GGB (MediaTek 多系統 G-Mouse) 規格書：
//    $GNGGA  定位資料 (時間 / 位置 / 品質 / 衛星數 / HDOP / 高度)
//    $GNGLL  經緯度 + 狀態 + 模式
//    $GNGSA  DOP 與使用中衛星 (每個系統一句，最後欄位為 GNSS System ID)
//    $GPGSV / $GLGSV / $GAGSV / $GBGSV  各系統可見衛星 (仰角 / 方位角 / C/N0)
//    $GNRMC  時間 / 日期 / 位置 / 速度 / 航向 / 模式
//    $GNVTG  航向與對地速度
//    $GNZDA  UTC 日期時間 (選配)
// =============================================================
#pragma once

#include <Arduino.h>

// 衛星系統 (網頁顯示用)
enum GnssSys : uint8_t {
  SYS_GPS = 0, SYS_GLO, SYS_GAL, SYS_BDS, SYS_QZSS, SYS_SBAS, SYS_OTHER, SYS_COUNT
};

struct GnssSat {
  uint8_t  sys = 0;       // GnssSys
  uint16_t prn = 0;
  int8_t   elev = -1;     // 仰角 (度)，-1 = 無資料
  int16_t  az = -1;       // 方位角 (度)，-1 = 無資料
  int8_t   snr = -1;      // C/N0 (dB-Hz)，-1 = 未追蹤
  uint32_t seenMs = 0;    // 最後一次在 GSV 中出現的時間
};

struct GnssFix {
  // GGA
  bool     valid = false;
  uint8_t  quality = 0;      // 0 無定位, 1 SPS, 2 DGPS, 3 PPS, 4 RTK Fixed, 5 RTK Float, 6 推估
  uint8_t  sats = 0;         // 使用中衛星數 (00~56)
  float    hdop = 99.9f;
  double   lat = 0, lon = 0; // 度
  double   altMsl = 0;       // 海拔高 (m)
  double   geoidSep = 0;     // 大地起伏 (m)
  float    dgpsAge = -1;     // 差分資料齡期 (秒)，-1 = 未使用 DGPS
  int16_t  dgpsStation = -1; // 差分站 ID (0000~1023)
  char     utcTime[12] = "";
  uint32_t lastGgaMs = 0;
  // RMC / GLL / VTG
  char     rmcStatus = 'V';  // 'A' 有效, 'V' 警告
  char     mode = 'N';       // 'N' 無效, 'A' 自主, 'D' 差分, 'E' 推估
  char     utcDate[8] = "";  // ddmmyy
  float    speedKn = 0;
  float    speedKmh = 0;
  float    course = 0;
  // ZDA
  uint16_t zdaYear = 0;
  uint8_t  zdaMonth = 0, zdaDay = 0;
  // GSA
  uint8_t  fixType = 0;      // 1 無, 2 2D, 3 3D
  float    pdop = 99.9f, vdop = 99.9f;
  // GSV (各系統可見衛星總數)
  uint8_t  svGps = 0, svGlo = 0, svGal = 0, svBds = 0, svQzss = 0, svOther = 0;

  double ellipsoidHeight() const { return altMsl + geoidSep; }
};

class GnssParser {
public:
  typedef void (*NmeaCb)(const char *line);
  typedef void (*RtcmCb)(const uint8_t *frame, size_t len, uint16_t msgType);

  static const int MAX_SATS = 64;
  static const uint32_t SAT_TIMEOUT_MS = 5000;  // GSV 超過此時間未出現就視為不可見

  void onNmea(NmeaCb cb) { _nmeaCb = cb; }
  void onRtcm(RtcmCb cb) { _rtcmCb = cb; }

  void feed(uint8_t b);
  void feed(const uint8_t *buf, size_t len) { for (size_t i = 0; i < len; i++) feed(buf[i]); }

  const GnssFix &fix() const { return _fix; }

  // 衛星列表 (含逾時判斷)
  int satCount() const { return _satN; }
  const GnssSat &sat(int i) const { return _sats[i]; }
  bool satFresh(int i, uint32_t now) const { return now - _sats[i].seenMs < SAT_TIMEOUT_MS; }
  bool satUsed(const GnssSat &s) const;      // 是否出現在 GSA 使用中衛星清單

  uint32_t nmeaCount = 0, nmeaErrors = 0;
  uint32_t rtcmCount = 0, rtcmErrors = 0;
  uint32_t bytesIn = 0;
  uint32_t lastNmeaMs = 0;   // 最後一句 checksum 正確的 NMEA

  static uint32_t crc24q(const uint8_t *data, size_t len);
  static const char *sysName(uint8_t sys);

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

  // GSV 衛星表
  GnssSat  _sats[MAX_SATS];
  int      _satN = 0;

  // GSA 使用中衛星：依 GNSS System ID (1 GPS, 2 GLONASS, 3 Galileo, 4 BDS, 5 QZSS) 分組
  static const int GSA_GROUPS = 6;
  static const int GSA_MAX = 24;
  uint16_t _used[GSA_GROUPS][GSA_MAX];
  uint8_t  _usedN[GSA_GROUPS] = {0};
  uint32_t _usedEpoch[GSA_GROUPS] = {0};
  uint32_t _epoch = 0;     // 每收到一筆 GGA 加 1，用來區分 GSA 是否為新的一輪

  void handleNmea();
  void handleRtcm();
  void parseGGA(char **f, int n);
  void parseGLL(char **f, int n);
  void parseRMC(char **f, int n);
  void parseVTG(char **f, int n);
  void parseZDA(char **f, int n);
  void parseGSA(const char *talker, char **f, int n);
  void parseGSV(const char *talker, char **f, int n);
  void upsertSat(uint8_t sys, uint16_t prn, int elev, int az, int snr);
};

// 依 RTCM 位元流讀取 (MSB first)
uint64_t rtcmGetBits(const uint8_t *buf, uint32_t pos, uint8_t len);
int64_t  rtcmGetBitsSigned(const uint8_t *buf, uint32_t pos, uint8_t len);
