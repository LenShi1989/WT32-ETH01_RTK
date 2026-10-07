'use strict';

// ============================================================
//  共用工具
// ============================================================
const $ = (id) => document.getElementById(id);

function esc(s) {
  return String(s ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}

function fmtBytes(n) {
  if (n == null) return '-';
  if (n < 1024) return n + ' B';
  if (n < 1048576) return (n / 1024).toFixed(1) + ' KB';
  return (n / 1048576).toFixed(2) + ' MB';
}

function fmtDur(sec) {
  sec = Math.floor(sec || 0);
  const d = Math.floor(sec / 86400), h = Math.floor(sec % 86400 / 3600), m = Math.floor(sec % 3600 / 60), s = sec % 60;
  return (d ? d + ' 天 ' : '') + String(h).padStart(2, '0') + ':' + String(m).padStart(2, '0') + ':' + String(s).padStart(2, '0');
}

function num(v, d = 3) {
  return v == null || Number.isNaN(v) ? '-' : Number(v).toFixed(d);
}

function kv(tableId, rows) {
  $(tableId).innerHTML = rows.map(([k, v]) => `<tr><td>${esc(k)}</td><td>${v === undefined ? '-' : esc(v)}</td></tr>`).join('');
}

let toastTimer;
function toast(msg, isErr) {
  const t = $('toast');
  t.textContent = msg;
  t.className = 'toast show' + (isErr ? ' err' : '');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => (t.className = 'toast'), 3000);
}

async function getJson(url) {
  const r = await fetch(url, { cache: 'no-store' });
  if (!r.ok) throw new Error('HTTP ' + r.status);
  return r.json();
}

async function post(url, data) {
  const body = new URLSearchParams();
  if (data) for (const [k, v] of Object.entries(data)) body.append(k, v);
  try {
    const r = await fetch(url, { method: 'POST', body });
    const j = await r.json();
    toast(j.msg, !j.ok);
    return j;
  } catch (e) {
    toast('連線失敗：' + e.message, true);
    return { ok: false };
  }
}

function formData(form) {
  const o = {};
  for (const el of form.elements) {
    if (!el.name) continue;
    o[el.name] = el.type === 'checkbox' ? (el.checked ? '1' : '0') : el.value;
  }
  return o;
}

function fillForm(form, cfg) {
  for (const el of form.elements) {
    if (!el.name || !(el.name in cfg)) continue;
    if (el.type === 'checkbox') el.checked = !!cfg[el.name];
    else el.value = cfg[el.name];
  }
}

function setPill(id, cls) {
  $(id).className = 'pill' + (cls ? ' ' + cls : '');
}

// ============================================================
//  頁面切換 / 側邊欄
// ============================================================
const pages = ['net-info', 'spiffs', 'sys-info', 'eth', 'wifi', 'rtk', 'rtk-cfg', 'ota'];
let currentPage = 'net-info';

function showPage(p) {
  if (!pages.includes(p)) p = 'net-info';
  currentPage = p;
  document.querySelectorAll('.page').forEach((el) => el.classList.toggle('active', el.id === 'page-' + p));
  document.querySelectorAll('.sidebar a').forEach((a) => a.classList.toggle('active', a.dataset.page === p));
  $('sidebar').classList.remove('open');
  $('backdrop').classList.remove('show');
  if (['eth', 'wifi', 'rtk-cfg'].includes(p)) loadConfig();
  refreshStatus();
  if (p === 'rtk') refreshRtk();
}

window.addEventListener('hashchange', () => showPage(location.hash.slice(1)));
$('menuBtn').onclick = () => {
  $('sidebar').classList.toggle('open');
  $('backdrop').classList.toggle('show');
};
$('backdrop').onclick = () => {
  $('sidebar').classList.remove('open');
  $('backdrop').classList.remove('show');
};

// ============================================================
//  系統狀態 (網路 / SPIFFS / 系統資訊)
// ============================================================
let lastStatus = null;

async function refreshStatus() {
  let s;
  try {
    s = await getJson('/api/status');
  } catch (e) {
    setPill('pillEth', 'bad');
    return;
  }
  lastStatus = s;
  const n = s.net, sys = s.system, fs = s.spiffs;

  // 上方狀態燈
  setPill('pillEth', n.eth.hasIp ? 'ok' : n.eth.link ? 'warn' : 'bad');
  setPill('pillWifi', n.sta.connected ? 'ok' : n.sta.configured ? 'warn' : '');
  $('navFoot').textContent = `${sys.fwName} v${sys.fwVersion}`;

  // ---- 網路 ----
  const ethB = $('ethBadge');
  ethB.textContent = n.eth.hasIp ? '已連線' : n.eth.link ? '取得 IP 中' : '未接網路線';
  ethB.className = 'badge ' + (n.eth.hasIp ? 'ok' : n.eth.link ? 'warn' : 'bad');
  kv('ethTable', [
    ['連線模式', n.eth.dhcp ? 'DHCP' : '固定 IP'],
    ['連線速度', n.eth.link ? `${n.eth.speed} Mbps ${n.eth.fullDuplex ? '全雙工' : '半雙工'}` : '-'],
    ['IP 位址', n.eth.ip],
    ['子網路遮罩', n.eth.mask],
    ['預設閘道', n.eth.gw],
    ['DNS', n.eth.dns],
    ['MAC', n.eth.mac],
    ['預設上網路由', n.eth.isDefault ? '是' : '否'],
  ]);

  const staB = $('staBadge');
  staB.textContent = n.sta.connected ? '已連線' : n.sta.configured ? '連線中' : '未設定';
  staB.className = 'badge ' + (n.sta.connected ? 'ok' : n.sta.configured ? 'warn' : '');
  $('wifiStaBadge').textContent = staB.textContent;
  $('wifiStaBadge').className = staB.className;
  kv('staTable', [
    ['SSID', n.sta.ssid || '-'],
    ['IP 位址', n.sta.ip],
    ['子網路遮罩', n.sta.mask],
    ['預設閘道', n.sta.gw],
    ['訊號強度', n.sta.connected ? n.sta.rssi + ' dBm' : '-'],
    ['頻道', n.sta.channel || '-'],
    ['BSSID', n.sta.bssid || '-'],
    ['MAC', n.sta.mac],
    ['預設上網路由', n.sta.isDefault ? '是' : '否'],
  ]);
  kv('apTable', [
    ['SSID', n.ap.ssid],
    ['加密', n.ap.open ? '開放' : 'WPA2'],
    ['IP 位址', n.ap.ip],
    ['MAC', n.ap.mac],
    ['已連線裝置', n.ap.clients],
    ['主機名稱', n.hostname + '.local'],
  ]);
  $('ethCurIp').textContent = n.eth.ip;

  // ---- SPIFFS ----
  const pct = fs.total ? (fs.used / fs.total) * 100 : 0;
  $('fsBar').style.width = pct.toFixed(1) + '%';
  $('fsText').textContent = `已使用 ${fmtBytes(fs.used)} / 共 ${fmtBytes(fs.total)} (${pct.toFixed(1)}%)，剩餘 ${fmtBytes(fs.total - fs.used)}`;
  $('fsList').innerHTML = fs.files.length
    ? fs.files.map((f) => `<tr><td><a href="${esc(f.name)}" target="_blank">${esc(f.name)}</a></td><td class="r">${fmtBytes(f.size)}</td>
        <td class="r"><button class="danger" data-del="${esc(f.name)}">刪除</button></td></tr>`).join('')
    : '<tr><td colspan="3" class="muted">沒有檔案</td></tr>';

  // ---- 系統資訊 ----
  kv('fwTable', [
    ['名稱', sys.fwName],
    ['版本', sys.fwVersion],
    ['編譯時間', sys.build],
    ['Arduino Core', sys.core],
    ['ESP-IDF', sys.sdk],
    ['運行時間', fmtDur(sys.uptime)],
    ['上次重置原因', sys.resetReason],
  ]);
  kv('chipTable', [
    ['晶片型號', `${sys.chip} rev ${sys.chipRev}`],
    ['核心數', sys.cores],
    ['CPU 時脈', sys.cpuMHz + ' MHz'],
    ['Flash 容量', fmtBytes(sys.flashSize)],
    ['Flash 速度', (sys.flashSpeed / 1e6).toFixed(0) + ' MHz'],
    ['晶片溫度', num(sys.temp, 1) + ' °C'],
  ]);
  kv('memTable', [
    ['Heap 總量', fmtBytes(sys.heapSize)],
    ['Heap 可用', fmtBytes(sys.heapFree)],
    ['Heap 歷史最低', fmtBytes(sys.heapMin)],
    ['最大可配置區塊', fmtBytes(sys.heapMaxAlloc)],
    ['PSRAM', sys.psram ? fmtBytes(sys.psram) : '無'],
    ['韌體大小', fmtBytes(sys.sketchSize)],
    ['OTA 可用空間', fmtBytes(sys.sketchFree)],
  ]);
  kv('otaInfo', [
    ['目前版本', sys.fwVersion + ' (' + sys.build + ')'],
    ['韌體大小', fmtBytes(sys.sketchSize)],
    ['OTA 分割區可用', fmtBytes(sys.sketchFree)],
    ['SPIFFS 容量', fmtBytes(fs.total)],
  ]);
}

$('fsList').addEventListener('click', async (e) => {
  const name = e.target.dataset.del;
  if (!name || !confirm('確定刪除 ' + name + '？')) return;
  await post('/api/fs/delete', { name });
  refreshStatus();
});

$('fsUploadBtn').onclick = async () => {
  const files = $('fsFiles').files;
  if (!files.length) return toast('請先選擇檔案', true);
  for (const f of files) {
    const d = new FormData();
    d.append('file', f, f.name);
    const r = await fetch('/api/fs/upload', { method: 'POST', body: d });
    const j = await r.json();
    if (!j.ok) return toast(f.name + '：' + j.msg, true);
  }
  toast(`已上傳 ${files.length} 個檔案`);
  $('fsFiles').value = '';
  refreshStatus();
};

$('fsFormatBtn').onclick = async () => {
  if (!confirm('格式化會刪除所有網頁檔案，確定嗎？')) return;
  await post('/api/fs/format');
  refreshStatus();
};

$('rebootBtn').onclick = () => confirm('確定重新啟動？') && post('/api/reboot');
$('factoryBtn').onclick = () => confirm('所有設定將被清除，確定恢復原廠設定？') && post('/api/factory');

// ============================================================
//  設定表單
// ============================================================
async function loadConfig() {
  let c;
  try {
    c = await getJson('/api/config');
  } catch (e) {
    return;
  }
  // RJ45
  const ef = $('ethForm');
  fillForm(ef, { dhcp: c.ethDhcp, ip: c.ethIp, mask: c.ethMask, gw: c.ethGw, dns: c.ethDns, hostname: c.hostname });
  updateEthFields();
  // WiFi
  $('wifiSsid').value = c.staSsid;
  $('wifiPass').value = '';
  $('wifiPass').placeholder = c.staHasPass ? '已設定 (留空沿用原密碼)' : '開放式網路請留空';
  fillForm($('apForm'), { ssid: c.apSsid, pass: '' });
  // RTK
  fillForm($('rtkForm'), c);
  updateBaseFields();
  fillForm($('outForm'), c);
  $('outForm').elements.ntripPass.placeholder = c.ntripHasPass ? '已設定 (留空表示不變更)' : '請輸入 Caster 密碼';
}

function updateEthFields() {
  $('ethStatic').style.display = $('ethDhcp').checked ? 'none' : '';
}
$('ethDhcp').onchange = updateEthFields;

$('ethForm').onsubmit = async (e) => {
  e.preventDefault();
  if (!confirm('儲存後裝置會重新啟動，若 IP 變更請改用新 IP 連線。確定？')) return;
  await post('/api/eth', formData(e.target));
};

$('wifiShowPass').onchange = (e) => ($('wifiPass').type = e.target.checked ? 'text' : 'password');

$('wifiForm').onsubmit = async (e) => {
  e.preventDefault();
  const d = formData(e.target);
  if (!d.pass) d.open = '0';
  await post('/api/wifi', d);
  setTimeout(refreshStatus, 4000);
};

$('wifiClearBtn').onclick = async () => {
  if (!confirm('確定清除 WiFi STA 設定並中斷連線？')) return;
  await post('/api/wifi/clear');
  $('wifiSsid').value = '';
  $('wifiPass').value = '';
  refreshStatus();
};

$('apForm').onsubmit = async (e) => {
  e.preventDefault();
  if (!confirm('儲存後裝置會重新啟動，確定？')) return;
  await post('/api/ap', formData(e.target));
};

// ---- WiFi 掃描 ----
$('scanBtn').onclick = async () => {
  const btn = $('scanBtn');
  btn.disabled = true;
  btn.textContent = '掃描中...';
  $('scanList').innerHTML = '<tr><td colspan="4" class="muted">掃描中，請稍候...</td></tr>';
  try {
    await fetch('/api/wifi/scan?start=1');
    for (let i = 0; i < 30; i++) {
      await new Promise((r) => setTimeout(r, 1000));
      const j = await getJson('/api/wifi/scan');
      if (j.status === 'done') {
        renderScan(j.networks);
        break;
      }
    }
  } catch (e) {
    toast('掃描失敗', true);
  }
  btn.disabled = false;
  btn.textContent = '重新掃描';
};

function rssiBars(r) {
  const lv = r > -55 ? 4 : r > -67 ? 3 : r > -75 ? 2 : 1;
  return '▂▄▆█'.slice(0, lv) + '<span class="muted">' + '▂▄▆█'.slice(lv) + '</span>';
}

function renderScan(list) {
  list.sort((a, b) => b.rssi - a.rssi);
  $('scanList').innerHTML = list.length
    ? list.map((n) => `<tr data-ssid="${esc(n.ssid)}"><td>${esc(n.ssid) || '<span class="muted">(隱藏)</span>'}</td>
        <td class="r">${rssiBars(n.rssi)} ${n.rssi}</td><td class="r">${n.ch}</td><td>${esc(n.auth)}</td></tr>`).join('')
    : '<tr><td colspan="4" class="muted">找不到網路</td></tr>';
}

$('scanList').addEventListener('click', (e) => {
  const tr = e.target.closest('tr');
  if (!tr || !tr.dataset.ssid) return;
  $('wifiSsid').value = tr.dataset.ssid;
  $('wifiPass').value = '';
  $('wifiPass').focus();
});

// ---- RTK 設定 ----
function updateBaseFields() {
  const fixed = $('baseMode').value === '1';
  $('svFields').style.display = fixed ? 'none' : '';
  $('fixFields').style.display = fixed ? '' : 'none';
}
$('baseMode').onchange = updateBaseFields;

$('rtkForm').onsubmit = async (e) => {
  e.preventDefault();
  await post('/api/rtk/config', formData(e.target));
};

$('outForm').onsubmit = async (e) => {
  e.preventDefault();
  await post('/api/output', formData(e.target));
};

// ============================================================
//  RTK 數據
// ============================================================
const RTCM_DESC = {
  1001: 'GPS L1 觀測量', 1002: 'GPS L1 觀測量 (擴充)', 1003: 'GPS L1/L2 觀測量', 1004: 'GPS L1/L2 觀測量 (擴充)',
  1005: '基站天線參考點 (ARP)', 1006: '基站 ARP + 天線高', 1007: '天線描述', 1008: '天線描述 + 序號',
  1009: 'GLONASS L1', 1010: 'GLONASS L1 (擴充)', 1011: 'GLONASS L1/L2', 1012: 'GLONASS L1/L2 (擴充)',
  1019: 'GPS 星曆', 1020: 'GLONASS 星曆', 1033: '接收機與天線描述', 1042: 'BDS 星曆', 1044: 'QZSS 星曆',
  1045: 'Galileo F/NAV 星曆', 1046: 'Galileo I/NAV 星曆', 1230: 'GLONASS 碼相位偏差',
  1071: 'GPS MSM1', 1072: 'GPS MSM2', 1073: 'GPS MSM3', 1074: 'GPS MSM4', 1075: 'GPS MSM5', 1076: 'GPS MSM6', 1077: 'GPS MSM7',
  1081: 'GLONASS MSM1', 1084: 'GLONASS MSM4', 1085: 'GLONASS MSM5', 1087: 'GLONASS MSM7',
  1091: 'Galileo MSM1', 1094: 'Galileo MSM4', 1095: 'Galileo MSM5', 1097: 'Galileo MSM7',
  1111: 'SBAS MSM1', 1114: 'SBAS MSM4', 1117: 'SBAS MSM7',
  1121: 'BDS MSM1', 1124: 'BDS MSM4', 1125: 'BDS MSM5', 1127: 'BDS MSM7',
  1131: 'NavIC MSM1', 1134: 'NavIC MSM4', 1137: 'NavIC MSM7',
  4072: 'u-blox 專有',
};

const MODE_TEXT = { N: '無效', A: '自主定位', D: '差分', E: '推估' };
const FIX_TYPE_TEXT = { 1: '未定位', 2: '2D', 3: '3D' };
const STATE_TEXT = { wait_fix: ['等待定位', 'warn'], surveying: ['Survey-in 中', 'warn'], ready: ['運作中', 'ok'] };
const corrHist = [];

function setStat(id, text, cls) {
  const el = $(id);
  el.textContent = text;
  el.className = 'stat-value' + (cls ? ' ' + cls : '');
}

async function refreshRtk() {
  let d;
  try {
    d = await getJson('/api/rtk');
  } catch (e) {
    return;
  }
  const g = d.gnss, r = d.rtk, o = d.out;

  // 上方狀態燈
  setPill('pillGnss', !g.receiving ? 'bad' : g.valid ? 'ok' : 'warn');
  setPill('pillBase', r.state === 'ready' ? 'ok' : r.state === 'surveying' ? 'warn' : 'bad');
  if (currentPage !== 'rtk') return;

  const st = STATE_TEXT[r.state] || ['-', ''];
  setStat('sState', st[0], st[1]);
  setStat('sFix', g.receiving ? g.qualityName : '無資料', !g.receiving ? 'bad' : g.valid ? 'ok' : 'warn');
  setStat('sSats', g.sats);
  setStat('sHdop', num(g.hdop, 2), g.hdop < 2 ? 'ok' : g.hdop < 5 ? 'warn' : 'bad');
  setStat('sRtcm', fmtBytes(r.rtcm.bytesOut));
  const nClients = o.tcp.clients.filter((c) => c.type !== '握手中').length + (o.ntrip.connected ? 1 : 0);
  setStat('sClients', nClients);

  // ---- Survey-in ----
  const sv = r.survey;
  const surveying = r.mode === 'survey';
  $('surveyCard').style.display = surveying ? '' : 'none';
  if (surveying) {
    const tp = Math.min(100, (sv.elapsed / sv.minSec) * 100);
    $('svTimeBar').style.width = (r.state === 'ready' ? 100 : tp) + '%';
    $('svTimeText').textContent = `時間 ${fmtDur(sv.elapsed)} / 最少 ${fmtDur(sv.minSec)}`;
    const ap = sv.std3d == null ? 0 : Math.min(100, (sv.accLimit / sv.std3d) * 100);
    $('svAccBar').style.width = (r.state === 'ready' ? 100 : ap) + '%';
    $('svAccText').textContent = `3D 標準差 ${num(sv.std3d, 3)} m / 門檻 ${num(sv.accLimit, 2)} m`;
    kv('svTable', [
      ['取樣數', sv.samples],
      ['平均緯度', num(sv.meanLat, 9)],
      ['平均經度', num(sv.meanLon, 9)],
      ['平均橢球高', num(sv.meanH, 3) + ' m'],
    ]);
    $('svSaveBtn').disabled = r.state !== 'ready';
  }

  // ---- 基站座標 ----
  const b = r.base;
  $('baseBadge').textContent = b.valid ? (b.source === 'survey' ? 'Survey-in 結果' : '固定座標') : '尚未確定';
  $('baseBadge').className = 'badge ' + (b.valid ? 'ok' : 'warn');
  kv('baseTable', b.valid ? [
    ['緯度', num(b.lat, 9) + '°'],
    ['經度', num(b.lon, 9) + '°'],
    ['橢球高', num(b.h, 4) + ' m'],
    ['ECEF X', num(b.x, 4) + ' m'],
    ['ECEF Y', num(b.y, 4) + ' m'],
    ['ECEF Z', num(b.z, 4) + ' m'],
  ] : [['狀態', r.state === 'surveying' ? 'Survey-in 進行中' : '等待 GNSS 定位']]);

  // ---- GNSS ----
  kv('gnssTable', [
    ['接收狀態', !g.receiving ? `無資料 (${g.baud} bps)，請檢查接線 / 供電`
      : g.nmeaOk ? `接收中 (${g.baud} bps)` : `有資料但 NMEA 無效 (${g.baud} bps)${g.autoBaud ? '，自動偵測鮑率中' : '，請確認鮑率'}`],
    ['定位品質', `${g.qualityName} (${g.quality})`],
    ['RMC 狀態 / 模式', `${g.rmcStatus === 'A' ? '有效' : '警告'} / ${MODE_TEXT[g.mode] || g.mode}`],
    ['UTC', (g.date ? g.date + ' ' : '') + (g.utcTime ? `${g.utcTime.slice(0, 2)}:${g.utcTime.slice(2, 4)}:${g.utcTime.slice(4)}` : '-')],
    ['緯度', num(g.lat, 9) + '°'],
    ['經度', num(g.lon, 9) + '°'],
    ['海拔高 (MSL)', num(g.altMsl, 3) + ' m'],
    ['大地起伏', num(g.geoidSep, 3) + ' m'],
    ['橢球高', num(g.hEll, 3) + ' m'],
    ['定位型態', FIX_TYPE_TEXT[g.fixType] || '-'],
    ['PDOP / HDOP / VDOP', `${num(g.pdop, 2)} / ${num(g.hdop, 2)} / ${num(g.vdop, 2)}`],
    ['速度 / 航向', `${num(g.speedKmh, 2)} km/h / ${num(g.course, 1)}°`],
    ['差分齡期 / 站號', g.dgpsAge >= 0 ? `${num(g.dgpsAge, 1)} s / ${g.dgpsStation}` : '未使用'],
    ...(g.pps === undefined ? [] : [['1PPS', g.pps ? `正常 (${g.ppsCount})` : '無脈衝']]),
    ['接收位元組', fmtBytes(g.bytesIn)],
    ['NMEA (錯誤)', `${g.nmeaCount} (${g.nmeaErrors})`],
    ['RTCM (CRC 錯誤)', `${g.rtcmCount} (${g.rtcmErrors})`],
  ]);

  // ---- 衛星 ----
  $('svBars').innerHTML = g.systems.filter(([, v]) => v > 0).map(([n, v, u]) => `<div class="svrow"><span>${esc(n)}</span>
    <div class="bar"><div class="bar-fill" style="width:${Math.min(100, v / 20 * 100)}%"></div></div><b>${u}/${v}</b></div>`).join('')
    || '<p class="muted">尚未收到 GSV 衛星資料</p>';
  const sats = g.satList.slice().sort((a, b) => a[0] - b[0] || a[1] - b[1]);
  $('satList').innerHTML = sats.map(([sys, prn, el, az, snr, used]) => `<tr class="${used ? 'used' : ''}">
    <td>${esc(g.systems[sys] ? g.systems[sys][0] : '?')}</td><td class="r">${prn}</td>
    <td class="r">${el < 0 ? '-' : el + '°'}</td><td class="r">${az < 0 ? '-' : az + '°'}</td>
    <td><div class="snr"><div class="bar"><div class="bar-fill ${snr >= 35 ? 'alt' : snr >= 25 ? 'mid' : 'low'}" style="width:${snr < 0 ? 0 : Math.min(100, snr / 50 * 100)}%"></div></div>
    <span>${snr < 0 ? '-' : snr}</span></div></td><td>${used ? '✔' : ''}</td></tr>`).join('')
    || '<tr><td colspan="6" class="muted">無</td></tr>';

  // ---- 差分修正 ----
  const c = r.corr;
  $('cE').textContent = c.valid ? num(c.e, 3) : '-';
  $('cN').textContent = c.valid ? num(c.n, 3) : '-';
  $('cU').textContent = c.valid ? num(c.u, 3) : '-';
  kv('corrTable', [
    ['水平誤差 (2D)', c.valid ? num(c.h2d, 3) + ' m' : '-'],
    ['3D 誤差', c.valid ? num(c.d3, 3) + ' m' : '-'],
    ['水平 RMS', num(c.rmsH, 3) + ' m'],
    ['垂直 RMS', num(c.rmsV, 3) + ' m'],
    ['水平最大誤差', num(c.maxH, 3) + ' m'],
    ['計算筆數', c.epochs],
  ]);
  if (c.valid) {
    corrHist.push([-c.e, -c.n]);  // 量測點相對基站位置 = −修正量
    if (corrHist.length > 300) corrHist.shift();
  }
  drawScatter();

  // ---- RTCM ----
  const types = r.rtcm.types.slice().sort((a, b) => a.type - b.type);
  $('rtcmList').innerHTML = types.length
    ? types.map((t) => `<tr><td>${t.type}</td><td>${esc(RTCM_DESC[t.type] || '')}</td><td class="r">${t.count}</td>
        <td class="r">${fmtBytes(t.bytes)}</td><td class="r">${t.age}</td></tr>`).join('')
    : '<tr><td colspan="5" class="muted">接收機未輸出 RTCM3 (僅 NMEA)</td></tr>';
  kv('rtcmTable', [
    ['自產 1005', r.rtcm.inject1005 ? (r.rtcm.rxHas1005 ? '停用 (接收機已輸出)' : `已送出 ${r.rtcm.sent1005} 次`) : '關閉'],
    ['接收機 1005 基站 ID', r.rtcm.rxHas1005 ? r.rtcm.rxStationId : '-'],
    ['接收機 1005 座標', r.rtcm.rxHas1005 ? `${num(r.rtcm.rxLat, 8)}, ${num(r.rtcm.rxLon, 8)}, ${num(r.rtcm.rxH, 3)}` : '-'],
  ]);

  // ---- 輸出 ----
  kv('outTable', [
    ['內建 Caster', o.tcp.enable ? `埠 ${o.tcp.port}，掛載點 /${o.tcp.mount}` : '停用'],
    ['NTRIP Server', o.ntrip.enable ? `${o.ntrip.host}:${o.ntrip.port}/${o.ntrip.mount}` : '停用'],
    ['NTRIP 狀態', o.ntrip.status],
    ['NTRIP 已推送', fmtBytes(o.ntrip.bytes) + (o.ntrip.connected ? `，連線 ${fmtDur(o.ntrip.uptime)}` : '')],
  ]);
  $('clientList').innerHTML = o.tcp.clients.length
    ? o.tcp.clients.map((x) => `<tr><td>${esc(x.ip)}</td><td>${esc(x.type)}</td><td class="r">${fmtBytes(x.bytes)}</td><td class="r">${fmtDur(x.sec)}</td></tr>`).join('')
    : '<tr><td colspan="4" class="muted">目前沒有用戶連線</td></tr>';
}

function drawScatter() {
  const cv = $('scatter');
  const ctx = cv.getContext('2d');
  const W = cv.width, H = cv.height, cx = W / 2, cy = H / 2;
  const dark = matchMedia('(prefers-color-scheme: dark)').matches;
  ctx.clearRect(0, 0, W, H);

  let maxR = 0.5;
  for (const [e, n] of corrHist) maxR = Math.max(maxR, Math.abs(e), Math.abs(n));
  const nice = [0.5, 1, 2, 5, 10, 20, 50, 100];
  const range = nice.find((v) => v >= maxR * 1.1) || maxR;
  const sc = (W / 2 - 20) / range;

  ctx.strokeStyle = dark ? '#2a323e' : '#dde2ea';
  ctx.fillStyle = dark ? '#8d98a8' : '#6b7686';
  ctx.font = '11px sans-serif';
  for (let i = 1; i <= 2; i++) {
    const rr = (range * i / 2) * sc;
    ctx.beginPath();
    ctx.arc(cx, cy, rr, 0, Math.PI * 2);
    ctx.stroke();
    ctx.fillText((range * i / 2) + ' m', cx + rr * 0.72 + 2, cy - rr * 0.72 - 2);
  }
  ctx.beginPath();
  ctx.moveTo(cx, 8); ctx.lineTo(cx, H - 8);
  ctx.moveTo(8, cy); ctx.lineTo(W - 8, cy);
  ctx.stroke();
  ctx.fillText('N', cx + 4, 16);
  ctx.fillText('E', W - 16, cy - 4);

  corrHist.forEach(([e, n], i) => {
    const last = i === corrHist.length - 1;
    ctx.fillStyle = last ? '#d64545' : `rgba(47,111,237,${0.25 + 0.6 * i / corrHist.length})`;
    ctx.beginPath();
    ctx.arc(cx + e * sc, cy - n * sc, last ? 4 : 2, 0, Math.PI * 2);
    ctx.fill();
  });
  ctx.fillStyle = '#1f9d55';
  ctx.fillRect(cx - 3, cy - 3, 6, 6);
}

$('svRestartBtn').onclick = async () => {
  if (!confirm('重新開始 Survey-in 會捨棄目前累積的資料，確定？')) return;
  await post('/api/rtk/restart');
  corrHist.length = 0;
  refreshRtk();
};

$('svSaveBtn').onclick = async () => {
  if (!confirm('將目前 Survey-in 結果存為固定座標？之後開機將直接使用此座標。')) return;
  await post('/api/rtk/savefixed');
};

// ---- NMEA 監看 ----
let nmeaSeq = -1;
async function refreshNmea() {
  if (currentPage !== 'rtk' || $('nmeaPause').checked) return;
  try {
    const j = await getJson('/api/nmea');
    if (j.seq === nmeaSeq) return;
    nmeaSeq = j.seq;
    const con = $('nmeaConsole');
    const atBottom = con.scrollTop + con.clientHeight >= con.scrollHeight - 10;
    con.textContent = j.lines.join('\n');
    if (atBottom) con.scrollTop = con.scrollHeight;
  } catch (e) { /* 忽略 */ }
}

// ============================================================
//  OTA
// ============================================================
$('otaForm').onsubmit = (e) => {
  e.preventDefault();
  const f = $('otaFile').files[0];
  if (!f) return;
  const target = $('otaTarget').value;
  if (!confirm(`確定上傳 ${f.name} (${fmtBytes(f.size)}) 更新${target === 'fs' ? ' SPIFFS' : '韌體'}？`)) return;

  const d = new FormData();
  d.append('file', f, f.name);
  const x = new XMLHttpRequest();
  x.open('POST', '/api/ota?target=' + target);
  $('otaBtn').disabled = true;
  x.upload.onprogress = (ev) => {
    if (!ev.lengthComputable) return;
    const p = (ev.loaded / ev.total) * 100;
    $('otaBar').style.width = p.toFixed(1) + '%';
    $('otaMsg').textContent = `上傳中 ${p.toFixed(1)}% (${fmtBytes(ev.loaded)} / ${fmtBytes(ev.total)})`;
  };
  x.onload = () => {
    $('otaBtn').disabled = false;
    let j = {};
    try { j = JSON.parse(x.responseText); } catch (err) { j = { ok: false, msg: x.responseText }; }
    $('otaMsg').textContent = j.msg;
    toast(j.msg, !j.ok);
    if (j.ok) {
      $('otaMsg').textContent = j.msg + '，約 10 秒後自動重新載入...';
      setTimeout(() => location.reload(), 10000);
    }
  };
  x.onerror = () => {
    $('otaBtn').disabled = false;
    toast('上傳失敗', true);
  };
  x.send(d);
};

// ============================================================
//  啟動
// ============================================================
showPage(location.hash.slice(1) || 'net-info');
setInterval(refreshStatus, 3000);
let rtkTick = 0;
setInterval(() => {
  if (currentPage === 'rtk' || ++rtkTick % 3 === 0) refreshRtk();  // 非 RTK 頁面僅更新狀態燈
}, 1000);
setInterval(refreshNmea, 1000);
