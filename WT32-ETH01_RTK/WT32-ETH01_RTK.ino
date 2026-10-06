// =============================================================
//  WT32-ETH01 RTK 差分定位地面基地站
//
//  硬體：
//    - WT32-ETH01 (ESP32 + LAN8720)
//    - GNSS 接收機 RS232 -> MAX3232 -> RXD2(IO5) / TXD2(IO17)
//  功能：
//    - RJ45 DHCP / 固定 IP 上網，WiFi STA + AP 同時運作
//    - 接收 GNSS (NMEA + RTCM3 混合串流)
//    - Survey-in / 固定座標，計算差分修正量，產生 RTCM 1005
//    - 內建 NTRIP Caster (TCP 2101) 與 NTRIP Server 推送
//    - SPIFFS 網頁介面、OTA 更新
//
//  開發環境：Arduino-ESP32 core 3.x，開發板選 "WT32-ETH01 Ethernet Module"
// =============================================================

#include "config.h"
#include "App.h"
#include "Settings.h"
#include "NetManager.h"
#include "RtkBase.h"
#include "CorrectionServer.h"
#include "WebUI.h"
#include "JsonOut.h"

HardwareSerial GnssSerial(GNSS_UART_NUM);
GnssParser gnss;

// ---------------- NMEA 監看環形緩衝 ----------------
static String   nmeaLog[NMEA_LOG_LINES];
static uint8_t  nmeaHead = 0;
static uint32_t nmeaSeq = 0;
static uint32_t lastByteMs = 0;

// ---------------------------------------------------------------
//  GNSS 接收回呼
// ---------------------------------------------------------------
static void onNmea(const char *line) {
  nmeaLog[nmeaHead] = line;
  nmeaHead = (nmeaHead + 1) % NMEA_LOG_LINES;
  nmeaSeq++;

  // GGA：交給 RTK 基站做 Survey-in / 差分修正量計算
  if (strlen(line) > 6 && !strncmp(line + 3, "GGA", 3)) {
    rtkBase.handleGga(gnss.fix());
  }
}

static void onRtcm(const uint8_t *frame, size_t len, uint16_t type) {
  rtkBase.handleRtcm(frame, len, type);  // 統計並轉送至 Caster / NTRIP
}

static void onCorrectionOut(const uint8_t *data, size_t len) {
  CorrectionServer::broadcast(data, len);
}

void gnssSetBaud(uint32_t baud) {
  GnssSerial.end();
  GnssSerial.setRxBufferSize(GNSS_RX_BUFFER);
  GnssSerial.begin(baud, SERIAL_8N1, GNSS_RX_PIN, GNSS_TX_PIN);
  Serial.printf("[GNSS] UART%d %lu bps (RX=IO%d, TX=IO%d)\n", GNSS_UART_NUM, (unsigned long)baud, GNSS_RX_PIN, GNSS_TX_PIN);
}

static void gnssLoop() {
  uint8_t buf[256];
  int guard = 0;
  while (GnssSerial.available() && guard++ < 32) {
    size_t n = GnssSerial.read(buf, sizeof(buf));
    if (!n) break;
    gnss.feed(buf, n);
    lastByteMs = millis();
  }
}

// ---------------------------------------------------------------
//  JSON
// ---------------------------------------------------------------
String gnssJson() {
  const GnssFix &f = gnss.fix();
  static const char *qName[] = {"無定位", "單點定位", "DGPS", "PPS", "RTK Fixed", "RTK Float", "推估", "手動", "模擬"};
  uint32_t now = millis();

  JsonOut j(900);
  j.add("receiving", lastByteMs && now - lastByteMs < 3000)
   .add("baud", (unsigned long)settings.gnssBaud)
   .add("valid", f.valid)
   .add("quality", (int)f.quality)
   .add("qualityName", f.quality < 9 ? qName[f.quality] : "?")
   .add("fixType", (int)f.fixType)
   .add("sats", (int)f.sats)
   .add("hdop", (double)f.hdop, 2)
   .add("pdop", (double)f.pdop, 2)
   .add("vdop", (double)f.vdop, 2)
   .add("lat", f.lat, 9)
   .add("lon", f.lon, 9)
   .add("altMsl", f.altMsl, 3)
   .add("geoidSep", f.geoidSep, 3)
   .add("hEll", f.ellipsoidHeight(), 3)
   .add("utcTime", f.utcTime)
   .add("utcDate", f.utcDate)
   .add("speedKmh", (double)f.speedKn * 1.852, 2)
   .add("ggaAge", f.lastGgaMs ? (unsigned long)((now - f.lastGgaMs) / 1000) : 9999UL)
   .add("svGps", (int)f.svGps)
   .add("svGlo", (int)f.svGlo)
   .add("svGal", (int)f.svGal)
   .add("svBds", (int)f.svBds)
   .add("svQzss", (int)f.svQzss)
   .add("svOther", (int)f.svOther)
   .add("bytesIn", (unsigned long)gnss.bytesIn)
   .add("nmeaCount", (unsigned long)gnss.nmeaCount)
   .add("nmeaErrors", (unsigned long)gnss.nmeaErrors)
   .add("rtcmCount", (unsigned long)gnss.rtcmCount)
   .add("rtcmErrors", (unsigned long)gnss.rtcmErrors);
  return j.end();
}

String nmeaLogJson() {
  String arr = "[";
  bool first = true;
  for (int i = 0; i < NMEA_LOG_LINES; i++) {
    const String &l = nmeaLog[(nmeaHead + i) % NMEA_LOG_LINES];
    if (l.isEmpty()) continue;
    if (!first) arr += ',';
    first = false;
    arr += jsonEscape(l);
  }
  arr += "]";
  JsonOut j(arr.length() + 32);
  j.add("seq", (unsigned long)nmeaSeq).raw("lines", arr);
  return j.end();
}

// ---------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("========================================");
  Serial.println("  " FW_NAME "  v" FW_VERSION);
  Serial.println("========================================");

  settings.load();

  gnss.onNmea(onNmea);
  gnss.onRtcm(onRtcm);
  gnssSetBaud(settings.gnssBaud);

  NetManager::begin();
  rtkBase.begin(onCorrectionOut);
  CorrectionServer::begin();
  WebUI::begin();
}

void loop() {
  gnssLoop();        // 接收 GPS 訊號
  rtkBase.loop();    // RTK 基站：定時產生 RTCM 1005
  CorrectionServer::loop();
  NetManager::loop();
  WebUI::loop();
  delay(1);
}
