#include "Settings.h"
#include "config.h"
#include <Preferences.h>
#include <esp_mac.h>

Settings settings;

static const char *NS = "rtkbase";

static String defaultApSsid() {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
  char buf[32];
  snprintf(buf, sizeof(buf), DEFAULT_AP_PREFIX "%02X%02X", mac[4], mac[5]);
  return String(buf);
}

void Settings::load() {
  Preferences p;
  p.begin(NS, true);

  ethDhcp  = p.getBool("ethDhcp", true);
  ethIp    = p.getString("ethIp", "192.168.1.200");
  ethMask  = p.getString("ethMask", "255.255.255.0");
  ethGw    = p.getString("ethGw", "192.168.1.1");
  ethDns   = p.getString("ethDns", "8.8.8.8");
  hostname = p.getString("host", DEFAULT_HOSTNAME);

  staSsid = p.getString("staSsid", "");
  staPass = p.getString("staPass", "");
  apSsid  = p.getString("apSsid", "");
  apPass  = p.getString("apPass", DEFAULT_AP_PASS);

  gnssBaud = p.getUInt("gnssBaud", GNSS_DEFAULT_BAUD);

  baseMode         = p.getUChar("baseMode", BASE_MODE_SURVEY_IN);
  surveyMinSec     = p.getUInt("svMin", DEFAULT_SURVEY_MIN_SEC);
  surveyAccM       = p.getFloat("svAcc", DEFAULT_SURVEY_ACC_M);
  fixedLat         = p.getDouble("fixLat", 0);
  fixedLon         = p.getDouble("fixLon", 0);
  fixedH           = p.getDouble("fixH", 0);
  stationId        = p.getUShort("staId", 0);
  inject1005       = p.getBool("inj1005", true);
  rtcm1005Interval = p.getUShort("int1005", DEFAULT_1005_INTERVAL);
  outputDiffNmea   = p.getBool("diffNmea", false);

  tcpEnable   = p.getBool("tcpEn", true);
  tcpPort     = p.getUShort("tcpPort", DEFAULT_TCP_PORT);
  casterMount = p.getString("cMount", DEFAULT_MOUNT);

  ntripEnable = p.getBool("ntEn", false);
  ntripHost   = p.getString("ntHost", "rtk2go.com");
  ntripPort   = p.getUShort("ntPort", 2101);
  ntripMount  = p.getString("ntMount", "");
  ntripPass   = p.getString("ntPass", "");

  p.end();

  if (apSsid.isEmpty()) apSsid = defaultApSsid();
  if (hostname.isEmpty()) hostname = DEFAULT_HOSTNAME;
  if (gnssBaud < 4800) gnssBaud = GNSS_DEFAULT_BAUD;
  if (rtcm1005Interval == 0) rtcm1005Interval = DEFAULT_1005_INTERVAL;
  if (stationId > 4095) stationId = 0;
}

void Settings::save() {
  Preferences p;
  p.begin(NS, false);

  p.putBool("ethDhcp", ethDhcp);
  p.putString("ethIp", ethIp);
  p.putString("ethMask", ethMask);
  p.putString("ethGw", ethGw);
  p.putString("ethDns", ethDns);
  p.putString("host", hostname);

  p.putString("staSsid", staSsid);
  p.putString("staPass", staPass);
  p.putString("apSsid", apSsid);
  p.putString("apPass", apPass);

  p.putUInt("gnssBaud", gnssBaud);

  p.putUChar("baseMode", baseMode);
  p.putUInt("svMin", surveyMinSec);
  p.putFloat("svAcc", surveyAccM);
  p.putDouble("fixLat", fixedLat);
  p.putDouble("fixLon", fixedLon);
  p.putDouble("fixH", fixedH);
  p.putUShort("staId", stationId);
  p.putBool("inj1005", inject1005);
  p.putUShort("int1005", rtcm1005Interval);
  p.putBool("diffNmea", outputDiffNmea);

  p.putBool("tcpEn", tcpEnable);
  p.putUShort("tcpPort", tcpPort);
  p.putString("cMount", casterMount);

  p.putBool("ntEn", ntripEnable);
  p.putString("ntHost", ntripHost);
  p.putUShort("ntPort", ntripPort);
  p.putString("ntMount", ntripMount);
  p.putString("ntPass", ntripPass);

  p.end();
}

void Settings::clearWifi() {
  staSsid = "";
  staPass = "";
  Preferences p;
  p.begin(NS, false);
  p.remove("staSsid");
  p.remove("staPass");
  p.end();
}

void Settings::factoryReset() {
  Preferences p;
  p.begin(NS, false);
  p.clear();
  p.end();
}
