// =============================================================
//  WT32-ETH01 RTK 地面基地站 - 全域設定
// =============================================================
#pragma once

#include <Arduino.h>

#if !defined(ESP_ARDUINO_VERSION_MAJOR) || ESP_ARDUINO_VERSION_MAJOR < 3
#error "本專案需要 Arduino-ESP32 core 3.x 以上版本 (開發板管理員 -> esp32 by Espressif Systems)"
#endif

#define FW_NAME    "WT32-ETH01 RTK Base"
#define FW_VERSION "1.0.0"

// ---------- GNSS 序列埠 (RS232 需經 MAX3232 轉 TTL) ----------
// WT32-ETH01 板上標示 RXD2 = IO5, TXD2 = IO17
#define GNSS_UART_NUM      2
#define GNSS_RX_PIN        5
#define GNSS_TX_PIN        17
#define GNSS_DEFAULT_BAUD  115200
#define GNSS_RX_BUFFER     8192

// ---------- 網路預設值 ----------
#define DEFAULT_HOSTNAME   "wt32-rtk"
#define DEFAULT_AP_PREFIX  "WT32-RTK-"    // 後面接 MAC 末 4 碼
#define DEFAULT_AP_PASS    "12345678"     // 至少 8 碼，空字串則為開放式 AP
#define AP_IP              IPAddress(192, 168, 4, 1)
#define AP_MASK            IPAddress(255, 255, 255, 0)

// ---------- 差分資料輸出 ----------
#define DEFAULT_TCP_PORT   2101           // 內建 TCP / NTRIP Caster 埠
#define MAX_TCP_CLIENTS    4
#define DEFAULT_MOUNT      "WT32RTK"

// ---------- RTK 基站預設值 ----------
#define DEFAULT_SURVEY_MIN_SEC   300      // Survey-in 最短時間 (秒)
#define DEFAULT_SURVEY_ACC_M     2.0      // Survey-in 3D 標準差門檻 (公尺)
#define DEFAULT_1005_INTERVAL    10       // 自產 RTCM 1005 間隔 (秒)

// ---------- 其他 ----------
#define NMEA_LOG_LINES     40             // 網頁上 NMEA 即時監看保留行數
