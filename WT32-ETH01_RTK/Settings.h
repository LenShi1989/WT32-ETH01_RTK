// =============================================================
//  設定值 (以 Preferences / NVS 永久保存)
// =============================================================
#pragma once

#include <Arduino.h>

enum BaseMode : uint8_t {
  BASE_MODE_SURVEY_IN = 0,  // 自動平均定位 (Survey-in)
  BASE_MODE_FIXED     = 1,  // 使用者輸入的已知座標
};

struct Settings {
  // ---- RJ45 ----
  bool     ethDhcp = true;
  String   ethIp, ethMask, ethGw, ethDns;
  String   hostname;

  // ---- WiFi STA / AP ----
  String   staSsid, staPass;
  String   apSsid, apPass;

  // ---- GNSS ----
  uint32_t gnssBaud = 0;

  // ---- RTK 基站 ----
  uint8_t  baseMode = BASE_MODE_SURVEY_IN;
  uint32_t surveyMinSec = 0;
  float    surveyAccM = 0;
  double   fixedLat = 0, fixedLon = 0, fixedH = 0;  // 度 / 度 / 橢球高 (m)
  uint16_t stationId = 0;
  bool     inject1005 = true;     // 自行產生 RTCM 1005 基站座標訊息
  uint16_t rtcm1005Interval = 0;  // 秒
  bool     outputDiffNmea = false;  // 額外輸出 $PRTKD 位置域差分修正語句

  // ---- 差分輸出 ----
  bool     tcpEnable = true;
  uint16_t tcpPort = 0;
  String   casterMount;           // 內建 Caster 掛載點名稱

  bool     ntripEnable = false;   // 推送到外部 NTRIP Caster (NTRIP Server Rev1)
  String   ntripHost;
  uint16_t ntripPort = 2101;
  String   ntripMount;
  String   ntripPass;

  void load();
  void save();
  void clearWifi();
  void factoryReset();
};

extern Settings settings;
