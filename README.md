# WT32-ETH01 RTK 差分定位地面基地站

以 WT32-ETH01 (ESP32 + LAN8720) 實作的 RTK 基地站：接收 GNSS 模組 (YIC GT-504GGB) 資料，決定基站精確座標，
並透過 RJ45 / WiFi 以 NTRIP 或 TCP 提供 RTCM3 差分資料給移動站 (Rover)。

## 功能

| 類別 | 內容 |
|---|---|
| 網路 | RJ45 DHCP / 固定 IP 上網 (預設路由優先走 RJ45)、WiFi STA + AP 同時運作、mDNS (`http://wt32-rtk.local`) |
| GNSS | UART2 接收 GT-504GGB NMEA (GGA / GLL / GSA / GSV / RMC / VTG / ZDA)，checksum 驗證、自動鮑率偵測、逐顆衛星 C/N0 與使用狀態；亦可轉送 RTCM3 (CRC24Q 驗證) |
| RTK 基站 | Survey-in 自動平均定位 / 固定已知座標、位置域差分修正量 (ΔE/ΔN/ΔU)、自產 RTCM 1005 |
| 差分輸出 | 內建 NTRIP Caster (TCP 2101，同時支援純 TCP Client)、NTRIP Server Rev1 推送到外部 Caster (如 rtk2go) |
| 網頁 | SPIFFS 網頁、側邊欄介面：系統狀態 / 網路設定 / RTK 數據 / OTA 更新 |

## 硬體接線 (YIC GT-504GGB → WT32-ETH01)

GT-504GGB 為 **UART/TTL 3.3V** 介面 (VOH ≥ 2.4V / VIH ≥ 2.0V)，**不需要 MAX3232**，直接接 WT32-ETH01 的 RXD2 / TXD2。
預設 115200 bps、8N1、1Hz 更新，NMEA-0183 輸出。

| GT-504GGB 線 (JST 版顏色) | WT32-ETH01 腳位 | 說明 |
|---|---|---|
| VCC (紅) | **3V3** | 模組 3.0~5.5V、約 32mA；接 3V3 確保 UART 輸出為 3.3V 準位 |
| GND (黑) | GND | 共地 |
| TXD (白) | **RXD (IO5 / RXD2)** | 模組輸出 NMEA → ESP32 接收 |
| RXD (綠) | **TXD (IO17 / TXD2)** | ESP32 → 模組 (目前韌體不送指令，可不接) |
| PPS (僅 -N 版) | IO39 (選配) | 1PPS 時間脈衝；啟用需把 `config.h` 的 `GNSS_PPS_PIN` 改為 `39` |

```
 GT-504GGB                      WT32-ETH01
 ┌────────────┐                ┌──────────────────────┐
 │ VCC (紅) ──┼───────────────►│ 3V3                  │
 │ GND (黑) ──┼───────────────►│ GND                  │
 │ TXD (白) ──┼───────────────►│ RXD  (IO5,  RXD2)    │
 │ RXD (綠) ◄─┼────────────────│ TXD  (IO17, TXD2)    │
 │ PPS      ──┼- - - - - - - -►│ IO39 (選配，僅輸入)  │
 └────────────┘                └──────────────────────┘
```

各型號線序 (依規格書第 2 章)：

| 型號 | 接頭 | 線序 |
|---|---|---|
| GT-504GGB-JST | 1.25mm JST 4P | 1 GND 黑 / 2 VCC 紅 / 3 TXD 白 / 4 RXD 綠 |
| GT-504GGB-N2 | 4 線開放端 | VCC / GND / TXD / RXD |
| GT-504GGB-N | 4 線開放端 | VCC / GND / TXD / **PPS** (無 RXD) |
| GT-504GGB-E25 / E35 | 4 極 2.5 / 3.5mm 耳機插頭 | 1 VCC / 2 RXD / 3 TXD / 4 GND |
| GT-504GGB-E253 / E353 | 3 極 2.5 / 3.5mm 耳機插頭 | 1 VCC / 2 TXD / 3 GND |

> ⚠️ WT32-ETH01 腳位圖 (`docs/gpio_pin腳圖.jpg`) 中 TXD 誤標為 IO5，依規格書 V1.4 正確為 **RXD = IO5 (RXD2)、TXD = IO17 (TXD2)**。
> 板上 LED3 / LED4 分別接在 RXD2 / TXD2，收到 GNSS 資料時 LED3 會閃爍。
> IO0 為 LAN8720 的 50MHz 時脈輸入，請勿另作他用。燒錄時 IO0 接 GND 進入下載模式 (USB-TTL 接 TXD0 / RXD0 / GND)。
> 模組 LED：不亮 = 未供電、恆亮 = 搜星中、閃爍 = 已定位。

### GT-504GGB 輸出語句與解析

| 語句 | 解析內容 |
|---|---|
| `$GNGGA` | UTC、經緯度、定位品質 (0 無 / 1 SPS / 2 DGPS / 3 PPS / 6 推估)、使用衛星數、HDOP、海拔、大地起伏、差分齡期 / 站號 |
| `$GNGLL` | 模式指示 (N / A / D / E) |
| `$GNGSA` | 2D/3D、PDOP / HDOP / VDOP、使用中衛星 PRN (每系統一句，以 GNSS System ID 1 GPS / 2 GLONASS / 3 Galileo / 4 BDS 區分) |
| `$GPGSV` `$GLGSV` `$GAGSV` `$GBGSV` | 各系統可見衛星：PRN、仰角、方位角、C/N0 (GPGSV 內 PRN 33~64 為 SBAS、193~ 為 QZSS) |
| `$GNRMC` | 狀態 A/V、日期、速度、航向、模式 |
| `$GNVTG` | 航向、速度 (節 / km/h) |
| `$GNZDA` | 4 位數年份日期 (選配) |

韌體若收到資料但一直沒有 checksum 正確的 NMEA，會自動依序嘗試 115200 / 9600 / 38400 / 57600 / 230400 / 460800 / 921600 / 19200 / 4800 bps，
找到後存入設定 (可在 `config.h` 以 `GNSS_AUTO_BAUD 0` 關閉)。

## 關於「RTK 差分計算」的分工

真正的載波相位 RTK 解算是在**移動站 (Rover)** 端進行，基地站的工作是：

1. **決定基站精確座標**：Survey-in (取樣平均，3D 標準差達門檻且超過最短時間即完成) 或直接輸入已知點座標。
2. **提供差分資料**：
   - 接收機若能輸出原始觀測量 (例如 u-blox ZED-F9P、Unicore UM980 設定輸出 RTCM 1074/1084/1094/1124 MSM)，
     本韌體會把 RTCM3 原封轉送給所有用戶，並在接收機沒有輸出 1005 時，用基站座標自動補上 **RTCM 1005**。
   - 位置域差分修正量 = 基站已知座標 − 即時量測座標 (ENU)，可在網頁觀察，或勾選輸出 `$PRTKD` 語句：
     `$PRTKD,hhmmss.ss,dE,dN,dU,定位品質,衛星數*CS` (單位 m)，移動站把自身位置加上修正量即為 DGPS 位置。
3. 若接收機只輸出 NMEA (一般 GPS 模組)，只能提供位置域 DGPS 修正 (公尺級)，無法達到公分級 RTK。

> 📌 **GT-504GGB 屬於第 3 類**：MediaTek 單頻 (L1/E1/B1I) 模組，只輸出 NMEA、精度約 1.5m CEP，不輸出原始觀測量 / RTCM MSM。
> 搭配本模組時基站提供的是位置域差分修正量 (`$PRTKD`) 與 RTCM 1005 基站座標；要做公分級 RTK 需改用可輸出 RTCM MSM 的接收機
> (如 u-blox ZED-F9P、Unicore UM980)，接線與韌體不需修改。
> Survey-in 3D 標準差門檻預設 2.0m，配合本模組精度建議最短時間設長一些 (例如 1800 秒以上) 讓平均值更穩定。

## 開發環境

1. **Arduino IDE 1.8.19**，「偏好設定 → 額外的開發板管理員網址」加入
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`，
   開發板管理員安裝 **esp32 by Espressif Systems 3.3.12**。
2. 工具選單：開發板選 **WT32-ETH01 Ethernet Module**，Partition Scheme 選 **Default 4MB with spiffs (1.2MB APP/1.5MB SPIFFS)**，
   Upload Speed 115200 / 921600。
3. 不需要任何第三方函式庫 (只用 core 內建的 ETH / WiFi / WebServer / SPIFFS / Update / Preferences)。
4. 開啟 `WT32-ETH01_RTK/WT32-ETH01_RTK.ino` 編譯上傳。

### 上傳網頁 (SPIFFS)，四選一

- **Arduino IDE 1.8.19 外掛**：安裝 [arduino-esp32fs-plugin](https://github.com/me-no-dev/arduino-esp32fs-plugin) 後，
  「工具 → ESP32 Sketch Data Upload」即可上傳 `data/` 資料夾 (上傳時請關閉序列埠監控視窗)。

- **網頁救援頁**：韌體燒好後若 SPIFFS 沒有網頁，瀏覽器開啟裝置 IP 會出現救援頁，可直接選取 `data/` 內所有檔案上傳。
- **OTA**：執行 `powershell -ExecutionPolicy Bypass -File tools\build_spiffs.ps1` 產生 `build\spiffs.bin`，
  在網頁「OTA 更新」選「SPIFFS 網頁映像」上傳。
- **esptool**：`esptool --chip esp32 --port COMx write_flash 0x290000 build\spiffs.bin`

## 第一次使用

1. 接上網路線，裝置透過 DHCP 取得 IP (序列埠 115200 會印出 IP)，或
2. 手機連 WiFi 熱點 `WT32-RTK-XXXX` (密碼 `12345678`)，開啟 `http://192.168.4.1`。
3. 「RTK 地面基地站數據」頁確認 GNSS 接收中、可見衛星與 C/N0；「基站 / 輸出設定」設定基站模式 (GT-504GGB 鮑率預設 115200，會自動偵測)。
4. Rover 的 NTRIP Client 設定：主機 = 基站 IP、埠 = 2101、掛載點 = `WT32RTK` (帳密任意)。

## 檔案結構

```
WT32-ETH01_RTK/
├── WT32-ETH01_RTK.ino   主程式：接收 GPS 訊號、串接各模組
├── config.h             腳位與預設值
├── Settings.*           設定值 (NVS 永久保存)
├── NetManager.*         RJ45 + WiFi STA/AP
├── GnssParser.*         GT-504GGB NMEA (GGA/GLL/GSA/GSV/RMC/VTG/ZDA) / RTCM3 串流解析
├── Geodesy.h            WGS-84 LLA ↔ ECEF ↔ ENU
├── RtkBase.*            Survey-in、差分修正量、RTCM 1005 產生
├── CorrectionServer.*   內建 NTRIP Caster / NTRIP Server 推送
├── WebUI.*              HTTP API、SPIFFS 靜態檔、OTA
├── JsonOut.h            輕量 JSON 輸出
└── data/                SPIFFS 網頁 (index.html / style.css / app.js)
tools/build_spiffs.ps1   產生 SPIFFS 映像
```

## HTTP API

| 方法 | 路徑 | 說明 |
|---|---|---|
| GET | `/api/status` | 系統、網路、SPIFFS 狀態 |
| GET | `/api/config` | 所有設定 (密碼不回傳) |
| POST | `/api/eth` | `dhcp, ip, mask, gw, dns, hostname` (重新啟動) |
| POST | `/api/wifi` | `ssid, pass` |
| POST | `/api/wifi/clear` | 清除 WiFi STA 設定 |
| GET | `/api/wifi/scan[?start=1]` | 啟動 / 取得 WiFi 掃描結果 |
| POST | `/api/ap` | `ssid, pass` (重新啟動) |
| GET | `/api/rtk` | GNSS、基站、差分修正、RTCM、輸出狀態 |
| GET | `/api/nmea` | 最近 NMEA 語句 |
| POST | `/api/rtk/config` | 基站設定 |
| POST | `/api/rtk/restart` | 重新 Survey-in |
| POST | `/api/rtk/savefixed` | Survey-in 結果存為固定座標 |
| POST | `/api/output` | Caster / NTRIP Server 設定 |
| POST | `/api/fs/upload`, `/api/fs/delete`, `/api/fs/format` | SPIFFS 檔案管理 |
| POST | `/api/ota?target=fw\|fs` | OTA 更新韌體 / SPIFFS |
| POST | `/api/reboot`, `/api/factory` | 重新啟動 / 恢復原廠設定 |

> 網頁與 API 沒有登入驗證，請只在受信任的區域網路中使用。
