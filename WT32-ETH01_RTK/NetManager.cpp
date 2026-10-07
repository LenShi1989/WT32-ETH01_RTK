#include "NetManager.h"
#include "Settings.h"
#include "JsonOut.h"
#include "config.h"

#include <ETH.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <DNSServer.h>

namespace {

volatile bool ethLink = false;
volatile bool ethIp = false;
volatile bool staIp = false;
uint32_t staRetryAt = 0;
bool scanRequested = false;
DNSServer dnsServer;  // Captive Portal：AP 用戶端的所有網域都解析到本機

void onNetEvent(arduino_event_id_t event, arduino_event_info_t info) {
  switch (event) {
    case ARDUINO_EVENT_ETH_START:
      ETH.setHostname(settings.hostname.c_str());
      Serial.println("[ETH] 啟動");
      break;
    case ARDUINO_EVENT_ETH_CONNECTED:
      ethLink = true;
      Serial.println("[ETH] 網路線已連接");
      break;
    case ARDUINO_EVENT_ETH_GOT_IP:
      ethIp = true;
      ETH.setDefault();  // 上網優先走 RJ45
      Serial.printf("[ETH] 取得 IP %s (%s)\n", ETH.localIP().toString().c_str(), settings.ethDhcp ? "DHCP" : "Static");
      break;
    case ARDUINO_EVENT_ETH_LOST_IP:
      ethIp = false;
      Serial.println("[ETH] 失去 IP");
      break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
      ethLink = false;
      ethIp = false;
      Serial.println("[ETH] 網路線已拔除");
      if (staIp) WiFi.STA.setDefault();
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      staIp = true;
      if (!ethIp) WiFi.STA.setDefault();
      Serial.printf("[WiFi] STA 取得 IP %s\n", WiFi.localIP().toString().c_str());
      break;
    case ARDUINO_EVENT_WIFI_STA_LOST_IP:
      staIp = false;
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      staIp = false;
      break;
    case ARDUINO_EVENT_WIFI_AP_START:
      Serial.printf("[WiFi] AP 啟動 SSID=%s IP=%s\n", settings.apSsid.c_str(), WiFi.softAPIP().toString().c_str());
      break;
    default:
      break;
  }
}

void startEthernet() {
  // WT32-ETH01：LAN8720, PHY addr 1, MDC 23, MDIO 18, POWER 16, 50MHz clock 由 GPIO0 輸入
  if (!ETH.begin(ETH_PHY_LAN8720, 1, 23, 18, 16, ETH_CLOCK_GPIO0_IN)) {
    Serial.println("[ETH] 初始化失敗");
    return;
  }
  if (!settings.ethDhcp) {
    IPAddress ip, mask, gw, dns;
    if (ip.fromString(settings.ethIp) && mask.fromString(settings.ethMask) && gw.fromString(settings.ethGw)) {
      if (!dns.fromString(settings.ethDns)) dns = gw;
      ETH.config(ip, gw, mask, dns);
    } else {
      Serial.println("[ETH] 固定 IP 格式錯誤，改用 DHCP");
    }
  }
}

void startWifi() {
  WiFi.persistent(false);  // 憑證由 Settings 管理，不寫入 WiFi NVS
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);

  WiFi.softAPConfig(AP_IP, AP_IP, AP_MASK);
  const char *apPass = settings.apPass.length() >= 8 ? settings.apPass.c_str() : nullptr;
  WiFi.softAP(settings.apSsid.c_str(), apPass);

  // 手機/電腦連上 AP 後會探測連網狀態，DNS 全部指向本機即可觸發自動彈出網頁
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(53, "*", AP_IP);

  WiFi.setAutoReconnect(true);
  if (!settings.staSsid.isEmpty()) {
    WiFi.begin(settings.staSsid.c_str(), settings.staPass.c_str());
    Serial.printf("[WiFi] STA 連線至 %s ...\n", settings.staSsid.c_str());
  }
}

String ipStr(const IPAddress &ip) { return ip.toString(); }

const char *authName(wifi_auth_mode_t m) {
  switch (m) {
    case WIFI_AUTH_OPEN:            return "OPEN";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-EAP";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3";
    default:                        return "?";
  }
}

}  // namespace

namespace NetManager {

void begin() {
  Network.onEvent(onNetEvent);
  Network.setHostname(settings.hostname.c_str());
  startWifi();
  startEthernet();

  if (MDNS.begin(settings.hostname.c_str())) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("[mDNS] http://%s.local\n", settings.hostname.c_str());
  }
}

void loop() {
  dnsServer.processNextRequest();

  // STA 長時間連不上時定期重試 (避免持續掃描頻道干擾 AP)
  if (!settings.staSsid.isEmpty() && WiFi.status() != WL_CONNECTED && millis() > staRetryAt) {
    staRetryAt = millis() + 30000;
    if (WiFi.scanComplete() != WIFI_SCAN_RUNNING) {
      WiFi.disconnect(false, false);
      WiFi.begin(settings.staSsid.c_str(), settings.staPass.c_str());
    }
  }
}

void connectSta(const String &ssid, const String &pass) {
  WiFi.disconnect(false, false);
  staIp = false;
  if (ssid.isEmpty()) return;
  WiFi.begin(ssid.c_str(), pass.c_str());
  staRetryAt = millis() + 30000;
}

void disconnectSta() {
  WiFi.disconnect(false, true);
  staIp = false;
}

void startScan() {
  WiFi.scanDelete();
  WiFi.scanNetworks(true /*async*/, false /*hidden*/);
  scanRequested = true;
}

int scanStatus() {
  if (!scanRequested) return -2;
  return WiFi.scanComplete();
}

bool ethLinkUp() { return ethLink; }
bool ethHasIp() { return ethIp; }
bool staConnected() { return staIp && WiFi.status() == WL_CONNECTED; }
bool internetUp() { return ethIp || staConnected(); }

String statusJson() {
  JsonOut eth(384);
  eth.add("link", (bool)ethLink)
     .add("hasIp", (bool)ethIp)
     .add("dhcp", settings.ethDhcp)
     .add("mac", ETH.macAddress())
     .add("ip", ipStr(ETH.localIP()))
     .add("mask", ipStr(ETH.subnetMask()))
     .add("gw", ipStr(ETH.gatewayIP()))
     .add("dns", ipStr(ETH.dnsIP()))
     .add("speed", ethLink ? (int)ETH.linkSpeed() : 0)
     .add("fullDuplex", ethLink ? ETH.fullDuplex() : false)
     .add("isDefault", ethIp && ETH.isDefault());

  bool sc = staConnected();
  JsonOut sta(384);
  sta.add("configured", !settings.staSsid.isEmpty())
     .add("ssid", settings.staSsid)
     .add("connected", sc)
     .add("mac", WiFi.macAddress())
     .add("ip", sc ? ipStr(WiFi.localIP()) : String("0.0.0.0"))
     .add("mask", sc ? ipStr(WiFi.subnetMask()) : String("0.0.0.0"))
     .add("gw", sc ? ipStr(WiFi.gatewayIP()) : String("0.0.0.0"))
     .add("rssi", sc ? (int)WiFi.RSSI() : 0)
     .add("channel", sc ? (int)WiFi.channel() : 0)
     .add("bssid", sc ? WiFi.BSSIDstr() : String(""))
     .add("isDefault", sc && WiFi.STA.isDefault());

  JsonOut ap(256);
  ap.add("ssid", settings.apSsid)
    .add("open", settings.apPass.length() < 8)
    .add("ip", ipStr(WiFi.softAPIP()))
    .add("mac", WiFi.softAPmacAddress())
    .add("clients", (int)WiFi.softAPgetStationNum());

  JsonOut j(1200);
  j.add("hostname", settings.hostname)
   .add("internet", internetUp())
   .raw("eth", eth.end())
   .raw("sta", sta.end())
   .raw("ap", ap.end());
  return j.end();
}

String scanResultJson() {
  int n = scanStatus();
  String list = "[";
  if (n > 0) {
    for (int i = 0; i < n; i++) {
      if (i) list += ',';
      JsonOut e(128);
      e.add("ssid", WiFi.SSID(i))
       .add("rssi", (int)WiFi.RSSI(i))
       .add("ch", (int)WiFi.channel(i))
       .add("auth", authName(WiFi.encryptionType(i)));
      list += e.end();
    }
  }
  list += "]";
  JsonOut j(list.length() + 64);
  j.add("status", n == WIFI_SCAN_RUNNING ? "running" : (n >= 0 ? "done" : "idle"))
   .raw("networks", list);
  return j.end();
}

}  // namespace NetManager
