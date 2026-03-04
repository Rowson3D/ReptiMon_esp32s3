/* ReptiMon — App Core */
'use strict';

/* ── State ────────────────────────────────────────────────────── */
const State = {
  data:     {},          // latest sensor snapshot from /ws
  wifi:     {},          // latest wifi status
  settings: {},          // from /api/settings/get
  ota:      {},          // from /api/ota/check
  units:    'C',
  connected: false,
  hasData:   false,      // true once first sensor snapshot received
  activeTab: '',         // blank so first navigate() always loads
};

/* ── Config ───────────────────────────────────────────────────── */
const WS_URL     = `ws://${location.host}/ws`;
const WS_CAM_URL = `ws://${location.host}/ws/cam`;
const COMP_BASE  = '/components/';

/* ── WebSocket manager (sensor data) ─────────────────────────── */
const DataWS = (() => {
  let ws, retryTimer, retryDelay = 1500;

  function connect() {
    ws = new WebSocket(WS_URL);
    setWsIndicator(false);

    ws.onopen = () => {
      retryDelay = 1500;
      setWsIndicator(true);
    };

    ws.onmessage = ({ data }) => {
      try {
        const msg = JSON.parse(data);
        // WiFi events during captive portal
        if (msg.event === 'wifi_connected') { handleWifiEvent(msg); return; }
        if (msg.event === 'wifi_failed')    { handleWifiEvent(msg); return; }
        // Regular sensor snapshot
        State.hasData = true;
        Object.assign(State.data, msg);
        if (msg.units) State.units = msg.units;
        if (msg.wifi)  State.wifi  = msg.wifi;
        // Accumulate sparkline history globally (survives tab changes)
        if (msg.valid) {
          const HIST_MAX = 60;
          if (!State._tHist) State._tHist = [];
          if (!State._hHist) State._hHist = [];
          if (msg.temperature != null) { State._tHist.push(Number(msg.temperature)); if (State._tHist.length > HIST_MAX) State._tHist.shift(); }
          if (msg.humidity    != null) { State._hHist.push(Number(msg.humidity));    if (State._hHist.length > HIST_MAX) State._hHist.shift(); }
        }
        Tabs.refresh();
        updateTopbar();
        updateSidebar();
        updateAlerts();
        updateConnChip();
      } catch { /* ignore malformed */ }
    };

    ws.onerror = () => {};
    ws.onclose = () => {
      setWsIndicator(false);
      retryTimer = setTimeout(connect, retryDelay);
      retryDelay = Math.min(retryDelay * 1.5, 12000);
    };
  }

  return { connect, get: () => ws };
})();

/* ── WebSocket camera ─────────────────────────────────────────── */
const CamWS = (() => {
  let ws, retryTimer, retryDelay = 2000;
  let consumers = [];  // array of { canvas, ctx }

  function addConsumer(canvas) {
    const ctx = canvas.getContext('2d');
    consumers.push({ canvas, ctx });
  }
  function removeConsumer(canvas) {
    consumers = consumers.filter(c => c.canvas !== canvas);
    if (consumers.length === 0) disconnect();
  }
  function connect() {
    if (ws && ws.readyState <= 1) return; // already open/connecting
    ws = new WebSocket(WS_CAM_URL);
    ws.binaryType = 'arraybuffer';

    ws.onmessage = ({ data }) => {
      const blob = new Blob([data], { type: 'image/jpeg' });
      const url  = URL.createObjectURL(blob);
      const img  = new Image();
      img.onload = () => {
        consumers.forEach(({ canvas, ctx }) => {
          canvas.width  = img.naturalWidth;
          canvas.height = img.naturalHeight;
          ctx.drawImage(img, 0, 0);
        });
        URL.revokeObjectURL(url);
      };
      img.src = url;
    };

    ws.onclose = () => {
      if (consumers.length > 0) {
        retryTimer = setTimeout(connect, retryDelay);
        retryDelay = Math.min(retryDelay * 1.4, 10000);
      }
    };
    ws.onerror = () => {};
  }
  function disconnect() {
    clearTimeout(retryTimer);
    if (ws) ws.close();
  }
  return { connect, disconnect, addConsumer, removeConsumer };
})();

/* ── Tab / component loader ───────────────────────────────────── */
const Tabs = (() => {
  const loaded  = {};
  const refreshFns = {};

  async function navigate(tabId) {
    if (State.activeTab === tabId) return;
    State.activeTab = tabId;

    // Update sidebar active state
    document.querySelectorAll('.side-item').forEach(el => {
      el.classList.toggle('active', el.dataset.tab === tabId);
    });

    // Show/hide panels
    document.querySelectorAll('.panel').forEach(el => {
      el.classList.toggle('active', el.id === `panel-${tabId}`);
    });

    // Load component HTML if not yet loaded
    if (!loaded[tabId]) {
      const panel = document.getElementById(`panel-${tabId}`);
      try {
        const r = await fetch(`${COMP_BASE}${tabId}.html`);
        if (!r.ok) throw new Error(r.status);
        const html = await r.text();

        // innerHTML does not execute <script> tags — parse them out and
        // re-inject as real script elements so component JS actually runs.
        const tmp = document.createElement('div');
        tmp.innerHTML = html;
        // Move non-script nodes into the panel first
        panel.innerHTML = '';
        tmp.childNodes.forEach(node => {
          if (node.nodeName !== 'SCRIPT') panel.appendChild(node.cloneNode(true));
        });
        // Then execute each script in order
        tmp.querySelectorAll('script').forEach(orig => {
          const s = document.createElement('script');
          if (orig.src) { s.src = orig.src; s.async = false; }
          else s.textContent = orig.textContent;
          panel.appendChild(s);
        });

        loaded[tabId] = true;
        if (window[`init_${tabId}`]) window[`init_${tabId}`]();
      } catch (e) {
        panel.innerHTML = `<div class="card"><div class="card-body"><p class="muted">Failed to load component: ${tabId} (${e.message})</p></div></div>`;
      }
    }

    // Always refresh with latest data
    refresh(tabId);
  }

  function refresh(tabId) {
    const t = tabId || State.activeTab;
    if (refreshFns[t]) refreshFns[t]();
  }

  function register(tabId, fn) { refreshFns[tabId] = fn; }

  return { navigate, refresh, register };
})();

/* ── Alert system ─────────────────────────────────────────────── */
function updateAlerts() {
  const stack = document.getElementById('alert-stack');
  if (!stack) return;
  const d = State.data;
  const alerts = [];

  if (d.tempStatus === 'high')     alerts.push({ cls: 'warn',   msg: `⚠ Temperature HIGH: ${fmt(d.temperature)}°${State.units}` });
  if (d.tempStatus === 'low')      alerts.push({ cls: 'warn',   msg: `⚠ Temperature LOW: ${fmt(d.temperature)}°${State.units}` });
  if (d.tempStatus === 'critical') alerts.push({ cls: 'danger', msg: `🔴 Temperature CRITICAL: ${fmt(d.temperature)}°${State.units}` });
  if (d.humStatus  === 'high')     alerts.push({ cls: 'warn',   msg: `⚠ Humidity HIGH: ${fmt(d.humidity)}%` });
  if (d.humStatus  === 'low')      alerts.push({ cls: 'warn',   msg: `⚠ Humidity LOW: ${fmt(d.humidity)}%` });
  if (d.humStatus  === 'critical') alerts.push({ cls: 'danger', msg: `🔴 Humidity CRITICAL: ${fmt(d.humidity)}%` });

  // Only rebuild DOM if alerts changed
  const key = JSON.stringify(alerts);
  if (stack._lastKey === key) return;
  stack._lastKey = key;

  stack.innerHTML = alerts.map(a => `
    <div class="alert-banner ${a.cls}">
      <span class="alert-msg">${a.msg}</span>
      <span class="alert-close" onclick="this.parentElement.remove()">✕</span>
    </div>`).join('');
}

/* ── Topbar updater ───────────────────────────────────────────── */
function updateTopbar() {
  const d = State.data;
  const mood = document.getElementById('topbar-mood');
  const ipEl = document.getElementById('topbar-ip');

  if (!State.hasData) { if (mood) { mood.className = 'mood'; mood.textContent = 'Connecting…'; } return; }
  if (!d.valid) { if (mood) { mood.className = 'mood'; mood.textContent = 'Sensor offline'; } return; }

  const ts = d.tempStatus || 'ok';
  const hs = d.humStatus  || 'ok';
  const worst = ts === 'critical' || hs === 'critical' ? 'bad'
              : ts === 'high' || ts === 'low' || hs === 'high' || hs === 'low' ? 'warn' : 'good';

  if (mood) {
    mood.className = `mood ${worst}`;
    mood.textContent = worst === 'good'
      ? `${fmt(d.temperature)}°${State.units}  ·  ${fmt(d.humidity)}% RH  —  All normal`
      : `${fmt(d.temperature)}°${State.units}  ·  ${fmt(d.humidity)}% RH  —  Check alerts`;
  }
  if (ipEl && d.system?.ip) ipEl.textContent = d.system.ip;
}

/* ── Sidebar updater ──────────────────────────────────────────── */
function updateSidebar() {
  const d = State.data;
  setText('sb-uptime', d.system?.uptime ? fmtUptime(d.system.uptime) : '—');
  if (d.system?.ip)       setText('sb-ip',   d.system.ip);
  if (d.system?.hostname) setText('sb-host', d.system.hostname + '.local');
}

/* ── WS indicator ─────────────────────────────────────────────── */
function setWsIndicator(connected) {
  State.connected = connected;
  const el = document.getElementById('ws-indicator');
  if (el) {
    el.style.background = connected ? 'var(--accent)' : '#555';
    el.title = connected ? 'Connected' : 'Disconnected';
  }
  updateConnChip();
}

/* ── Connection chip ─────────────────────────────────────────────── */
let _chipTimer = null;

function updateConnChip() {
  const chip = document.getElementById('conn-chip');
  if (!chip) return;

  const isAp = State.data?.ap_mode === true
             || State.data?.mode === 'ap'
             || State.wifi?.mode === 'ap'
             || State.data?.system?.ap === true;

  // AP mode — show immediately, user genuinely needs to act
  if (isAp) {
    clearTimeout(_chipTimer);
    _chipTimer = null;
    chip.className = 'conn-chip cc-ap';
    chip.innerHTML = 'Hotspot mode — <a href="/portal.html">Set up device</a>';
    chip.style.display = 'flex';
    return;
  }

  // Connected and not AP — hide chip, clear any pending timer
  if (State.connected) {
    clearTimeout(_chipTimer);
    _chipTimer = null;
    chip.style.display = 'none';
    return;
  }

  // Disconnected — wait 5s grace period before showing anything
  if (!_chipTimer) {
    _chipTimer = setTimeout(() => {
      if (!State.connected) {
        chip.className = 'conn-chip cc-offline';
        chip.innerHTML = '<span class="conn-chip-spinner"></span>Reconnecting…';
        chip.style.display = 'flex';
      }
      _chipTimer = null;
    }, 5000);
  }
}

/* ── WiFi events (captive portal) ────────────────────────────── */
function handleWifiEvent(msg) {
  if (msg.event === 'wifi_connected') {
    showToast('Connected to ' + (msg.ssid || 'network') + ' — ' + msg.ip, 'ok');
    setTimeout(() => location.href = '/', 3000);
  } else {
    showToast('WiFi failed: ' + (msg.reason || 'timeout'), 'danger');
  }
}

/* ── Toast ────────────────────────────────────────────────────── */
function showToast(msg, type = 'ok', duration = 4000) {
  const t = document.createElement('div');
  t.className = `alert-banner ${type}`;
  t.style.cssText = 'position:fixed;bottom:56px;right:16px;max-width:340px;z-index:300;';
  t.innerHTML = `<span class="alert-msg">${msg}</span><span class="alert-close" onclick="this.parentElement.remove()">✕</span>`;
  document.body.appendChild(t);
  setTimeout(() => t.remove(), duration);
}

/* ── API helpers ──────────────────────────────────────────────── */
async function apiGet(path) {
  const r = await fetch(path);
  if (!r.ok) throw new Error(r.status);
  return r.json();
}
async function apiPost(path, body = {}) {
  const r = await fetch(path, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
  });
  if (!r.ok) {
    let msg = r.status;
    try { const j = await r.json(); if (j.error) msg = j.error; } catch {}
    throw new Error(String(msg));
  }
  return r.json().catch(() => ({}));
}

/* ── Formatting helpers ───────────────────────────────────────── */
function fmt(v, d = 1) {
  return (v == null || isNaN(v)) ? '—' : Number(v).toFixed(d);
}
function fmtUptime(sec) {
  if (!sec) return '—';
  const d = Math.floor(sec / 86400);
  const h = Math.floor((sec % 86400) / 3600);
  const m = Math.floor((sec % 3600) / 60);
  return d > 0 ? `${d}d ${h}h ${m}m` : h > 0 ? `${h}h ${m}m` : `${m}m`;
}
function fmtBytes(b) {
  if (b == null) return '—';
  if (b < 1024) return b + ' B';
  if (b < 1048576) return (b/1024).toFixed(1) + ' KB';
  return (b/1048576).toFixed(2) + ' MB';
}
function rssiDots(rssi) {
  const s = rssi == null ? 0 : rssi > -55 ? 4 : rssi > -67 ? 3 : rssi > -78 ? 2 : 1;
  return `<span class="rssi-dots">${[1,2,3,4].map(i => `<span class="${i<=s?'lit':''}"></span>`).join('')}</span>`;
}
function statusBadge(status) {
  const map = { ok:'ok', normal:'ok', perfect:'ok', high:'warn', low:'warn', 'too cold':'warn', 'too hot':'warn', 'too dry':'warn', 'too wet':'warn', critical:'danger' };
  const cls = map[status?.toLowerCase()] || '';
  const label = status ? status.charAt(0).toUpperCase() + status.slice(1) : '—';
  return `<span class="badge ${cls}"><span class="badge-dot"></span>${label}</span>`;
}
function setText(id, val) {
  const el = document.getElementById(id);
  if (el) el.textContent = val;
}
function setHTML(id, val) {
  const el = document.getElementById(id);
  if (el) el.innerHTML = val;
}

/* ── Footer clock ─────────────────────────────────────────────── */
function tickFooter() {
  const d = State.data;
  setText('footer-uptime', d.system?.uptime ? fmtUptime(d.system.uptime) : '—');
  if (d.system?.ip) setText('footer-ip', d.system.ip);
}

/* ── Theme toggle ─────────────────────────────────────────────── */
function initTheme() {
  const saved = localStorage.getItem('rm-theme') || 'light';
  applyTheme(saved);
  document.getElementById('themeToggle')?.addEventListener('click', () => {
    const next = document.documentElement.dataset.theme === 'dark' ? 'light' : 'dark';
    applyTheme(next);
    localStorage.setItem('rm-theme', next);
  });
}
function applyTheme(t) {
  document.documentElement.dataset.theme = t;
  const moon = document.getElementById('theme-icon-moon');
  const sun  = document.getElementById('theme-icon-sun');
  if (moon) moon.style.display = t === 'dark' ? 'none' : '';
  if (sun)  sun.style.display  = t === 'dark' ? '' : 'none';
}

/* ── Sidebar collapse ─────────────────────────────────────────── */
function initSidebar() {
  const btn = document.getElementById('burger');
  const sb  = document.getElementById('sidebar');
  btn?.addEventListener('click', () => {
    const collapsed = sb.classList.toggle('collapsed');
    document.body.classList.toggle('sb-collapsed', collapsed);
  });
}

/* ── PiP camera ───────────────────────────────────────────────── */
function initPip() {
  const pip    = document.getElementById('pip');
  const canvas = document.getElementById('pip-canvas');
  const toggle = document.getElementById('pip-toggle');
  if (!pip || !canvas) return;

  CamWS.addConsumer(canvas);
  CamWS.connect();

  toggle?.addEventListener('click', () => {
    pip.classList.toggle('pip-mini');
    toggle.textContent = pip.classList.contains('pip-mini') ? '▴' : '▾';
  });

  // Expose cam WS consumers for Camera tab
  window._CamWS = CamWS;
}

/* ── OTA badge ────────────────────────────────────────────────── */
async function checkOta() {
  try {
    const data = await apiGet('/api/ota/check');
    State.ota = data;
    const dot = document.getElementById('sb-ota-dot');
    if (dot) dot.style.display = data.hasUpdate ? '' : 'none';
    setText('footer-version', data.current ? `v${data.current}` : '—');
    setText('brand-version',  data.current ? `v${data.current}` : '—');
  } catch { /* offline */ }
}

/* ── Initial data load ────────────────────────────────────────── */
async function loadInitialData() {
  try {
    const d = await apiGet('/api/data');
    Object.assign(State.data, d);
    if (d.units) State.units = d.units;
    if (d.valid) State.hasData = true;
    updateTopbar();
    updateSidebar();
    updateAlerts();
    Tabs.refresh();
    tickFooter();
  } catch { /* will get from WS */ }

  try {
    const w = await apiGet('/api/wifi/status');
    Object.assign(State.wifi, w);
    if (w.ip) setText('footer-ip', w.ip);
  } catch { /* ignore */ }

  try {
    const s = await apiGet('/api/settings/get');
    Object.assign(State.settings, s);
    if (s.units) State.units = s.units;
  } catch { /* ignore */ }
}

/* ── Global navigate helper ───────────────────────────────────── */
window.App = { navigate: (tab) => Tabs.navigate(tab), apiGet, apiPost, State, fmt, fmtUptime, fmtBytes, rssiDots, statusBadge, setText, setHTML, showToast, CamWS };

/* ── Boot ─────────────────────────────────────────────────────── */
document.addEventListener('DOMContentLoaded', async () => {
  initTheme();
  initSidebar();

  // Wire sidebar nav clicks
  document.querySelectorAll('.side-item[data-tab]').forEach(btn => {
    btn.addEventListener('click', () => Tabs.navigate(btn.dataset.tab));
  });

  // Load initial tab (dashboard), then data
  await Tabs.navigate('dashboard');
  await loadInitialData();
  checkOta();

  // Periodic ticks
  setInterval(tickFooter, 5000);
  setInterval(checkOta, 60000);

  // Start WebSocket
  DataWS.connect();
});
