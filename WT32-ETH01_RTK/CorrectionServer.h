// =============================================================
//  差分資料輸出
//   - 內建 TCP / NTRIP Caster (預設埠 2101)：
//       * NTRIP Client 送 "GET /掛載點" -> 回 ICY 200 OK 後串流 RTCM
//       * 純 TCP Client (不送任何請求) -> 直接串流 RTCM
//   - NTRIP Server (Rev1)：主動推送至外部 Caster (如 rtk2go.com)
//     於獨立 FreeRTOS 任務執行，避免網路阻塞影響 GNSS 接收
// =============================================================
#pragma once

#include <Arduino.h>

namespace CorrectionServer {

void begin();
void loop();
void restart();  // 設定變更後重新套用

void broadcast(const uint8_t *data, size_t len);

String json();

}  // namespace CorrectionServer
