"use strict";

const API = "/api/v1";
const PSI_MIN = -15;
const PSI_MAX = 25;
const OVERBOOST = 18;
const ARC_START = 135;
const ARC_RANGE = 270;
const ZERO_GAP_VAC = 3.6;
const ZERO_GAP_BOOST = 4.0;
const sampleHistory = [];
const HISTORY_WINDOW_MS = 60_000;
const GAUGE_GAP_RESET_MS = 1000;
const GAUGE_FRAME_MS = 1000 / 60;
const SPARKLINE_FRAME_MS = 250;
const CANVAS_DPR_MAX = 2;

const state = {
  activeThemeId: "pit-lane",
  themes: [],
  config: null,
  network: null,
  scannedNetworks: [],
  connected: false,
  liveSocket: null,
  reconnectTimer: null,
  pollTimer: null,
  pollInFlight: false,
  fallbackActive: false,
  heartbeatTimer: null,
  activeUpload: null,
  mediaDeleteInFlight: false,
  palette: null,
  gaugeTarget: null,
  gaugePsi: 0,
  gaugeRaf: null,
  gaugeLastAt: 0,
  sparklineLastAt: 0,
};

const el = {
  connection: document.getElementById("connection"),
  connectionText: document.getElementById("connectionText"),
  errorBox: document.getElementById("errorBox"),
  okBox: document.getElementById("okBox"),
  canvas: document.getElementById("gaugeCanvas"),
  sparkline: document.getElementById("sparkline"),
  gaugeDevice: document.getElementById("gaugeDevice"),
  sampleCount: document.getElementById("sampleCount"),
  emptyState: document.getElementById("emptyState"),
  firmwareVersion: document.getElementById("firmwareVersion"),
  uptime: document.getElementById("uptime"),
  deviceClock: document.getElementById("deviceClock"),
  brightnessNow: document.getElementById("brightnessNow"),
  refreshBtn: document.getElementById("refreshBtn"),
  syncTimeBtn: document.getElementById("syncTimeBtn"),
  tzOffset: document.getElementById("tzOffset"),
  scheduleEnabled: document.getElementById("scheduleEnabled"),
  scheduleStart: document.getElementById("scheduleStart"),
  scheduleEnd: document.getElementById("scheduleEnd"),
  brightnessHigh: document.getElementById("brightnessHigh"),
  brightnessLow: document.getElementById("brightnessLow"),
  brightnessHighOut: document.getElementById("brightnessHighOut"),
  brightnessLowOut: document.getElementById("brightnessLowOut"),
  saveConfigBtn: document.getElementById("saveConfigBtn"),
  themeList: document.getElementById("themeList"),
  loadLogsBtn: document.getElementById("loadLogsBtn"),
  clearLogsBtn: document.getElementById("clearLogsBtn"),
  logSummary: document.getElementById("logSummary"),
  exportLogsBtn: document.getElementById("exportLogsBtn"),
  mediaFile: document.getElementById("mediaFile"),
  uploadMediaBtn: document.getElementById("uploadMediaBtn"),
  deleteMediaBtn: document.getElementById("deleteMediaBtn"),
  mediaStatus: document.getElementById("mediaStatus"),
  mediaProgress: document.getElementById("mediaProgress"),
  mediaPreview: document.getElementById("mediaPreview"),
  otaFile: document.getElementById("otaFile"),
  uploadOtaBtn: document.getElementById("uploadOtaBtn"),
  otaStatus: document.getElementById("otaStatus"),
  otaProgress: document.getElementById("otaProgress"),
  netMode: document.getElementById("netMode"),
  netStaState: document.getElementById("netStaState"),
  netStaIp: document.getElementById("netStaIp"),
  netApSsid: document.getElementById("netApSsid"),
  netModeSelect: document.getElementById("netModeSelect"),
  netSsid: document.getElementById("netSsid"),
  netPassword: document.getElementById("netPassword"),
  netKeepPassword: document.getElementById("netKeepPassword"),
  saveNetworkBtn: document.getElementById("saveNetworkBtn"),
  reconnectNetworkBtn: document.getElementById("reconnectNetworkBtn"),
  networkRefreshBtn: document.getElementById("networkRefreshBtn"),
  scanNetworksBtn: document.getElementById("scanNetworksBtn"),
  netScanSelect: document.getElementById("netScanSelect"),
  netHint: document.getElementById("netHint"),
};

const ctx = el.canvas.getContext("2d");
const sparkCtx = el.sparkline.getContext("2d");

function updatePalette() {
  const css = getComputedStyle(document.documentElement);
  state.palette = {
    face: css.getPropertyValue("--face").trim() || "#000",
    track: css.getPropertyValue("--track").trim(),
    text: css.getPropertyValue("--text").trim(),
    muted: css.getPropertyValue("--muted").trim(),
    vacuum: css.getPropertyValue("--vacuum").trim(),
    boost: css.getPropertyValue("--boost").trim(),
    overboost: css.getPropertyValue("--overboost").trim(),
    zero: css.getPropertyValue("--zero").trim(),
  };
}

function setTheme(theme) {
  if (!theme || !theme.colors) return;
  const root = document.documentElement;
  root.style.setProperty("--face", theme.colors.face);
  root.style.setProperty("--track", theme.colors.track);
  root.style.setProperty("--text", theme.colors.text);
  root.style.setProperty("--muted", theme.colors.muted);
  root.style.setProperty("--vacuum", theme.colors.vacuum);
  root.style.setProperty("--boost", theme.colors.boost);
  root.style.setProperty("--overboost", theme.colors.overboost);
  root.style.setProperty("--zero", theme.colors.zero);
  state.activeThemeId = theme.id;
  updatePalette();
  scheduleGaugeRender();
  drawSparkline();
}

function zoneFor(psi) {
  if (psi >= OVERBOOST) return "OVER";
  if (psi >= 0.35) return "BOOST";
  if (psi > -0.35) return "ATMO";
  return "VAC";
}

function colorFor(psi) {
  const palette = state.palette;
  if (psi >= OVERBOOST) return palette.overboost;
  if (psi >= 0.35) return palette.boost;
  if (psi > -0.35) return palette.text;
  return palette.vacuum;
}

function psiToAngle(psi) {
  const clamped = Math.max(PSI_MIN, Math.min(PSI_MAX, psi));
  return ARC_START + ((clamped - PSI_MIN) / (PSI_MAX - PSI_MIN)) * ARC_RANGE;
}

function degToRad(deg) {
  return (deg * Math.PI) / 180;
}

function signed(psi) {
  const n = Number(psi);
  return `${n >= 0 ? "+" : ""}${n.toFixed(1)}`;
}

function drawFixedPsi(psi, decimalX, baselineY, scale) {
  const value = Number(psi);
  const absoluteTenths = Math.round(Math.abs(value) * 10);
  const whole = Math.floor(absoluteTenths / 10);
  const tenth = absoluteTenths % 10;
  const gap = 5 * scale;
  const fractionalGap = 11 * scale;

  /* The integer's right edge and the fractional digit's left edge are fixed.
   * A tens digit grows left instead of shifting the value as a whole. */
  ctx.textAlign = "right";
  ctx.fillText(`${value < -0.05 ? "−" : ""}${whole}`, decimalX - gap, baselineY);
  ctx.textAlign = "center";
  ctx.fillText(".", decimalX, baselineY);
  ctx.textAlign = "left";
  ctx.fillText(String(tenth), decimalX + fractionalGap, baselineY);
  ctx.textAlign = "center";
}
/**
 * Match firmware boost_gauge.c: fill from zero notch toward current PSI.
 * Vacuum grows counter-clockwise; boost clockwise. Small gaps hide rounded ends.
 */
function valueArcAngles(psi) {
  const zero = psiToAngle(0);
  const val = psiToAngle(psi);
  let start;
  let end;
  if (psi >= 0) {
    start = zero + ZERO_GAP_BOOST;
    end = val;
    if (end < start) end = start;
  } else {
    start = val;
    end = zero - ZERO_GAP_VAC;
    if (end < start) end = start;
  }
  if (Math.abs(end - start) < 0.4) {
    start = zero;
    end = zero;
  }
  return { start, end };
}

function drawGauge(sample) {
  const dpr = Math.min(window.devicePixelRatio || 1, CANVAS_DPR_MAX);
  const rect = el.canvas.getBoundingClientRect();
  const cssW = Math.max(1, rect.width);
  const cssH = Math.max(1, rect.height);
  const pixelW = Math.round(cssW * dpr);
  const pixelH = Math.round(cssH * dpr);
  if (el.canvas.width !== pixelW || el.canvas.height !== pixelH) {
    el.canvas.width = pixelW;
    el.canvas.height = pixelH;
  }

  const width = el.canvas.width;
  const height = el.canvas.height;
  /* Work in CSS pixels so geometry matches the 466 face regardless of DPR. */
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  const size = Math.min(cssW, cssH);
  const scale = size / 466;
  const cx = cssW / 2;
  const cy = cssH / 2;

  /* Physical panel arc: 410 px diameter, 45 px stroke, safely inset. */
  const outerR = (410 / 2) * scale;
  const stroke = 45 * scale;
  const radius = outerR - stroke / 2;
  const psi = Number(sample.psi ?? 0);
  const { start, end } = valueArcAngles(psi);

  ctx.clearRect(0, 0, cssW, cssH);

  /* Pure black face */
  ctx.fillStyle = state.palette.face;
  ctx.beginPath();
  ctx.arc(cx, cy, size / 2, 0, Math.PI * 2);
  ctx.fill();

  /* Background track 270° */
  ctx.lineWidth = stroke;
  ctx.lineCap = "round";
  ctx.strokeStyle = state.palette.track;
  ctx.globalAlpha = 0.6;
  ctx.beginPath();
  ctx.arc(cx, cy, radius, degToRad(ARC_START), degToRad(ARC_START + ARC_RANGE));
  ctx.stroke();
  ctx.globalAlpha = 1;

  /* Value arc from zero toward psi */
  if (Math.abs(end - start) >= 0.4) {
    ctx.strokeStyle = colorFor(psi);
    ctx.beginPath();
    ctx.arc(cx, cy, radius, degToRad(start), degToRad(end));
    ctx.stroke();
  }

  /* Zero notch — thick radial ice tick inside the ring */
  const zero = degToRad(psiToAngle(0));
  const rOuter = outerR - 1 * scale;
  const rInner = rOuter - stroke + 1 * scale;
  ctx.strokeStyle = state.palette.zero;
  ctx.lineWidth = 20 * scale;
  ctx.lineCap = "round";
  ctx.beginPath();
  ctx.moveTo(cx + rInner * Math.cos(zero), cy + rInner * Math.sin(zero));
  ctx.lineTo(cx + rOuter * Math.cos(zero), cy + rOuter * Math.sin(zero));
  ctx.stroke();

  /* Tick labels */
  ctx.textAlign = "center";
  ctx.textBaseline = "middle";
  ctx.fillStyle = state.palette.muted;
  ctx.font = `700 ${Math.max(12, 16 * scale)}px system-ui, -apple-system, "Segoe UI", sans-serif`;
  for (const [value, label] of [[-15, "-15"], [0, "0"], [10, "10"], [18, "18"], [25, "25"]]) {
    let r = 140 * scale;
    if (Math.abs(value) < 0.01) r = 122 * scale;
    const a = degToRad(psiToAngle(value));
    ctx.fillText(label, cx + r * Math.cos(a), cy + r * Math.sin(a));
  }

  /* Center stack — zone / PSI / unit / peak / mode (matches physical UI) */
  const zone = sample.zone || zoneFor(psi);
  const peak = Math.max(0, Number(sample.peakPsi || 0));
  ctx.textAlign = "center";
  ctx.textBaseline = "middle";

  ctx.fillStyle = colorFor(psi);
  ctx.font = `700 ${Math.max(12, 15 * scale)}px system-ui, -apple-system, "Segoe UI", sans-serif`;
  ctx.fillText(zone, cx, cy - 88 * scale);

  ctx.fillStyle = psi >= OVERBOOST ? state.palette.overboost : state.palette.text;
  ctx.font = `700 ${Math.max(40, 58 * scale)}px system-ui, -apple-system, "Segoe UI", sans-serif`;
  drawFixedPsi(psi, cx, cy - 16 * scale, scale);

  ctx.fillStyle = state.palette.muted;
  ctx.font = `700 ${Math.max(12, 15 * scale)}px system-ui, -apple-system, "Segoe UI", sans-serif`;
  ctx.fillText("PSI", cx, cy + 28 * scale);

  ctx.fillStyle = peak >= OVERBOOST ? state.palette.overboost : state.palette.boost;
  ctx.font = `700 ${Math.max(12, 14 * scale)}px system-ui, -apple-system, "Segoe UI", sans-serif`;
  ctx.fillText(`PEAK  ${peak.toFixed(1)}`, cx, cy + 56 * scale);

  ctx.fillStyle = state.palette.muted;
  ctx.font = `700 ${Math.max(11, 12 * scale)}px system-ui, -apple-system, "Segoe UI", sans-serif`;
  ctx.fillText(sample.demo ? "DEMO" : "LIVE", cx, cy + 84 * scale);
}

function scheduleGaugeRender() {
  if (state.gaugeRaf !== null) return;
  state.gaugeRaf = requestAnimationFrame(renderGaugeFrame);
}

function renderGaugeFrame(at) {
  state.gaugeRaf = null;
  const target = state.gaugeTarget;
  if (!target) return;

  const previousAt = state.gaugeLastAt;
  const rawElapsedMs = previousAt ? at - previousAt : GAUGE_FRAME_MS;
  const elapsedMs = rawElapsedMs > GAUGE_GAP_RESET_MS ? GAUGE_FRAME_MS : rawElapsedMs;
  state.gaugeLastAt = at;
  const alpha = 1 - Math.exp(-elapsedMs / 90);
  state.gaugePsi += (Number(target.psi ?? 0) - state.gaugePsi) * alpha;

  drawGauge({ ...target, psi: state.gaugePsi });

  if (Math.abs(Number(target.psi ?? 0) - state.gaugePsi) > 0.01) {
    scheduleGaugeRender();
  }
}

function drawSparkline() {
  const dpr = Math.min(window.devicePixelRatio || 1, CANVAS_DPR_MAX);
  const rect = el.sparkline.getBoundingClientRect();
  el.sparkline.width = Math.max(1, Math.round(rect.width * dpr));
  el.sparkline.height = Math.max(1, Math.round(rect.height * dpr));
  const w = el.sparkline.width;
  const h = el.sparkline.height;
  sparkCtx.setTransform(1, 0, 0, 1, 0, 0);
  sparkCtx.clearRect(0, 0, w, h);
  sparkCtx.fillStyle = "rgba(32, 36, 44, 0.42)";
  sparkCtx.fillRect(0, 0, w, h);

  const zeroY = h - ((0 - PSI_MIN) / (PSI_MAX - PSI_MIN)) * h;
  sparkCtx.strokeStyle = state.palette.muted;
  sparkCtx.globalAlpha = 0.38;
  sparkCtx.beginPath();
  sparkCtx.moveTo(0, zeroY);
  sparkCtx.lineTo(w, zeroY);
  sparkCtx.stroke();
  sparkCtx.globalAlpha = 1;

  if (sampleHistory.length < 2) return;
  const endMs = sampleHistory.at(-1).historyTimeMs;
  const startMs = endMs - HISTORY_WINDOW_MS;
  sparkCtx.lineWidth = Math.max(2, 2 * dpr);
  sparkCtx.lineJoin = "round";
  sparkCtx.lineCap = "round";
  sparkCtx.strokeStyle = state.palette.vacuum;
  sparkCtx.beginPath();
  sampleHistory.forEach((sample, index) => {
    const x = Math.max(0, Math.min(w, ((sample.historyTimeMs - startMs) / HISTORY_WINDOW_MS) * w));
    const y = h - ((sample.psi - PSI_MIN) / (PSI_MAX - PSI_MIN)) * h;
    if (index === 0) sparkCtx.moveTo(x, y);
    else sparkCtx.lineTo(x, y);
  });
  sparkCtx.stroke();

  const last = sampleHistory.at(-1);
  sparkCtx.fillStyle = colorFor(last.psi);
  const lx = Math.max(0, Math.min(w, ((last.historyTimeMs - startMs) / HISTORY_WINDOW_MS) * w));
  const ly = h - ((last.psi - PSI_MIN) / (PSI_MAX - PSI_MIN)) * h;
  sparkCtx.beginPath();
  sparkCtx.arc(lx, ly, 4 * dpr, 0, Math.PI * 2);
  sparkCtx.fill();
}

function pushSample(sample) {
  const uptimeMs = Number(sample?.uptimeMs);
  const targetUptimeMs = Number(state.gaugeTarget?.uptimeMs);
  if (Number.isFinite(uptimeMs) && Number.isFinite(targetUptimeMs) && uptimeMs <= targetUptimeMs) {
    return false;
  }

  const historyTimeMs = Number.isFinite(uptimeMs) ? uptimeMs : performance.now();
  sample.historyTimeMs = historyTimeMs;
  sampleHistory.push(sample);
  const cutoffMs = historyTimeMs - HISTORY_WINDOW_MS;
  while (sampleHistory.length && sampleHistory[0].historyTimeMs < cutoffMs) sampleHistory.shift();
  el.sampleCount.textContent = "Last 60 seconds";
  el.emptyState.hidden = sampleHistory.length > 0;
  state.gaugeTarget = sample;
  if (sampleHistory.length === 1) state.gaugePsi = Number(sample.psi ?? 0);
  scheduleGaugeRender();

  const now = performance.now();
  if (now - state.sparklineLastAt >= SPARKLINE_FRAME_MS) {
    state.sparklineLastAt = now;
    drawSparkline();
  }
  return true;
}

function updateConnection(mode, text) {
  if (state.connected === (mode === "online") && el.connectionText.textContent === text) return;
  state.connected = mode === "online";
  el.connection.classList.remove("online", "offline");
  el.connection.classList.add(mode);
  el.connectionText.textContent = text;
}

function clearHeartbeat() {
  if (state.heartbeatTimer !== null) {
    window.clearInterval(state.heartbeatTimer);
    state.heartbeatTimer = null;
  }
}

function startHeartbeat(socket) {
  clearHeartbeat();
  state.heartbeatTimer = window.setInterval(() => {
    if (state.liveSocket !== socket || socket.readyState !== WebSocket.OPEN) {
      clearHeartbeat();
      return;
    }
    try {
      socket.send("ping");
    } catch (_) {
      clearHeartbeat();
    }
  }, 750);
}

function showError(message) {
  el.errorBox.hidden = !message;
  el.errorBox.textContent = message || "";
  if (message) {
    el.okBox.hidden = true;
  }
}

function showOk(message) {
  el.okBox.hidden = !message;
  el.okBox.textContent = message || "";
  if (message) {
    el.errorBox.hidden = true;
    window.setTimeout(() => {
      if (el.okBox.textContent === message) {
        el.okBox.hidden = true;
      }
    }, 3500);
  }
}

async function api(path, options = {}) {
  const opts = { ...options };
  const headers = new Headers(options.headers || {});
  if (opts.body && !(opts.body instanceof FormData) && !(opts.body instanceof Blob) && !headers.has("Content-Type")) {
    headers.set("Content-Type", "application/json");
  }
  opts.headers = headers;
  const response = await fetch(`${API}${path}`, opts);
  if (!response.ok) {
    const text = await response.text();
    let msg = text || `${response.status} ${response.statusText}`;
    try {
      const j = JSON.parse(text);
      if (j.error) msg = j.error;
    } catch (_) {
      /* keep raw */
    }
    throw new Error(msg);
  }
  if (response.status === 204) return null;
  const type = response.headers.get("Content-Type") || "";
  return type.includes("json") ? response.json() : response.text();
}

function formatDuration(ms) {
  const total = Math.floor(ms / 1000);
  const hours = Math.floor(total / 3600);
  const minutes = Math.floor((total % 3600) / 60);
  const seconds = total % 60;
  if (hours) return `${hours}h ${minutes}m`;
  if (minutes) return `${minutes}m ${seconds}s`;
  return `${seconds}s`;
}

function formatClock(epochMs, offsetMinutes) {
  if (!epochMs || epochMs < 1e12) return "--";
  const shifted = new Date(epochMs + offsetMinutes * 60000);
  return `${String(shifted.getUTCHours()).padStart(2, "0")}:${String(shifted.getUTCMinutes()).padStart(2, "0")}`;
}

function minutesToTime(minutes) {
  const h = Math.floor(minutes / 60) % 24;
  const m = minutes % 60;
  return `${String(h).padStart(2, "0")}:${String(m).padStart(2, "0")}`;
}

function timeToMinutes(value) {
  if (!value) return 0;
  const [h, m] = value.split(":").map(Number);
  return h * 60 + m;
}

function renderState(sample) {
  if (sample.firmwareVersion) {
    el.firmwareVersion.textContent = sample.firmwareVersion;
  }
  el.uptime.textContent = formatDuration(sample.uptimeMs || 0);
  el.deviceClock.textContent = formatClock(sample.epochMs, sample.timezoneOffsetMinutes || 0);
  if (sample.brightness != null) {
    el.brightnessNow.textContent = `${sample.brightness}%`;
  }
  if (sample.activeThemeId && sample.activeThemeId !== state.activeThemeId) {
    state.activeThemeId = sample.activeThemeId;
    const activeTheme = state.themes.find((theme) => theme.id === state.activeThemeId);
    if (activeTheme) {
      setTheme(activeTheme);
      renderThemes();
    }
  }
  pushSample(sample);
}

function renderConfig(config) {
  state.config = config;
  el.tzOffset.value = config.timezoneOffsetMinutes ?? 0;
  el.scheduleEnabled.checked = Boolean(config.dimSchedule?.enabled);
  el.scheduleStart.value = minutesToTime(config.dimSchedule?.startMinutes ?? 1260);
  el.scheduleEnd.value = minutesToTime(config.dimSchedule?.endMinutes ?? 420);
  el.brightnessHigh.value = config.brightnessHigh ?? 100;
  el.brightnessLow.value = config.brightnessLow ?? 12;
  el.brightnessHighOut.textContent = `${el.brightnessHigh.value}%`;
  el.brightnessLowOut.textContent = `${el.brightnessLow.value}%`;
}

function renderNetwork(net) {
  state.network = net;
  el.netMode.textContent = (net.mode || "--").toUpperCase();
  el.netModeSelect.value = net.mode === "ap" ? "ap" : "apsta";
  el.netStaState.textContent = net.staConnected
    ? `UP ${net.rssi || ""} dBm`.trim()
    : net.staEnabled
      ? "Connecting…"
      : "Off";
  el.netStaIp.textContent = net.staIp || "—";
  el.netApSsid.textContent = net.apSsid || "—";
  if (document.activeElement !== el.netSsid) {
    el.netSsid.value = net.staSsid || "";
  }
  el.netHint.textContent = net.staConnected && net.staIp
    ? `Live on http://${net.staIp}/ · SoftAP ${net.apSsid || ""} still online`
    : "SoftAP stays up as fallback. Saving STA may change the LAN IP.";
}

function authLabel(auth) {
  return Number(auth) === 0 ? "open" : "secured";
}

function renderScannedNetworks(networks) {
  state.scannedNetworks = Array.isArray(networks) ? networks : [];
  const currentSsid = el.netSsid.value;
  el.netScanSelect.replaceChildren(new Option("Manual SSID entry", ""));
  for (const net of state.scannedNetworks) {
    if (!net || !net.ssid) continue;
    const label = `${net.ssid} · ${net.rssi} dBm · ${authLabel(net.auth)}`;
    el.netScanSelect.append(new Option(label, net.ssid));
  }
  el.netScanSelect.value = state.scannedNetworks.some((net) => net.ssid === currentSsid) ? currentSsid : "";
}

async function scanNetworks() {
  const previous = el.scanNetworksBtn.textContent;
  el.scanNetworksBtn.disabled = true;
  el.scanNetworksBtn.textContent = "Scanning…";
  try {
    const payload = await api("/network/scan");
    renderScannedNetworks(payload.networks || []);
    showOk(state.scannedNetworks.length
      ? `Found ${state.scannedNetworks.length} network${state.scannedNetworks.length === 1 ? "" : "s"}`
      : "No networks found");
  } catch (error) {
    renderScannedNetworks([]);
    showError(`Scan failed: ${error.message}`);
  } finally {
    el.scanNetworksBtn.disabled = false;
    el.scanNetworksBtn.textContent = previous;
  }
}

function renderThemes() {
  el.themeList.replaceChildren();
  for (const theme of state.themes) {
    const button = document.createElement("button");
    button.className = `theme-option${theme.id === state.activeThemeId ? " active" : ""}`;
    button.type = "button";
    button.innerHTML = `
      <span class="theme-dots">
        <i style="background:${theme.colors.vacuum}"></i>
        <i style="background:${theme.colors.boost}"></i>
        <i style="background:${theme.colors.overboost}"></i>
      </span>
      <span>${theme.name}</span>
      <span>${theme.id === state.activeThemeId ? "ACTIVE" : ""}</span>
    `;
    button.addEventListener("click", async () => {
      try {
        showError("");
        const payload = await api("/themes/active", {
          method: "PUT",
          body: JSON.stringify({ id: theme.id }),
        });
        state.activeThemeId = payload.activeThemeId || theme.id;
        setTheme(theme);
        state.config = { ...state.config, activeThemeId: state.activeThemeId };
        renderThemes();
        showOk(`Theme ${theme.name}`);
        /* brightness may have changed with theme */
        const st = await api("/state");
        renderState(st);
      } catch (error) {
        showError(error.message);
      }
    });
    el.themeList.append(button);
  }
}

async function refreshNetwork() {
  const net = await api("/network");
  renderNetwork(net);
  return net;
}

async function refreshAll() {
  try {
    showError("");
    /* Keep device wall-clock aligned with the browser so dim schedules match local time. */
    try {
      const now = new Date();
      const tz = -now.getTimezoneOffset();
      await api("/time", {
        method: "POST",
        body: JSON.stringify({ epochMs: now.getTime(), timezoneOffsetMinutes: tz }),
      });
      el.tzOffset.value = tz;
    } catch (_) {
      /* non-fatal; schedule may wait until manual Sync */
    }
    const [statePayload, config, themes, media, network] = await Promise.all([
      api("/state"),
      api("/config"),
      api("/themes"),
      api("/media/status"),
      api("/network"),
    ]);
    state.themes = themes.themes || [];
    state.activeThemeId = themes.activeThemeId || statePayload.activeThemeId || state.activeThemeId;
    renderThemes();
    setTheme(state.themes.find((theme) => theme.id === state.activeThemeId));
    renderConfig(config);
    renderState(statePayload);
    renderMediaStatus(media);
    renderNetwork(network);
    updateConnection("online", "Live");
  } catch (error) {
    updateConnection("offline", "Disconnected");
    showError(error.message);
  }
}

async function pollState() {
  if (state.pollInFlight) return;
  state.pollInFlight = true;
  try {
    const sample = await api("/state");
    renderState(sample);
    updateConnection("online", "Live");
    showError("");
  } catch (error) {
    updateConnection("offline", "Disconnected");
    showError(error.message);
  } finally {
    state.pollInFlight = false;
  }
}

function startPolling() {
  if (state.pollTimer) return;
  state.fallbackActive = true;
  state.pollTimer = window.setInterval(() => { void pollState(); }, POLL_FRAME_MS);
  void pollState();
}

function stopPolling() {
  if (state.pollTimer) {
    window.clearInterval(state.pollTimer);
    state.pollTimer = null;
  }
  state.fallbackActive = false;
}

function scheduleReconnect() {
  if (state.reconnectTimer !== null) return;
  state.reconnectTimer = window.setTimeout(() => {
    state.reconnectTimer = null;
    connectEvents();
  }, 1000);
}

function connectEvents() {
  if (state.liveSocket && (state.liveSocket.readyState === WebSocket.OPEN || state.liveSocket.readyState === WebSocket.CONNECTING)) {
    return;
  }
  if (typeof WebSocket !== "function") {
    startPolling();
    return;
  }
  const scheme = location.protocol === "https:" ? "wss:" : "ws:";
  const socket = new WebSocket(`${scheme}//${location.host}${API}/state/ws`);
  state.liveSocket = socket;
  updateConnection("offline", "Disconnected");
  socket.onopen = () => {
    if (state.liveSocket !== socket) return;
    stopPolling();
    startHeartbeat(socket);
    updateConnection("online", "Live");
    showError("");
  };
  socket.onmessage = (event) => {
    if (state.liveSocket !== socket) return;
    try {
      const sample = JSON.parse(event.data);
      renderState(sample);
      updateConnection("online", "Live");
      showError("");
    } catch (error) {
      showError(`Live telemetry error: ${error.message}`);
    }
  };
  socket.onerror = () => {
    /* onclose owns fallback/reconnect; never surface a rejected socket event. */
  };
  socket.onclose = () => {
    if (state.liveSocket !== socket) return;
    clearHeartbeat();
    state.liveSocket = null;
    startPolling();
    scheduleReconnect();
  };
}

async function syncTime() {
  const now = new Date();
  const timezoneOffsetMinutes = Number(el.tzOffset.value);
  const tz = Number.isFinite(timezoneOffsetMinutes)
    ? timezoneOffsetMinutes
    : -now.getTimezoneOffset();
  const response = await api("/time", {
    method: "POST",
    body: JSON.stringify({ epochMs: now.getTime(), timezoneOffsetMinutes: tz }),
  });
  el.tzOffset.value = response.timezoneOffsetMinutes;
  renderState(response);
  showOk("Time synchronized");
}

async function saveConfig() {
  const payload = {
    brightnessHigh: Number(el.brightnessHigh.value),
    brightnessLow: Number(el.brightnessLow.value),
    timezoneOffsetMinutes: Number(el.tzOffset.value),
    activeThemeId: state.activeThemeId,
    dimSchedule: {
      enabled: el.scheduleEnabled.checked,
      startMinutes: timeToMinutes(el.scheduleStart.value),
      endMinutes: timeToMinutes(el.scheduleEnd.value),
    },
  };
  /* Ensure wall clock is current before evaluating the schedule window. */
  try {
    const now = new Date();
    const tz = Number(el.tzOffset.value);
    await api("/time", {
      method: "POST",
      body: JSON.stringify({
        epochMs: now.getTime(),
        timezoneOffsetMinutes: Number.isFinite(tz) ? tz : -now.getTimezoneOffset(),
      }),
    });
  } catch (_) {}
  renderConfig(await api("/config", { method: "PUT", body: JSON.stringify(payload) }));
  /* Give the control task a beat to apply brightness, then refresh. */
  await new Promise((r) => setTimeout(r, 300));
  const st = await api("/state");
  renderState(st);
  if (payload.dimSchedule.enabled) {
    showOk(`Schedule saved · brightness now ${st.brightness}%`);
  } else {
    showOk(`Schedule off · brightness now ${st.brightness}%`);
  }
}

async function saveNetwork() {
  const body = {
    mode: el.netModeSelect.value,
    ssid: el.netSsid.value,
    keepPassword: el.netKeepPassword.checked && !el.netPassword.value,
  };
  if (el.netPassword.value) {
    body.password = el.netPassword.value;
    body.keepPassword = false;
  }
  const beforeIp = state.network?.staIp;
  showOk("Applying Wi‑Fi…");
  const net = await api("/network", { method: "PUT", body: JSON.stringify(body) });
  el.netPassword.value = "";
  renderNetwork(net);
  if (net.staConnected && net.staIp && net.staIp !== beforeIp) {
    showOk(`Connected — open http://${net.staIp}/ if this page drops`);
  } else if (net.staEnabled && !net.staConnected) {
    showOk("Saved — waiting for association (SoftAP still up)");
  } else {
    showOk("Network settings saved");
  }
}

async function reconnectNetwork() {
  const net = await api("/network/reconnect", { method: "POST", body: "{}" });
  renderNetwork(net);
  showOk("Reconnect requested");
}

async function loadLogs() {
  const payload = await api("/logs?limit=120");
  const samples = payload.samples || [];
  if (!samples.length) {
    el.logSummary.textContent = "History is empty.";
    return;
  }
  const peak = Math.max(...samples.map((sample) => sample.psi));
  const low = Math.min(...samples.map((sample) => sample.psi));
  el.logSummary.textContent = `${samples.length} samples loaded. Range ${signed(low)} to ${signed(peak)} PSI.`;
  showOk(`Loaded ${samples.length} log samples`);
}

async function clearLogs() {
  await api("/logs", { method: "DELETE" });
  el.logSummary.textContent = "History cleared.";
  showOk("Logs cleared");
}

async function exportLogs() {
  const response = await fetch(`${API}/logs.csv`, { cache: "no-store" });
  if (!response.ok) throw new Error(`CSV export failed (${response.status})`);
  const csv = await response.blob();
  const sampleCount = Math.max(0, (await csv.text()).trim().split("\n").length - 1);
  const url = URL.createObjectURL(csv);
  const link = document.createElement("a");
  link.href = url;
  link.download = "boost-gauge-log.csv";
  document.body.append(link);
  link.click();
  link.remove();
  window.setTimeout(() => URL.revokeObjectURL(url), 1000);
  showOk(`CSV exported · ${sampleCount} samples`);
}

function renderMediaStatus(media) {
  if (!media || !media.present) {
    el.mediaStatus.textContent = "No GIF uploaded";
    el.mediaPreview.hidden = true;
    el.mediaPreview.removeAttribute("src");
    return;
  }
  const kb = Math.round((media.sizeBytes ?? media.size ?? 0) / 1024);
  el.mediaStatus.textContent = `${media.name || "active.gif"} · ${kb} KB · ${media.playbackSupported ? "playing on AMOLED" : "ready"}`;
  el.mediaPreview.hidden = true;
  el.mediaPreview.removeAttribute("src");
}

function setUploadControls(uploading) {
  el.mediaFile.disabled = uploading;
  el.uploadMediaBtn.disabled = uploading;
  el.uploadOtaBtn.disabled = uploading;
  const mediaUpload = state.activeUpload?.path === "/media";
  el.deleteMediaBtn.disabled = state.mediaDeleteInFlight || (!mediaUpload && uploading);
  el.deleteMediaBtn.textContent = mediaUpload ? "Cancel & delete" : "Delete";
}

async function uploadWithProgress(path, file, progressEl, statusEl, contentType) {
  if (state.activeUpload) throw new Error("An upload is already in progress.");
  let settled = false;
  let settle;
  const settlement = new Promise((resolve) => { settle = resolve; });
  const upload = { path, xhr: null, settlement, cancelRequested: false };
  state.activeUpload = upload;
  setUploadControls(true);
  const resumePolling = Boolean(state.pollTimer);
  if (resumePolling) stopPolling();
  while (state.pollInFlight) await new Promise((resolve) => setTimeout(resolve, 20));
  try {
    return await new Promise((resolve, reject) => {
      const xhr = new XMLHttpRequest();
      upload.xhr = xhr;
      const timeout = window.setTimeout(() => xhr.abort(), path === "/media" ? 300000 : 120000);
      if (upload.cancelRequested) {
        xhr.abort();
        return;
      }
      const finish = (error, payload) => {
        if (settled) return;
        settled = true;
        window.clearTimeout(timeout);
        if (state.activeUpload === upload) state.activeUpload = null;
        setUploadControls(false);
        if (resumePolling) startPolling();
        settle();
        if (error) reject(error); else resolve(payload);
      };
      xhr.open("POST", `${API}${path}`);
      xhr.setRequestHeader("Content-Type", contentType || file.type || "application/octet-stream");
      xhr.setRequestHeader("X-Filename", encodeURIComponent(file.name || "upload.bin"));
      xhr.upload.onprogress = (event) => {
        if (event.lengthComputable) progressEl.value = Math.round((event.loaded / event.total) * 100);
      };
      xhr.onload = () => {
        if (xhr.status >= 200 && xhr.status < 300) {
          progressEl.value = 100;
          try { finish(null, xhr.responseText ? JSON.parse(xhr.responseText) : {}); }
          catch (error) { finish(new Error(`Bad upload response: ${error.message}`)); }
          return;
        }
        let msg = xhr.responseText || `Upload failed with ${xhr.status}`;
        try { const payload = JSON.parse(xhr.responseText); if (payload.error) msg = payload.error; } catch (_) { /* raw */ }
        finish(new Error(msg));
      };
      xhr.onerror = () => finish(new Error("Upload failed or timed out"));
      xhr.onabort = () => {
        const error = new Error("Upload canceled");
        error.name = "AbortError";
        statusEl.textContent = "Upload canceled";
        finish(error);
      };
      statusEl.textContent = "Uploading…";
      progressEl.value = 0;
      xhr.send(file);
    });
  } finally {
    if (!settled) {
      settled = true;
      if (state.activeUpload === upload) state.activeUpload = null;
      setUploadControls(false);
      if (resumePolling) startPolling();
      settle();
    }
  }
}

async function uploadMedia() {
  const file = el.mediaFile.files[0];
  if (!file) throw new Error("Choose a GIF first.");
  const maxWidth = Number(el.mediaFile.dataset.maxWidth);
  const maxHeight = Number(el.mediaFile.dataset.maxHeight);
  if (!maxWidth || !maxHeight) throw new Error("Dashboard GIF limits are unavailable; refresh the page and retry.");
  const payload = await uploadWithProgress("/media", file, el.mediaProgress, el.mediaStatus, "image/gif");
  renderMediaStatus(payload);
  showOk(payload.playbackSupported ? "GIF uploaded and playing on the AMOLED" : "GIF uploaded");
}
async function deleteMedia() {
  if (state.mediaDeleteInFlight) return;
  state.mediaDeleteInFlight = true;
  setUploadControls(Boolean(state.activeUpload));
  try {
    const upload = state.activeUpload;
    if (upload?.path === "/media") {
      upload.cancelRequested = true;
      upload.xhr?.abort();
      await upload.settlement;
    }
    await api("/media", { method: "DELETE" });
    el.mediaProgress.value = 0;
    renderMediaStatus({ present: false });
    showOk("GIF deleted");
  } finally {
    state.mediaDeleteInFlight = false;
    setUploadControls(Boolean(state.activeUpload));
  }
}

async function uploadOta() {
  const file = el.otaFile.files[0];
  if (!file) throw new Error("Choose an ESP-IDF app binary first.");
  const payload = await uploadWithProgress("/ota", file, el.otaProgress, el.otaStatus, "application/octet-stream");
  el.otaStatus.textContent = payload.restartRequired ? "Verified · restart the gauge to activate" : (payload.status || "OTA accepted");
  showOk(el.otaStatus.textContent);
}

function wireControls() {
  el.refreshBtn.addEventListener("click", () => refreshAll().catch((e) => showError(e.message)));
  el.syncTimeBtn.addEventListener("click", () => syncTime().catch((error) => showError(error.message)));
  el.saveConfigBtn.addEventListener("click", () => saveConfig().catch((error) => showError(error.message)));
  el.loadLogsBtn.addEventListener("click", () => loadLogs().catch((error) => showError(error.message)));
  el.exportLogsBtn.addEventListener("click", () => exportLogs().catch((error) => showError(error.message)));
  el.clearLogsBtn.addEventListener("click", () => clearLogs().catch((error) => showError(error.message)));
  el.uploadMediaBtn.addEventListener("click", () => uploadMedia().catch((error) => showError(error.message)));
  el.deleteMediaBtn.addEventListener("click", () => deleteMedia().catch((error) => showError(error.message)));
  el.uploadOtaBtn.addEventListener("click", () => uploadOta().catch((error) => showError(error.message)));
  el.saveNetworkBtn.addEventListener("click", () => saveNetwork().catch((error) => showError(error.message)));
  el.reconnectNetworkBtn.addEventListener("click", () => reconnectNetwork().catch((error) => showError(error.message)));
  el.networkRefreshBtn.addEventListener("click", () => refreshNetwork().catch((error) => showError(error.message)));
  el.scanNetworksBtn.addEventListener("click", () => scanNetworks());
  el.netScanSelect.addEventListener("change", () => {
    if (el.netScanSelect.value) {
      el.netSsid.value = el.netScanSelect.value;
      el.netModeSelect.value = "apsta";
    }
  });
  el.brightnessHigh.addEventListener("input", () => { el.brightnessHighOut.textContent = `${el.brightnessHigh.value}%`; });
  el.brightnessLow.addEventListener("input", () => { el.brightnessLowOut.textContent = `${el.brightnessLow.value}%`; });
  el.mediaFile.addEventListener("change", () => {
    el.mediaStatus.textContent = el.mediaFile.files[0]?.name || "No active GIF";
  });
  el.otaFile.addEventListener("change", () => {
    el.otaStatus.textContent = el.otaFile.files[0]?.name || "Idle";
  });
  window.addEventListener("resize", () => {
    scheduleGaugeRender();
    drawSparkline();
  });
}

wireControls();

updatePalette();
state.gaugeTarget = { psi: 0, peakPsi: 0, zone: "ATMO", demo: true };
scheduleGaugeRender();
drawSparkline();
refreshAll().finally(connectEvents);
