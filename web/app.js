"use strict";

const API = "/api/v1";
const DEFAULT_PSI_MIN = -15;
const DEFAULT_PSI_MAX = 10;
const DEFAULT_PSI_OVERBOOST = 8;
const ARC_START = 135;
const ARC_RANGE = 270;
const DEFAULT_ZERO_ANGLE = 236.25;
const ZERO_GAP_VAC = 3.6;
const ZERO_GAP_BOOST = 4.0;
const sampleHistory = [];
const HISTORY_WINDOW_MS = 60_000;
const GAUGE_GAP_RESET_MS = 1000;
const GAUGE_FRAME_MS = 1000 / 60;
/* Needle smoothing time constants, in ms. The EMA exists only to hide the step
 * between telemetry packets, and it adds group delay roughly equal to tau - so
 * tau must be sized to the packet interval of the active transport and never
 * larger. Rule of thumb: tau ~= 0.7x the packet interval.
 *
 * WS is COUPLED to STATE_WS_PUSH_DECIMATION in main/boost_web.c. The firmware
 * pushes on every sample (62.5 Hz, ~16 ms packets) rather than on a
 * free-running 50 ms timer, so the WebSocket tau is 12 ms instead of the old
 * flat 35 ms. Lowering the firmware push rate without raising GAUGE_EMA_TAU_MS.ws
 * here will make the needle visibly step.
 *
 * The HTTP fallback still polls at POLL_FRAME_MS (250 ms) and keeps the old
 * 35 ms tau - the firmware change does not affect that path. */
const GAUGE_EMA_TAU_MS = { ws: 12, http: 35 };

function gaugeEmaTauMs() {
  const socket = state.liveSocket;
  const onWebSocket = Boolean(socket) && socket.readyState === WebSocket.OPEN;
  return onWebSocket ? GAUGE_EMA_TAU_MS.ws : GAUGE_EMA_TAU_MS.http;
}
const SPARKLINE_FRAME_MS = 250;
const POLL_FRAME_MS = 250;
/* Sensor-diagnostics cadences. GET /api/v1/sensors/calibration is deliberately
 * NOT on the 20 Hz WebSocket or the 4 Hz /state path - the firmware keeps it
 * separate so the state payload and the smaller WebSocket JSON buffer stay
 * small. These two timers are the only consumers.
 *
 * Settings panel: 500 ms (2 Hz). Fast enough that the live voltage/pressure
 * readouts feel live while the operator sets up a calibration, and fast enough
 * to notice a sensor going quiet inside the firmware's 2 s freshness window.
 * Two small JSON GETs per second against a panel that is only open while
 * someone is looking at it.
 *
 * Cockpit ambient: 1000 ms (1 Hz). Atmospheric pressure moves by fractions of a
 * kPa per hour, so the value itself needs nothing faster; the rate is set by
 * wanting to notice a stalled BMP280 promptly. Freshness is still evaluated
 * continuously at render time by ageing the last reported bmpAgeMs against the
 * browser clock, so a dead sensor flips the readout to "--" without waiting for
 * the next poll. */
const CAL_POLL_MS = 500;
const AMBIENT_POLL_MS = 1000;
/* Mirrors HUD_BMP_FRESH_MS in main/boost_gauge.c. Keep the two in step: the
 * browser mirror and the physical Night City face must agree on when the
 * atmospheric readout goes unavailable. */
const BMP_FRESH_MS = 2000;
/* Age fields arrive as -1 for "never read successfully" (firmware UINT32_MAX). */
const AGE_NEVER = -1;
const PAGE = document.body.dataset.page || "cockpit";
const IS_COCKPIT = PAGE === "cockpit";
const IS_SETTINGS = PAGE === "settings";
const CANVAS_DPR_MAX = 2;

const state = {
  activeThemeId: "dyno-cell",
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
  /* Mirrors the firmware defaults so the picker shows something honest in the
   * gap before the first /themes response lands. */
  pixelShift: true,
  pixelShiftSec: 90,
  demoMode: false,
  /* Whole GET /sensors/calibration body, folded back in from every response so
   * the settings render never reads a half-updated mirror. Null until the first
   * poll lands. */
  calibration: null,
  /* Cockpit-only: last measured atmosphere for the Night City corner readout.
   * kpa/ageMs come from the device, at is the browser clock when the response
   * landed, so the freshness window can advance between polls. */
  ambient: null,
};

const el = {
  shell: document.getElementById("shell"),
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
  pixelShiftMode: document.getElementById("pixelShiftMode"),
  teSync: document.getElementById("teSync"),
  demoMode: document.getElementById("demoMode"),
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
  psiMin: document.getElementById("psiMin"),
  psiMax: document.getElementById("psiMax"),
  psiOverboost: document.getElementById("psiOverboost"),
  zeroAngle: document.getElementById("zeroAngle"),
  saveRangeBtn: document.getElementById("saveRangeBtn"),
  rangeHint: document.getElementById("rangeHint"),
  supplyVolts: document.getElementById("supplyVolts"),
  calibrateBtn: document.getElementById("calibrateBtn"),
  calConfirm: document.getElementById("calConfirm"),
  calConfirmBtn: document.getElementById("calConfirmBtn"),
  calCancelBtn: document.getElementById("calCancelBtn"),
  calMapVolts: document.getElementById("calMapVolts"),
  calNominalKpa: document.getElementById("calNominalKpa"),
  calCorrectedKpa: document.getElementById("calCorrectedKpa"),
  calBmpKpa: document.getElementById("calBmpKpa"),
  calOffset: document.getElementById("calOffset"),
  calStateText: document.getElementById("calStateText"),
  calReference: document.getElementById("calReference"),
  calSensors: document.getElementById("calSensors"),
  calStatus: document.getElementById("calStatus"),
};

const ctx = el.canvas ? el.canvas.getContext("2d") : null;
const sparkCtx = el.sparkline ? el.sparkline.getContext("2d") : null;

function on(node, event, handler) {
  if (node) node.addEventListener(event, handler);
}


function finiteOr(value, fallback) {
  const n = Number(value);
  return Number.isFinite(n) ? n : fallback;
}

function psiRange() {
  const cfg = state.config || {};
  let psiMin = finiteOr(cfg.psiMin, DEFAULT_PSI_MIN);
  let psiMax = finiteOr(cfg.psiMax, DEFAULT_PSI_MAX);
  let psiOverboost = finiteOr(cfg.psiOverboost, DEFAULT_PSI_OVERBOOST);
  let zeroAngle = finiteOr(cfg.zeroAngle, DEFAULT_ZERO_ANGLE);
  if (!(psiMin < 0)) psiMin = DEFAULT_PSI_MIN;
  if (!(psiMax > 0)) psiMax = DEFAULT_PSI_MAX;
  if (!(psiMin < psiMax)) {
    psiMin = DEFAULT_PSI_MIN;
    psiMax = DEFAULT_PSI_MAX;
  }
  if (!(psiOverboost > 0 && psiOverboost < psiMax)) {
    psiOverboost = Math.min(DEFAULT_PSI_OVERBOOST, psiMax * 0.8);
    if (!(psiOverboost > 0 && psiOverboost < psiMax)) psiOverboost = psiMax * 0.8;
  }
  if (!(zeroAngle >= 180 && zeroAngle <= 315)) zeroAngle = DEFAULT_ZERO_ANGLE;
  return { psiMin, psiMax, psiOverboost, zeroAngle };
}

function formatTickLabel(value) {
  if (Math.abs(value) < 0.05) return "0";
  const rounded = Math.round(value * 10) / 10;
  return Number.isInteger(rounded) ? String(rounded) : rounded.toFixed(1);
}

function tickValues(psiMin, psiMax, psiOverboost, zeroAngle) {
  const midpoint = psiMax / 2;
  const midpointAngle = psiToAngle(midpoint, { psiMin, psiMax, zeroAngle });
  const overboostAngle = psiToAngle(psiOverboost, { psiMin, psiMax, zeroAngle });
  /* At r=140, 20 px type plus 8 px gap needs 11.5° of separation. */
  const showMidpoint = Math.abs(midpointAngle - overboostAngle) >= 12;
  return showMidpoint ? [psiMin, 0, midpoint, psiOverboost, psiMax] : [psiMin, 0, psiOverboost, psiMax];
}
/* The dashboard chrome stays on one fixed look regardless of gauge theme; this
 * only guarantees a sane gauge palette before the first theme loads. */
function updatePalette() {
  if (!state.palette) {
    state.palette = {
      face: "#090A0D", track: "#20242C", text: "#F5F7FA", muted: "#8C95A3",
      vacuum: "#4DD2FF", boost: "#B8F35A", overboost: "#FF4F6D", zero: "#FFFFFF",
    };
  }
}

/* Which layout the active theme renders. Themes carry a `style` from the API;
 * "arc" is the classic dyno-cell face and the safe default. */
function activeThemeStyle() {
  const t = state.themes.find((x) => x.id === state.activeThemeId);
  return (t && t.style) || "arc";
}

function clamp(v, lo, hi) {
  return v < lo ? lo : v > hi ? hi : v;
}

function hexToRgb(hex) {
  let h = String(hex || "").replace("#", "").trim();
  if (h.length === 3) h = h.split("").map((c) => c + c).join("");
  const n = parseInt(h || "000000", 16);
  return [(n >> 16) & 255, (n >> 8) & 255, n & 255];
}

/* Smoothly blend two hex colors; used by the big-digit background sweep. */
function lerpColor(a, b, t) {
  const A = hexToRgb(a);
  const B = hexToRgb(b);
  const k = clamp(t, 0, 1);
  return `rgb(${Math.round(A[0] + (B[0] - A[0]) * k)}, ${Math.round(
    A[1] + (B[1] - A[1]) * k
  )}, ${Math.round(A[2] + (B[2] - A[2]) * k)})`;
}

/* Point on a circle, angle in degrees measured clockwise from 12 o'clock,
 * in the centered 466-space the themed renderers draw in. */
const DEG = Math.PI / 180;
function polar(r, aDeg) {
  const t = aDeg * DEG;
  return [r * Math.sin(t), -r * Math.cos(t)];
}
function canvasAngle(aDeg) {
  return (aDeg - 90) * DEG;
}

/* Apply a theme to the GAUGE only. The web dashboard chrome keeps its own fixed
 * identity (the :root CSS vars) and does not re-skin with the gauge. */
function setTheme(theme) {
  if (!theme || !theme.colors) return;
  state.palette = {
    face: theme.colors.face || "#000",
    track: theme.colors.track,
    text: theme.colors.text,
    muted: theme.colors.muted,
    vacuum: theme.colors.vacuum,
    boost: theme.colors.boost,
    overboost: theme.colors.overboost,
    zero: theme.colors.zero,
  };
  state.activeThemeId = theme.id;
  if (IS_COCKPIT) {
    scheduleGaugeRender();
    drawSparkline();
  }
}

function zoneFor(psi) {
  const { psiOverboost } = psiRange();
  if (psi >= psiOverboost) return "OVER";
  if (psi >= 0.35) return "BOOST";
  if (psi > -0.35) return "ATMO";
  return "VAC";
}

function colorFor(psi) {
  const palette = state.palette;
  const { psiOverboost } = psiRange();
  if (psi >= psiOverboost) return palette.overboost;
  if (psi >= 0.35) return palette.boost;
  if (psi > -0.35) return palette.text;
  return palette.vacuum;
}

function psiToAngle(psi, range = psiRange()) {
  const { psiMin, psiMax, zeroAngle } = range;
  const clamped = Math.max(psiMin, Math.min(psiMax, psi));
  if (clamped < 0) {
    const span = -psiMin;
    const t = span > 0 ? (clamped - psiMin) / span : 1;
    return ARC_START + t * (zeroAngle - ARC_START);
  }
  const t = psiMax > 0 ? clamped / psiMax : 0;
  return zeroAngle + t * (ARC_START + ARC_RANGE - zeroAngle);
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

/* Shared canvas setup: size for DPR, work in CSS px, return the 466-space
 * geometry every renderer uses. */
function gaugeGeom() {
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
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  const size = Math.min(cssW, cssH);
  return { cssW, cssH, size, scale: size / 466, cx: cssW / 2, cy: cssH / 2 };
}

/* Dispatch the live face by the active theme's style. Each style is a distinct
 * layout, not a recolor of one gauge. */
function drawGauge(sample) {
  if (!ctx || !el.canvas) return;
  const g = gaugeGeom();
  const psi = Number(sample.psi ?? 0);
  ctx.clearRect(0, 0, g.cssW, g.cssH);
  switch (activeThemeStyle()) {
    case "vault":
      return drawVaultGauge(sample, psi, g);
    case "hud":
      return drawHudGauge(sample, psi, g);
    case "bigdigit":
      return drawBigDigitGauge(sample, psi, g);
    default:
      return drawArcGauge(sample, psi, g);
  }
}

/* ── Style: arc — the classic Dyno Cell dual-climate face ────────────────── */
function drawArcGauge(sample, psi, g) {
  const { cx, cy, scale, size } = g;
  const outerR = (410 / 2) * scale;
  const stroke = 45 * scale;
  const radius = outerR - stroke / 2;
  const { start, end } = valueArcAngles(psi);

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

  /* Tick labels follow live config: min / 0 / mid / overboost / max */
  const range = psiRange();
  ctx.textAlign = "center";
  ctx.textBaseline = "middle";
  ctx.fillStyle = state.palette.muted;
  ctx.font = `700 ${Math.max(12, 16 * scale)}px system-ui, -apple-system, "Segoe UI", sans-serif`;
  for (const value of tickValues(range.psiMin, range.psiMax, range.psiOverboost, range.zeroAngle)) {
    let r = 140 * scale;
    if (Math.abs(value) < 0.01) r = 122 * scale;
    const a = degToRad(psiToAngle(value));
    ctx.fillText(formatTickLabel(value), cx + r * Math.cos(a), cy + r * Math.sin(a));
  }

  /* Center stack — zone / PSI / unit / peak / mode (matches physical UI) */
  const zone = sample.zone || zoneFor(psi);
  const peak = Math.max(0, Number(sample.peakPsi || 0));
  ctx.fillStyle = colorFor(psi);
  ctx.font = `700 ${Math.max(12, 15 * scale)}px system-ui, -apple-system, "Segoe UI", sans-serif`;
  ctx.fillText(zone, cx, cy - 88 * scale);

  ctx.fillStyle = psi >= range.psiOverboost ? state.palette.overboost : state.palette.text;
  ctx.font = `700 ${Math.max(40, 58 * scale)}px system-ui, -apple-system, "Segoe UI", sans-serif`;
  drawFixedPsi(psi, cx, cy - 16 * scale, scale);

  ctx.fillStyle = state.palette.muted;
  ctx.font = `700 ${Math.max(12, 15 * scale)}px system-ui, -apple-system, "Segoe UI", sans-serif`;
  ctx.fillText("PSI", cx, cy + 28 * scale);

  ctx.fillStyle = peak >= range.psiOverboost ? state.palette.overboost : state.palette.boost;
  ctx.font = `700 ${Math.max(12, 14 * scale)}px system-ui, -apple-system, "Segoe UI", sans-serif`;
  ctx.fillText(`PEAK  ${peak.toFixed(1)}`, cx, cy + 56 * scale);

  ctx.fillStyle = state.palette.muted;
  ctx.font = `700 ${Math.max(11, 12 * scale)}px system-ui, -apple-system, "Segoe UI", sans-serif`;
  ctx.fillText(sample.demo ? "DEMO" : "LIVE", cx, cy + 84 * scale);
}

/* Rounded-rect path helper for the themed faces (466-space). */
function roundRectPath(x, y, w, h, r) {
  ctx.beginPath();
  ctx.moveTo(x + r, y);
  ctx.arcTo(x + w, y, x + w, y + h, r);
  ctx.arcTo(x + w, y + h, x, y + h, r);
  ctx.arcTo(x, y + h, x, y, r);
  ctx.arcTo(x, y, x + w, y, r);
  ctx.closePath();
}

/* Linear psi→angle for the stylized faces (symmetric sweep, clockwise from
 * top). The precise value lives in the center readout, so a linear dial reads
 * fine even when 0 isn't centered. */
function linMap(psi, range, a0, a1) {
  const t = (clamp(psi, range.psiMin, range.psiMax) - range.psiMin) / (range.psiMax - range.psiMin);
  return a0 + t * (a1 - a0);
}

/* Split a psi value into sign / integer / fraction parts for fixed-decimal
 * rendering. */
function splitNum(psi, decimals) {
  const neg = psi < -0.05;
  const [intPart, fracPart = ""] = Math.abs(psi).toFixed(decimals).split(".");
  return { neg, intPart, fracPart };
}

/* Draw a number with its decimal point pinned to `decimalX` so the value never
 * shifts left/right as digit counts change. `intStr` may include a sign glyph.
 * Uses the current ctx font/fillStyle/baseline. */
function drawFixedDecimal(intStr, fracStr, decimalX, y) {
  const half = ctx.measureText(".").width * 0.5;
  const prevAlign = ctx.textAlign;
  ctx.textAlign = "right";
  ctx.fillText(intStr, decimalX - half, y);
  ctx.textAlign = "center";
  ctx.fillText(".", decimalX, y);
  ctx.textAlign = "left";
  ctx.fillText(fracStr, decimalX + half, y);
  ctx.textAlign = prevAlign;
}

/* ── Style: vault — Vault-Tec phosphor dial + needle + CRT scanlines ─────── */
function drawVaultGauge(sample, psi, g) {
  const range = psiRange();
  const p = state.palette;
  /* Dial glow colour + vignette depth mirror the device settings. */
  const face = state.vaultFace || p.face;
  const vignAlpha = (state.vaultVignette ?? 60) / 100;
  const green = p.text;
  const warn = p.overboost;
  const dim = p.muted;
  /* Same 270° face + zero-angle mapping as Dyno Cell, so the settings-page
   * zero position and vacuum/boost scaling are honored here too. */
  const AP = (r, a) => [r * Math.cos(a * DEG), r * Math.sin(a * DEG)];
  ctx.save();
  ctx.translate(g.cx, g.cy);
  ctx.scale(g.scale, g.scale);

  /* face + bezel */
  ctx.beginPath();
  ctx.arc(0, 0, 233, 0, Math.PI * 2);
  ctx.fillStyle = face;
  ctx.fill();
  ctx.beginPath();
  ctx.arc(0, 0, 221, 0, Math.PI * 2);
  ctx.lineWidth = 2;
  ctx.globalAlpha = 0.5;
  ctx.strokeStyle = dim;
  ctx.stroke();
  ctx.globalAlpha = 1;

  /* ticks around the 270° sweep */
  for (let i = 0; i <= 40; i++) {
    const v = range.psiMin + ((range.psiMax - range.psiMin) * i) / 40;
    const major = i % 5 === 0;
    const a = psiToAngle(v, range);
    const [x0, y0] = AP(major ? 176 : 188, a);
    const [x1, y1] = AP(206, a);
    ctx.beginPath();
    ctx.moveTo(x0, y0);
    ctx.lineTo(x1, y1);
    ctx.strokeStyle = v >= range.psiOverboost ? warn : green;
    ctx.globalAlpha = v >= range.psiOverboost ? 1 : 0.82;
    ctx.lineWidth = major ? 4 : 2;
    ctx.stroke();
  }
  ctx.globalAlpha = 1;

  /* numerals */
  ctx.textAlign = "center";
  ctx.textBaseline = "middle";
  for (const v of tickValues(range.psiMin, range.psiMax, range.psiOverboost, range.zeroAngle)) {
    const [x, y] = AP(152, psiToAngle(v, range));
    ctx.fillStyle = v >= range.psiOverboost ? warn : green;
    ctx.font = `700 24px Consolas, "SF Mono", monospace`;
    ctx.fillText(formatTickLabel(v), x, y);
  }

  /* zero notch — thick radial phosphor tick at the configured zero angle */
  const za = psiToAngle(0, range);
  const [zx0, zy0] = AP(178, za);
  const [zx1, zy1] = AP(206, za);
  ctx.strokeStyle = green;
  ctx.lineWidth = 9;
  ctx.lineCap = "round";
  ctx.beginPath();
  ctx.moveTo(zx0, zy0);
  ctx.lineTo(zx1, zy1);
  ctx.stroke();

  /* peak tell-tale — a marker that holds at the session's max boost */
  const peak = Math.max(0, Number(sample.peakPsi || 0));
  if (peak >= 0.35) {
    const pa = psiToAngle(peak, range);
    const [mx, my] = AP(207, pa);
    ctx.save();
    ctx.translate(mx, my);
    ctx.rotate((pa + 90) * DEG);
    ctx.fillStyle = warn;
    /* Knocked back so the tell-tale does not compete with the overboost ticks;
     * matches the panel's LV_OPA_60. */
    ctx.globalAlpha = 0.6;
    ctx.beginPath();
    ctx.moveTo(0, 0);
    ctx.lineTo(-6, -13);
    ctx.lineTo(6, -13);
    ctx.closePath();
    ctx.fill();
    ctx.globalAlpha = 1;
    ctx.restore();
  }

  /* title (moved down, clear of the zero marker) */
  ctx.fillStyle = green;
  ctx.globalAlpha = 0.92;
  ctx.font = `700 16px "Bahnschrift", system-ui, sans-serif`;
  ctx.fillText("V A U L T - T E C", 0, -98);
  ctx.globalAlpha = 0.68;
  ctx.font = `600 12px "Bahnschrift", system-ui, sans-serif`;
  ctx.fillText("BOOST-O-METER", 0, -78);
  ctx.globalAlpha = 1;

  /* overboost alert (replaces the mascot): blinking warning + warm readout */
  const over = psi >= range.psiOverboost;
  if (over && Math.floor(Date.now() / 320) % 2 === 0) {
    ctx.fillStyle = warn;
    ctx.font = `700 15px "Bahnschrift", system-ui, sans-serif`;
    ctx.fillText("▲ OVER-PRESSURE ▲", 0, 72);
  }

  /* numeral window — decimal pinned to center so the value never shifts */
  ctx.strokeStyle = over ? warn : dim;
  ctx.lineWidth = 1.5;
  roundRectPath(-74, 96, 148, 46, 4);
  ctx.stroke();
  const { neg, intPart, fracPart } = splitNum(psi, 2);
  ctx.fillStyle = over ? warn : green;
  ctx.font = `700 32px Consolas, "SF Mono", monospace`;
  drawFixedDecimal(`${neg ? "−" : "+"}${intPart}`, fracPart, 0, 120);
  ctx.fillStyle = dim;
  ctx.font = `600 11px "Bahnschrift", system-ui, sans-serif`;
  ctx.fillText("MANIFOLD  PSI", 0, 158);
  ctx.fillStyle = warn;
  ctx.globalAlpha = 0.85;
  ctx.font = `700 13px Consolas, monospace`;
  ctx.fillText(`PEAK  ${peak.toFixed(1)}`, 0, 178);
  ctx.globalAlpha = 1;

  /* needle at the mapped angle (0° = east; up-pointing art rotated by a+90) */
  ctx.save();
  ctx.rotate((psiToAngle(psi, range) + 90) * DEG);
  ctx.beginPath();
  ctx.moveTo(-6, 26);
  ctx.lineTo(6, 26);
  ctx.lineTo(2, -150);
  ctx.lineTo(-2, -150);
  ctx.closePath();
  ctx.fillStyle = psi >= range.psiOverboost ? warn : green;
  ctx.fill();
  ctx.restore();
  ctx.beginPath();
  ctx.arc(0, 0, 14, 0, Math.PI * 2);
  ctx.fillStyle = face;
  ctx.fill();
  ctx.lineWidth = 2;
  ctx.strokeStyle = green;
  ctx.stroke();

  /* CRT scanlines + vignette, clipped to the face */
  ctx.beginPath();
  ctx.arc(0, 0, 221, 0, Math.PI * 2);
  ctx.clip();
  ctx.globalAlpha = 0.16;
  ctx.strokeStyle = "#000";
  ctx.lineWidth = 1.4;
  for (let y = -220; y < 220; y += 4) {
    ctx.beginPath();
    ctx.moveTo(-221, y);
    ctx.lineTo(221, y);
    ctx.stroke();
  }
  ctx.globalAlpha = 1;
  const vg = ctx.createRadialGradient(0, 0, 50, 0, 0, 233);
  vg.addColorStop(0, "rgba(0,0,0,0)");
  vg.addColorStop(1, `rgba(0,0,0,${vignAlpha})`);
  ctx.fillStyle = vg;
  ctx.beginPath();
  ctx.arc(0, 0, 233, 0, Math.PI * 2);
  ctx.fill();
  ctx.restore();
}

/* ── Style: hud — Night City cyberpunk targeting HUD ─────────────────────── */
let hudPrevPsi = 0;
function drawHudGauge(sample, psi, g) {
  const range = psiRange();
  const p = state.palette;
  const Y = p.boost;
  const C = p.vacuum;
  const R = p.overboost;
  const over = psi >= range.psiOverboost;
  const A0 = -116;
  const A1 = 116;
  ctx.save();
  ctx.translate(g.cx, g.cy);
  ctx.scale(g.scale, g.scale);

  ctx.beginPath();
  ctx.arc(0, 0, 233, 0, Math.PI * 2);
  ctx.fillStyle = p.face;
  ctx.fill();

  /* hazard chevrons across the top */
  ctx.globalAlpha = 0.8;
  for (let i = -3; i <= 3; i++) {
    const x = i * 30;
    ctx.beginPath();
    ctx.moveTo(x - 10, -142);
    ctx.lineTo(x, -130);
    ctx.lineTo(x + 10, -142);
    ctx.lineTo(x + 6, -142);
    ctx.lineTo(x, -135);
    ctx.lineTo(x - 6, -142);
    ctx.closePath();
    ctx.fillStyle = i % 2 ? Y : "#3a3600";
    ctx.fill();
  }
  ctx.globalAlpha = 1;

  /* Zero-referenced mapping into the HUD's top sweep: 0 psi sits at the
   * configured zero angle's proportion, and vacuum/boost scale with the range,
   * so the fill grows from zero just like Dyno Cell. */
  const S = A1 - A0;
  const zeroHud = A0 + ((range.zeroAngle - ARC_START) / ARC_RANGE) * S;
  const hudAngle = (v) => {
    const cl = clamp(v, range.psiMin, range.psiMax);
    if (cl < 0) {
      const t = range.psiMin < 0 ? (cl - range.psiMin) / (0 - range.psiMin) : 1;
      return A0 + t * (zeroHud - A0);
    }
    const t = range.psiMax > 0 ? cl / range.psiMax : 0;
    return zeroHud + t * (A1 - zeroHud);
  };

  /* angular tick frame */
  for (let i = 0; i <= 20; i++) {
    const v = range.psiMin + ((range.psiMax - range.psiMin) * i) / 20;
    const a = hudAngle(v);
    const [x0, y0] = polar(198, a);
    const [x1, y1] = polar(214, a);
    ctx.beginPath();
    ctx.moveTo(x0, y0);
    ctx.lineTo(x1, y1);
    ctx.strokeStyle = v >= range.psiOverboost ? R : C;
    ctx.lineWidth = i % 2 === 0 ? 4 : 2;
    ctx.stroke();
  }

  /* track + fill (grows from the zero notch) */
  ctx.beginPath();
  ctx.arc(0, 0, 206, canvasAngle(A0), canvasAngle(A1));
  ctx.strokeStyle = "#1a1c0a";
  ctx.lineWidth = 10;
  ctx.stroke();
  const va = hudAngle(psi);
  const loA = Math.min(zeroHud, va);
  const hiA = Math.max(zeroHud, va);
  if (hiA - loA > 0.5) {
    ctx.beginPath();
    ctx.arc(0, 0, 206, canvasAngle(loA), canvasAngle(hiA));
    ctx.strokeStyle = over ? R : psi < 0 ? C : Y;
    ctx.lineWidth = 10;
    ctx.stroke();
  }

  /* zero notch */
  const [zx0, zy0] = polar(196, zeroHud);
  const [zx1, zy1] = polar(217, zeroHud);
  ctx.strokeStyle = C;
  ctx.lineWidth = 5;
  ctx.lineCap = "round";
  ctx.beginPath();
  ctx.moveTo(zx0, zy0);
  ctx.lineTo(zx1, zy1);
  ctx.stroke();

  /* Readout geometry: one decimal, decimal pinned (offset from center so
   * left-heavy negatives stay balanced) and sized once from the range so the
   * value never shifts. */
  ctx.font = `700 italic 88px "Bahnschrift", "DIN Alternate", system-ui, sans-serif`;
  const nHalf = ctx.measureText(".").width * 0.5;
  const worstNegInt = range.psiMin < 0 ? "−" + String(Math.floor(Math.abs(range.psiMin))) : "";
  const worstPosInt = String(Math.floor(Math.abs(range.psiMax)));
  const Lmax = nHalf + Math.max(ctx.measureText(worstNegInt).width, ctx.measureText(worstPosInt).width);
  const Rmax = nHalf + ctx.measureText("0").width;
  const numDecimalX = (Lmax - Rmax) / 2;
  const bx = Math.min(152, (Lmax + Rmax) / 2 + 18);

  /* Kiroshi reticle brackets sized to enclose the readout */
  ctx.strokeStyle = C;
  ctx.lineWidth = 2.5;
  const arm = 24;
  for (const [xC, yC, sx, sy] of [[-bx, -50, 1, 1], [bx, -50, -1, 1], [-bx, 48, 1, -1], [bx, 48, -1, -1]]) {
    ctx.beginPath();
    ctx.moveTo(xC + sx * arm, yC);
    ctx.lineTo(xC, yC);
    ctx.lineTo(xC, yC + sy * 24);
    ctx.stroke();
  }

  /* header */
  ctx.fillStyle = C;
  ctx.textAlign = "center";
  ctx.textBaseline = "middle";
  ctx.font = `600 13px "Bahnschrift", system-ui, sans-serif`;
  ctx.fillText("◄  MANIFOLD PRESSURE  ►", 0, -108);

  /* big italic value with a glitch shear on fast spikes */
  const { neg, intPart, fracPart } = splitNum(psi, 1);
  const intStr = `${neg ? "−" : ""}${intPart}`;
  hudPrevPsi = psi;
  ctx.font = `700 italic 88px "Bahnschrift", "DIN Alternate", system-ui, sans-serif`;
  /* Permanent chromatic split rather than a spike-triggered flash: the
   * conditional version left stranded ghosts on the physical panel. */
  ctx.globalAlpha = 0.4;
  ctx.fillStyle = R;
  drawFixedDecimal(intStr, fracPart, numDecimalX - 3, 2);
  ctx.fillStyle = C;
  drawFixedDecimal(intStr, fracPart, numDecimalX + 3, 2);
  ctx.globalAlpha = 1;
  ctx.fillStyle = over ? R : Y;
  drawFixedDecimal(intStr, fracPart, numDecimalX, 2);
  ctx.fillStyle = p.muted;
  ctx.font = `600 13px Consolas, monospace`;
  ctx.fillText("PSI // FORCED INDUCTION", 0, 74);

  /* corner telemetry — measured atmosphere and real peak hold.
   * This used to render `MAP 101 + psi * 6.895` kPa, which was arithmetic on the
   * gauge reading dressed up as a sensor value. It is now the BMP280's own
   * measurement, or "--" when there is no fresh one; see ambientAtmText(). */
  const peak = Math.max(0, Number(sample.peakPsi || 0));
  ctx.textAlign = "left";
  ctx.fillStyle = C;
  ctx.font = `600 15px Consolas, monospace`;
  ctx.fillText(ambientAtmText(), -138, 128);
  ctx.textAlign = "right";
  ctx.fillText(`PK ${peak.toFixed(1)}`, 138, 128);
  ctx.fillStyle = p.muted;
  ctx.textAlign = "left";
  ctx.font = `600 12px Consolas, monospace`;
  ctx.fillText(sample.demo ? "SYS DEMO" : "SYS ONLINE", -138, 150);
  ctx.textAlign = "right";
  ctx.fillText("NC-2077", 138, 150);
  ctx.restore();
}

/* ── Style: bigdigit — huge Alvida number on a color-sweeping ground ──────── */
function bigDigitBackground(psi, range) {
  const p = state.palette;
  if (psi <= 0) return p.vacuum;
  /* Start the red ramp before the overboost threshold: squeezing it into
   * overboost..max made the transition snap rather than sweep. */
  const redStart = range.psiOverboost * 0.55;
  if (psi <= redStart) return lerpColor(p.vacuum, p.boost, psi / Math.max(0.001, redStart));
  const span = Math.max(0.001, range.psiMax - redStart);
  return lerpColor(p.boost, p.overboost, (psi - redStart) / span);
}

function drawBigDigitGauge(sample, psi, g) {
  const range = psiRange();
  const { cx, cy, size } = g;

  /* Solid round ground that sweeps cyan → lime → red with the reading. */
  ctx.fillStyle = bigDigitBackground(psi, range);
  ctx.beginPath();
  ctx.arc(cx, cy, size / 2, 0, Math.PI * 2);
  ctx.fill();

  ctx.save();
  ctx.translate(cx, cy);
  ctx.scale(g.scale, g.scale);
  ctx.textAlign = "center";
  ctx.textBaseline = "middle";

  const zone = sample.zone || zoneFor(psi);
  const peak = Math.max(0, Number(sample.peakPsi || 0));

  /* top zone chip */
  ctx.globalAlpha = 0.82;
  ctx.fillStyle = "#ffffff";
  ctx.font = `700 20px "Bahnschrift", system-ui, sans-serif`;
  ctx.fillText(zone, 0, -150);
  ctx.globalAlpha = 1;

  /* The number — TABULAR fixed slots. The face center (x = 0) sits halfway
   * between the ones digit and the decimal point; the decimal and tenths sit in
   * constant-width cells to the right, higher integer digits and the sign grow
   * leftward like an odometer. Each digit is centered in its cell, so varying
   * glyph widths never move anything already on screen. */
  const refFs = 100;
  ctx.font = `400 ${refFs}px "Alvida Fatface", Georgia, "Times New Roman", serif`;
  let slot100 = 0;
  for (let d = 0; d < 10; d++) slot100 = Math.max(slot100, ctx.measureText(String(d)).width);
  const dot100 = ctx.measureText(".").width;
  const barW100 = refFs * 0.32;
  const gap100 = refFs * 0.08;
  const maxInt = String(Math.floor(Math.max(Math.abs(range.psiMin), Math.abs(range.psiMax), 1))).length;
  /* Extents from the centered ones digit: integers + sign grow left, the
   * decimal + tenths sit right. Size so the widest reading stays in the face. */
  const leftExtent100 = (slot100 + dot100) / 4 + (maxInt - 0.5) * slot100 + (range.psiMin < 0 ? gap100 + barW100 : 0);
  const rightExtent100 = (3 * (slot100 + dot100)) / 4 + slot100 / 2;
  const fs = clamp((204 / Math.max(leftExtent100, rightExtent100)) * refFs, 96, 172);

  ctx.font = `400 ${fs}px "Alvida Fatface", Georgia, "Times New Roman", serif`;
  let slotW = 0;
  for (let d = 0; d < 10; d++) slotW = Math.max(slotW, ctx.measureText(String(d)).width);
  const dotW = ctx.measureText(".").width;
  const numY = -8;
  const onesCenter = -(slotW + dotW) / 4;
  const decimalX = (slotW + dotW) / 4;
  const tenthsCenter = (3 * (slotW + dotW)) / 4;

  const absTenths = Math.round(Math.abs(psi) * 10);
  const whole = Math.floor(absTenths / 10);
  const tenth = absTenths % 10;
  const isNeg = psi < -0.05;
  const intStr = String(whole);

  ctx.shadowColor = "rgba(0,0,0,0.30)";
  ctx.shadowBlur = 22;
  ctx.shadowOffsetY = 6;
  ctx.fillStyle = "#ffffff";
  ctx.textAlign = "center";
  ctx.fillText(".", decimalX, numY);
  ctx.fillText(String(tenth), tenthsCenter, numY);
  for (let i = 0; i < intStr.length; i++) {
    ctx.fillText(intStr[intStr.length - 1 - i], onesCenter - i * slotW, numY);
  }
  if (isNeg) {
    /* Fat, gently convex dash left of the leftmost integer cell. */
    const leftmostCenter = onesCenter - (intStr.length - 1) * slotW;
    const barW = fs * 0.46;
    const barH = fs * 0.135;
    const bcx = leftmostCenter - slotW / 2 - fs * 0.08 - barW / 2;
    const bcy = numY - fs * 0.15;
    const x0 = bcx - barW / 2;
    const x1 = bcx + barW / 2;
    const y0 = bcy - barH / 2;
    const y1 = bcy + barH / 2;
    /* Slanted rectangle: both edges lean the same way. */
    const slant = barH * 0.62;
    ctx.beginPath();
    ctx.moveTo(x0 + slant, y0);
    ctx.lineTo(x1, y0);
    ctx.lineTo(x1 - slant, y1);
    ctx.lineTo(x0, y1);
    ctx.closePath();
    ctx.fill();
  }
  ctx.shadowColor = "transparent";
  ctx.shadowBlur = 0;
  ctx.shadowOffsetY = 0;

  /* unit + peak */
  ctx.globalAlpha = 0.9;
  ctx.fillStyle = "#ffffff";
  ctx.font = `700 30px "Bahnschrift", system-ui, sans-serif`;
  ctx.fillText("PSI", 0, 118);
  ctx.globalAlpha = 0.72;
  ctx.font = `600 18px Consolas, monospace`;
  ctx.fillText(`PEAK ${peak.toFixed(1)}   ${sample.demo ? "DEMO" : "LIVE"}`, 0, 168);
  ctx.globalAlpha = 1;
  ctx.restore();
}

function scheduleGaugeRender() {
  if (!IS_COCKPIT || !el.canvas || state.gaugeRaf !== null) return;
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
  const alpha = 1 - Math.exp(-elapsedMs / gaugeEmaTauMs());
  state.gaugePsi += (Number(target.psi ?? 0) - state.gaugePsi) * alpha;

  drawGauge({ ...target, psi: state.gaugePsi });

  if (Math.abs(Number(target.psi ?? 0) - state.gaugePsi) > 0.01) {
    scheduleGaugeRender();
  }
}

function drawSparkline() {
  if (!sparkCtx || !el.sparkline) return;
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

  const { psiMin, psiMax } = psiRange();
  const span = Math.max(0.001, psiMax - psiMin);
  const zeroY = h - ((0 - psiMin) / span) * h;
  sparkCtx.strokeStyle = "#8c95a3";
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
  sparkCtx.strokeStyle = "#4dd2ff";
  sparkCtx.beginPath();
  sampleHistory.forEach((sample, index) => {
    const x = Math.max(0, Math.min(w, ((sample.historyTimeMs - startMs) / HISTORY_WINDOW_MS) * w));
    const y = h - ((sample.psi - psiMin) / span) * h;
    if (index === 0) sparkCtx.moveTo(x, y);
    else sparkCtx.lineTo(x, y);
  });
  sparkCtx.stroke();

  const last = sampleHistory.at(-1);
  sparkCtx.fillStyle = colorFor(last.psi);
  const lx = Math.max(0, Math.min(w, ((last.historyTimeMs - startMs) / HISTORY_WINDOW_MS) * w));
  const ly = h - ((last.psi - psiMin) / span) * h;
  sparkCtx.beginPath();
  sparkCtx.arc(lx, ly, 4 * dpr, 0, Math.PI * 2);
  sparkCtx.fill();
}

function pushSample(sample) {
  if (!IS_COCKPIT) return false;
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
  if (el.sampleCount) el.sampleCount.textContent = "Last 60 seconds";
  if (el.emptyState) el.emptyState.hidden = sampleHistory.length > 0;
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

function updateConnection(mode, transport = null) {
  if (!el.connection || !el.connectionText) return;
  const online = mode === "online";
  const text = online
    ? (transport === "http" || (!transport && state.fallbackActive) ? "Live · HTTP 4 Hz" : "Live · WebSocket 60 Hz")
    : "Disconnected";
  if (state.connected === online && el.connectionText.textContent === text) return;
  state.connected = online;
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
  if (IS_COCKPIT) {
    if (sample.firmwareVersion && el.firmwareVersion) el.firmwareVersion.textContent = sample.firmwareVersion;
    if (el.uptime) el.uptime.textContent = formatDuration(sample.uptimeMs || 0);
    if (el.deviceClock) el.deviceClock.textContent = formatClock(sample.epochMs, sample.timezoneOffsetMinutes || 0);
    if (sample.brightness != null && el.brightnessNow) el.brightnessNow.textContent = `${sample.brightness}%`;
  }
  if (sample.activeThemeId && sample.activeThemeId !== state.activeThemeId) {
    state.activeThemeId = sample.activeThemeId;
    const activeTheme = state.themes.find((theme) => theme.id === state.activeThemeId);
    if (activeTheme) {
      setTheme(activeTheme);
      if (IS_COCKPIT) renderThemes();
    }
  }
  pushSample(sample);
}

function renderConfig(config) {
  state.config = config;
  if (IS_COCKPIT) {
    if (el.tzOffset) el.tzOffset.value = config.timezoneOffsetMinutes ?? 0;
    if (el.scheduleEnabled) el.scheduleEnabled.checked = Boolean(config.dimSchedule?.enabled);
    if (el.scheduleStart) el.scheduleStart.value = minutesToTime(config.dimSchedule?.startMinutes ?? 1260);
    if (el.scheduleEnd) el.scheduleEnd.value = minutesToTime(config.dimSchedule?.endMinutes ?? 420);
    if (el.brightnessHigh) el.brightnessHigh.value = config.brightnessHigh ?? 100;
    if (el.brightnessLow) el.brightnessLow.value = config.brightnessLow ?? 12;
    if (el.brightnessHighOut) el.brightnessHighOut.textContent = `${el.brightnessHigh.value}%`;
    if (el.brightnessLowOut) el.brightnessLowOut.textContent = `${el.brightnessLow.value}%`;
  }
  const range = psiRange();
  if (el.psiMin && document.activeElement !== el.psiMin) el.psiMin.value = String(range.psiMin);
  if (el.psiMax && document.activeElement !== el.psiMax) el.psiMax.value = String(range.psiMax);
  if (el.psiOverboost && document.activeElement !== el.psiOverboost) el.psiOverboost.value = String(range.psiOverboost);
  if (el.zeroAngle && document.activeElement !== el.zeroAngle) el.zeroAngle.value = String(range.zeroAngle);
  if (el.rangeHint) {
    el.rangeHint.textContent =
      `Scale ${formatTickLabel(range.psiMin)} → ${formatTickLabel(range.psiMax)} PSI · zero ${range.zeroAngle.toFixed(2)}° · midpoint shown only when clear of overboost.`;
  }
  if (IS_COCKPIT) {
    scheduleGaugeRender();
    drawSparkline();
  }
}

function renderNetwork(net) {
  state.network = net;
  if (el.netMode) el.netMode.textContent = (net.mode || "--").toUpperCase();
  if (el.netModeSelect) el.netModeSelect.value = net.mode === "ap" ? "ap" : "apsta";
  if (el.netStaState) {
    el.netStaState.textContent = net.staConnected
      ? `UP ${net.rssi || ""} dBm`.trim()
      : net.staEnabled ? "Connecting…" : "Off";
  }
  if (el.netStaIp) el.netStaIp.textContent = net.staIp || "—";
  if (el.netApSsid) el.netApSsid.textContent = net.apSsid || "—";
  if (el.netSsid && document.activeElement !== el.netSsid) el.netSsid.value = net.staSsid || "";
  if (el.netHint) {
    el.netHint.textContent = net.staConnected && net.staIp
      ? `Live on http://${net.staIp}/ · SoftAP ${net.apSsid || ""} still online`
      : "SoftAP stays up as fallback. Saving STA may change the LAN IP.";
  }
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

let openThemeEditor = null;
let colorPutTimer = null;

/* Colour inputs fire continuously while dragging. Debounce so one drag is a
 * single PUT rather than fifty, each of which rebuilds the panel scene. */
function queueThemeConfig(body, okMsg) {
  clearTimeout(colorPutTimer);
  colorPutTimer = setTimeout(async () => {
    try {
      showError("");
      const payload = await api("/themes/config", {
        method: "PUT",
        body: JSON.stringify(body),
      });
      state.themes = payload.themes || state.themes;
      state.bigDigitStaticBg = !!payload.bigDigitStaticBg;
      state.bigDigitColorText = !!payload.bigDigitColorText;
      state.bigDigitStaticColor = payload.bigDigitStaticColor || state.bigDigitStaticColor;
      state.bigDigitTextColor = payload.bigDigitTextColor || state.bigDigitTextColor;
      state.arcGradient = !!payload.arcGradient;
      state.hudGradient = !!payload.hudGradient;
      state.teSync = !!payload.teSync;
      state.vaultFace = payload.vaultFace || state.vaultFace;
      if (payload.vaultVignette !== undefined) state.vaultVignette = payload.vaultVignette;
      const active = state.themes.find((t) => t.id === state.activeThemeId);
      if (active) setTheme(active);
      renderThemes();
      if (okMsg) showOk(okMsg);
    } catch (error) {
      showError(error.message);
    }
  }, 250);
}

function themeEditor(theme) {
  const wrap = document.createElement("div");
  wrap.className = "theme-editor";

  const fields = [
    ["vacuum", "Vacuum"],
    ["boost", "Boost"],
    ["overboost", "Overboost"],
  ];
  for (const [key, label] of fields) {
    const row = document.createElement("label");
    row.className = "theme-color-row";
    const input = document.createElement("input");
    input.type = "color";
    input.value = theme.colors[key];
    input.addEventListener("input", () => {
      /* Repaint the local canvas immediately; the device catches up on the
       * debounced PUT. Waiting for the round trip makes the picker feel dead. */
      theme.colors[key] = input.value;
      if (theme.id === state.activeThemeId) setTheme(theme);
      queueThemeConfig({ id: theme.id, colors: { [key]: input.value } });
    });
    const name = document.createElement("span");
    name.textContent = label;
    row.append(input, name);
    wrap.append(row);
  }

  const addToggle = (key, label, onMsg, offMsg) => {
    const row = document.createElement("label");
    row.className = "theme-toggle-row";
    const box = document.createElement("input");
    box.type = "checkbox";
    box.checked = !!state[key];
    box.addEventListener("change", () =>
      queueThemeConfig({ [key]: box.checked }, box.checked ? onMsg : offMsg),
    );
    const name = document.createElement("span");
    name.textContent = label;
    row.append(box, name);
    wrap.append(row);
  };

  if (theme.style === "vault") {
    /* Dial glow colour. Brighter greens read as a stronger illuminated dial. */
    const crow = document.createElement("label");
    crow.className = "theme-color-row";
    const cin = document.createElement("input");
    cin.type = "color";
    cin.value = state.vaultFace || "#05281a";
    cin.addEventListener("input", () => {
      state.vaultFace = cin.value;
      queueThemeConfig({ vaultFace: cin.value });
    });
    const cname = document.createElement("span");
    cname.textContent = "Dial glow colour";
    crow.append(cin, cname);
    wrap.append(crow);

    /* Vignette depth: how dark the edges fall relative to the centre. */
    const vrow = document.createElement("div");
    vrow.className = "theme-range-row";
    const vin = document.createElement("input");
    vin.type = "range";
    vin.min = "0"; vin.max = "90"; vin.step = "5";
    vin.value = String(state.vaultVignette ?? 60);
    const vname = document.createElement("span");
    const setLabel = () => { vname.textContent = `Edge darkening ${vin.value}%`; };
    setLabel();
    vin.addEventListener("input", () => {
      setLabel();
      state.vaultVignette = Number(vin.value);
      queueThemeConfig({ vaultVignette: Number(vin.value) });
    });
    vrow.append(vname, vin);
    wrap.append(vrow);
  }

  if (theme.style === "arc") {
    addToggle("arcGradient", "Gradient fill (smooth colour transition)",
              "Gradient fill", "Zone colours");
  }
  if (theme.style === "hud") {
    addToggle("hudGradient", "Gradient fill (smooth colour transition)",
              "Gradient fill", "Zone colours");
  }
  if (theme.style === "bigdigit") {
    addToggle("bigDigitStaticBg", "Static background (no colour sweep)",
              "Static background", "Colour sweep");
    addToggle("bigDigitColorText", "Colour the readout instead of the background",
              "Readout colour", "White readout");

    if (!state.bigDigitColorText) {
      const row = document.createElement("label");
      row.className = "theme-color-row";
      const input = document.createElement("input");
      input.type = "color";
      input.value = state.bigDigitTextColor || "#ffffff";
      input.addEventListener("input", () => {
        state.bigDigitTextColor = input.value;
        queueThemeConfig({ bigDigitTextColor: input.value });
      });
      const name = document.createElement("span");
      name.textContent = "Readout text colour";
      row.append(input, name);
      wrap.append(row);
    }

    if (state.bigDigitStaticBg) {
      const row = document.createElement("label");
      row.className = "theme-color-row";
      const input = document.createElement("input");
      input.type = "color";
      input.value = state.bigDigitStaticColor || "#000000";
      input.addEventListener("input", () => {
        state.bigDigitStaticColor = input.value;
        queueThemeConfig({ bigDigitStaticColor: input.value });
      });
      const name = document.createElement("span");
      name.textContent = "Background colour (black = pixels off)";
      row.append(input, name);
      wrap.append(row);
    }
  }

  if (theme.customized) {
    const reset = document.createElement("button");
    reset.type = "button";
    reset.className = "theme-reset";
    reset.textContent = "Reset to default colours";
    reset.addEventListener("click", () =>
      queueThemeConfig({ id: theme.id, reset: true }, `${theme.name} reset`),
    );
    wrap.append(reset);
  }
  return wrap;
}

function renderThemes() {
  el.themeList.replaceChildren();
  for (const theme of state.themes) {
    const row = document.createElement("div");
    row.className = "theme-row";

    const button = document.createElement("button");
    button.className = `theme-option${theme.id === state.activeThemeId ? " active" : ""}`;
    button.type = "button";
    button.innerHTML = `
      <span class="theme-dots">
        <i style="background:${theme.colors.vacuum}"></i>
        <i style="background:${theme.colors.boost}"></i>
        <i style="background:${theme.colors.overboost}"></i>
      </span>
      <span>${theme.name}${theme.customized ? " *" : ""}</span>
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
        state.themes = payload.themes || state.themes;
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

    const edit = document.createElement("button");
    edit.type = "button";
    edit.className = "theme-edit";
    edit.title = `Edit ${theme.name} colours`;
    edit.textContent = openThemeEditor === theme.id ? "Close" : "Colours";
    edit.addEventListener("click", () => {
      openThemeEditor = openThemeEditor === theme.id ? null : theme.id;
      renderThemes();
    });

    row.append(button, edit);
    el.themeList.append(row);
    if (openThemeEditor === theme.id) el.themeList.append(themeEditor(theme));
  }
}

/* Pixel shift is one control, not two. The device keeps an on/off flag and an
 * interval, but a checkbox beside a period picker can be left reading
 * "off / every 90 s", which is a state the user has to reason about and the
 * device cannot honour. Collapsing both into one <select> makes "Off" simply
 * the first choice, and the interval the device remembers from last time is
 * still there when they turn it back on. */
function pixelShiftLabel(seconds) {
  if (seconds % 60 === 0 && seconds >= 60) {
    const minutes = seconds / 60;
    return `Every ${minutes} minute${minutes === 1 ? "" : "s"}`;
  }
  return `Every ${seconds} seconds`;
}

/* The API accepts any interval in its range, not just the three offered here,
 * so a value set by some other client must still be selectable rather than
 * silently rewritten to 90 s the first time this page loads. */
function ensurePixelShiftOption(seconds) {
  const select = el.pixelShiftMode;
  if (!select) return;
  const value = String(seconds);
  if ([...select.options].some((option) => option.value === value)) return;
  const option = document.createElement("option");
  option.value = value;
  option.textContent = pixelShiftLabel(seconds);
  option.dataset.custom = "1";
  /* Keep the list ordered by period; "off" sorts first because it parses NaN. */
  const after = [...select.options].find(
    (existing) => Number(existing.value) > seconds,
  );
  select.insertBefore(option, after || null);
}

/* Settings-page display controls. All live on the themes/config endpoint
 * because they are panel-render behaviour, not gauge range. */
function syncDisplayToggles() {
  if (el.pixelShiftMode) {
    const seconds = Number(state.pixelShiftSec) || 90;
    ensurePixelShiftOption(seconds);
    el.pixelShiftMode.value = state.pixelShift ? String(seconds) : "off";
  }
  if (el.teSync) el.teSync.checked = !!state.teSync;
  if (el.demoMode) el.demoMode.checked = !!state.demoMode;
}

function wireDisplayToggles() {
  const send = async (body, label) => {
    try {
      showError("");
      const payload = await api("/themes/config", {
        method: "PUT",
        body: JSON.stringify(body),
      });
      state.pixelShift = !!payload.pixelShift;
      state.pixelShiftSec = Number(payload.pixelShiftSec) || state.pixelShiftSec;
      state.teSync = !!payload.teSync;
      state.demoMode = !!payload.demoMode;
      state.bigDigitStaticBg = !!payload.bigDigitStaticBg;
      state.bigDigitColorText = !!payload.bigDigitColorText;
      state.themes = payload.themes || state.themes;
      syncDisplayToggles();
      showOk(label);
    } catch (error) {
      /* Put the control back where the device actually is. */
      syncDisplayToggles();
      showError(error.message);
    }
  };
  if (el.teSync) {
    el.teSync.addEventListener("change", () =>
      send({ teSync: el.teSync.checked },
           el.teSync.checked ? "Tear sync on" : "Tear sync off"),
    );
  }
  if (el.demoMode) {
    el.demoMode.addEventListener("change", () =>
      send({ demoMode: el.demoMode.checked },
           el.demoMode.checked ? "Demo mode on" : "Demo mode off (live sensors)"),
    );
  }
  if (el.pixelShiftMode) {
    el.pixelShiftMode.addEventListener("change", () => {
      const choice = el.pixelShiftMode.value;
      if (choice === "off") {
        /* Interval deliberately not sent: leaving it stored is what lets the
         * picker come back to the user's own choice, not the default. */
        send({ pixelShift: false }, "Pixel shift off");
        return;
      }
      const seconds = Number(choice);
      send({ pixelShift: true, pixelShiftSec: seconds },
           `Pixel shift ${pixelShiftLabel(seconds).toLowerCase()}`);
    });
  }
}

/* ═══ MAP atmosphere calibration ══════════════════════════════════════════
 *
 * Backed by GET/POST /api/v1/sensors/calibration and PUT /api/v1/sensors/supply.
 * Everything here is settings-only except captureAmbient/ambientAtmText, which
 * the cockpit uses for the Night City corner readout.
 */

/* One message per machine code the contract defines. A bare code in the error
 * box tells the operator nothing about what to go and check. */
const CAL_ERROR_TEXT = {
  no_ads: "No ADS1115. The MAP sensor's ADC is not answering at 0x48 — check its wiring, power, and the sensor bus before calibrating.",
  no_bmp: "No usable BMP280 atmospheric reference. Either the sensor is absent at 0x76 or the reading has fallen back to the 101.325 kPa standard atmosphere, which is a constant and not a measurement.",
  stale_reading: "Sensor readings went stale during the measurement. One of the sensors stopped returning fresh samples — check the bus and try again.",
  unstable_reading: "The readings moved too much over the two-second window. Let the pressure settle (engine off, no airflow across the sensors) and try again.",
  implausible_pressure: "The measured pressure is not plausible for atmosphere. Check the MAP supply voltage setting above and that the MAP port really is open to air.",
  correction_out_of_range: "The required correction is larger than ±10 kPa. That points at the wrong sensor, the wrong supply voltage, or a MAP port still connected to the manifold. Nothing was saved.",
  persist_failed: "The measurement was good but writing it to NVS failed. The previous calibration is still active and still in force.",
  busy: "A calibration is already running on the device. Wait for it to finish, then try again.",
};

const SUPPLY_ERROR_TEXT = {
  invalid_supply: "MAP supply voltage must be a number between 4.50 V and 5.50 V.",
  persist_failed: "The supply voltage could not be written to NVS. The previous setting is still in force.",
};

function calErrorMessage(code) {
  return CAL_ERROR_TEXT[code] || `Calibration failed: ${code}`;
}

function supplyErrorMessage(code) {
  return SUPPLY_ERROR_TEXT[code] || `Supply voltage not saved: ${code}`;
}

const calUi = {
  pollTimer: null,
  inFlight: false,
  busy: false,        /* POST in flight; it blocks ~2 s server-side */
  supplyTimer: null,  /* debounce handle */
  supplySaving: false,
  pollFailed: false,
};

/* Calibration results go to a panel-local line rather than the shared
 * #errorBox / #okBox. showError("") runs on every successful /state poll, which
 * on HTTP fallback is 4 Hz - an error posted there is gone in 250 ms, and a
 * two-second calibration that fails deserves better than a flash. */
function setCalStatus(kind, message) {
  if (!el.calStatus) return;
  el.calStatus.hidden = !message;
  el.calStatus.textContent = message || "";
  el.calStatus.classList.toggle("error", kind === "error");
  el.calStatus.classList.toggle("ok", kind === "ok");
  el.calStatus.classList.toggle("pending", kind === "pending");
}

function setReadout(node, text, tone) {
  if (!node) return;
  node.textContent = text;
  node.classList.toggle("cal-unavailable", tone === "unavailable");
  node.classList.toggle("cal-warn", tone === "warn");
}

function signedFixed(value, digits) {
  const n = Number(value);
  if (!Number.isFinite(n)) return "--";
  return `${n < 0 ? "−" : "+"}${Math.abs(n).toFixed(digits)}`;
}

/* Ages arrive as -1 for "never read successfully" (firmware UINT32_MAX). A
 * negative number on screen would read as a measurement; it is not one. */
function formatAge(ms) {
  const n = Number(ms);
  if (!Number.isFinite(n) || n <= AGE_NEVER || n < 0) return "never read";
  return n < 1000 ? `${Math.round(n)} ms` : `${(n / 1000).toFixed(1)} s`;
}

function ageIsFresh(ms) {
  const n = Number(ms);
  return Number.isFinite(n) && n >= 0 && n <= BMP_FRESH_MS;
}

function formatCalDate(epochMs) {
  const n = Number(epochMs);
  if (!Number.isFinite(n) || n < 1e12) return "";
  const d = new Date(n);
  const pad = (v) => String(v).padStart(2, "0");
  return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())} ${pad(d.getHours())}:${pad(d.getMinutes())}`;
}

/* The supply field is the one control on this panel the user types into, so a
 * poll or a save response must never yank it out from under a newer edit. */
function supplyEditPending() {
  return calUi.supplyTimer !== null || calUi.supplySaving;
}

function shouldWriteSupplyInput(force) {
  if (!el.supplyVolts) return false;
  if (supplyEditPending()) return false;
  return force ? true : document.activeElement !== el.supplyVolts;
}

function syncSupplyInput() {
  if (!el.supplyVolts) return;
  const volts = Number(state.calibration && state.calibration.supplyVolts);
  el.supplyVolts.value = Number.isFinite(volts) ? volts.toFixed(2) : "";
}

/* Single entry point for every response that carries the calibration body:
 * poll, supply PUT, and calibration POST all return the same shape.
 *
 * Regression ledger, "Toggle unchecks itself but the effect sticks": a debounced
 * write whose handler re-renders from local state must fold the WHOLE response
 * into that state first, or the render races the save and shows the pre-save
 * mirror. renderCalibration/syncSupplyInput below read only from
 * state.calibration, and state.calibration is replaced wholesale here. */
function applyCalibrationPayload(payload, opts = {}) {
  if (!payload || typeof payload !== "object") return;
  state.calibration = payload;
  renderCalibration();
  if (shouldWriteSupplyInput(Boolean(opts.force))) syncSupplyInput();
}

function renderCalibration() {
  if (!el.calMapVolts) return;
  const payload = state.calibration;
  const live = payload && typeof payload.live === "object" ? payload.live : null;
  const cal = payload && typeof payload.calibration === "object" ? payload.calibration : null;

  if (!live) {
    setReadout(el.calMapVolts, "--", "unavailable");
    setReadout(el.calNominalKpa, "--", "unavailable");
    setReadout(el.calCorrectedKpa, "--", "unavailable");
    setReadout(el.calBmpKpa, "--", "unavailable");
    setReadout(el.calSensors, "No sensor data", "unavailable");
  } else {
    const mapFresh = Boolean(live.adsPresent) && ageIsFresh(live.mapAgeMs);
    const bmpMeasured = Boolean(live.bmpPresent) && !live.ambientIsFallback && ageIsFresh(live.bmpAgeMs);
    const volts = Number(live.mapVolts);

    setReadout(
      el.calMapVolts,
      Number.isFinite(volts) && Boolean(live.adsPresent) ? `${volts.toFixed(4)} V` : "--",
      live.adsPresent ? (mapFresh ? null : "warn") : "unavailable",
    );
    setReadout(el.calNominalKpa, kpaText(live.nominalKpa, live.adsPresent), live.adsPresent ? null : "unavailable");
    setReadout(el.calCorrectedKpa, kpaText(live.correctedKpa, live.adsPresent), live.adsPresent ? null : "unavailable");

    if (!live.bmpPresent) {
      setReadout(el.calBmpKpa, "Absent", "unavailable");
    } else if (live.ambientIsFallback) {
      /* Never present the standard-atmosphere constant as a measurement. */
      setReadout(el.calBmpKpa, "Fallback constant", "warn");
    } else {
      setReadout(el.calBmpKpa, kpaText(live.bmpKpa, true), bmpMeasured ? null : "warn");
    }

    const updates = Number(live.bmpUpdates);
    const sensorBits = [
      `ADS ${live.adsPresent ? formatAge(live.mapAgeMs) : "absent"}`,
      `BMP ${live.bmpPresent ? formatAge(live.bmpAgeMs) : "absent"}`,
      `${Number.isFinite(updates) ? updates : 0} BMP reads`,
    ];
    if (live.fault) sensorBits.push("fault");
    setReadout(el.calSensors, sensorBits.join(" · "), live.fault || !mapFresh || !bmpMeasured ? "warn" : null);
  }

  if (cal && cal.valid) {
    setReadout(el.calOffset, `${signedFixed(cal.offsetKpa, 2)} kPa · ${signedFixed(cal.offsetPsi, 3)} PSI`, null);
    const when = formatCalDate(cal.epochMs);
    setReadout(el.calStateText, when ? `Calibrated ${when}` : "Calibrated (device clock unset)", null);
    const refVolts = Number(cal.refMapVolts);
    const refBmp = Number(cal.refBmpKpa);
    const refSupply = Number(cal.supplyVolts);
    setReadout(
      el.calReference,
      `${Number.isFinite(refVolts) ? refVolts.toFixed(4) : "--"} V @ ${Number.isFinite(refSupply) ? refSupply.toFixed(2) : "--"} V · ${
        Number.isFinite(refBmp) ? refBmp.toFixed(2) : "--"
      } kPa · ${Number(cal.samples) || 0} samples`,
      null,
    );
  } else {
    setReadout(el.calOffset, "None (0.00 kPa)", "unavailable");
    setReadout(el.calStateText, "Never calibrated", "warn");
    setReadout(el.calReference, "No reference stored", "unavailable");
  }
}

function kpaText(value, present) {
  const n = Number(value);
  if (!present || !Number.isFinite(n)) return "--";
  return `${n.toFixed(2)} kPa`;
}

async function pollCalibration() {
  /* Never race a write: an in-flight POST/PUT owns the panel until it settles,
   * and a queued supply edit must not be overwritten by an older device value. */
  if (calUi.inFlight || calUi.busy || supplyEditPending()) return;
  calUi.inFlight = true;
  try {
    const payload = await api("/sensors/calibration");
    applyCalibrationPayload(payload);
    if (calUi.pollFailed) {
      calUi.pollFailed = false;
      setCalStatus(null, "");
    }
  } catch (error) {
    if (!calUi.pollFailed) {
      calUi.pollFailed = true;
      setCalStatus("error", `Sensor diagnostics unavailable: ${error.message}`);
    }
    state.calibration = null;
    renderCalibration();
  } finally {
    calUi.inFlight = false;
  }
}

function startCalibrationPolling() {
  if (!IS_SETTINGS || !el.calMapVolts || calUi.pollTimer !== null) return;
  if (document.hidden) return;
  calUi.pollTimer = window.setInterval(() => { void pollCalibration(); }, CAL_POLL_MS);
  void pollCalibration();
}

function stopCalibrationPolling() {
  if (calUi.pollTimer === null) return;
  window.clearInterval(calUi.pollTimer);
  calUi.pollTimer = null;
}

function readSupplyInput() {
  const volts = Number(el.supplyVolts.value);
  if (!Number.isFinite(volts)) throw new Error("MAP supply voltage must be a number");
  if (!(volts >= 4.5 && volts <= 5.5)) {
    throw new Error("MAP supply voltage must be between 4.50 V and 5.50 V");
  }
  return volts;
}

/* Typing in a number field fires per keystroke; one PUT per edit, not per key. */
function queueSupplySave() {
  if (!el.supplyVolts) return;
  window.clearTimeout(calUi.supplyTimer);
  calUi.supplyTimer = window.setTimeout(() => {
    calUi.supplyTimer = null;
    void saveSupplyVolts();
  }, 400);
}

async function saveSupplyVolts() {
  let volts;
  try {
    volts = readSupplyInput();
  } catch (error) {
    setCalStatus("error", error.message);
    return;
  }
  calUi.supplySaving = true;
  try {
    setCalStatus(null, "");
    const payload = await api("/sensors/supply", {
      method: "PUT",
      body: JSON.stringify({ supplyVolts: volts }),
    });
    calUi.supplySaving = false;
    applyCalibrationPayload(payload, { force: true });
    setCalStatus("ok", `MAP supply ${volts.toFixed(2)} V saved`);
  } catch (error) {
    calUi.supplySaving = false;
    setCalStatus("error", supplyErrorMessage(error.message));
    /* Put the field back where the device actually is. */
    if (shouldWriteSupplyInput(true)) syncSupplyInput();
  }
}

function showCalConfirm(show) {
  if (!el.calConfirm) return;
  el.calConfirm.hidden = !show;
}

async function runCalibration() {
  /* The POST blocks ~2 s server-side. Guard against a double fire from a second
   * click, a keyboard activation, or an impatient double-click. */
  if (calUi.busy) return;
  calUi.busy = true;
  showCalConfirm(false);
  const btn = el.calibrateBtn;
  const label = btn ? btn.textContent : "";
  if (btn) {
    btn.disabled = true;
    btn.dataset.pending = "1";
    btn.textContent = "Calibrating…";
  }
  if (el.calConfirmBtn) el.calConfirmBtn.disabled = true;
  setCalStatus("pending", "Measuring atmosphere — about two seconds…");
  try {
    const payload = await api("/sensors/calibration", { method: "POST", body: "{}" });
    applyCalibrationPayload(payload, { force: true });
    const cal = (state.calibration && state.calibration.calibration) || {};
    setCalStatus(
      "ok",
      `Calibrated · offset ${signedFixed(cal.offsetKpa, 2)} kPa (${signedFixed(cal.offsetPsi, 3)} PSI) from ${Number(cal.samples) || 0} samples`,
    );
  } catch (error) {
    setCalStatus("error", calErrorMessage(error.message));
  } finally {
    calUi.busy = false;
    if (btn) {
      btn.disabled = false;
      delete btn.dataset.pending;
      btn.textContent = label;
    }
    if (el.calConfirmBtn) el.calConfirmBtn.disabled = false;
  }
}

function wireCalibration() {
  if (!IS_SETTINGS || !el.calMapVolts) return;
  on(el.supplyVolts, "input", queueSupplySave);
  on(el.supplyVolts, "change", queueSupplySave);
  on(el.calibrateBtn, "click", () => {
    if (calUi.busy) return;
    showCalConfirm(el.calConfirm ? el.calConfirm.hidden : true);
  });
  on(el.calCancelBtn, "click", () => showCalConfirm(false));
  on(el.calConfirmBtn, "click", () => { void runCalibration(); });
  renderCalibration();
}

/* ── Cockpit: measured atmosphere for the Night City corner readout ───────── */

const ambientUi = { pollTimer: null, inFlight: false };

function captureAmbient(live) {
  if (!live || typeof live !== "object") {
    state.ambient = null;
    return;
  }
  const ageMs = Number(live.bmpAgeMs);
  state.ambient = {
    kpa: Number(live.bmpKpa),
    ageMs: Number.isFinite(ageMs) ? ageMs : AGE_NEVER,
    present: Boolean(live.bmpPresent),
    fallback: Boolean(live.ambientIsFallback),
    at: performance.now(),
  };
}

/* Mirrors the atm_fresh gate in update_hud() in main/boost_gauge.c: the value is
 * shown only when the BMP280 actually measured it. bmpPresent is a boot-time
 * flag, ambientIsFallback marks the 101.325 kPa constant, and bmpAgeMs catches a
 * sensor that answered at boot and has since gone quiet. The poll interval is
 * added back onto the reported age so the label goes unavailable on schedule
 * instead of on the next poll. */
function ambientAtmText() {
  const a = state.ambient;
  if (!a || !a.present || a.fallback) return "ATM --kPa";
  if (!Number.isFinite(a.kpa) || a.ageMs < 0) return "ATM --kPa";
  if (a.ageMs + (performance.now() - a.at) > BMP_FRESH_MS) return "ATM --kPa";
  return `ATM ${Math.round(a.kpa)}kPa`;
}

async function pollAmbient() {
  if (ambientUi.inFlight) return;
  ambientUi.inFlight = true;
  try {
    const payload = await api("/sensors/calibration");
    captureAmbient(payload && payload.live);
  } catch (_) {
    /* Unreachable, or firmware without the endpoint. Fall back to the
     * unavailable placeholder — never to a stale or synthetic number — and stay
     * quiet: this is a diagnostic garnish, not the telemetry path, and it must
     * not take over the cockpit error box from /state. */
    state.ambient = null;
  } finally {
    ambientUi.inFlight = false;
  }
}

function startAmbientPolling() {
  if (!IS_COCKPIT || ambientUi.pollTimer !== null) return;
  if (document.hidden) return;
  ambientUi.pollTimer = window.setInterval(() => { void pollAmbient(); }, AMBIENT_POLL_MS);
  void pollAmbient();
}

function stopAmbientPolling() {
  if (ambientUi.pollTimer === null) return;
  window.clearInterval(ambientUi.pollTimer);
  ambientUi.pollTimer = null;
}

/* A hidden tab needs neither readout. Both timers are cheap, but neither is on
 * the telemetry path, so there is nothing to be gained by running them blind.
 * Called at boot as well as on every visibility change: no visibilitychange
 * event fires for the state a page loads in, so a page opened in a background
 * tab would otherwise start its timers and leave them to browser throttling. */
function syncSensorPolling() {
  if (document.hidden) {
    stopCalibrationPolling();
    stopAmbientPolling();
    return;
  }
  startCalibrationPolling();
  startAmbientPolling();
}

async function refreshNetwork() {
  const net = await api("/network");
  renderNetwork(net);
  return net;
}

async function refreshAll() {
  try {
    showError("");
    if (IS_COCKPIT) {
      /* Keep device wall-clock aligned with the browser so dim schedules match local time. */
      try {
        const now = new Date();
        const tz = -now.getTimezoneOffset();
        await api("/time", {
          method: "POST",
          body: JSON.stringify({ epochMs: now.getTime(), timezoneOffsetMinutes: tz }),
        });
        if (el.tzOffset) el.tzOffset.value = tz;
      } catch (_) {
        /* non-fatal; schedule may wait until manual Sync */
      }
    }
    const requests = [api("/state"), api("/config"), api("/themes"), api("/network")];
    if (IS_COCKPIT) requests.push(api("/media/status"));
    const [statePayload, config, themes, network, media] = await Promise.all(requests);
    state.themes = themes.themes || [];
    state.bigDigitStaticBg = !!themes.bigDigitStaticBg;
    state.bigDigitColorText = !!themes.bigDigitColorText;
    state.bigDigitStaticColor = themes.bigDigitStaticColor || "#000000";
    state.bigDigitTextColor = themes.bigDigitTextColor || "#ffffff";
    state.arcGradient = !!themes.arcGradient;
    state.hudGradient = !!themes.hudGradient;
    state.teSync = !!themes.teSync;
    state.demoMode = !!themes.demoMode;
    state.vaultFace = themes.vaultFace || "#05281a";
    state.vaultVignette = themes.vaultVignette ?? 60;
    state.pixelShift = !!themes.pixelShift;
    state.pixelShiftSec = Number(themes.pixelShiftSec) || state.pixelShiftSec;
    syncDisplayToggles();
    state.activeThemeId = themes.activeThemeId || statePayload.activeThemeId || state.activeThemeId;
    if (IS_COCKPIT) renderThemes();
    setTheme(state.themes.find((theme) => theme.id === state.activeThemeId));
    renderConfig(config);
    renderState(statePayload);
    if (IS_COCKPIT) renderMediaStatus(media);
    renderNetwork(network);
    /* A successful refresh must not relabel an already-open WebSocket as HTTP. */
    if (!state.liveSocket || state.liveSocket.readyState !== WebSocket.OPEN) updateConnection("online", "http");
  } catch (error) {
    if (!state.liveSocket || state.liveSocket.readyState !== WebSocket.OPEN) updateConnection("offline");
    showError(error.message);
  }
}

async function pollState() {
  if (state.pollInFlight) return;
  state.pollInFlight = true;
  try {
    const sample = await api("/state");
    renderState(sample);
    updateConnection("online", "http");
    showError("");
  } catch (error) {
    updateConnection("offline");
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
  if (!state.fallbackActive) updateConnection("offline");
  socket.onopen = () => {
    if (state.liveSocket !== socket) return;
    stopPolling();
    startHeartbeat(socket);
    updateConnection("online", "websocket");
    showError("");
  };
  socket.onmessage = (event) => {
    if (state.liveSocket !== socket) return;
    try {
      const sample = JSON.parse(event.data);
      renderState(sample);
      updateConnection("online", "websocket");
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

function readRangeForm() {
  const psiMin = Number(el.psiMin.value);
  const psiMax = Number(el.psiMax.value);
  const psiOverboost = Number(el.psiOverboost.value);
  const zeroAngle = Number(el.zeroAngle.value);
  if (!Number.isFinite(psiMin) || !Number.isFinite(psiMax) || !Number.isFinite(psiOverboost) || !Number.isFinite(zeroAngle)) {
    throw new Error("Gauge range values must be numbers");
  }
  if (!(psiMin >= -30 && psiMin <= -1)) throw new Error("Min PSI must be between −30 and −1");
  if (!(psiMax >= 5 && psiMax <= 40)) throw new Error("Max PSI must be between 5 and 40");
  if (!(psiOverboost > 0 && psiOverboost < psiMax)) {
    throw new Error("Overboost must be greater than 0 and less than max PSI");
  }
  if (!(zeroAngle >= 180 && zeroAngle <= 315)) {
    throw new Error("Zero position must be between 180° and 315°");
  }
  return { psiMin, psiMax, psiOverboost, zeroAngle };
}

async function saveRange() {
  const range = readRangeForm();
  const payload = { ...range };
  renderConfig(await api("/config", { method: "PUT", body: JSON.stringify(payload) }));
  showOk(
    `Gauge range saved · ${formatTickLabel(range.psiMin)} / ${formatTickLabel(range.psiMax)} · zero ${range.zeroAngle.toFixed(2)}°`,
  );
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
  if (payload.restartRequired) {
    el.otaStatus.textContent = "Verified · restarting to activate…";
    try {
      await api("/restart", { method: "POST" });
      el.otaStatus.textContent = "Restarting · reconnecting…";
    } catch (error) {
      /* The device may drop the connection as it reboots; that is success. */
      el.otaStatus.textContent = "Restarting · reconnecting…";
    }
  } else {
    el.otaStatus.textContent = payload.status || "OTA accepted";
  }
  showOk(el.otaStatus.textContent);
}

function wireControls() {
  on(el.refreshBtn, "click", () => refreshAll().catch((e) => showError(e.message)));
  on(el.syncTimeBtn, "click", () => syncTime().catch((error) => showError(error.message)));
  on(el.saveConfigBtn, "click", () => saveConfig().catch((error) => showError(error.message)));
  on(el.saveRangeBtn, "click", () => saveRange().catch((error) => showError(error.message)));
  on(el.loadLogsBtn, "click", () => loadLogs().catch((error) => showError(error.message)));
  on(el.exportLogsBtn, "click", () => exportLogs().catch((error) => showError(error.message)));
  on(el.clearLogsBtn, "click", () => clearLogs().catch((error) => showError(error.message)));
  on(el.uploadMediaBtn, "click", () => uploadMedia().catch((error) => showError(error.message)));
  on(el.deleteMediaBtn, "click", () => deleteMedia().catch((error) => showError(error.message)));
  on(el.uploadOtaBtn, "click", () => uploadOta().catch((error) => showError(error.message)));
  on(el.saveNetworkBtn, "click", () => saveNetwork().catch((error) => showError(error.message)));
  on(el.reconnectNetworkBtn, "click", () => reconnectNetwork().catch((error) => showError(error.message)));
  on(el.networkRefreshBtn, "click", () => refreshNetwork().catch((error) => showError(error.message)));
  on(el.scanNetworksBtn, "click", () => scanNetworks());
  on(el.netScanSelect, "change", () => {
    if (el.netScanSelect.value) {
      el.netSsid.value = el.netScanSelect.value;
      el.netModeSelect.value = "apsta";
    }
  });
  on(el.brightnessHigh, "input", () => { if (el.brightnessHighOut) el.brightnessHighOut.textContent = `${el.brightnessHigh.value}%`; });
  on(el.brightnessLow, "input", () => { if (el.brightnessLowOut) el.brightnessLowOut.textContent = `${el.brightnessLow.value}%`; });
  on(el.mediaFile, "change", () => { if (el.mediaStatus) el.mediaStatus.textContent = el.mediaFile.files[0]?.name || "No active GIF"; });
  on(el.otaFile, "change", () => { if (el.otaStatus) el.otaStatus.textContent = el.otaFile.files[0]?.name || "Idle"; });
  window.addEventListener("resize", () => {
    if (!IS_COCKPIT) return;
    scheduleGaugeRender();
    drawSparkline();
  });
}

updatePalette();
state.config = {
  psiMin: DEFAULT_PSI_MIN,
  psiMax: DEFAULT_PSI_MAX,
  psiOverboost: DEFAULT_PSI_OVERBOOST,
};
state.gaugeTarget = { psi: 0, peakPsi: 0, zone: "ATMO", demo: true };
wireControls();
wireDisplayToggles();
wireCalibration();
document.addEventListener("visibilitychange", syncSensorPolling);
syncSensorPolling();
if (IS_COCKPIT) {
  scheduleGaugeRender();
  drawSparkline();
}
refreshAll().finally(connectEvents);
