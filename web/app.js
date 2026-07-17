"use strict";

const API = "/api/v1";
const PSI_MIN = -15;
const PSI_MAX = 25;
const OVERBOOST = 18;
const ARC_START = 135;
const ARC_RANGE = 270;
const sampleHistory = [];
const maxSamples = 420;

const state = {
  activeThemeId: "pit-lane",
  themes: [],
  config: null,
  connected: false,
  lastEventAt: 0,
  pollTimer: null,
};

const el = {
  connection: document.getElementById("connection"),
  connectionText: document.getElementById("connectionText"),
  errorBox: document.getElementById("errorBox"),
  canvas: document.getElementById("gaugeCanvas"),
  sparkline: document.getElementById("sparkline"),
  gaugeDevice: document.getElementById("gaugeDevice"),
  zone: document.getElementById("zone"),
  psi: document.getElementById("psi"),
  peak: document.getElementById("peak"),
  modeChip: document.getElementById("modeChip"),
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
  mediaFile: document.getElementById("mediaFile"),
  uploadMediaBtn: document.getElementById("uploadMediaBtn"),
  deleteMediaBtn: document.getElementById("deleteMediaBtn"),
  mediaStatus: document.getElementById("mediaStatus"),
  mediaProgress: document.getElementById("mediaProgress"),
  otaFile: document.getElementById("otaFile"),
  uploadOtaBtn: document.getElementById("uploadOtaBtn"),
  otaStatus: document.getElementById("otaStatus"),
  otaProgress: document.getElementById("otaProgress"),
};

const ctx = el.canvas.getContext("2d");
const sparkCtx = el.sparkline.getContext("2d");

function cssVar(name) {
  return getComputedStyle(document.documentElement).getPropertyValue(name).trim();
}

function setTheme(theme) {
  if (!theme) return;
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
  renderThemes();
  drawGauge(sampleHistory.at(-1) || { psi: 0, peakPsi: 0, zone: "ATMO", demo: true });
  drawSparkline();
}

function zoneFor(psi) {
  if (psi >= OVERBOOST) return "OVER";
  if (psi >= 0.35) return "BOOST";
  if (psi > -0.35) return "ATMO";
  return "VAC";
}

function colorFor(psi) {
  if (psi >= OVERBOOST) return cssVar("--overboost");
  if (psi >= 0) return cssVar("--boost");
  return cssVar("--vacuum");
}

function psiToAngle(psi) {
  const clamped = Math.max(PSI_MIN, Math.min(PSI_MAX, psi));
  return ARC_START + ((clamped - PSI_MIN) / (PSI_MAX - PSI_MIN)) * ARC_RANGE;
}

function degToRad(deg) {
  return (deg * Math.PI) / 180;
}

function signed(psi) {
  return `${psi >= 0 ? "+" : ""}${Number(psi).toFixed(1)}`;
}

function placeTicks() {
  document.querySelectorAll(".tick").forEach((tick) => tick.remove());
  const size = el.gaugeDevice.clientWidth;
  const cx = size / 2;
  const cy = size / 2;
  const radius = size * 0.404;
  for (const [value, label] of [[-15, "-15"], [0, "0"], [10, "10"], [18, "18"], [25, "25"]]) {
    const rad = degToRad(psiToAngle(value));
    const tick = document.createElement("div");
    tick.className = "tick";
    tick.textContent = label;
    tick.style.left = `${cx + radius * Math.cos(rad)}px`;
    tick.style.top = `${cy + radius * Math.sin(rad)}px`;
    el.gaugeDevice.appendChild(tick);
  }
}

function drawGauge(sample) {
  const dpr = window.devicePixelRatio || 1;
  const rect = el.canvas.getBoundingClientRect();
  if (el.canvas.width !== Math.round(rect.width * dpr)) {
    el.canvas.width = Math.round(rect.width * dpr);
    el.canvas.height = Math.round(rect.height * dpr);
  }

  const width = el.canvas.width;
  const height = el.canvas.height;
  const scale = width / 466;
  const cx = width / 2;
  const cy = height / 2;
  const radius = 191 * scale;
  const lineWidth = 18 * scale;
  const psi = Number(sample.psi || 0);
  const end = psiToAngle(psi);

  ctx.clearRect(0, 0, width, height);
  ctx.lineWidth = lineWidth;
  ctx.lineCap = "butt";
  ctx.strokeStyle = cssVar("--track");
  ctx.beginPath();
  ctx.arc(cx, cy, radius, degToRad(ARC_START), degToRad(ARC_START + ARC_RANGE));
  ctx.stroke();

  ctx.lineCap = "round";
  ctx.strokeStyle = colorFor(psi);
  ctx.beginPath();
  ctx.arc(cx, cy, radius, degToRad(ARC_START), degToRad(end));
  ctx.stroke();

  const zero = degToRad(psiToAngle(0));
  ctx.strokeStyle = cssVar("--zero");
  ctx.lineWidth = 4 * scale;
  ctx.beginPath();
  ctx.moveTo(cx + 200 * scale * Math.cos(zero), cy + 200 * scale * Math.sin(zero));
  ctx.lineTo(cx + 222 * scale * Math.cos(zero), cy + 222 * scale * Math.sin(zero));
  ctx.stroke();

  const zone = sample.zone || zoneFor(psi);
  el.psi.textContent = signed(psi);
  el.psi.style.color = psi >= OVERBOOST ? cssVar("--overboost") : cssVar("--text");
  el.zone.textContent = zone;
  el.zone.style.color = colorFor(psi);
  el.peak.textContent = `PEAK ${signed(Math.max(0, Number(sample.peakPsi || 0)))}`;
  el.peak.style.color = Number(sample.peakPsi || 0) >= OVERBOOST ? cssVar("--overboost") : cssVar("--boost");
  el.modeChip.textContent = sample.demo ? "DEMO" : "LIVE";
}

function drawSparkline() {
  const dpr = window.devicePixelRatio || 1;
  const rect = el.sparkline.getBoundingClientRect();
  el.sparkline.width = Math.round(rect.width * dpr);
  el.sparkline.height = Math.round(rect.height * dpr);
  const w = el.sparkline.width;
  const h = el.sparkline.height;
  sparkCtx.clearRect(0, 0, w, h);
  sparkCtx.fillStyle = "rgba(32, 36, 44, 0.42)";
  sparkCtx.fillRect(0, 0, w, h);

  const zeroY = h - ((0 - PSI_MIN) / (PSI_MAX - PSI_MIN)) * h;
  sparkCtx.strokeStyle = cssVar("--muted");
  sparkCtx.globalAlpha = 0.38;
  sparkCtx.beginPath();
  sparkCtx.moveTo(0, zeroY);
  sparkCtx.lineTo(w, zeroY);
  sparkCtx.stroke();
  sparkCtx.globalAlpha = 1;

  if (sampleHistory.length < 2) return;
  sparkCtx.lineWidth = Math.max(2, 2 * dpr);
  sparkCtx.lineJoin = "round";
  sparkCtx.lineCap = "round";
  sparkCtx.strokeStyle = cssVar("--vacuum");
  sparkCtx.beginPath();
  sampleHistory.forEach((sample, index) => {
    const x = (index / (maxSamples - 1)) * w;
    const y = h - ((sample.psi - PSI_MIN) / (PSI_MAX - PSI_MIN)) * h;
    if (index === 0) sparkCtx.moveTo(x, y);
    else sparkCtx.lineTo(x, y);
  });
  sparkCtx.stroke();

  const last = sampleHistory.at(-1);
  sparkCtx.fillStyle = colorFor(last.psi);
  const lx = ((sampleHistory.length - 1) / (maxSamples - 1)) * w;
  const ly = h - ((last.psi - PSI_MIN) / (PSI_MAX - PSI_MIN)) * h;
  sparkCtx.beginPath();
  sparkCtx.arc(lx, ly, 4 * dpr, 0, Math.PI * 2);
  sparkCtx.fill();
}

function pushSample(sample) {
  sampleHistory.push(sample);
  while (sampleHistory.length > maxSamples) sampleHistory.shift();
  el.sampleCount.textContent = `${sampleHistory.length} samples`;
  el.emptyState.hidden = sampleHistory.length > 0;
  drawGauge(sample);
  drawSparkline();
}

function updateConnection(mode, text) {
  state.connected = mode === "online";
  el.connection.classList.remove("online", "offline");
  el.connection.classList.add(mode);
  el.connectionText.textContent = text;
}

function showError(message) {
  el.errorBox.hidden = !message;
  el.errorBox.textContent = message || "";
}

async function api(path, options = {}) {
  const headers = options.headers || (options.body instanceof FormData ? undefined : { "Content-Type": "application/json" });
  const response = await fetch(`${API}${path}`, {
    headers,
    ...options,
  });
  if (!response.ok) {
    const text = await response.text();
    throw new Error(text || `${response.status} ${response.statusText}`);
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
  if (!epochMs) return "--";
  const shifted = new Date(epochMs + offsetMinutes * 60000);
  return `${String(shifted.getUTCHours()).padStart(2, "0")}:${String(shifted.getUTCMinutes()).padStart(2, "0")}`;
}

function minutesToTime(minutes) {
  const h = Math.floor(minutes / 60);
  const m = minutes % 60;
  return `${String(h).padStart(2, "0")}:${String(m).padStart(2, "0")}`;
}

function timeToMinutes(value) {
  if (!value) return 0;
  const [h, m] = value.split(":").map(Number);
  return h * 60 + m;
}

function renderState(sample) {
  el.firmwareVersion.textContent = sample.firmwareVersion || "--";
  el.uptime.textContent = formatDuration(sample.uptimeMs || 0);
  el.deviceClock.textContent = formatClock(sample.epochMs, sample.timezoneOffsetMinutes || 0);
  el.brightnessNow.textContent = `${sample.brightness ?? "--"}%`;
  if (sample.activeThemeId && sample.activeThemeId !== state.activeThemeId) {
    state.activeThemeId = sample.activeThemeId;
    const activeTheme = state.themes.find((theme) => theme.id === state.activeThemeId);
    if (activeTheme) setTheme(activeTheme);
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
      const payload = await api("/themes/active", { method: "PUT", body: JSON.stringify({ id: theme.id }) });
      state.activeThemeId = payload.activeThemeId || theme.id;
      setTheme(theme);
      state.config = { ...state.config, activeThemeId: state.activeThemeId };
      renderThemes();
    });
    el.themeList.append(button);
  }
}

async function refreshAll() {
  try {
    showError("");
    const [statePayload, config, themes, media] = await Promise.all([
      api("/state"),
      api("/config"),
      api("/themes"),
      api("/media/status"),
    ]);
    state.themes = themes.themes || [];
    state.activeThemeId = themes.activeThemeId || statePayload.activeThemeId || state.activeThemeId;
    renderThemes();
    setTheme(state.themes.find((theme) => theme.id === state.activeThemeId));
    renderConfig(config);
    renderState(statePayload);
    renderMediaStatus(media);
    updateConnection("online", "HTTP ready");
  } catch (error) {
    updateConnection("offline", "Offline");
    showError(error.message);
  }
}

async function pollState() {
  try {
    const sample = await api("/state");
    renderState(sample);
    updateConnection("online", "HTTP polling");
    showError("");
  } catch (error) {
    updateConnection("offline", "Offline");
    showError(error.message);
  }
}

function startPolling() {
  if (state.pollTimer) return;
  state.pollTimer = window.setInterval(pollState, 1000);
}

function stopPolling() {
  if (!state.pollTimer) return;
  window.clearInterval(state.pollTimer);
  state.pollTimer = null;
}

function connectEvents() {
  if (!window.EventSource) {
    startPolling();
    return;
  }
  const events = new EventSource(`${API}/events`);
  events.onopen = () => {
    stopPolling();
    updateConnection("online", "Live stream");
  };
  events.onmessage = (message) => {
    try {
      state.lastEventAt = Date.now();
      renderState(JSON.parse(message.data));
      stopPolling();
      updateConnection("online", "Live stream");
      showError("");
    } catch (error) {
      showError(`Bad event payload: ${error.message}`);
    }
  };
  events.onerror = () => {
    updateConnection("offline", "Stream retrying");
    startPolling();
  };
  window.setInterval(() => {
    if (state.lastEventAt && Date.now() - state.lastEventAt > 3500) {
      updateConnection("offline", "Stream stale");
      startPolling();
    }
  }, 1500);
}

async function syncTime() {
  const now = new Date();
  const timezoneOffsetMinutes = -now.getTimezoneOffset();
  const response = await api("/time", {
    method: "POST",
    body: JSON.stringify({ epochMs: now.getTime(), timezoneOffsetMinutes }),
  });
  el.tzOffset.value = response.timezoneOffsetMinutes;
  await refreshAll();
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
  renderConfig(await api("/config", { method: "PUT", body: JSON.stringify(payload) }));
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
  el.logSummary.textContent = `${samples.length} samples loaded. Range ${signed(low)} to ${signed(peak)} PSI${payload.sessionStartedAt ? `. Session ${payload.sessionStartedAt}.` : "."}`;
}

async function clearLogs() {
  await api("/logs", { method: "DELETE" });
  el.logSummary.textContent = "History cleared.";
}

function renderMediaStatus(media) {
  if (!media || !media.present) {
    el.mediaStatus.textContent = "No active GIF";
    return;
  }
  const kb = Math.round((media.sizeBytes ?? media.size ?? 0) / 1024);
  const playbackEnabled = media.playbackEnabled ?? media.playbackSupported ?? false;
  const playback = playbackEnabled ? "playback enabled" : "playback deferred";
  el.mediaStatus.textContent = `${media.name || "active.gif"} · ${kb} KB · ${playback}`;
}

function uploadWithProgress(path, file, progressEl, statusEl, contentType) {
  return new Promise((resolve, reject) => {
    const xhr = new XMLHttpRequest();
    xhr.open("POST", `${API}${path}`);
    xhr.setRequestHeader("Content-Type", contentType || file.type || "application/octet-stream");
    xhr.setRequestHeader("X-Filename", encodeURIComponent(file.name || "upload.bin"));
    xhr.upload.onprogress = (event) => {
      if (event.lengthComputable) progressEl.value = Math.round((event.loaded / event.total) * 100);
    };
    xhr.onload = () => {
      if (xhr.status >= 200 && xhr.status < 300) {
        progressEl.value = 100;
        let payload = {};
        try {
          payload = xhr.responseText ? JSON.parse(xhr.responseText) : {};
        } catch (error) {
          reject(new Error(`Bad upload response: ${error.message}`));
          return;
        }
        resolve(payload);
      } else {
        reject(new Error(xhr.responseText || `Upload failed with ${xhr.status}`));
      }
    };
    xhr.onerror = () => {
      statusEl.textContent = "Upload failed";
      reject(new Error("Upload failed"));
    };
    statusEl.textContent = "Uploading...";
    progressEl.value = 0;
    xhr.send(file);
  });
}

async function uploadMedia() {
  const file = el.mediaFile.files[0];
  if (!file) throw new Error("Choose a GIF first.");
  const payload = await uploadWithProgress("/media", file, el.mediaProgress, el.mediaStatus, "image/gif");
  renderMediaStatus(payload);
}

async function deleteMedia() {
  await api("/media", { method: "DELETE" });
  el.mediaProgress.value = 0;
  renderMediaStatus({ present: false });
}

async function uploadOta() {
  const file = el.otaFile.files[0];
  if (!file) throw new Error("Choose an ESP-IDF app binary first.");
  const payload = await uploadWithProgress("/ota", file, el.otaProgress, el.otaStatus, "application/octet-stream");
  el.otaStatus.textContent = payload.status || (payload.pendingReboot ? "OTA accepted, reboot pending" : "OTA accepted");
}

function wireControls() {
  el.refreshBtn.addEventListener("click", refreshAll);
  el.syncTimeBtn.addEventListener("click", () => syncTime().catch((error) => showError(error.message)));
  el.saveConfigBtn.addEventListener("click", () => saveConfig().catch((error) => showError(error.message)));
  el.loadLogsBtn.addEventListener("click", () => loadLogs().catch((error) => showError(error.message)));
  el.clearLogsBtn.addEventListener("click", () => clearLogs().catch((error) => showError(error.message)));
  el.uploadMediaBtn.addEventListener("click", () => uploadMedia().catch((error) => showError(error.message)));
  el.deleteMediaBtn.addEventListener("click", () => deleteMedia().catch((error) => showError(error.message)));
  el.uploadOtaBtn.addEventListener("click", () => uploadOta().catch((error) => showError(error.message)));
  el.brightnessHigh.addEventListener("input", () => { el.brightnessHighOut.textContent = `${el.brightnessHigh.value}%`; });
  el.brightnessLow.addEventListener("input", () => { el.brightnessLowOut.textContent = `${el.brightnessLow.value}%`; });
  el.mediaFile.addEventListener("change", () => {
    el.mediaStatus.textContent = el.mediaFile.files[0]?.name || "No active GIF";
  });
  el.otaFile.addEventListener("change", () => {
    el.otaStatus.textContent = el.otaFile.files[0]?.name || "Idle";
  });
  window.addEventListener("resize", () => {
    placeTicks();
    drawGauge(sampleHistory.at(-1) || { psi: 0, peakPsi: 0, zone: "ATMO", demo: true });
    drawSparkline();
  });
}

wireControls();
placeTicks();
drawGauge({ psi: 0, peakPsi: 0, zone: "ATMO", demo: true });
drawSparkline();
refreshAll().then(connectEvents);
