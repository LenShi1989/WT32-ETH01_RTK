// =============================================================
//  RTK 地面基地站核心：
//   1. Survey-in：累積 GNSS 定位求平均，決定基站精確座標
//   2. 固定座標模式：使用者輸入已知點座標
//   3. 差分修正量計算：已知座標 - 即時量測 (ECEF / ENU)
//   4. 產生 RTCM3 1005 (基站天線參考點) 並與接收機 RTCM 觀測量一併輸出
// =============================================================
#pragma once

#include <Arduino.h>
#include "Geodesy.h"
#include "GnssParser.h"

enum BaseState : uint8_t {
  BASE_WAIT_FIX = 0,   // 等待 GNSS 定位
  BASE_SURVEYING = 1,  // Survey-in 進行中
  BASE_READY = 2,      // 基站座標已確定，正常輸出差分
};

struct RtcmStat {
  uint16_t type = 0;
  uint32_t count = 0;
  uint32_t lastMs = 0;
  uint32_t bytes = 0;
};

class RtkBase {
public:
  typedef void (*OutputCb)(const uint8_t *data, size_t len);

  void begin(OutputCb out);
  void loop();

  void handleGga(const GnssFix &fix);
  void handleRtcm(const uint8_t *frame, size_t len, uint16_t type);

  void restart();            // 依目前設定重新開始 (Survey-in 會重新累積)
  bool saveSurveyAsFixed();  // 將 Survey-in 結果存為固定座標

  String json() const;
  String rtcmStatsJson() const;

  BaseState state() const { return _state; }

private:
  OutputCb _out = nullptr;
  BaseState _state = BASE_WAIT_FIX;

  // ---- 基站座標 ----
  Geo::Ecef _baseEcef;
  Geo::Lla  _baseLla;
  bool      _baseFromSurvey = false;

  // ---- Survey-in 統計 (Welford) ----
  uint32_t  _svN = 0;
  uint32_t  _svStartMs = 0;
  double    _mx = 0, _my = 0, _mz = 0;
  double    _sx = 0, _sy = 0, _sz = 0;
  double svStd3d() const;
  uint32_t svElapsed() const;

  // ---- 即時差分修正量 ----
  Geo::Enu  _corr;          // 修正量 = 基站已知座標 - 量測座標
  bool      _corrValid = false;
  double    _hErrSq = 0, _vErrSq = 0;
  uint32_t  _corrN = 0;
  double    _hErrMax = 0;

  // ---- RTCM ----
  static const int MAX_TYPES = 24;
  RtcmStat  _stats[MAX_TYPES];
  uint32_t  _rtcmBytesOut = 0;
  uint32_t  _last1005Ms = 0;
  uint32_t  _sent1005 = 0;
  bool      _rxHas1005 = false;   // 接收機自己就有送 1005/1006
  uint32_t  _rx1005Ms = 0;
  Geo::Ecef _rxArp;               // 接收機回報的 ARP
  uint16_t  _rxStationId = 0;

  void setBase(const Geo::Ecef &e, bool fromSurvey);
  void send1005();
  void sendDiffNmea(const GnssFix &fix);
  void countRtcm(uint16_t type, size_t len);
};

extern RtkBase rtkBase;
