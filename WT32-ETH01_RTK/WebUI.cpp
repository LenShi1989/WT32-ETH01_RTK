#include "WebUI.h"
#include "App.h"
#include "Settings.h"
#include "NetManager.h"
#include "RtkBase.h"
#include "CorrectionServer.h"
#include "JsonOut.h"
#include "config.h"

#include <WebServer.h>
#include <SPIFFS.h>
#include <Update.h>
#include <esp_system.h>

namespace {

WebServer server(80);
uint32_t rebootAt = 0;

// SPIFFS 檔案上傳暫存
File uploadFile;
String uploadError;

// OTA 狀態
String otaError;
bool otaIsFs = false;

// ---------------------------------------------------------------
void sendJson(const String &json, int code = 200) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json", json);
}

void sendOk(const char *msg = "OK") {
  JsonOut j(64);
  j.add("ok", true).add("msg", msg);
  sendJson(j.end());
}

void sendErr(const String &msg, int code = 400) {
  JsonOut j(96);
  j.add("ok", false).add("msg", msg);
  sendJson(j.end(), code);
}

void scheduleReboot(uint32_t ms = 1500) { rebootAt = millis() + ms; }

String arg(const char *name) { return server.arg(name); }
bool hasArg(const char *name) { return server.hasArg(name); }
bool argBool(const char *name) {
  String v = server.arg(name);
  return v == "1" || v == "true" || v == "on";
}

bool validIp(const String &s) {
  IPAddress ip;
  return ip.fromString(s);
}

const char *resetReasonStr() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "上電重置";
    case ESP_RST_SW:       return "軟體重啟";
    case ESP_RST_PANIC:    return "例外當機";
    case ESP_RST_INT_WDT:  return "中斷看門狗";
    case ESP_RST_TASK_WDT: return "任務看門狗";
    case ESP_RST_WDT:      return "看門狗";
    case ESP_RST_BROWNOUT: return "電壓不足";
    case ESP_RST_EXT:      return "外部重置";
    case ESP_RST_DEEPSLEEP:return "深度睡眠喚醒";
    default:               return "未知";
  }
}

String contentType(const String &path) {
  if (path.endsWith(".html") || path.endsWith(".htm")) return "text/html; charset=utf-8";
  if (path.endsWith(".css"))  return "text/css";
  if (path.endsWith(".js"))   return "application/javascript";
  if (path.endsWith(".json")) return "application/json";
  if (path.endsWith(".png"))  return "image/png";
  if (path.endsWith(".jpg"))  return "image/jpeg";
  if (path.endsWith(".svg"))  return "image/svg+xml";
  if (path.endsWith(".ico"))  return "image/x-icon";
  if (path.endsWith(".txt"))  return "text/plain; charset=utf-8";
  return "application/octet-stream";
}

// ---------------------------------------------------------------
//  SPIFFS 尚未上傳網頁時的備援頁面
// ---------------------------------------------------------------
const char FALLBACK_HTML[] PROGMEM = R"HTML(<!doctype html><html lang="zh-Hant"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>WT32 RTK 救援頁</title>
<style>body{font-family:sans-serif;max-width:640px;margin:24px auto;padding:0 16px;background:#f4f6f9;color:#222}
section{background:#fff;border-radius:8px;padding:16px;margin-bottom:16px;box-shadow:0 1px 3px #0002}
h1{font-size:20px}h2{font-size:16px;margin-top:0}button{padding:6px 14px}progress{width:100%}</style></head><body>
<h1>WT32-ETH01 RTK 基地站</h1>
<section><p>SPIFFS 中找不到 <code>/index.html</code>。請上傳 <code>data/</code> 資料夾內的網頁檔案，或上傳 SPIFFS 映像檔。</p></section>
<section><h2>上傳網頁檔案到 SPIFFS (可多選)</h2>
<input type="file" id="files" multiple><button onclick="upFiles()">上傳</button><p id="fmsg"></p></section>
<section><h2>OTA 更新</h2>
<select id="target"><option value="fw">韌體 (.bin)</option><option value="fs">SPIFFS 映像 (.bin)</option></select>
<input type="file" id="ota"><button onclick="upOta()">更新</button><progress id="pg" value="0" max="100"></progress><p id="omsg"></p></section>
<script>
async function upFiles(){const fs=document.getElementById('files').files,m=document.getElementById('fmsg');
for(const f of fs){const d=new FormData();d.append('file',f,f.name);m.textContent='上傳 '+f.name+' ...';
const r=await fetch('/api/fs/upload',{method:'POST',body:d});const j=await r.json();if(!j.ok){m.textContent='失敗: '+j.msg;return;}}
m.textContent='完成，請重新整理頁面';}
function upOta(){const f=document.getElementById('ota').files[0];if(!f)return;const t=document.getElementById('target').value;
const d=new FormData();d.append('file',f,f.name);const x=new XMLHttpRequest();x.open('POST','/api/ota?target='+t);
x.upload.onprogress=e=>{document.getElementById('pg').value=e.loaded/e.total*100};
x.onload=()=>{document.getElementById('omsg').textContent=x.responseText};x.send(d);}
</script></body></html>)HTML";

// ---------------------------------------------------------------
//  API：系統狀態
// ---------------------------------------------------------------
String systemJson() {
  JsonOut j(768);
  j.add("fwName", FW_NAME)
   .add("fwVersion", FW_VERSION)
   .add("build", __DATE__ " " __TIME__)
   .add("chip", ESP.getChipModel())
   .add("chipRev", (int)ESP.getChipRevision())
   .add("cores", (int)ESP.getChipCores())
   .add("cpuMHz", (int)ESP.getCpuFreqMHz())
   .add("flashSize", (unsigned long)ESP.getFlashChipSize())
   .add("flashSpeed", (unsigned long)ESP.getFlashChipSpeed())
   .add("heapSize", (unsigned long)ESP.getHeapSize())
   .add("heapFree", (unsigned long)ESP.getFreeHeap())
   .add("heapMin", (unsigned long)ESP.getMinFreeHeap())
   .add("heapMaxAlloc", (unsigned long)ESP.getMaxAllocHeap())
   .add("psram", (unsigned long)ESP.getPsramSize())
   .add("sketchSize", (unsigned long)ESP.getSketchSize())
   .add("sketchFree", (unsigned long)ESP.getFreeSketchSpace())
   .add("sdk", ESP.getSdkVersion())
   .add("core", ESP_ARDUINO_VERSION_STR)
   .add("uptime", (unsigned long)(millis() / 1000))
   .add("temp", (double)temperatureRead(), 1)
   .add("resetReason", resetReasonStr());
  return j.end();
}

String spiffsJson() {
  String files = "[";
  bool first = true;
  File root = SPIFFS.open("/");
  if (root) {
    File f = root.openNextFile();
    while (f) {
      if (!first) files += ',';
      first = false;
      JsonOut e(96);
      String name = f.name();
      if (!name.startsWith("/")) name = "/" + name;
      e.add("name", name).add("size", (unsigned long)f.size());
      files += e.end();
      f = root.openNextFile();
    }
  }
  files += "]";
  JsonOut j(files.length() + 96);
  j.add("total", (unsigned long)SPIFFS.totalBytes())
   .add("used", (unsigned long)SPIFFS.usedBytes())
   .raw("files", files);
  return j.end();
}

void apiStatus() {
  JsonOut j(3072);
  j.raw("system", systemJson())
   .raw("net", NetManager::statusJson())
   .raw("spiffs", spiffsJson());
  sendJson(j.end());
}

// ---------------------------------------------------------------
//  API：設定讀取 (密碼不回傳明文)
// ---------------------------------------------------------------
void apiConfig() {
  JsonOut j(1024);
  j.add("ethDhcp", settings.ethDhcp)
   .add("ethIp", settings.ethIp)
   .add("ethMask", settings.ethMask)
   .add("ethGw", settings.ethGw)
   .add("ethDns", settings.ethDns)
   .add("hostname", settings.hostname)
   .add("staSsid", settings.staSsid)
   .add("staHasPass", !settings.staPass.isEmpty())
   .add("apSsid", settings.apSsid)
   .add("apHasPass", settings.apPass.length() >= 8)
   .add("gnssBaud", (unsigned long)settings.gnssBaud)
   .add("baseMode", (int)settings.baseMode)
   .add("surveyMinSec", (unsigned long)settings.surveyMinSec)
   .add("surveyAccM", (double)settings.surveyAccM, 2)
   .add("fixedLat", settings.fixedLat, 9)
   .add("fixedLon", settings.fixedLon, 9)
   .add("fixedH", settings.fixedH, 4)
   .add("stationId", (int)settings.stationId)
   .add("inject1005", settings.inject1005)
   .add("rtcm1005Interval", (int)settings.rtcm1005Interval)
   .add("outputDiffNmea", settings.outputDiffNmea)
   .add("tcpEnable", settings.tcpEnable)
   .add("tcpPort", (int)settings.tcpPort)
   .add("casterMount", settings.casterMount)
   .add("ntripEnable", settings.ntripEnable)
   .add("ntripHost", settings.ntripHost)
   .add("ntripPort", (int)settings.ntripPort)
   .add("ntripMount", settings.ntripMount)
   .add("ntripHasPass", !settings.ntripPass.isEmpty());
  sendJson(j.end());
}

// ---------------------------------------------------------------
//  API：網路設定
// ---------------------------------------------------------------
void apiEth() {
  bool dhcp = argBool("dhcp");
  String host = arg("hostname");
  host.trim();
  if (!dhcp) {
    if (!validIp(arg("ip")) || !validIp(arg("mask")) || !validIp(arg("gw")))
      return sendErr("IP / 子網路遮罩 / 閘道格式錯誤");
    if (arg("dns").length() && !validIp(arg("dns")))
      return sendErr("DNS 格式錯誤");
    settings.ethIp = arg("ip");
    settings.ethMask = arg("mask");
    settings.ethGw = arg("gw");
    settings.ethDns = arg("dns");
  }
  settings.ethDhcp = dhcp;
  if (host.length()) settings.hostname = host;
  settings.save();
  sendOk("已儲存，裝置將重新啟動");
  scheduleReboot();
}

void apiWifi() {
  String ssid = arg("ssid");
  if (ssid.isEmpty()) return sendErr("SSID 不可空白");
  if (ssid.length() > 32) return sendErr("SSID 過長");
  // 密碼欄位留空且 SSID 未變更時，沿用舊密碼
  String pass = arg("pass");
  if (pass.isEmpty() && ssid == settings.staSsid && !argBool("open")) pass = settings.staPass;
  if (pass.length() && pass.length() < 8) return sendErr("WiFi 密碼至少 8 碼");
  settings.staSsid = ssid;
  settings.staPass = pass;
  settings.save();
  NetManager::connectSta(ssid, pass);
  sendOk("已儲存，正在連線");
}

void apiWifiClear() {
  settings.clearWifi();
  NetManager::disconnectSta();
  sendOk("WiFi 設定已清除");
}

void apiWifiScan() {
  if (hasArg("start")) {
    NetManager::startScan();
    return sendOk("掃描中");
  }
  sendJson(NetManager::scanResultJson());
}

void apiAp() {
  String ssid = arg("ssid");
  String pass = arg("pass");
  if (ssid.isEmpty() || ssid.length() > 32) return sendErr("AP SSID 長度需 1~32");
  if (pass.length() && pass.length() < 8) return sendErr("AP 密碼至少 8 碼，留空為開放式");
  settings.apSsid = ssid;
  settings.apPass = pass;
  settings.save();
  sendOk("已儲存，裝置將重新啟動");
  scheduleReboot();
}

// ---------------------------------------------------------------
//  API：RTK 基站
// ---------------------------------------------------------------
void apiRtk() {
  JsonOut j(4096);
  j.raw("gnss", gnssJson())
   .raw("rtk", rtkBase.json())
   .raw("out", CorrectionServer::json());
  sendJson(j.end());
}

void apiNmea() { sendJson(nmeaLogJson()); }

void apiRtkConfig() {
  int mode = arg("baseMode").toInt();
  if (mode != BASE_MODE_SURVEY_IN && mode != BASE_MODE_FIXED) return sendErr("模式錯誤");

  long svMin = arg("surveyMinSec").toInt();
  float svAcc = arg("surveyAccM").toFloat();
  if (svMin < 10 || svMin > 86400) return sendErr("Survey-in 時間需 10~86400 秒");
  if (svAcc < 0.01f || svAcc > 100) return sendErr("精度門檻需 0.01~100 m");

  double lat = arg("fixedLat").toDouble(), lon = arg("fixedLon").toDouble(), h = arg("fixedH").toDouble();
  if (mode == BASE_MODE_FIXED) {
    if (lat < -90 || lat > 90 || lon < -180 || lon > 180 || (lat == 0 && lon == 0))
      return sendErr("固定座標無效");
    if (h < -1000 || h > 10000) return sendErr("橢球高無效");
  }

  long sid = arg("stationId").toInt();
  if (sid < 0 || sid > 4095) return sendErr("基站 ID 需 0~4095");
  long iv = arg("rtcm1005Interval").toInt();
  if (iv < 1 || iv > 300) return sendErr("1005 間隔需 1~300 秒");
  long baud = arg("gnssBaud").toInt();
  if (baud < 4800 || baud > 921600) return sendErr("鮑率錯誤");

  settings.baseMode = (uint8_t)mode;
  settings.surveyMinSec = (uint32_t)svMin;
  settings.surveyAccM = svAcc;
  if (mode == BASE_MODE_FIXED) {
    settings.fixedLat = lat;
    settings.fixedLon = lon;
    settings.fixedH = h;
  }
  settings.stationId = (uint16_t)sid;
  settings.inject1005 = argBool("inject1005");
  settings.rtcm1005Interval = (uint16_t)iv;
  settings.outputDiffNmea = argBool("outputDiffNmea");
  bool baudChanged = settings.gnssBaud != (uint32_t)baud;
  settings.gnssBaud = (uint32_t)baud;
  settings.save();

  if (baudChanged) gnssSetBaud(settings.gnssBaud);
  rtkBase.restart();
  sendOk("基站設定已套用");
}

void apiRtkRestart() {
  rtkBase.restart();
  sendOk("已重新開始");
}

void apiRtkSaveFixed() {
  if (!rtkBase.saveSurveyAsFixed()) return sendErr("尚未取得基站座標");
  sendOk("Survey-in 結果已存為固定座標");
}

void apiOutput() {
  long port = arg("tcpPort").toInt();
  if (port < 1 || port > 65535 || port == 80) return sendErr("TCP 埠號錯誤");
  String cm = arg("casterMount");
  cm.trim();
  if (cm.isEmpty()) return sendErr("掛載點不可空白");

  bool ntEn = argBool("ntripEnable");
  long ntPort = arg("ntripPort").toInt();
  String ntHost = arg("ntripHost"), ntMount = arg("ntripMount");
  ntHost.trim();
  ntMount.trim();
  if (ntMount.startsWith("/")) ntMount.remove(0, 1);
  if (ntEn && (ntHost.isEmpty() || ntMount.isEmpty() || ntPort < 1 || ntPort > 65535))
    return sendErr("NTRIP Caster 主機 / 埠 / 掛載點不完整");

  settings.tcpEnable = argBool("tcpEnable");
  settings.tcpPort = (uint16_t)port;
  settings.casterMount = cm;
  settings.ntripEnable = ntEn;
  settings.ntripHost = ntHost;
  if (ntPort > 0 && ntPort <= 65535) settings.ntripPort = (uint16_t)ntPort;
  settings.ntripMount = ntMount;
  if (hasArg("ntripPass") && arg("ntripPass").length()) settings.ntripPass = arg("ntripPass");
  settings.save();
  CorrectionServer::restart();
  sendOk("輸出設定已套用");
}

// ---------------------------------------------------------------
//  SPIFFS 檔案管理
// ---------------------------------------------------------------
void fsUploadChunk() {
  HTTPUpload &up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    uploadError = "";
    String name = up.filename;
    int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);
    if (name.isEmpty() || name.length() > 30) { uploadError = "檔名長度需 1~30 字元"; return; }
    uploadFile = SPIFFS.open("/" + name, FILE_WRITE);
    if (!uploadFile) uploadError = "無法建立檔案";
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (uploadFile && uploadFile.write(up.buf, up.currentSize) != up.currentSize) {
      uploadError = "寫入失敗 (空間不足?)";
      uploadFile.close();
    }
  } else if (up.status == UPLOAD_FILE_END || up.status == UPLOAD_FILE_ABORTED) {
    if (uploadFile) uploadFile.close();
    if (up.status == UPLOAD_FILE_ABORTED) uploadError = "上傳中斷";
  }
}

void fsUploadDone() {
  if (uploadError.length()) return sendErr(uploadError);
  sendOk("上傳完成");
}

void fsDelete() {
  String name = arg("name");
  if (!name.startsWith("/")) name = "/" + name;
  if (!SPIFFS.exists(name)) return sendErr("檔案不存在", 404);
  SPIFFS.remove(name);
  sendOk("已刪除");
}

void fsFormat() {
  SPIFFS.format();
  sendOk("SPIFFS 已格式化");
}

// ---------------------------------------------------------------
//  OTA
// ---------------------------------------------------------------
void otaChunk() {
  HTTPUpload &up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    otaError = "";
    otaIsFs = server.arg("target") == "fs";
    Serial.printf("[OTA] 開始更新 %s：%s\n", otaIsFs ? "SPIFFS" : "韌體", up.filename.c_str());
    if (otaIsFs) SPIFFS.end();
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, otaIsFs ? U_SPIFFS : U_FLASH)) {
      otaError = String("Update.begin 失敗：") + Update.errorString();
    }
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (otaError.isEmpty() && Update.write(up.buf, up.currentSize) != up.currentSize) {
      otaError = String("寫入失敗：") + Update.errorString();
    }
  } else if (up.status == UPLOAD_FILE_END) {
    if (otaError.isEmpty() && !Update.end(true)) {
      otaError = String("驗證失敗：") + Update.errorString();
    }
    Serial.printf("[OTA] 結束，%u bytes %s\n", up.totalSize, otaError.isEmpty() ? "成功" : otaError.c_str());
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    otaError = "上傳中斷";
  }
}

void otaDone() {
  if (otaError.length()) {
    if (otaIsFs) SPIFFS.begin(true);
    return sendErr(otaError, 500);
  }
  sendOk("更新成功，裝置將重新啟動");
  scheduleReboot(2000);
}

// ---------------------------------------------------------------
//  靜態檔案
// ---------------------------------------------------------------
void serveStatic() {
  String path = server.uri();
  if (path.endsWith("/")) path += "index.html";

  if (path.startsWith("/api/")) return sendErr("未知的 API", 404);

  String gz = path + ".gz";
  if (SPIFFS.exists(gz) || SPIFFS.exists(path)) {
    bool useGz = SPIFFS.exists(gz);
    File f = SPIFFS.open(useGz ? gz : path, FILE_READ);
    if (useGz) server.sendHeader("Content-Encoding", "gzip");
    server.sendHeader("Cache-Control", "max-age=60");
    server.streamFile(f, contentType(path));
    f.close();
    return;
  }

  if (path == "/index.html") {
    server.send_P(200, "text/html; charset=utf-8", FALLBACK_HTML);
    return;
  }
  server.send(404, "text/plain; charset=utf-8", "404 找不到檔案");
}

}  // namespace

namespace WebUI {

void begin() {
  if (!SPIFFS.begin(true)) Serial.println("[SPIFFS] 掛載失敗");

  server.on("/api/status", HTTP_GET, apiStatus);
  server.on("/api/config", HTTP_GET, apiConfig);

  server.on("/api/eth", HTTP_POST, apiEth);
  server.on("/api/wifi", HTTP_POST, apiWifi);
  server.on("/api/wifi/clear", HTTP_POST, apiWifiClear);
  server.on("/api/wifi/scan", HTTP_GET, apiWifiScan);
  server.on("/api/ap", HTTP_POST, apiAp);

  server.on("/api/rtk", HTTP_GET, apiRtk);
  server.on("/api/nmea", HTTP_GET, apiNmea);
  server.on("/api/rtk/config", HTTP_POST, apiRtkConfig);
  server.on("/api/rtk/restart", HTTP_POST, apiRtkRestart);
  server.on("/api/rtk/savefixed", HTTP_POST, apiRtkSaveFixed);
  server.on("/api/output", HTTP_POST, apiOutput);

  server.on("/api/fs/upload", HTTP_POST, fsUploadDone, fsUploadChunk);
  server.on("/api/fs/delete", HTTP_POST, fsDelete);
  server.on("/api/fs/format", HTTP_POST, fsFormat);

  server.on("/api/ota", HTTP_POST, otaDone, otaChunk);
  server.on("/api/reboot", HTTP_POST, []() { sendOk("裝置將重新啟動"); scheduleReboot(); });
  server.on("/api/factory", HTTP_POST, []() {
    settings.factoryReset();
    sendOk("已恢復原廠設定，裝置將重新啟動");
    scheduleReboot();
  });

  server.onNotFound(serveStatic);
  server.begin();
  Serial.println("[Web] HTTP 伺服器啟動於埠 80");
}

void loop() {
  server.handleClient();
  if (rebootAt && millis() > rebootAt) {
    Serial.println("[System] 重新啟動...");
    delay(100);
    ESP.restart();
  }
}

}  // namespace WebUI
