// =============================================================
//  主程式 (.ino) 對外提供的 GNSS 介面
// =============================================================
#pragma once

#include <Arduino.h>
#include "GnssParser.h"

extern GnssParser gnss;

void   gnssSetBaud(uint32_t baud);
String gnssJson();      // 即時 GNSS 定位狀態
String nmeaLogJson();   // 最近 NMEA 語句 (網頁監看)
