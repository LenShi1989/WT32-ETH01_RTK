# =============================================================
#  將 WT32-ETH01_RTK/data 打包成 SPIFFS 映像 (spiffs.bin)
#  產生的檔案可在網頁「OTA 更新 -> SPIFFS 網頁映像」上傳，
#  或用 esptool 燒錄到 0x290000：
#    esptool --chip esp32 --port COMx write_flash 0x290000 build\spiffs.bin
#
#  分割區：Default 4MB with spiffs (spiffs @0x290000, 大小 0x160000)
# =============================================================
param(
  [string]$DataDir = "$PSScriptRoot\..\WT32-ETH01_RTK\data",
  [string]$OutFile = "$PSScriptRoot\..\WT32-ETH01_RTK\build\spiffs.bin",
  [int]$Size = 0x160000
)

$mk = Get-ChildItem "$env:LOCALAPPDATA\Arduino15\packages\esp32\tools\mkspiffs" -Recurse -Filter mkspiffs.exe -ErrorAction SilentlyContinue |
      Select-Object -First 1
if (-not $mk) {
  Write-Error "找不到 mkspiffs.exe，請先在 Arduino IDE 安裝 esp32 開發板套件"
  exit 1
}

New-Item -ItemType Directory -Force (Split-Path $OutFile) | Out-Null
& $mk.FullName -c $DataDir -b 4096 -p 256 -s $Size $OutFile
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host "完成：$((Resolve-Path $OutFile).Path)"
