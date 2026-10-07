// =============================================================
//  WT32-ETH01 RTK 差分定位地面基地站
//
//  硬體：
//    - WT32-ETH01 (ESP32 + LAN8720)
//    - GNSS 模組 YIC GT-504GGB (UART/TTL 3.3V，115200 8N1，1Hz)
//        TXD -> RXD2(IO5)、RXD <- TXD2(IO17)、VCC -> 3V3、GND -> GND (詳見 config.h)
//  功能：
//    - RJ45 DHCP / 固定 IP 上網，WiFi STA + AP 同時運作
//    - 接收 GNSS：GGA / GLL / GSA / GSV / RMC / VTG / ZDA (亦可轉送 RTCM3)
//    - Survey-in / 固定座標，計算差分修正量，產生 RTCM 1005
//    - 內建 NTRIP Caster (TCP 2101) 與 NTRIP Server 推送
//    - SPIFFS 網頁介面、OTA 更新
//
//  開發環境：Arduino IDE 1.8.19 + Arduino-ESP32 core 3.3.12，開發板選 "WT32-ETH01 Ethernet Module"
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
static uint32_t gnssBaudNow = 0;   // 目前 UART 實際鮑率 (自動偵測時可能與設定不同)
static uint32_t baudSetMs = 0;

#if GNSS_PPS_PIN >= 0
static volatile uint32_t ppsCount = 0, ppsLastMs = 0;
static void IRAM_ATTR onPps() {
  ppsLastMs = millis();
  ppsCount++;
}
#endif

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
  gnssBaudNow = baud;
  baudSetMs = millis();
  Serial.printf("[GNSS] UART%d %lu bps (RX=IO%d, TX=IO%d)\n", GNSS_UART_NUM, (unsigned long)baud, GNSS_RX_PIN, GNSS_TX_PIN);
}

#if GNSS_AUTO_BAUD
// 有資料進來但一直沒有 checksum 正確的 NMEA → 鮑率不對，輪詢下一個
static void gnssAutoBaud() {
  static const uint32_t bauds[] = {115200, 9600, 38400, 57600, 230400, 460800, 921600, 19200, 4800};
  static const int N = sizeof(bauds) / sizeof(bauds[0]);
  uint32_t now = millis();
  if (now - baudSetMs < 4000) return;                  // 每個鮑率至少觀察 4 秒
  if (!lastByteMs || now - lastByteMs > 2000) return;  // 沒有資料：可能未接線 / 未供電，不輪詢

  bool locked = gnss.lastNmeaMs && (int32_t)(gnss.lastNmeaMs - baudSetMs) > 0 && now - gnss.lastNmeaMs < 4000;
  if (locked) {
    if (gnssBaudNow != settings.gnssBaud) {
      Serial.printf("[GNSS] 偵測到鮑率 %lu bps，已儲存\n", (unsigned long)gnssBaudNow);
      settings.gnssBaud = gnssBaudNow;
      settings.save();
    }
    return;
  }

  int idx = -1;
  for (int i = 0; i < N; i++) if (bauds[i] == gnssBaudNow) idx = i;
  uint32_t next = bauds[(idx + 1) % N];
  Serial.printf("[GNSS] %lu bps 收不到有效 NMEA，改試 %lu bps\n", (unsigned long)gnssBaudNow, (unsigned long)next);
  gnssSetBaud(next);
}
#endif

static void gnssLoop() {
  uint8_t buf[256];
  int guard = 0;
  while (GnssSerial.available() && guard++ < 32) {
    size_t n = GnssSerial.read(buf, sizeof(buf));
    if (!n) break;
    gnss.feed(buf, n);
    lastByteMs = millis();
  }
#if GNSS_AUTO_BAUD
  gnssAutoBaud();
#endif
}

// 衛星列表：[[系統, PRN, 仰角, 方位角, C/N0, 使用中], ...]
static String satListJson(uint32_t now, int *inView, int *used) {
  String a = "[";
  bool first = true;
  for (int i = 0; i < gnss.satCount(); i++) {
    if (!gnss.satFresh(i, now)) continue;
    const GnssSat &s = gnss.sat(i);
    bool u = gnss.satUsed(s);
    inView[s.sys]++;
    if (u) used[s.sys]++;
    char buf[40];
    snprintf(buf, sizeof(buf), "%s[%u,%u,%d,%d,%d,%d]", first ? "" : ",",
             (unsigned)s.sys, (unsigned)s.prn, (int)s.elev, (int)s.az, (int)s.snr, u ? 1 : 0);
    a += buf;
    first = false;
  }
  return a + "]";
}

// ---------------------------------------------------------------
//  JSON
// ---------------------------------------------------------------
String gnssJson() {
  const GnssFix &f = gnss.fix();
  static const char *qName[] = {"無定位", "單點定位", "DGPS", "PPS", "RTK Fixed", "RTK Float", "推估", "手動", "模擬"};
  uint32_t now = millis();

  int inView[SYS_COUNT] = {0}, used[SYS_COUNT] = {0};
  String sats = satListJson(now, inView, used);
  String sysArr = "[";
  for (int i = 0; i < SYS_COUNT; i++) {
    if (i) sysArr += ',';
    sysArr += "[" + jsonEscape(GnssParser::sysName(i)) + "," + inView[i] + "," + used[i] + "]";
  }
  sysArr += "]";

  // 日期：優先使用 ZDA (4 位數年份)，否則用 RMC ddmmyy
  char date[12] = "";
  if (f.zdaYear) snprintf(date, sizeof(date), "%04u-%02u-%02u", (unsigned)f.zdaYear, (unsigned)f.zdaMonth, (unsigned)f.zdaDay);
  else if (strlen(f.utcDate) == 6)
    snprintf(date, sizeof(date), "20%.2s-%.2s-%.2s", f.utcDate + 4, f.utcDate + 2, f.utcDate);

  JsonOut j(1200 + sats.length() + sysArr.length());
  j.add("receiving", lastByteMs && now - lastByteMs < 3000)
   .add("nmeaOk", gnss.lastNmeaMs && now - gnss.lastNmeaMs < 3000)
   .add("baud", (unsigned long)gnssBaudNow)
   .add("autoBaud", (bool)GNSS_AUTO_BAUD)
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
   .add("date", date)
   .add("rmcStatus", String(f.rmcStatus))
   .add("mode", String(f.mode))
   .add("dgpsAge", (double)f.dgpsAge, 1)
   .add("dgpsStation", (int)f.dgpsStation)
   .add("speedKmh", (double)f.speedKn * 1.852, 2)
   .add("course", (double)f.course, 1)
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
#if GNSS_PPS_PIN >= 0
  uint32_t ppsLast = ppsLastMs;
  j.add("pps", ppsCount > 0 && now - ppsLast < 2000)
   .add("ppsCount", (unsigned long)ppsCount);
#endif
  j.raw("systems", sysArr).raw("satList", sats);
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
#if GNSS_PPS_PIN >= 0
  pinMode(GNSS_PPS_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(GNSS_PPS_PIN), onPps, RISING);
#endif

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
