// =============================================================
//  網路管理：RJ45 (LAN8720) + WiFi STA + WiFi AP 同時運作
// =============================================================
#pragma once

#include <Arduino.h>

namespace NetManager {

void begin();
void loop();

// WiFi STA：套用新的 SSID/密碼 (空字串 = 斷線)
void connectSta(const String &ssid, const String &pass);
void disconnectSta();

// WiFi 掃描 (非同步)：回傳 -1 掃描中, -2 未啟動/失敗, >=0 結果數量
void startScan();
int  scanStatus();

bool ethLinkUp();
bool ethHasIp();
bool staConnected();
bool internetUp();  // 任一上行介面 (ETH 或 STA) 已取得 IP

// 輸出各介面狀態 JSON 片段 (不含外層大括號)
String statusJson();
String scanResultJson();

}  // namespace NetManager
