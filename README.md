# WT32-ETH01 RTK 差分定位地面基地站

以 WT32-ETH01 (ESP32 + LAN8720) 實作的 RTK 基地站：接收 RS232 GNSS 接收機資料，決定基站精確座標，
並透過 RJ45 / WiFi 以 NTRIP 或 TCP 提供 RTCM3 差分資料給移動站 (Rover)。

## 功能

| 類別 | 內容 |
|---|---|
| 網路 | RJ45 DHCP / 固定 IP 上網 (預設路由優先走 RJ45)、WiFi STA + AP 同時運作、mDNS (`http://wt32-rtk.local`) |
| GNSS | UART2 接收 NMEA + RTCM3 混合串流，NMEA checksum 與 RTCM CRC24Q 驗證 |
| RTK 基站 | Survey-in 自動平均定位 / 固定已知座標、位置域差分修正量 (ΔE/ΔN/ΔU)、自產 RTCM 1005 |
| 差分輸出 | 內建 NTRIP Caster (TCP 2101，同時支援純 TCP Client)、NTRIP Server Rev1 推送到外部 Caster (如 rtk2go) |
| 網頁 | SPIFFS 網頁、側邊欄介面：系統狀態 / 網路設定 / RTK 數據 / OTA 更新 |

## 硬體接線

```
GNSS 接收機 RS232 ──► MAX3232 (RS232 ↔ 3.3V TTL) ──► WT32-ETH01
        TXD  ────────────► R1IN  R1OUT ─────────────► RXD2 (IO5)
        RXD  ◄──────────── T1OUT T1IN  ◄───────────── TXD2 (IO17)
        GND  ─────────────────── GND ──────────────── GND
```

> ⚠️ RS232 電壓為 ±12V，**不可**直接接到 ESP32 腳位，必須經過 MAX3232 等電位轉換。
> IO0 為 LAN8720 的 50MHz 時脈輸入，請勿另作他用。燒錄時 IO0 接 GND 進入下載模式。

## 關於「RTK 差分計算」的分工

真正的載波相位 RTK 解算是在**移動站 (Rover)** 端進行，基地站的工作是：

1. **決定基站精確座標**：Survey-in (取樣平均，3D 標準差達門檻且超過最短時間即完成) 或直接輸入已知點座標。
2. **提供差分資料**：
   - 接收機若能輸出原始觀測量 (例如 u-blox ZED-F9P、Unicore UM980 設定輸出 RTCM 1074/1084/1094/1124 MSM)，
     本韌體會把 RTCM3 原封轉送給所有用戶，並在接收機沒有輸出 1005 時，用基站座標自動補上 **RTCM 1005**。
   - 位置域差分修正量 = 基站已知座標 − 即時量測座標 (ENU)，可在網頁觀察，或勾選輸出 `$PRTKD` 語句：
     `$PRTKD,hhmmss.ss,dE,dN,dU,定位品質,衛星數*CS` (單位 m)，移動站把自身位置加上修正量即為 DGPS 位置。
3. 若接收機只輸出 NMEA (一般 GPS 模組)，只能提供位置域 DGPS 修正 (公尺級)，無法達到公分級 RTK。

## 開發環境

1. Arduino IDE 2.x，開發板管理員安裝 **esp32 by Espressif Systems 3.x** (已用 3.3.6 驗證編譯)。
2. 開發板選 **WT32-ETH01 Ethernet Module**，Partition Scheme 選 **Default 4MB with spiffs (1.2MB APP/1.5MB SPIFFS)**。
3. 不需要任何第三方函式庫 (只用 core 內建的 ETH / WiFi / WebServer / SPIFFS / Update / Preferences)。
4. 開啟 `WT32-ETH01_RTK/WT32-ETH01_RTK.ino` 編譯上傳。

### 上傳網頁 (SPIFFS)，三選一

- **網頁救援頁**：韌體燒好後若 SPIFFS 沒有網頁，瀏覽器開啟裝置 IP 會出現救援頁，可直接選取 `data/` 內所有檔案上傳。
- **OTA**：執行 `powershell -ExecutionPolicy Bypass -File tools\build_spiffs.ps1` 產生 `build\spiffs.bin`，
  在網頁「OTA 更新」選「SPIFFS 網頁映像」上傳。
- **esptool**：`esptool --chip esp32 --port COMx write_flash 0x290000 build\spiffs.bin`

## 第一次使用

1. 接上網路線，裝置透過 DHCP 取得 IP (序列埠 115200 會印出 IP)，或
2. 手機連 WiFi 熱點 `WT32-RTK-XXXX` (密碼 `12345678`)，開啟 `http://192.168.4.1`。
3. 「基站 / 輸出設定」設定 GNSS 鮑率 (預設 115200) 與基站模式。
4. Rover 的 NTRIP Client 設定：主機 = 基站 IP、埠 = 2101、掛載點 = `WT32RTK` (帳密任意)。

## 檔案結構

```
WT32-ETH01_RTK/
├── WT32-ETH01_RTK.ino   主程式：接收 GPS 訊號、串接各模組
├── config.h             腳位與預設值
├── Settings.*           設定值 (NVS 永久保存)
├── NetManager.*         RJ45 + WiFi STA/AP
├── GnssParser.*         NMEA / RTCM3 串流解析
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
