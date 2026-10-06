#include "CorrectionServer.h"
#include "Settings.h"
#include "NetManager.h"
#include "RtkBase.h"
#include "JsonOut.h"
#include "config.h"

#include <WiFi.h>
#include <freertos/ringbuf.h>

namespace {

// ---------------- 內建 TCP / Caster ----------------
struct Slot {
  NetworkClient client;
  bool     used = false;
  bool     streaming = false;
  bool     ntrip = false;
  uint32_t acceptMs = 0;
  uint32_t bytes = 0;
  String   req;
  String   ip;
};

NetworkServer *server = nullptr;
Slot slots[MAX_TCP_CLIENTS];
uint16_t serverPort = 0;

// ---------------- NTRIP Server 任務 ----------------
RingbufHandle_t ntripRing = nullptr;
TaskHandle_t    ntripTask = nullptr;
volatile bool   ntripConnected = false;
uint32_t        ntripBytes = 0;
volatile uint32_t ntripSinceMs = 0;
uint32_t        ntripReconnects = 0;
char            ntripStatus[64] = "停用";
volatile bool   ntripRestartFlag = true;

// 任務內使用的設定副本 (避免與網頁執行緒同時存取 String)
struct NtripCfg {
  bool enable = false;
  String host, mount, pass;
  uint16_t port = 2101;
} ncfg;

void setNtripStatus(const char *s) {
  strncpy(ntripStatus, s, sizeof(ntripStatus) - 1);
  ntripStatus[sizeof(ntripStatus) - 1] = 0;
}

void closeSlot(Slot &s) {
  s.client.stop();
  s.used = false;
  s.streaming = false;
  s.ntrip = false;
  s.req = "";
}

String sourceTable() {
  char line[256];
  snprintf(line, sizeof(line),
           "STR;%s;%s;RTCM 3.x;1005(10);2;GPS+GLO+GAL+BDS;%s;TWN;0.00;0.00;0;0;WT32-ETH01;none;N;N;0;\r\n",
           settings.casterMount.c_str(), settings.casterMount.c_str(), settings.hostname.c_str());
  String body = String(line) + "ENDSOURCETABLE\r\n";
  String r = "SOURCETABLE 200 OK\r\nServer: NTRIP WT32RTK/" FW_VERSION "\r\nContent-Type: text/plain\r\nContent-Length: ";
  r += body.length();
  r += "\r\n\r\n";
  r += body;
  return r;
}

void handleRequest(Slot &s) {
  // 第一行：GET /MOUNT HTTP/1.x
  int sp1 = s.req.indexOf(' ');
  int sp2 = s.req.indexOf(' ', sp1 + 1);
  String path = (sp1 > 0 && sp2 > sp1) ? s.req.substring(sp1 + 1, sp2) : String("/");
  if (path.startsWith("/")) path.remove(0, 1);

  if (path.length() && path.equalsIgnoreCase(settings.casterMount)) {
    s.client.print("ICY 200 OK\r\n\r\n");
    s.streaming = true;
    s.ntrip = true;
    Serial.printf("[Caster] NTRIP Client %s 連線掛載點 /%s\n", s.ip.c_str(), path.c_str());
  } else {
    s.client.print(sourceTable());
    closeSlot(s);
  }
}

void startServer() {
  if (server) {
    for (auto &s : slots) if (s.used) closeSlot(s);
    server->end();
    delete server;
    server = nullptr;
  }
  if (!settings.tcpEnable) return;
  serverPort = settings.tcpPort;
  server = new NetworkServer(serverPort);
  server->begin();
  server->setNoDelay(true);
  Serial.printf("[Caster] TCP/NTRIP 服務啟動於埠 %u，掛載點 /%s\n", serverPort, settings.casterMount.c_str());
}

void serverLoop() {
  if (!server) return;

  NetworkClient c = server->accept();
  if (c) {
    int idx = -1;
    for (int i = 0; i < MAX_TCP_CLIENTS; i++) if (!slots[i].used) { idx = i; break; }
    if (idx < 0) {
      c.print("HTTP/1.0 503 Service Unavailable\r\n\r\n");
      c.stop();
    } else {
      Slot &s = slots[idx];
      s.client = c;
      s.client.setNoDelay(true);
      s.client.setTimeout(200);
      s.used = true;
      s.streaming = false;
      s.ntrip = false;
      s.bytes = 0;
      s.req = "";
      s.acceptMs = millis();
      s.ip = c.remoteIP().toString();
    }
  }

  for (auto &s : slots) {
    if (!s.used) continue;
    if (!s.client.connected()) {
      Serial.printf("[Caster] Client %s 離線\n", s.ip.c_str());
      closeSlot(s);
      continue;
    }
    if (s.streaming) {
      // 丟棄 Client 上傳的資料 (例如 NTRIP Client 回傳的 GGA)
      while (s.client.available()) s.client.read();
      continue;
    }
    // 等待請求標頭
    while (s.client.available() && s.req.length() < 512) s.req += (char)s.client.read();
    if (s.req.indexOf("\r\n\r\n") >= 0 || s.req.indexOf("\n\n") >= 0) {
      if (s.req.startsWith("GET ")) handleRequest(s);
      else { s.streaming = true; }
    } else if (millis() - s.acceptMs > 1500) {
      if (s.req.length() == 0) {
        s.streaming = true;  // 純 TCP Client
        Serial.printf("[Caster] TCP Client %s 連線 (raw)\n", s.ip.c_str());
      } else {
        closeSlot(s);
      }
    }
  }
}

// ---------------- NTRIP Server (推送到外部 Caster) ----------------
bool ntripHandshake(NetworkClient &c) {
  String req = "SOURCE " + ncfg.pass + " /" + ncfg.mount + "\r\n";
  req += "Source-Agent: NTRIP WT32RTK/" FW_VERSION "\r\n";
  req += "STR: \r\n\r\n";
  c.print(req);

  String resp;
  uint32_t t0 = millis();
  while (millis() - t0 < 5000 && c.connected()) {
    while (c.available()) {
      char ch = (char)c.read();
      if (resp.length() < 128) resp += ch;
    }
    if (resp.indexOf('\n') >= 0) break;
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  if (resp.indexOf("200") >= 0) return true;

  resp.trim();
  if (resp.isEmpty()) resp = "Caster 無回應";
  setNtripStatus(("被拒絕: " + resp.substring(0, 40)).c_str());
  return false;
}

void ntripTaskFn(void *) {
  NetworkClient c;
  uint32_t nextTry = 0;
  for (;;) {
    if (ntripRestartFlag) {
      ntripRestartFlag = false;
      ncfg.enable = settings.ntripEnable;
      ncfg.host = settings.ntripHost;
      ncfg.port = settings.ntripPort;
      ncfg.mount = settings.ntripMount;
      ncfg.pass = settings.ntripPass;
      c.stop();
      ntripConnected = false;
      nextTry = 0;
    }

    bool want = ncfg.enable && !ncfg.host.isEmpty() && !ncfg.mount.isEmpty();
    if (!want) {
      if (c.connected()) c.stop();
      ntripConnected = false;
      setNtripStatus("停用");
      size_t sz;
      void *item = xRingbufferReceive(ntripRing, &sz, pdMS_TO_TICKS(200));
      if (item) vRingbufferReturnItem(ntripRing, item);  // 未啟用時丟棄
      continue;
    }

    if (!ntripConnected) {
      if (!NetManager::internetUp()) {
        setNtripStatus("等待網路連線");
        vTaskDelay(pdMS_TO_TICKS(1000));
        continue;
      }
      if (millis() < nextTry) {
        size_t sz;
        void *item = xRingbufferReceive(ntripRing, &sz, pdMS_TO_TICKS(200));
        if (item) vRingbufferReturnItem(ntripRing, item);
        continue;
      }
      setNtripStatus("連線中...");
      c.setTimeout(5000);
      if (c.connect(ncfg.host.c_str(), ncfg.port, 5000) && ntripHandshake(c)) {
        ntripConnected = true;
        ntripSinceMs = millis();
        ntripReconnects++;
        c.setTimeout(3000);
        setNtripStatus("已連線，推送中");
        Serial.printf("[NTRIP] 已連線 %s:%u/%s\n", ncfg.host.c_str(), ncfg.port, ncfg.mount.c_str());
        // 清除連線前累積的舊資料
        size_t sz;
        void *item;
        while ((item = xRingbufferReceive(ntripRing, &sz, 0)) != nullptr) vRingbufferReturnItem(ntripRing, item);
      } else {
        c.stop();
        if (strncmp(ntripStatus, "被拒絕", strlen("被拒絕")) != 0) setNtripStatus("連線失敗，10 秒後重試");
        nextTry = millis() + 10000;
      }
      continue;
    }

    // 已連線：轉送資料
    size_t sz = 0;
    uint8_t *item = (uint8_t *)xRingbufferReceiveUpTo(ntripRing, &sz, pdMS_TO_TICKS(200), 1024);
    if (item) {
      size_t w = c.write(item, sz);
      vRingbufferReturnItem(ntripRing, item);
      if (w != sz) {
        Serial.println("[NTRIP] 寫入失敗，重新連線");
        c.stop();
        ntripConnected = false;
        setNtripStatus("連線中斷，10 秒後重試");
        nextTry = millis() + 10000;
        continue;
      }
      ntripBytes += sz;
    }
    while (c.available()) c.read();
    if (!c.connected()) {
      ntripConnected = false;
      setNtripStatus("Caster 已斷線，10 秒後重試");
      nextTry = millis() + 10000;
    }
  }
}

}  // namespace

namespace CorrectionServer {

void begin() {
  ntripRing = xRingbufferCreate(16 * 1024, RINGBUF_TYPE_BYTEBUF);
  xTaskCreatePinnedToCore(ntripTaskFn, "ntrip", 6144, nullptr, 1, &ntripTask, 0);
  startServer();
}

void restart() {
  startServer();
  ntripRestartFlag = true;
}

void loop() { serverLoop(); }

void broadcast(const uint8_t *data, size_t len) {
  for (auto &s : slots) {
    if (!s.used || !s.streaming) continue;
    size_t w = s.client.write(data, len);
    if (w != len) {
      Serial.printf("[Caster] Client %s 寫入失敗，中斷\n", s.ip.c_str());
      closeSlot(s);
      continue;
    }
    s.bytes += len;
  }
  if (ntripRing && ntripConnected) {
    xRingbufferSend(ntripRing, data, len, 0);
  }
}

String json() {
  String list = "[";
  bool first = true;
  for (auto &s : slots) {
    if (!s.used) continue;
    if (!first) list += ',';
    first = false;
    JsonOut e(128);
    e.add("ip", s.ip)
     .add("type", s.streaming ? (s.ntrip ? "NTRIP" : "TCP") : "握手中")
     .add("bytes", (unsigned long)s.bytes)
     .add("sec", (unsigned long)((millis() - s.acceptMs) / 1000));
    list += e.end();
  }
  list += "]";

  JsonOut tcp(512);
  tcp.add("enable", settings.tcpEnable)
     .add("port", (int)settings.tcpPort)
     .add("mount", settings.casterMount)
     .raw("clients", list);

  JsonOut nt(384);
  nt.add("enable", settings.ntripEnable)
    .add("host", settings.ntripHost)
    .add("port", (int)settings.ntripPort)
    .add("mount", settings.ntripMount)
    .add("connected", (bool)ntripConnected)
    .add("status", String(ntripStatus))
    .add("bytes", (unsigned long)ntripBytes)
    .add("uptime", ntripConnected ? (unsigned long)((millis() - ntripSinceMs) / 1000) : 0UL)
    .add("connects", (unsigned long)ntripReconnects);

  JsonOut j(1024);
  j.raw("tcp", tcp.end()).raw("ntrip", nt.end());
  return j.end();
}

}  // namespace CorrectionServer
