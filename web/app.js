"use strict";

const API = "/api/v1";
const DEFAULT_PSI_MIN = -15;
const DEFAULT_PSI_MAX = 10;
const DEFAULT_PSI_OVERBOOST = 8;
const ARC_START = 135;
const ARC_RANGE = 270;
const DEFAULT_ZERO_ANGLE = 236.25;
const ZERO_GAP_VAC = 5.0;
const ZERO_GAP_BOOST = 5.0;
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
 * NOT on the 62.5 Hz WebSocket or the 4 Hz /state path - the firmware keeps it
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
/* TPMS art and overlays use the device 466 px face coordinate system. Keep
 * geometry centralized here; the PNG is currently drawn 1:1. */
const TPMS_FACE_SIZE = 466;
const TPMS_FACE_CENTER = TPMS_FACE_SIZE / 2;
const TPMS_POWERTRAIN_SRC = "/tpms_powertrain.png";
/* Final tire bounds from process_tpms_powertrain.py with the +2 px inflation
 * that covers the art's anti-aliased tire edge, matching TPMS_CAPSULE_GROW in
 * boost_tpms_ui.c. */
const TPMS_CAPSULE_GROW = 2;
const TPMS_CAPSULES = [
  { x: 129 - TPMS_CAPSULE_GROW, y: 80 - TPMS_CAPSULE_GROW, w: 52 + TPMS_CAPSULE_GROW * 2, h: 104 + TPMS_CAPSULE_GROW * 2, radius: 26 + TPMS_CAPSULE_GROW, textX: 121, textY: 132, align: "right" },
  { x: 284 - TPMS_CAPSULE_GROW, y: 80 - TPMS_CAPSULE_GROW, w: 53 + TPMS_CAPSULE_GROW * 2, h: 104 + TPMS_CAPSULE_GROW * 2, radius: 26 + TPMS_CAPSULE_GROW, textX: 345, textY: 132, align: "left" },
  { x: 114 - TPMS_CAPSULE_GROW, y: 277 - TPMS_CAPSULE_GROW, w: 54 + TPMS_CAPSULE_GROW * 2, h: 109 + TPMS_CAPSULE_GROW * 2, radius: 27 + TPMS_CAPSULE_GROW, textX: 106, textY: 332, align: "right" },
  { x: 297 - TPMS_CAPSULE_GROW, y: 277 - TPMS_CAPSULE_GROW, w: 54 + TPMS_CAPSULE_GROW * 2, h: 109 + TPMS_CAPSULE_GROW * 2, radius: 27 + TPMS_CAPSULE_GROW, textX: 359, textY: 332, align: "left" },
];
const tpmsPowertrainImg = new Image();
tpmsPowertrainImg.onload = () => scheduleGaugeRender();
tpmsPowertrainImg.onerror = () => scheduleGaugeRender();
tpmsPowertrainImg.src = TPMS_POWERTRAIN_SRC;

const state = {
  activeThemeId: "dyno-cell",
  activePage: 0,
  tpms: { status: 2, wheels: [] },
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
  sparklineRaf: null,
  sparklineLastAt: 0,
  /* Mirrors the firmware defaults so the picker shows something honest in the
   * gap before the first /themes response lands. */
  pixelShift: true,
  pixelShiftSec: 90,
  demoMode: false,
  demoFastSweep: false,
  rotation: 0,
  vaultNeedleRed: false,
  vaultNeedleTail: false,
  neonLayout: 1,
  neonPreset: 0,
  hudTrueBlack: false,
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
  connection: document.getElementById("connection"),
  connectionText: document.getElementById("connectionText"),
  errorBox: document.getElementById("errorBox"),
  okBox: document.getElementById("okBox"),
  canvas: document.getElementById("gaugeCanvas"),
  sparkline: document.getElementById("sparkline"),
  gaugeDevice: document.getElementById("gaugeDevice"),
  pageToggle: document.getElementById("pageToggle"),
  pageSegments: [...document.querySelectorAll(".page-segment")],
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
  regionDBuf: document.getElementById("regionDBuf"),
  teScanline: document.getElementById("teScanline"),
  rotation: document.getElementById("rotation"),
  demoMode: document.getElementById("demoMode"),
  tpmsBle: document.getElementById("tpmsBle"),
  tpmsLowPsi: document.getElementById("tpmsLowPsi"),
  tpmsStaleSec: document.getElementById("tpmsStaleSec"),
  obdStateText: document.getElementById("obdStateText"),
  obdPeer: document.getElementById("obdPeer"),
  obdRpm: document.getElementById("obdRpm"),
  obdSpeed: document.getElementById("obdSpeed"),
  obdCoolant: document.getElementById("obdCoolant"),
  obdBattery: document.getElementById("obdBattery"),
  loadLogsBtn: document.getElementById("loadLogsBtn"),
  clearLogsBtn: document.getElementById("clearLogsBtn"),
  logSummary: document.getElementById("logSummary"),
  exportLogsBtn: document.getElementById("exportLogsBtn"),
  mediaFile: document.getElementById("mediaFile"),
  uploadMediaBtn: document.getElementById("uploadMediaBtn"),
  deleteMediaBtn: document.getElementById("deleteMediaBtn"),
  mediaStatus: document.getElementById("mediaStatus"),
  mediaProgress: document.getElementById("mediaProgress"),
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
  savedNetworksList: document.getElementById("savedNetworksList"),
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
/* SF Alien Encounters glyphs run from the baseline up 0.70 em, and the
 * generated fonts carry base_line 0 - so with an alphabetic baseline the y
 * coordinate IS the ink bottom, and ink top is y - 0.70 em. Both neon readout
 * sizes measure the same ratio (76/108 and 108/154). */
const NEON_INK_EM = 0.70;
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
    scheduleSparklineRender();
  }
}

function zoneFor(psi) {
  const { psiOverboost } = psiRange();
  if (psi >= psiOverboost) return "OVER";
  if (psi >= 0.35) return "BOOST";
  if (psi > -0.35) return "ATMO";
  return "VAC";
}

const GRADIENT_POSITIVE_STEPS = 24;

/* Match firmware gradient_rgb_for_psi()/big_color_for_step(): vacuum is one
 * sentinel color, while every quantized bucket is reserved for positive boost. */
function gradientColorFor(psi) {
  const palette = state.palette;
  const { psiMax, psiOverboost } = psiRange();
  if (!Number.isFinite(psi) || psi <= 0 || !(psiMax > 0)) return palette.vacuum;
  const positive = clamp(psi, 0, psiMax);
  const step = Math.min(GRADIENT_POSITIVE_STEPS, Math.max(1,
    Math.ceil((positive / psiMax) * GRADIENT_POSITIVE_STEPS)));
  const stepPsi = psiMax * step / GRADIENT_POSITIVE_STEPS;
  const redStart = psiOverboost * 0.55;
  if (stepPsi <= redStart) {
    return lerpColor(palette.vacuum, palette.boost, stepPsi / Math.max(0.001, redStart));
  }
  return lerpColor(palette.boost, palette.overboost,
    (stepPsi - redStart) / Math.max(0.001, psiMax - redStart));
}

function colorFor(psi) {
  const palette = state.palette;
  const { psiOverboost } = psiRange();
  if (psi >= psiOverboost) return palette.overboost;
  if (psi >= 0.35) return palette.boost;
  if (psi > -0.35) return palette.text;
  return palette.vacuum;
}

function arcColorFor(psi) {
  return state.arcGradient ? gradientColorFor(psi) : colorFor(psi);
}

function hudColorFor(psi) {
  if (state.hudGradient) return gradientColorFor(psi);
  const palette = state.palette;
  const { psiOverboost } = psiRange();
  if (psi >= psiOverboost) return palette.overboost;
  return psi < 0 ? palette.vacuum : palette.boost;
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

function psiToSweep(psi, a0, a1, range = psiRange()) {
  const zeroAt = a0 + ((range.zeroAngle - ARC_START) / ARC_RANGE) * (a1 - a0);
  const value = clamp(psi, range.psiMin, range.psiMax);
  if (value < 0) {
    const span = -range.psiMin;
    return a0 + (span > 0 ? (value - range.psiMin) / span : 1) * (zeroAt - a0);
  }
  return zeroAt + (range.psiMax > 0 ? value / range.psiMax : 0) * (a1 - zeroAt);
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
  const gap = 16 * scale;
  const fractionalGap = 16 * scale;

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
  if (state.activePage === 1) {
    drawTpmsFace(sample, g);
    return;
  }
  switch (activeThemeStyle()) {
    case "vault":
      return drawVaultGauge(sample, psi, g);
    case "hud":
      return drawHudGauge(sample, psi, g);
    case "bigdigit":
      return drawBigDigitGauge(sample, psi, g);
    case "neon":
      return drawNeonGauge(sample, psi, g);
    default:
      return drawArcGauge(sample, psi, g);
  }
}

/* ── Style: arc — the classic Dyno Cell dual-climate face ────────────────── */
function drawArcGauge(sample, psi, g) {
  const { cx, cy, scale, size } = g;
  const outerR = 231 * scale;
  const stroke = 54 * scale;
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
    ctx.strokeStyle = arcColorFor(psi);
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
  ctx.font = `700 ${Math.max(14, 19 * scale)}px system-ui, -apple-system, "Segoe UI", sans-serif`;
  for (const value of tickValues(range.psiMin, range.psiMax, range.psiOverboost, range.zeroAngle)) {
    let r = 160 * scale;
    if (Math.abs(value) < 0.01) r = 142 * scale;
    const a = degToRad(psiToAngle(value));
    ctx.fillText(formatTickLabel(value), cx + r * Math.cos(a), cy + r * Math.sin(a));
  }

  /* Center stack — zone / PSI / unit / peak / mode (matches physical UI) */
  const zone = sample.zone || zoneFor(psi);
  const peak = Math.max(0, Number(sample.peakPsi || 0));
  ctx.fillStyle = colorFor(psi);
  ctx.font = `700 ${Math.max(14, 20 * scale)}px system-ui, -apple-system, "Segoe UI", sans-serif`;
  ctx.fillText(zone, cx, cy - 88 * scale);

  ctx.fillStyle = psi >= range.psiOverboost ? state.palette.overboost : state.palette.text;
  ctx.font = `400 ${Math.max(40, 56 * scale)}px "Archivo Black", sans-serif`;
  drawFixedPsi(psi, cx + 8 * scale, cy - 6 * scale, scale);

  ctx.fillStyle = state.palette.muted;
  ctx.font = `700 ${Math.max(13, 17 * scale)}px system-ui, -apple-system, "Segoe UI", sans-serif`;
  ctx.fillText("PSI", cx, cy + 85 * scale);

  ctx.fillStyle = peak >= range.psiOverboost ? state.palette.overboost : state.palette.boost;
  ctx.font = `700 ${Math.max(13, 16 * scale)}px system-ui, -apple-system, "Segoe UI", sans-serif`;
  ctx.fillText(`PEAK  ${peak.toFixed(1)}`, cx, cy + 113 * scale);

  /* DEMO only in demo. The panel sets this label to "" on the real-sensor path
   * (boost_gauge.c), so drawing "LIVE" here made the mirror disagree with the
   * device. Skip the draw entirely rather than painting an empty string. */
  if (sample.demo) {
    ctx.fillStyle = state.palette.muted;
    ctx.font = `700 ${Math.max(13, 14 * scale)}px system-ui, -apple-system, "Segoe UI", sans-serif`;
    ctx.fillText("DEMO", cx, cy + 141 * scale);
  }
}

/* ── Style: neon — fixed-cell bloom ring / tube / marquee mirror ─────────── */
function drawNeonGauge(sample, psi, g) {
  const p = state.palette;
  const range = psiRange();
  const layout = Number.isInteger(Number(state.neonLayout)) ? Number(state.neonLayout) : 1;
  const S = g.scale;
  const nseg = 45;
  const ringR = 228;
  const segmentW = 30;
  const capW = 6;
  /* Segment pitch is FIXED at 6 degrees (4 lit + 2 gap), an exact division of
   * the 270 sweep (45 x 6 = 270) with every boundary a whole degree, because
   * LVGL rasterizes arcs at whole-degree resolution and a fractional slot
   * renders as a mix of widths. Keep in lockstep with the firmware's
   * NEON_SEG_PITCH / NEON_SEG_START. */
  const NEON_SEG_GAP = 2.0;
  /* Tube only: the lit run is FOUR bands - the innermost dark halo (20 px),
   * the track split into an inner half (bloomed, 16 px) and a lighter outer
   * half (16 px, NEON_TUBE_TRACK_LIFT 0.4 toward white), and the skinny
   * white cap (6 px) back at ringR. Radii are the band OUTER edges, matching
   * k_neon_tube_band_geom in boost_gauge.c; each track half (16) is less
   * than the halo (20), so the track (32) is under twice it. */
  const tubeHaloW = 20, tubeTrackHalf = 16;
  const tubeTrackOuterR = ringR - capW + 1;                     /* 223 */
  const tubeTrackInnerR = tubeTrackOuterR - tubeTrackHalf + 1;  /* 208 */
  const tubeHaloR = tubeTrackInnerR - tubeTrackHalf + 1;        /* 193 */
  const tubeBandDepth = ringR - tubeHaloR + tubeHaloW;          /* 55 (halo inner edge to 228) */
  /* Segments: the inner dark halo (NEON_SEG_HALO_W 20) is narrower than the
   * body (W - CAP + 1 = 25) - it used to match at 25/25 - so the full lit
   * depth is 20 + 25 + 6 - 2 shared px = 49. The mirror draws the dim band at
   * this depth and the body/cap cover it, exactly as the panel abuts them. */
  const segHaloW = 20;
  const segBodyW = segmentW - capW + 1;
  const segBandDepth = segHaloW + segBodyW + capW - 2;   /* 49 */
  const start = ARC_START;
  const sweep = ARC_RANGE;
  const zero = psiToSweep(0, start, start + sweep, range);
  const value = psiToSweep(psi, start, start + sweep, range);
  const zone = psi >= range.psiOverboost ? "OVERBOOST" : psi > 0.05 ? "BOOST" : "VACUUM";
  const accent = psi >= range.psiOverboost ? p.overboost : psi > 0.05 ? p.boost : p.vacuum;
  const lit = neonLitColor(accent);
  const tubeMid = neonTrackMiddle(accent);
  const trackOuter = neonTrackOuter(accent);
  const marquee = layout === 2;
  const tube = layout === 0;
  /* Marquee shares the ring layouts' readout metrics and lower-stack rhythm
   * now; only the border/bar art differs. The marquee draws the shared
   * readout/bar/stack at NEON_MARQUEE_CENTER_SCALE (0.87) so they fit inside
   * its spread rings - the panel scales its shared sprites at blit time, so
   * the mirror scales the canvas geometry. */
  const mq = marquee ? 0.87 : 1;
  /* The centre scale pulls the readout toward the face centre, so on the
   * marquee it gets an extra lift (NEON_MARQUEE_READOUT_LIFT) to keep the
   * bar and the peak/psi stack below with room to breathe. */
  const readoutLift = marquee ? 12 : 0;
  const stackDy = 0;
  /* Canvas CAN blur, unlike the panel's draw path, so the mirror reproduces
   * the baked glow with a shadow rather than approximating it. The firmware
   * bakes a box blur of radius 5 applied twice at 115% gain; a shadowBlur of
   * 14 matches its visible reach closely enough to be a fair mirror. */
  const GLOW = 14;
  const withGlow = (color, draw) => {
    ctx.save();
    ctx.shadowColor = color;
    ctx.shadowBlur = GLOW;
    draw();
    ctx.restore();
  };

  ctx.save();
  ctx.translate(g.cx, g.cy);
  ctx.scale(S, S);
  ctx.fillStyle = "#000000";
  ctx.beginPath(); ctx.arc(0, 0, 233, 0, Math.PI * 2); ctx.fill();

  /* LVGL's arc radius is the stroke's OUTER EDGE. Canvas centres a stroke on
   * the path it is given. Passing ringR straight through therefore centred all
   * three bands on the same circle, which put the 6 px white cap in the MIDDLE
   * of the 42 px band and made the segment read as a symmetrical stripe. The
   * panel stacks them inward from a shared outer edge: white cap outermost,
   * then the bloomed body, then the raw palette colour on the inside. */
  const arc = (a0, a1, width, color) => {
    ctx.strokeStyle = color; ctx.lineWidth = width; ctx.lineCap = "butt";
    ctx.beginPath();
    ctx.arc(0, 0, ringR - width / 2, degToRad(a0), degToRad(a1));
    ctx.stroke();
  };
  /* Variant for bands with an explicit OUTER edge instead of ringR: the tube's
   * four bands each stack at their own outer radius (halo 199, track inner
   * 211, track outer 223, cap 228), matching k_neon_tube_band_geom. */
  const arcAt = (outer, a0, a1, width, color) => {
    ctx.strokeStyle = color; ctx.lineWidth = width; ctx.lineCap = "butt";
    ctx.beginPath();
    ctx.arc(0, 0, outer - width / 2, degToRad(a0), degToRad(a1));
    ctx.stroke();
  };
  const segPitch = 6.0;
  const segStart = start + (sweep - nseg * segPitch) / 2;
  if (marquee) {
    ctx.fillStyle = p.track; roundRectPath(-132 * mq, 68 * mq, 264 * mq, 16 * mq, 8); ctx.fill();
    /* Three concentric bulb rings, one zone each - innermost vacuum, middle
     * boost, outermost overboost - at the spread 24px step (NEON_BULB_RINGS /
     * NEON_BULB_RING_STEP on the panel; parity reads both from
     * boost_gauge.c). Dead bulbs stay dim track; every ring's 24 accent bulbs
     * light LIVE in that ring's OWN zone colour once the reading has REACHED
     * that zone (zone id >= ring index, cumulative - a stage ladder). The
     * accent phase is ANCHORED per ring (NEON_BULB_ACCENT_OFFSET(z)): inner
     * and outer share pair positions at 0,1 (top centre), middle ring's pairs
     * at 3,4 (bottom centre, bulb N/2 at 6 o'clock as the pair's first dot,
     * partner one dot left) - so lit rings align inner/outer and the middle
     * marks the boost stage (NEON_BULB_IS_ACCENT). */
    const ringRgb = [p.vacuum, p.boost, p.overboost];
    const zone = psi >= range.psiOverboost ? 2 : psi > 0.05 ? 1 : 0;
    /* Per-ring bulb counts for uniform chord spacing: outer 72, middle 66,
     * inner 54 (parity reads NEON_BULB_N_INNER/MID/OUTER from the panel).
     * All divisible by 6 so the 2-lit/4-dark accent pattern stays seamless. */
    const ringN = [54, 66, 72];
    /* Marquee chase (neonMarqueeSpin): one ring advances per spin tick,
     * round-robin, so ring z's phase after T ticks is dir_z * floor((T + 2 - z)
     * / 3) + 1 (for T >= z), mod 6 - the panel advances ring (tick % 3) each
     * NEON_MARQUEE_SPIN_MS, inner/outer clockwise (-1) and middle
     * counterclockwise (+1). The pattern period is 6, so a ring has exactly 6
     * phase states. The mirror derives T from wall clock; the panel restarts
     * at 0 on every scene build, so the two are not phase-locked (the chase
     * is decorative, not a measurement). */
    let spinTicks = 0;
    if (state.neonMarqueeSpin) {
      /* The parity test injects state.neonSpinTicks for a deterministic phase;
       * production leaves it undefined and derives T from wall clock. */
      spinTicks = state.neonSpinTicks ?? Math.floor(performance.now() / NEON_MARQUEE_SPIN_MS);
    }
    const spinDir = [-1, 1, -1];
    /* Only advance when the chase is ON: the panel never advances the phase
     * while disabled, so the mirror must not either (its phase formula would
     * otherwise give ring 0 a non-zero phase at tick 0 even though the static
     * pattern is what ships with the toggle off). */
    const spinPhase = [0, 0, 0];
    if (state.neonMarqueeSpin) {
      for (let z = 0; z < 3; z++) {
        if (spinTicks >= z) {
          spinPhase[z] = (((spinDir[z] * (Math.floor((spinTicks - z) / 3) + 1)) % 6) + 6) % 6;
        }
      }
    }
    for (let z = 0; z < 3; z++) {
      const rr = 224 - (2 - z) * 24;
      const n = ringN[z];
      for (let i = 0; i < n; i++) {
        /* -90 puts bulb 0 at 12 o'clock (top); n even means bulb n/2 lands at
         * the bottom too, so every ring has a centred dot at top and bottom
         * and the rings share those two alignment axes (matches the panel). */
        const a = (i * 360 / n - 90) * DEG;
        const isAccent = (i + (z === 1 ? 3 : (z === 2 ? 2 : 0)) + spinPhase[z]) % 6 < 2;
        ctx.fillStyle = (isAccent && z <= zone) ? neonBulbColor(ringRgb[z]) : p.track;
        ctx.beginPath(); ctx.arc(Math.cos(a) * rr, Math.sin(a) * rr, 4, 0, Math.PI * 2); ctx.fill();
      }
    }
    const x0 = -132 * mq + (psiToSweep(0, 0, 264 * mq, range));
    const xv = -132 * mq + (psiToSweep(psi, 0, 264 * mq, range));
    const lo = Math.min(x0, xv), hi = Math.max(x0, xv);
    if (hi - lo > 2) {
      ctx.fillStyle = lit; roundRectPath(lo, 68 * mq, hi - lo, 16 * mq, 8); ctx.fill();
    }
    ctx.fillStyle = "#ffffff"; roundRectPath(x0 - 3.5 * mq, 62.5 * mq, 7 * mq, 27 * mq, 3.5 * mq); ctx.fill();
  } else {
    /* The unlit track shares the same outer edge as the lit bands, and stays
     * at the body width while the lit run extends further in. The tube's track
     * is a solid arc matching its wider lit footprint; segments draws individual
     * unlit segment blocks. */
    if (tube) {
      arc(start, start + sweep, ringR - tubeHaloR + 1, p.track);
    } else {
      for (let i = 0; i < nseg; i++) {
        const a0 = segStart + i * segPitch + NEON_SEG_GAP / 2;
        const a1 = a0 + segPitch - NEON_SEG_GAP;
        arc(a0, a1, segmentW, p.track);
      }
    }
    const lo = Math.min(zero, value), hi = Math.max(zero, value);
    const zeroSeg = Math.floor((zero - segStart) / segPitch);
    if (tube) {
      if (hi - lo > 2) {
        /* White cap + three tones, drawn inner to outer: dark halo, bloomed
         * track inner half, then the lighter track outer half (the run the
         * panel paints via neon_tube_band_color). */
        arcAt(tubeHaloR, lo, hi, tubeHaloW, neonDim(accent));
        arcAt(tubeTrackInnerR, lo, hi, tubeTrackHalf, tubeMid);
        arcAt(tubeTrackOuterR, lo, hi, tubeTrackHalf, trackOuter);
        arcAt(ringR, lo, hi, capW, "#ffffff");
      }
      /* After the run, and +-NEON_TUBE_ZERO_DEG (2.25): the panel's run starts
       * exactly at zero and sweeps outward at this same radius and width, so
       * drawing the marker first would leave only half of it - which is what
       * used to happen on the panel itself. The marker spans the full band
       * depth (tubeBandDepth). Canvas arcs are symmetric, so the mirror needs
       * no centring compensation (the panel applies NEON_TUBE_ZERO_CENTER). */
      arc(zero - 2.25, zero + 2.25, tubeBandDepth, "#ffffff");
    } else {
      /* Index by floor at BOTH ends, exactly as boost_neon_lit_span() does, so
       * a value landing mid-segment lights that segment whole. Testing each
       * segment's START angle against the span instead dropped the segment
       * containing zero, because zero sits at 101.25 degrees and the pitch is 6. */
      const litRun = hi - lo >= segPitch * 0.5;
      const first = Math.max(0, Math.floor((lo - segStart) / segPitch));
      const last = Math.min(nseg - 1, Math.floor((hi - segStart) / segPitch));
      for (let i = 0; i < nseg; i++) {
        const a0 = segStart + i * segPitch + NEON_SEG_GAP / 2;
        const a1 = a0 + segPitch - NEON_SEG_GAP;
        if (i === zeroSeg) { arc(a0, a1, segBandDepth, "#ffffff"); continue; }
        if (!litRun || i < first || i > last) continue;
        arc(a0, a1, segBandDepth, neonDim(accent));
        arc(a0, a1, segmentW, lit);
        arc(a0, a1, capW, "#ffffff");
      }
    }
  }

  const tenthsTotal = Math.round(Math.abs(psi) * 10);
  const whole = Math.floor(tenthsTotal / 10);
  const chars = `${whole >= 10 ? Math.floor(whole / 10) : ""}${whole % 10}.${tenthsTotal % 10}`;
  /* NEON_SLOT_W / NEON_DOT_W, verbatim, on every layout now - marquee lost
   * its own wider metrics when the readout was unified. They had drifted
   * badly once (64/33 against the panel's 88/34), which made the mirror draw
   * a visibly tighter readout than the panel ever showed. The marquee scales
   * these by mq (NEON_MARQUEE_CENTER_SCALE) to match its smaller sprites. */
  const widths = [...chars].map((ch) => ch === "." ? 34 * mq : 88 * mq);
  const total = widths.reduce((a, b) => a + b, 0);
  let x = -total / 2;
  const readoutTop = -42 * mq - readoutLift;
  const fontPx = 118 * mq;
  /* Weight 400, matching what styles.css actually declares for this face. It
   * asked for 700, which the family does not provide - the browser then shapes
   * the SYSTEM fallback instead, silently, and canvas never corrects itself
   * because it does not watch for webfont swaps. That is why the readout lost
   * its typeface. */
  ctx.font = `italic ${fontPx}px "SF Alien Encounters", sans-serif`;
  /* Alphabetic, not "top". lv_draw_label anchors to the TOP OF ITS BOX and this
   * font has base_line 0, so its ink runs from the box top down 0.70 em. Canvas
   * "top" anchors to the font's ASCENT metric, which sits above the ink here -
   * so the same y drew the readout low. Placing the baseline at
   * readoutTop + 0.70 em puts the INK top where the panel puts it. */
  ctx.textAlign = "center"; ctx.textBaseline = "alphabetic";
  const readoutBaseline = readoutTop + fontPx * NEON_INK_EM;
  ctx.fillStyle = lit;
  withGlow(lit, () => {
    let gx = x;
    for (let i = 0; i < chars.length; i++) { const w = widths[i]; ctx.fillText(chars[i], gx + w / 2, readoutBaseline); gx += w; }
  });
  if (psi < 0 && tenthsTotal !== 0) {
    const adv = [780, 409, 767, 753, 673, 767, 762, 677, 770, 763];
    const lsb = [133, 133, 113, 92, 99, 107, 133, 126, 128, 147];
    const first = Number(chars[0]);
    const font = fontPx;
    const sw = 42 * mq;
    const gap = 8 * mq;
    const firstCell = 88 * mq;
    const glyphLeft = -total / 2 + firstCell / 2 - (adv[first] * font / 1000) / 2 + lsb[first] * font / 1000;
    const sx = glyphLeft - gap - sw / 2;
    const skew = 0.2126;                       /* tan(12 deg), the italic angle */
    const barH = font * 8 / 128, period = font * 10.4 / 128, lean = barH * skew;
    ctx.fillStyle = lit;
    /* Centre the mark on the digits' line box, not on the face - and derive
     * that centre the way the panel does, as top + line_h/2 with line_h the
     * generated font's own integer line height, rather than from a hand-fitted
     * em fraction that has to be re-fitted every time the font changes. Both
     * generated fonts measure exactly NEON_INK_EM of the em (83 at 118 px,
     * 91 at 130 px), so rounding that product reproduces line_h. */
    const lineH = Math.round(fontPx * NEON_INK_EM);
    const inkMid = readoutTop + Math.floor(lineH / 2);
    const top = inkMid - (period + barH) / 2;
    const bars = [];
    for (let i = 0; i < 2; i++) {
      const y0 = top + i * period, y1 = y0 + barH;
      /* Shear the WHOLE mark about its centre line, not just each bar's own
       * edges. Without this both bars sat at the same x, so the upper bar did
       * not step right and the mark leaned backwards against the italic digits
       * next to it. Matches boost_neon_sign_bars(). */
      const dx = Math.round((inkMid - (y0 + y1) / 2) * skew);
      const xb0 = sx - sw / 2 + dx, xb1 = sx + sw / 2 + dx;
      bars.push({ y0, y1, xb0, xb1 });
    }
    /* One glow over BOTH bars, not one per bar. Drawing them separately would
     * let each bar's shadow fall across the other, filling the gap between
     * them - which is exactly the failure the panel's own first sign glow had
     * when it inflated the bars instead of blurring them. */
    withGlow(lit, () => {
      ctx.beginPath();
      for (const b of bars) {
        ctx.moveTo(b.xb0 + lean, b.y0); ctx.lineTo(b.xb1 + lean, b.y0);
        ctx.lineTo(b.xb1, b.y1); ctx.lineTo(b.xb0, b.y1);
        ctx.closePath();
      }
      ctx.fill();
    });
  }
  /* The zone word and the unit mark share the firmware's 24 px neon_label, not
   * a system font - they are set in the same typeface as the readout.
   *
   * Alphabetic again, for the same reason as the readout. The firmware puts
   * "P S I" in a 200x30 box top-anchored at NEON_UNIT_Y - 15, and neon_label
   * has base_line 0 with a 17 px line height, so its ink runs 81..98 and the
   * ink BOTTOM sits at NEON_UNIT_Y + 2 - not at NEON_UNIT_Y. Drawing it
   * centred on NEON_UNIT_Y pushed the ink down far enough for its antialiasing
   * to touch the rule 10 px below, which is the reported collision. With
   * base_line 0 an alphabetic baseline IS the ink bottom, so the y here is the
   * measured 98 and the rule at 106 clears it by 8. */
  ctx.textBaseline = "alphabetic";
  /* Upright, not italic. The zone word and the unit mark are set in the
   * typeface's regular face while the readout stays italic - styles.css inlines
   * both faces under one family, so dropping the `italic` keyword is what picks
   * the upright one. Leaving the keyword in would silently keep the italic
   * face, since a family with only one face serves it for any style asked. */
  ctx.font = `24px "SF Alien Encounters", sans-serif`;
  /* The zone word glows too now - it is baked through the same blur as the
   * readout on the panel, having been the one lit element there with no glow
   * at all. */
  ctx.fillStyle = lit;
  withGlow(lit, () => ctx.fillText(zone, 0, -95));
  /* NEON_UNIT_Y / NEON_RULE_Y / NEON_PEAK_Y, which moved down 40 as a unit.
   * The unit mark's y is UNIT_Y + 2 because neon_label has base_line 0 and an
   * alphabetic baseline IS the ink bottom - see the note above. */
  ctx.fillStyle = p.muted; ctx.fillText("P S I", 0, Math.round(128 * mq) + 2 + stackDy);
  ctx.strokeStyle = p.muted; ctx.lineWidth = 1; ctx.beginPath(); ctx.moveTo(-62 * mq, Math.round(138 * mq) + stackDy); ctx.lineTo(62 * mq, Math.round(138 * mq) + stackDy); ctx.stroke();
  /* Peak is clamped at zero on the panel, as on every other face here. */
  ctx.textBaseline = "middle";
  ctx.font = `700 ${16 * mq}px monospace`;
  ctx.fillText(`PEAK ${Math.max(0, Number(sample.peakPsi) || 0).toFixed(1)}`, 0, Math.round(158 * mq) + stackDy);
  ctx.restore();
}

/* Mirrors neon_lit() in main/boost_gauge.c, including its overflow handling -
 * which this function never had. It clamped each channel independently, so
 * every saturated palette entry pinned to the same corner of the colour cube
 * and the three zones converged on nearly the same post-bloom colour. That is
 * why the zones still looked indistinguishable HERE after the panel had been
 * fixed twice: the fix had only ever been applied on the firmware side.
 *
 * Overflow past full scale desaturates toward white rather than being clipped
 * or scaled away - see the long note on NEON_WHITE_LIFT in boost_gauge.c for
 * why both of those failed. */
const NEON_BLOOM = 1.92, NEON_SAT = 1.30, NEON_WHITE_LIFT = 0.35;
/* NEON_HALO_DIM: the ring's inner band is a dimmed zone colour, not the raw
 * palette entry, so the three bands read dark -> bright -> white outward. */
const NEON_HALO_DIM = 0.55;
/* Marquee chase tick (matches NEON_MARQUEE_SPIN_MS in boost_gauge.c): one
 * ring advances per tick, round-robin; the pattern period is 6 so a ring
 * has exactly 6 phase states. The parity test asserts this constant and the
 * direction table against the firmware. */
const NEON_MARQUEE_SPIN_MS = 90;
function neonDim(hex, k = NEON_HALO_DIM) {
  const [r, g, b] = hexToRgb(hex);
  return `rgb(${Math.round(r * k)}, ${Math.round(g * k)}, ${Math.round(b * k)})`;
}
/* Mirrors neon_bulb_accent() in main/boost_gauge.c: the zone colour is dimmed
 * to 55% with the firmware's scale_rgb() rounding FIRST, then bloomed. Folding
 * the dim into neon_lit's gain (as the old accent bulbs did not have to care
 * about, being live) rounds differently and drifts a channel off the panel. */
function neonBulbColor(hex) {
  const [r0, g0, b0] = hexToRgb(hex);
  const dim = [Math.round(r0 * 0.55), Math.round(g0 * 0.55), Math.round(b0 * 0.55)];
  const [dr, dg, db] = dim;
  const luma = 0.299 * dr + 0.587 * dg + 0.114 * db;
  let r = Math.max(0, (luma + (dr - luma) * NEON_SAT) * NEON_BLOOM);
  let g = Math.max(0, (luma + (dg - luma) * NEON_SAT) * NEON_BLOOM);
  let b = Math.max(0, (luma + (db - luma) * NEON_SAT) * NEON_BLOOM);
  const peak = Math.max(r, g, b);
  if (peak > 255) {
    const scale = 255 / peak;
    r *= scale; g *= scale; b *= scale;
    const w = (1 - scale) * NEON_WHITE_LIFT;
    r += (255 - r) * w; g += (255 - g) * w; b += (255 - b) * w;
  }
  return `rgb(${Math.round(r)}, ${Math.round(g)}, ${Math.round(b)})`;
}
function neonLitColor(hex, factor = 1) {
  const [r0, g0, b0] = hexToRgb(hex);
  const luma = 0.299 * r0 + 0.587 * g0 + 0.114 * b0;
  const gain = NEON_BLOOM * factor;
  let r = Math.max(0, (luma + (r0 - luma) * NEON_SAT) * gain);
  let g = Math.max(0, (luma + (g0 - luma) * NEON_SAT) * gain);
  let b = Math.max(0, (luma + (b0 - luma) * NEON_SAT) * gain);
  const peak = Math.max(r, g, b);
  if (peak > 255) {
    const scale = 255 / peak;
    r *= scale; g *= scale; b *= scale;
    const w = (1 - scale) * NEON_WHITE_LIFT;
    r += (255 - r) * w; g += (255 - g) * w; b += (255 - b) * w;
  }
  return `rgb(${Math.round(r)}, ${Math.round(g)}, ${Math.round(b)})`;
}

/* The tube's track OUTER half colour: the bloomed+scaled MIDDLE lightened via
 * HSL lightness (hue and saturation preserved), mirroring neon_hsl_lighten()
 * in boost_gauge.c with NEON_TUBE_TRACK_LIGHT 0.26. The band stack must read
 * as a smooth gradient dark -> bloomed -> lighter -> white, so the outer half
 * is lighter than the middle - a deeper outer turns it into a dark-bright-dark
 * sandwich, and a white-lift washes it toward the cap. */
function rgbToHsl(r, g, b) {
  r /= 255; g /= 255; b /= 255;
  const mx = Math.max(r, g, b), mn = Math.min(r, g, b);
  const d = mx - mn;
  let h = 0, s = 0;
  const l = (mx + mn) / 2;
  if (d > 0) {
    s = l > 0.5 ? d / (2 - mx - mn) : d / (mx + mn);
    if (mx === r) h = (g - b) / d + (g < b ? 6 : 0);
    else if (mx === g) h = (b - r) / d + 2;
    else h = (r - g) / d + 4;
    h *= 60;
  }
  return [h, s, l];
}
function hslHue2rgb(p, q, t) {
  if (t < 0) t += 1;
  if (t > 1) t -= 1;
  if (t < 1 / 6) return p + (q - p) * 6 * t;
  if (t < 0.5) return q;
  if (t < 2 / 3) return p + (q - p) * (2 / 3 - t) * 6;
  return p;
}
/* The tube's track INNER half (the bloomed middle): the accent bloomed then
 * scaled to NEON_TUBE_MID_SCALE 0.88, mirroring neon_tube_band_color() in
 * boost_gauge.c. The scale keeps the middle vivid while giving the outer
 * lighten step chroma headroom - without it the bright zone colours (yellow,
 * magenta, cyan) bloomed so near-white that lightening them toward the cap
 * collapsed into the "too pale" wash. */
function neonTrackMiddle(hex) {
  const [r, g, b] = neonLitColor(hex).match(/\d+/g).map(Number);
  return `rgb(${Math.round(r * 0.88)}, ${Math.round(g * 0.88)}, ${Math.round(b * 0.88)})`;
}
function neonTrackOuter(hex) {
  const [r, g, b] = neonTrackMiddle(hex).match(/\d+/g).map(Number);
  const [h, s, l] = rgbToHsl(r, g, b);
  const nl = Math.min(1, l + 0.26);
  const q = nl < 0.5 ? nl * (1 + s) : nl + s - nl * s;
  const p = 2 * nl - q;
  const hk = h / 360;
  const nr = Math.round(hslHue2rgb(p, q, hk + 1 / 3) * 255);
  const ng = Math.round(hslHue2rgb(p, q, hk) * 255);
  const nb = Math.round(hslHue2rgb(p, q, hk - 1 / 3) * 255);
  return `rgb(${nr}, ${ng}, ${nb})`;
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
  const needle = state.vaultNeedleRed ? "#FF3B30" : (psi >= range.psiOverboost ? warn : green);
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
  const needleTail = state.vaultNeedleTail ? 26 : 0;
  ctx.beginPath();
  ctx.moveTo(-6, needleTail);
  ctx.lineTo(6, needleTail);
  ctx.lineTo(2, -150);
  ctx.lineTo(-2, -150);
  ctx.closePath();
  ctx.fillStyle = needle;
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
  ctx.fillStyle = state.hudTrueBlack ? "#000000" : p.face;
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
    const [x0, y0] = polar(217, a);
    const [x1, y1] = polar(233, a);
    ctx.beginPath();
    ctx.moveTo(x0, y0);
    ctx.lineTo(x1, y1);
    ctx.strokeStyle = v >= range.psiOverboost ? R : C;
    ctx.lineWidth = i % 2 === 0 ? 4 : 2;
    ctx.stroke();
  }

  /* track + fill (grows from the zero notch) */
  ctx.beginPath();
  ctx.arc(0, 0, 225, canvasAngle(A0), canvasAngle(A1));
  ctx.strokeStyle = "#1a1c0a";
  ctx.lineWidth = 15;
  ctx.stroke();
  const va = hudAngle(psi);
  const loA = Math.min(zeroHud, va);
  const hiA = Math.max(zeroHud, va);
  if (hiA - loA > 0.5) {
    ctx.beginPath();
    ctx.arc(0, 0, 225, canvasAngle(loA), canvasAngle(hiA));
    ctx.strokeStyle = hudColorFor(psi);
    ctx.lineWidth = 15;
    ctx.stroke();
  }

  /* zero notch */
  const [zx0, zy0] = polar(215, zeroHud);
  const [zx1, zy1] = polar(233, zeroHud);
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
  ctx.font = `700 italic 88px "Bahnschrift", "DIN Alternate", system-ui, sans-serif`;
  /* Match the physical face's visible chromatic ghost passes. Keep the offset
   * layers opaque at their pre-blended strength so the effect is not lost
   * beneath the main glyph pass. */
  ctx.globalAlpha = 0.7;
  ctx.fillStyle = R;
  drawFixedDecimal(intStr, fracPart, numDecimalX - 6, -4);
  ctx.fillStyle = C;
  drawFixedDecimal(intStr, fracPart, numDecimalX + 6, -4);
  ctx.globalAlpha = 1;
  ctx.fillStyle = over ? R : Y;
  drawFixedDecimal(intStr, fracPart, numDecimalX, 2);
  ctx.fillStyle = p.muted;
  ctx.font = `600 16px Consolas, monospace`;
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
  ctx.fillText(sample.demo ? "SYS DEMO" : "SYS LIVE", -138, 150);
  ctx.textAlign = "right";
  ctx.fillText("NC-2077", 138, 150);
  ctx.restore();
}
/* ── Style: bigdigit — huge Alvida number on a color-sweeping ground ──────── */
function bigDigitBackground(psi) {
  /* The physical Big Digit face uses the shared 24-bucket ramp. Reuse it here
   * so the browser changes color on the same boundaries. */
  return gradientColorFor(psi);
}

function drawBigDigitGauge(sample, psi, g) {
  const range = psiRange();
  const p = state.palette;
  const { cx, cy, size } = g;

  /* The panel uses the configured static colour when requested; otherwise the
   * ground follows the live sweep. */
  ctx.fillStyle = state.bigDigitStaticBg
    ? (state.bigDigitStaticColor || "#000000")
    : bigDigitBackground(psi);
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
  ctx.fillStyle = p.text || "#ffffff";
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
  const readoutColor = state.bigDigitColorText
    ? bigDigitBackground(psi)
    : (state.bigDigitTextColor || p.text || "#ffffff");
  ctx.fillStyle = readoutColor;
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
  ctx.fillStyle = p.text || "#ffffff";
  ctx.font = `700 30px "Bahnschrift", system-ui, sans-serif`;
  ctx.fillText("PSI", 0, 118);
  ctx.globalAlpha = 0.72;
  ctx.fillStyle = p.text || "#ffffff";
  ctx.font = `600 18px Consolas, monospace`;
  /* Matches the panel's `PEAK %.1f` exactly in live mode - no suffix, no
   * trailing separator. The DEMO marker returns only in demo. */
  ctx.fillText(sample.demo ? `PEAK ${peak.toFixed(1)}   DEMO`
                           : `PEAK ${peak.toFixed(1)}`, 0, 168);
  ctx.globalAlpha = 1;
  ctx.restore();
}

function drawTpmsFace(sample, g) {
  const { cx, cy, scale } = g;
  const wheels = state.tpms?.wheels || [];
  const status = Number(state.tpms?.status ?? 2);
  ctx.save();
  ctx.translate(cx - TPMS_FACE_CENTER * scale, cy - TPMS_FACE_CENTER * scale);
  ctx.scale(scale, scale);
  ctx.fillStyle = "#000000";
  ctx.fillRect(0, 0, TPMS_FACE_SIZE, TPMS_FACE_SIZE);
  /* Keep live wheel state usable even if the decorative art is unavailable,
   * matching the device's black-root fallback when its canvas allocation fails. */
  if (tpmsPowertrainImg.complete && tpmsPowertrainImg.naturalWidth) {
    ctx.drawImage(tpmsPowertrainImg, 0, 0, TPMS_FACE_SIZE, TPMS_FACE_SIZE);
  }

  for (let i = 0; i < TPMS_CAPSULES.length; i++) {
    const wheel = wheels[i] || {};
    const psi = Number(wheel.psi);
    const valid = Boolean(wheel.valid) && Number.isFinite(psi);
    let color = "#5A6573";
    let value = "--.-";
    if (status === 1) {
      color = "#FFB020";
      /* Stale data retains the last received PSI, matching the physical face. */
      value = Number.isFinite(psi) ? psi.toFixed(1) : "--.-";
    } else if (status === 0 && valid) {
      value = psi.toFixed(1);
      const lowPsi = Number(state.tpms?.lowPsi ?? 31.9);
      color = psi < lowPsi ? "#E8362E" : "#62D6A5";
    }
    const capsule = TPMS_CAPSULES[i];
    ctx.fillStyle = color;
    roundRectPath(capsule.x, capsule.y, capsule.w, capsule.h, capsule.radius);
    ctx.fill();

    ctx.textAlign = capsule.align;
    ctx.textBaseline = "middle";
    ctx.fillStyle = "#F2F5F8";
    ctx.font = '700 22px sans-serif';
    ctx.fillText(value, capsule.textX, capsule.textY);
  }
  ctx.restore();
}

function syncPageUI() {
  const page = state.activePage === 1 ? 1 : 0;
  state.activePage = page;
  if (el.pageToggle) el.pageToggle.dataset.page = String(page);
  el.pageSegments.forEach((button) => { const active = Number(button.dataset.page) === page; button.classList.toggle("active", active); button.setAttribute("aria-pressed", String(active)); });
  const sparkWrap = el.sparkline?.closest(".sparkline-wrap");
  if (sparkWrap) sparkWrap.hidden = page === 1;
  scheduleGaugeRender();
  if (page === 0) scheduleSparklineRender();
}

function setPage(page) {
  const p = Number(page);
  if (p !== 0 && p !== 1) return;
  if (p === state.activePage) return;
  state.activePage = p; syncPageUI();
  api("/page", { method: "PUT", body: JSON.stringify({ page: p }) }).catch(() => {});
}

function wirePageControls() {
  el.pageSegments.forEach((button) => button.addEventListener("click", () => setPage(button.dataset.page)));
  let startX = 0, startY = 0;
  on(el.gaugeDevice, "pointerdown", (event) => { startX = event.clientX; startY = event.clientY; el.gaugeDevice.setPointerCapture?.(event.pointerId); });
  on(el.gaugeDevice, "pointerup", (event) => { const dx = event.clientX - startX; const dy = event.clientY - startY; if (Math.abs(dx) >= 48 && Math.abs(dx) > Math.abs(dy) * 1.2 && ((state.activePage === 0 && dx < 0) || (state.activePage === 1 && dx > 0))) setPage(state.activePage === 0 ? 1 : 0); });
  syncPageUI();
}

function scheduleGaugeRender() {
  if (!IS_COCKPIT || !el.canvas || state.gaugeRaf !== null) return;
  state.gaugeRaf = requestAnimationFrame(renderGaugeFrame);
}

/* Canvas does not participate in webfont swapping: whatever face ctx.font
 * resolved to when a frame was drawn is what that frame keeps. With
 * font-display:swap and a base64 face this large, the first frames can land
 * before the font is usable and simply stay wrong. Ask for both neon sizes
 * explicitly and repaint once they report ready. */
if (typeof document !== "undefined" && document.fonts) {
  Promise.all([
    document.fonts.load('italic 108px "SF Alien Encounters"'),
    document.fonts.load('italic 154px "SF Alien Encounters"'),
    document.fonts.load('italic 24px "SF Alien Encounters"'),
    document.fonts.load('24px "SF Alien Encounters"'),
    document.fonts.load('400 56px "Archivo Black"'),
  ]).then(() => scheduleGaugeRender()).catch(() => { /* fallback face is fine */ });
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

function scheduleSparklineRender() {
  if (!IS_COCKPIT || !el.sparkline || state.sparklineRaf !== null) return;
  const wrap = el.sparkline.closest(".sparkline-wrap");
  if (document.hidden || wrap?.hidden) return;
  state.sparklineRaf = requestAnimationFrame(() => {
    state.sparklineRaf = null;
    drawSparkline();
  });
}

function drawSparkline() {
  if (!sparkCtx || !el.sparkline) return;
  const wrap = el.sparkline.closest(".sparkline-wrap");
  const rect = el.sparkline.getBoundingClientRect();
  /* display:none reports a zero-width box. Resizing in that state produced a
   * 1px backing store which CSS stretched bright green when Boost returned. */
  if (document.hidden || wrap?.hidden || rect.width <= 0 || rect.height <= 0) return;
  const dpr = Math.min(window.devicePixelRatio || 1, CANVAS_DPR_MAX);
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
    scheduleSparklineRender();
  }
  return true;
}

/* Backfill the 60 s sparkline window from the device RAM log ring (the same
 * 5 Hz samples /logs.csv exports). Only fills holes: ring points that already
 * have a nearby live sample are skipped, so the 62.5 Hz live trace keeps
 * covered regions. */
function seedHistoryFromLog(samples) {
  if (!IS_COCKPIT || !Array.isArray(samples) || !samples.length) return;
  const toleranceMs = 50;
  const inserted = [];
  for (const s of samples) {
    const t = Number(s.tMs);
    if (!Number.isFinite(t)) continue;
    if (sampleHistory.some((p) => Math.abs(p.historyTimeMs - t) <= toleranceMs)) continue;
    inserted.push({
      psi: Number(s.psi ?? 0),
      peakPsi: Number(s.peakPsi ?? 0),
      zone: s.zone,
      demo: !!s.demo,
      uptimeMs: t,
      historyTimeMs: t,
    });
  }
  if (!inserted.length) return;
  sampleHistory.push(...inserted);
  sampleHistory.sort((a, b) => a.historyTimeMs - b.historyTimeMs);
  const newest = sampleHistory.at(-1);
  const cutoffMs = newest.historyTimeMs - HISTORY_WINDOW_MS;
  while (sampleHistory.length && sampleHistory[0].historyTimeMs < cutoffMs) sampleHistory.shift();
  const newestSeeded = inserted.at(-1);
  if (!state.gaugeTarget || newestSeeded.historyTimeMs > Number(state.gaugeTarget.uptimeMs)) {
    state.gaugeTarget = newestSeeded;
    state.gaugePsi = Number(newestSeeded.psi);
  }
  if (el.sampleCount) el.sampleCount.textContent = "Last 60 seconds";
  if (el.emptyState) el.emptyState.hidden = sampleHistory.length > 0;
  scheduleSparklineRender();
}

async function resyncHistory() {
  if (!IS_COCKPIT) return;
  try {
    const payload = await api("/logs?limit=300");
    seedHistoryFromLog(payload?.samples);
  } catch (_) {
    /* non-fatal: the live feed covers the window on its own */
  }
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

/* Shared toast lifetime (#errorBox / #okBox, rendered as a fixed overlay).
 *
 * Two very different producers write this one toast and they must not fight
 * over it, so every write carries the source that raised it:
 *
 *   ERR_LIVE - the unattended telemetry path (pollState, WebSocket frames).
 *              Nobody asked for these messages, and on HTTP fallback the path
 *              runs at POLL_FRAME_MS (4 Hz), so whatever it raises it must also
 *              retract: a transient network error self-clears the instant
 *              polling recovers. As a toast it also auto-dismisses after
 *              TOAST_TRANSIENT_MS, so a vanished producer cannot pin it.
 *   ERR_USER - the outcome of a gesture (save, scan, reconnect, upload, range
 *              validation). Nothing is polling on the operator's behalf, so the
 *              message stays until another gesture supersedes it, a showOk()
 *              replaces it, or the operator clicks it away.
 *
 * A producer clears only what it raised. Before this, pollState() called
 * showError("") on every successful /state sample, so any user-facing error was
 * wiped within 250 ms of appearing - the reason the calibration panel grew its
 * own panel-local #calStatus line rather than trusting this box.
 *
 * ERR_USER outranks ERR_LIVE in both directions: a poll failure may not paper
 * over the Wi-Fi error being read, and a poll recovery may not erase it. No
 * transport information is lost by that - the connection badge is the
 * designated live-transport indicator and this path never touches it.
 *
 * The nodes themselves sit directly under <body> and are position: fixed, so a
 * notification is an overlay: it never shifts the page layout and stays
 * visible regardless of scroll position. */
const ERR_LIVE = "live";
const ERR_USER = "user";
const ERR_RANK = { [ERR_LIVE]: 1, [ERR_USER]: 2 };
/* Source of the message currently on screen; null when the toast is empty. */
let shownErrorSource = null;
/* Transient (ERR_LIVE) toasts auto-dismiss after this long; ERR_USER persists. */
const TOAST_TRANSIENT_MS = 3500;
let toastTimer = null;

function showError(message, source = ERR_USER) {
  if (!el.errorBox) return;
  if (!message) {
    clearError(source);
    return;
  }
  if (shownErrorSource && ERR_RANK[source] < ERR_RANK[shownErrorSource]) return;
  shownErrorSource = source;
  el.errorBox.hidden = false;
  el.errorBox.textContent = message;
  el.errorBox.title = "Click to dismiss";
  if (el.okBox) {
    el.okBox.hidden = true;
    el.okBox.textContent = "";
  }
  window.clearTimeout(toastTimer);
  if (source === ERR_LIVE) {
    toastTimer = window.setTimeout(() => clearError(ERR_LIVE), TOAST_TRANSIENT_MS);
  }
}

function clearError(source = ERR_USER) {
  if (!el.errorBox) return;
  if (shownErrorSource && ERR_RANK[source] < ERR_RANK[shownErrorSource]) return;
  window.clearTimeout(toastTimer);
  shownErrorSource = null;
  el.errorBox.hidden = true;
  el.errorBox.textContent = "";
  el.errorBox.removeAttribute("title");
}

function showOk(message) {
  if (!el.okBox) return;
  el.okBox.hidden = !message;
  el.okBox.textContent = message || "";
  if (message) {
    /* A success supersedes whatever error was on screen, including a sticky
     * user-sourced one. Clearing through clearError() rather than hiding the
     * node keeps shownErrorSource honest - a stale record would otherwise
     * outrank and suppress the next live error. */
    clearError(ERR_USER);
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

/* Fixed UTC-offset options for the Time zone dropdown. The device stores a
 * fixed offset in minutes (the DS3231 keeps UTC), so DST is handled by
 * re-picking the option for the current season - never by an IANA tz database
 * the firmware does not have. */
const TZ_OPTIONS = [
  { m: -720, label: "UTC-12:00 · Int. Date Line West" },
  { m: -660, label: "UTC-11:00 · Pago Pago" },
  { m: -600, label: "UTC-10:00 · Hawaii" },
  { m: -570, label: "UTC-09:30 · Marquesas" },
  { m: -540, label: "UTC-09:00 · Alaska" },
  { m: -480, label: "UTC-08:00 · Pacific Time" },
  { m: -420, label: "UTC-07:00 · Mountain Time" },
  { m: -360, label: "UTC-06:00 · Central Time" },
  { m: -300, label: "UTC-05:00 · Eastern Time" },
  { m: -240, label: "UTC-04:00 · Eastern DST / Atlantic" },
  { m: -210, label: "UTC-03:30 · Newfoundland" },
  { m: -180, label: "UTC-03:00 · Buenos Aires" },
  { m: -120, label: "UTC-02:00 · South Georgia" },
  { m: -60, label: "UTC-01:00 · Azores" },
  { m: 0, label: "UTC+00:00 · London / UTC" },
  { m: 60, label: "UTC+01:00 · Central Europe / West Africa" },
  { m: 120, label: "UTC+02:00 · Eastern Europe / Cairo" },
  { m: 180, label: "UTC+03:00 · Moscow / Nairobi" },
  { m: 210, label: "UTC+03:30 · Tehran" },
  { m: 240, label: "UTC+04:00 · Dubai / Baku" },
  { m: 270, label: "UTC+04:30 · Kabul" },
  { m: 300, label: "UTC+05:00 · Karachi / Tashkent" },
  { m: 330, label: "UTC+05:30 · Mumbai / Colombo" },
  { m: 345, label: "UTC+05:45 · Kathmandu" },
  { m: 360, label: "UTC+06:00 · Dhaka / Almaty" },
  { m: 390, label: "UTC+06:30 · Yangon" },
  { m: 420, label: "UTC+07:00 · Bangkok / Jakarta" },
  { m: 480, label: "UTC+08:00 · Beijing / Perth / Singapore" },
  { m: 525, label: "UTC+08:45 · Eucla" },
  { m: 540, label: "UTC+09:00 · Tokyo / Seoul / Yakutsk" },
  { m: 570, label: "UTC+09:30 · Adelaide / Darwin" },
  { m: 600, label: "UTC+10:00 · Sydney / Brisbane / Vladivostok" },
  { m: 630, label: "UTC+10:30 · Lord Howe" },
  { m: 660, label: "UTC+11:00 · Solomon Is. / Sakhalin" },
  { m: 690, label: "UTC+11:30 · Norfolk Is." },
  { m: 720, label: "UTC+12:00 · Auckland / Fiji" },
  { m: 765, label: "UTC+12:45 · Chatham" },
  { m: 780, label: "UTC+13:00 · Tonga / Apia" },
  { m: 840, label: "UTC+14:00 · Kiritimati" },
];

function populateTzSelect() {
  if (!el.tzOffset) return;
  el.tzOffset.innerHTML = "";
  for (const opt of TZ_OPTIONS) {
    const o = document.createElement("option");
    o.value = String(opt.m);
    o.textContent = opt.label;
    el.tzOffset.appendChild(o);
  }
}

/* Select the option matching `minutes`, adding a Custom entry when the device
 * holds an offset with no exact dropdown entry (e.g. a DST offset mid-season). */
function setTzOffsetSelect(select, minutes) {
  if (!select) return;
  const want = Number(minutes);
  const match = Array.from(select.options).find((o) => Number(o.value) === want);
  if (match) {
    select.value = match.value;
    return;
  }
  const sign = want >= 0 ? "+" : "-";
  const h = String(Math.floor(Math.abs(want) / 60)).padStart(2, "0");
  const mm = String(Math.abs(want) % 60).padStart(2, "0");
  const o = document.createElement("option");
  o.value = String(want);
  o.textContent = `UTC${sign}${h}:${mm} · Custom`;
  select.appendChild(o);
  select.value = o.value;
}

function renderState(sample) {
  if (IS_COCKPIT) {
    if (sample.firmwareVersion && el.firmwareVersion) el.firmwareVersion.textContent = sample.firmwareVersion;
    if (el.uptime) el.uptime.textContent = formatDuration(sample.uptimeMs || 0);
    if (el.deviceClock) el.deviceClock.textContent = formatClock(sample.epochMs, sample.timezoneOffsetMinutes || 0);
    if (sample.brightness != null && el.brightnessNow) el.brightnessNow.textContent = `${sample.brightness}%`;
    if (sample.tpms) {
      state.tpms = sample.tpms;
      scheduleGaugeRender();
    }
  }
  if (typeof sample.activePage === "number" && sample.activePage !== state.activePage) {
    state.activePage = sample.activePage;
    syncPageUI();
  }
  if (sample.activeThemeId && sample.activeThemeId !== state.activeThemeId) {
    state.activeThemeId = sample.activeThemeId;
    const activeTheme = state.themes.find((theme) => theme.id === state.activeThemeId);
    if (activeTheme) {
      setTheme(activeTheme);
      if (IS_COCKPIT) renderThemes();
    }
  }
  if (sample.obd) renderObd(sample.obd);
  pushSample(sample);
}

/* OBD2 BLE link readout: link state on both pages, live PIDs on the cockpit.
 * Values stay dashed until a fresh (non-stale) reading exists. */
const OBD_STATE_LABEL = ["Off", "Scanning", "Connecting", "Live", "Disconnected"];

function renderObd(obd) {
  const label = OBD_STATE_LABEL[obd.state] || "--";
  if (el.obdStateText) {
    el.obdStateText.textContent = label;
    el.obdStateText.dataset.state = String(obd.state);
  }
  if (el.obdPeer) {
    el.obdPeer.textContent = obd.state >= 3 ? (obd.peer || obd.peerAddr || "--") : "--";
  }
  if (el.obdRpm) el.obdRpm.textContent = obd.valid ? Math.round(obd.rpm || 0) : "--";
  if (el.obdSpeed) el.obdSpeed.textContent = obd.valid ? `${Math.round(obd.speedKph || 0)} km/h` : "--";
  if (el.obdCoolant) el.obdCoolant.textContent = obd.valid ? `${Math.round(obd.coolantC || 0)}°C` : "--";
  if (el.obdBattery) {
    el.obdBattery.textContent = obd.valid && obd.batteryV > 0 ? `${obd.batteryV.toFixed(1)} V` : "--";
  }
}

function renderConfig(config) {
  state.config = config;
  if (IS_COCKPIT) {
    if (el.tzOffset) setTzOffsetSelect(el.tzOffset, config.timezoneOffsetMinutes ?? 0);
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
    scheduleSparklineRender();
  }
}

function renderSavedNetworks(savedList, activeSsid) {
  if (!el.savedNetworksList) return;
  el.savedNetworksList.innerHTML = "";
  const list = Array.isArray(savedList) ? savedList : [];
  if (!list.length) {
    const p = document.createElement("p");
    p.className = "saved-net-empty";
    p.textContent = "No saved networks yet. Add one below.";
    el.savedNetworksList.appendChild(p);
    return;
  }
  for (const item of list) {
    const row = document.createElement("div");
    row.className = "saved-net-item";
    const nameWrap = document.createElement("div");
    nameWrap.className = "saved-net-name-wrap";
    const name = document.createElement("span");
    name.className = "saved-net-name";
    name.textContent = item.ssid || "";
    nameWrap.appendChild(name);
    if (item.ssid === activeSsid) {
      const tag = document.createElement("span");
      tag.className = "saved-net-badge";
      tag.textContent = "Active";
      nameWrap.appendChild(tag);
    }
    const delBtn = document.createElement("button");
    delBtn.className = "saved-net-del";
    delBtn.type = "button";
    delBtn.textContent = "Remove";
    delBtn.addEventListener("click", () => deleteSavedNetwork(item.ssid));
    row.appendChild(nameWrap);
    row.appendChild(delBtn);
    el.savedNetworksList.appendChild(row);
  }
}

async function deleteSavedNetwork(ssid) {
  if (!ssid) return;
  try {
    showOk(`Removing ${ssid}…`);
    const net = await api(`/network?ssid=${encodeURIComponent(ssid)}`, { method: "DELETE" });
    renderNetwork(net);
    showOk(`Removed ${ssid}`);
  } catch (err) {
    showError(`Failed to remove: ${err.message}`);
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
  renderSavedNetworks(net.saved, net.staSsid);
  if (el.netHint) {
    el.netHint.textContent = net.staConnected && net.staIp
      ? `Live on http://${net.staIp}/ · SoftAP ${net.apSsid || ""} still online`
      : "SoftAP stays up as fallback. Auto-connects to saved networks in range.";
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
/* Fold a /themes-shaped payload into state. `fallback` supplies defaults for
 * fields the payload omits (the cockpit refresh uses it for the hard-coded
 * big-digit/vault defaults); without a fallback an omitted field leaves the
 * previous state untouched. All servers currently echo every field, so the
 * guarded form is belt-and-braces against a partial response. */
function applyThemePayload(payload, fallback = {}) {
  const pick = (key) => (payload[key] !== undefined ? payload[key] : fallback[key]);
  const set = (key, value) => { if (value !== undefined) state[key] = value; };
  const bool = (key) => { const v = pick(key); return v === undefined ? undefined : !!v; };

  if (payload.themes !== undefined) state.themes = payload.themes;
  else if (fallback.themes !== undefined) state.themes = fallback.themes;

  set("bigDigitStaticBg", bool("bigDigitStaticBg"));
  set("bigDigitColorText", bool("bigDigitColorText"));
  set("bigDigitStaticColor", pick("bigDigitStaticColor"));
  set("bigDigitTextColor", pick("bigDigitTextColor"));
  set("arcGradient", bool("arcGradient"));
  set("hudGradient", bool("hudGradient"));
  set("hudTrueBlack", bool("hudTrueBlack"));
  set("teSync", bool("teSync"));
  set("regionDBuf", bool("regionDBuf"));
  set("teScanline", bool("teScanline"));
  set("rotation", (() => { const v = pick("rotation"); return v === undefined ? undefined : Number(v) || 0; })());
  set("neonMarqueeSpin", bool("neonMarqueeSpin"));
  set("pixelShift", bool("pixelShift"));
  set("pixelShiftSec", (() => { const v = pick("pixelShiftSec"); return v === undefined ? undefined : Number(v) || state.pixelShiftSec; })());
  set("demoMode", bool("demoMode"));
  set("demoFastSweep", bool("demoFastSweep"));
  set("tpmsBle", bool("tpmsBle"));
  set("vaultFace", pick("vaultFace"));
  set("vaultVignette", pick("vaultVignette"));
  set("vaultNeedleRed", bool("vaultNeedleRed"));
  set("vaultNeedleTail", bool("vaultNeedleTail"));
  set("neonLayout", (() => { const v = pick("neonLayout"); return v === undefined ? undefined : ([0, 1, 2].includes(Number(v)) ? Number(v) : 1); })());
  set("neonPreset", (() => { const v = pick("neonPreset"); return v === undefined ? undefined : ([0, 1, 2, 3].includes(Number(v)) ? Number(v) : 0); })());
}

function queueThemeConfig(body, okMsg) {
  clearTimeout(colorPutTimer);
  colorPutTimer = setTimeout(async () => {
    try {
      clearError(ERR_USER);
      const payload = await api("/themes/config", {
        method: "PUT",
        body: JSON.stringify(body),
      });
      applyThemePayload(payload);
      const active = state.themes.find((t) => t.id === state.activeThemeId);
      if (active) setTheme(active);
      renderThemes();
      if (okMsg) showOk(okMsg);
    } catch (error) {
      showError(error.message);
    }
  }, 250);
}

/* renderThemes() rebuilds the whole theme list with replaceChildren(), which
 * destroys and recreates every <input type="color"> in it. Destroying an input
 * whose native picker is open closes that picker - and the picker's own edits
 * are what schedule the rebuild: dragging a slider or typing a hex value fires
 * `input`, queueThemeConfig debounces 250 ms, the PUT resolves, and the
 * response handler calls renderThemes(). So the picker reliably closed a
 * fraction of a second after the user paused, which reads as "it closes while
 * I'm using it".
 *
 * Hold off the rebuild while a colour input is being edited, and run the
 * pending one when it closes. The gauge canvas still repaints live from the
 * `input` handler, so the preview stays immediate - only the list rebuild
 * waits. */
let colorEditActive = false;
let themesRenderPending = false;

function trackColorEditing(input) {
  input.addEventListener("focus", () => { colorEditActive = true; });
  const release = () => {
    if (!colorEditActive) return;
    colorEditActive = false;
    if (themesRenderPending) {
      themesRenderPending = false;
      renderThemes();
    }
  };
  /* blur covers dismissing the dialog; change covers committing it. Both are
   * wired because which one a browser fires (and in what order) varies, and
   * leaving the flag stuck would freeze the list until the next interaction. */
  input.addEventListener("blur", release);
  input.addEventListener("change", release);
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
    trackColorEditing(input);
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
    cname.textContent = "Dial glow color";
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

    const nrow = document.createElement("label");
    nrow.className = "theme-select-row";
    const nselect = document.createElement("select");
    nselect.innerHTML = `
      <option value="green">Phosphor green</option>
      <option value="red">Signal red</option>
    `;
    nselect.value = state.vaultNeedleRed ? "red" : "green";
    nselect.addEventListener("change", () => {
      state.vaultNeedleRed = nselect.value === "red";
      scheduleGaugeRender();
      queueThemeConfig({ vaultNeedleRed: state.vaultNeedleRed });
    });
    const nname = document.createElement("span");
    nname.textContent = "Needle color";
    nrow.append(nname, nselect);
    wrap.append(nrow);

    const trow = document.createElement("label");
    trow.className = "theme-toggle-row";
    const tbox = document.createElement("input");
    tbox.type = "checkbox";
    tbox.checked = !!state.vaultNeedleTail;
    tbox.addEventListener("change", () => {
      state.vaultNeedleTail = tbox.checked;
      scheduleGaugeRender();
      queueThemeConfig({ vaultNeedleTail: tbox.checked },
        tbox.checked ? "Vault needle tail enabled" : "Vault needle tail disabled");
    });
    const tname = document.createElement("span");
    tname.textContent = "Needle counterweight tail";
    trow.append(tbox, tname);
    wrap.append(trow);
  }

  if (theme.style === "neon") {
    const row = document.createElement("label");
    row.className = "theme-select-row";
    const select = document.createElement("select");
       select.innerHTML = `
       <option value="0">Neon tube</option>
       <option value="1">Neon segments</option>
       <option value="2">Neon marquee</option>
       `;
     select.value = String([0, 1, 2].includes(Number(state.neonLayout)) ? Number(state.neonLayout) : 1);
     select.addEventListener("change", () => {
       state.neonLayout = Number(select.value);
      scheduleGaugeRender();
      queueThemeConfig({ neonLayout: state.neonLayout }, `Neon ${select.options[select.selectedIndex].text.toLowerCase()}`);
    });
    const name = document.createElement("span");
    name.textContent = "Neon layout";
    row.append(name, select);
    wrap.append(row);
    const presetRow = document.createElement("label");
    presetRow.className = "theme-select-row";
    const preset = document.createElement("select");
  preset.innerHTML = `<option value="0">Violet</option><option value="1">Miami</option><option value="2">Toxic</option><option value="3">Blood Moon</option>`;
  preset.value = String([0, 1, 2, 3].includes(Number(state.neonPreset)) ? Number(state.neonPreset) : 0);
    preset.addEventListener("change", () => {
      state.neonPreset = Number(preset.value);
      queueThemeConfig({ neonPreset: state.neonPreset }, `Neon ${preset.options[preset.selectedIndex].text} preset`);
    });
    const presetName = document.createElement("span");
    presetName.textContent = "Color preset";
    presetRow.append(presetName, preset);
    wrap.append(presetRow);
    /* The ring-spin chase only exists on the marquee layout, so only offer it
     * there. Switching layout re-renders this panel (the PUT response handler
     * calls renderThemes()), which shows/hides the row. */
    if (Number(state.neonLayout) === 2) {
      addToggle("neonMarqueeSpin",
        "Ring spin effect",
        "Ring spin on", "Ring spin off");
    }
  }

  if (theme.style === "arc") {
    addToggle("arcGradient", "Gradient fill (smooth color transition)",
              "Gradient fill", "Zone colors");
  }
  if (theme.style === "hud") {
    addToggle("hudGradient", "Gradient fill (smooth color transition)",
              "Gradient fill", "Zone colors");
    addToggle("hudTrueBlack", "True black background (AMOLED pixels off)",
              "True black background", "Night City background");
  }
  if (theme.style === "bigdigit") {
    addToggle("bigDigitStaticBg", "Static background (no color sweep)",
              "Static background", "Color sweep");
    addToggle("bigDigitColorText", "Color the readout instead of the background",
              "Readout color", "White readout");

    if (!state.bigDigitColorText) {
      const row = document.createElement("label");
      row.className = "theme-color-row";
      const input = document.createElement("input");
      input.type = "color";
      input.value = state.bigDigitTextColor || "#ffffff";
      trackColorEditing(input);
      input.addEventListener("input", () => {
        state.bigDigitTextColor = input.value;
        queueThemeConfig({ bigDigitTextColor: input.value });
      });
      const name = document.createElement("span");
      name.textContent = "Readout text color";
      row.append(input, name);
      wrap.append(row);
    }

    if (state.bigDigitStaticBg) {
      const row = document.createElement("label");
      row.className = "theme-color-row";
      const input = document.createElement("input");
      input.type = "color";
      input.value = state.bigDigitStaticColor || "#000000";
      trackColorEditing(input);
      input.addEventListener("input", () => {
        state.bigDigitStaticColor = input.value;
        queueThemeConfig({ bigDigitStaticColor: input.value });
      });
      const name = document.createElement("span");
      name.textContent = "Background color (black = pixels off)";
      row.append(input, name);
      wrap.append(row);
    }
  }

  if (theme.customized) {
    const reset = document.createElement("button");
    reset.type = "button";
    reset.className = "theme-reset";
    reset.textContent = "Reset to default colors";
    reset.addEventListener("click", () =>
      queueThemeConfig({ id: theme.id, reset: true }, `${theme.name} reset`),
    );
    wrap.append(reset);
  }
  return wrap;
}

function renderThemes() {
  /* Deferred while a colour picker is open - see trackColorEditing(). */
  if (colorEditActive) { themesRenderPending = true; return; }
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
        clearError(ERR_USER);
        const payload = await api("/themes/active", {
          method: "PUT",
          body: JSON.stringify({ id: theme.id }),
        });
        state.activeThemeId = payload.activeThemeId || theme.id;
        state.themes = payload.themes || state.themes;
        setTheme(state.themes.find((t) => t.id === state.activeThemeId) || theme);
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
    edit.title = `Edit ${theme.name} colors`;
    edit.textContent = openThemeEditor === theme.id ? "Close" : "Colors";
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
  if (el.regionDBuf) el.regionDBuf.checked = !!state.regionDBuf;
  if (el.teScanline) el.teScanline.checked = !!state.teScanline;
  if (el.rotation) el.rotation.value = String(state.rotation ?? 0);
  if (el.demoMode) {
    el.demoMode.value = state.demoFastSweep ? "sweep"
      : (state.demoMode ? "demo" : "off");
  }
  if (el.tpmsBle) el.tpmsBle.checked = !!state.tpmsBle;
  if (el.tpmsLowPsi && state.tpmsConfig?.lowPsi != null) {
    el.tpmsLowPsi.value = Number(state.tpmsConfig.lowPsi).toFixed(1);
  }
  if (el.tpmsStaleSec && state.tpmsConfig?.staleAfterMs != null) {
    el.tpmsStaleSec.value = String(Math.round(state.tpmsConfig.staleAfterMs / 1000));
  }
}

function wireDisplayToggles() {
  const send = async (body, label) => {
    try {
      clearError(ERR_USER);
      const payload = await api("/themes/config", {
        method: "PUT",
        body: JSON.stringify(body),
      });
      applyThemePayload(payload);
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
  if (el.regionDBuf) {
    el.regionDBuf.addEventListener("change", () =>
      send({ regionDBuf: el.regionDBuf.checked },
           el.regionDBuf.checked ? "Region double-buffer on" : "Region double-buffer off"),
    );
  }
  if (el.teScanline) {
    el.teScanline.addEventListener("change", () =>
      send({ teScanline: el.teScanline.checked },
           el.teScanline.checked ? "TE scanline writeback on" : "TE scanline writeback off"),
    );
  }
  if (el.rotation) {
    /* The LVGL adapter takes rotation when the display is registered, so the
     * panel keeps its current orientation until the device restarts. Say so
     * rather than letting it look like the setting did not stick. */
    el.rotation.addEventListener("change", () =>
      send({ rotation: Number(el.rotation.value) },
           `Rotation ${el.rotation.value} deg - restart to apply`),
    );
  }
  if (el.demoMode) {
    el.demoMode.addEventListener("change", () => {
      const v = el.demoMode.value;
      if (v === "off") {
        send({ demoMode: false, demoFastSweep: false },
             "Demo off (live sensors)");
      } else if (v === "sweep") {
        send({ demoMode: true, demoFastSweep: true },
             "Demo on + linear sweep");
      } else {
        send({ demoMode: true, demoFastSweep: false },
             "Demo mode on");
      }
    });
  }
  if (el.tpmsBle) {
    el.tpmsBle.addEventListener("change", () =>
      send({ tpmsBle: el.tpmsBle.checked },
           el.tpmsBle.checked ? "OBD2 BLE link on" : "OBD2 BLE link off"),
    );
  }
  const saveTpmsField = async (body, label) => {
    try {
      clearError(ERR_USER);
      const payload = await api("/tpms/config", {
        method: "PUT",
        body: JSON.stringify(body),
      });
      state.tpmsConfig = payload;
      syncDisplayToggles();
      showOk(label);
    } catch (error) {
      showError(error.message, ERR_USER);
    }
  };
  if (el.tpmsLowPsi) {
    el.tpmsLowPsi.addEventListener("change", () => {
      const v = Number(el.tpmsLowPsi.value);
      if (!Number.isFinite(v) || v < 15 || v > 55) {
        showError("TPMS alert must be between 15 and 55 PSI", ERR_USER);
        return;
      }
      saveTpmsField({ lowPsi: v }, `TPMS alert ${v.toFixed(1)} PSI`);
    });
  }
  if (el.tpmsStaleSec) {
    el.tpmsStaleSec.addEventListener("change", () => {
      const sec = Number(el.tpmsStaleSec.value);
      if (!Number.isFinite(sec) || sec < 2 || sec > 120) {
        showError("TPMS stale must be between 2 and 120 seconds", ERR_USER);
        return;
      }
      saveTpmsField({ staleAfterMs: Math.round(sec * 1000) }, `TPMS stale ${sec}s`);
    });
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
 * #errorBox / #okBox. Originally that was a workaround: showError("") ran on
 * every successful /state poll, so a result posted to the shared box was gone
 * in 250 ms. The shared box no longer does that (see showError/clearError and
 * ERR_USER/ERR_LIVE), and this line stays for the reason it is worth keeping
 * on its own - a two-second measurement wants its verdict beside the readouts
 * it was taken from, not up in the page-level banner. Do not grow more of
 * these: the shared box is the place for everything else. */
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

/* `source` is ERR_USER for the Refresh button and ERR_LIVE for the unattended
 * boot bootstrap. A boot-time failure is a transport failure nobody asked
 * about, so it has to be retractable by the poll loop that follows it;
 * marking it ERR_USER would leave a stale "Failed to fetch" on screen after
 * the device came back. */
async function refreshAll(source = ERR_USER) {
  try {
    clearError(source);
    const requests = [api("/state"), api("/config"), api("/themes"), api("/network")];
    if (IS_COCKPIT) requests.push(api("/media/status"), api("/logs?limit=300"));
    else requests.push(api("/tpms/config").catch(() => null));
    const [statePayload, config, themes, network, extra, logs] = await Promise.all(requests);
    if (!IS_COCKPIT && extra) state.tpmsConfig = extra;
    const media = IS_COCKPIT ? extra : null;
    applyThemePayload(themes, {
      themes: [],
      bigDigitStaticColor: "#000000",
      bigDigitTextColor: "#ffffff",
      vaultFace: "#05281a",
      vaultVignette: 60,
      pixelShiftSec: state.pixelShiftSec,
    });
    syncDisplayToggles();
    state.activeThemeId = themes.activeThemeId || statePayload.activeThemeId || state.activeThemeId;
    if (IS_COCKPIT) renderThemes();
    setTheme(state.themes.find((theme) => theme.id === state.activeThemeId));
    renderConfig(config);
    if (IS_COCKPIT) {
      /* Keep the device wall clock calibrated against the browser. The timezone
       * comes from the dropdown (the persisted config), never the browser's own
       * live offset - a zone is set once and stays until the user changes it. */
      try {
        await syncDeviceClock();
      } catch (_) {
        /* non-fatal; schedule may wait until manual Sync */
      }
    }
    renderState(statePayload);
    if (IS_COCKPIT) seedHistoryFromLog(logs?.samples);
    if (IS_COCKPIT) renderMediaStatus(media);
    renderNetwork(network);
    /* A successful refresh must not relabel an already-open WebSocket as HTTP. */
    if (!state.liveSocket || state.liveSocket.readyState !== WebSocket.OPEN) updateConnection("online", "http");
  } catch (error) {
    if (!state.liveSocket || state.liveSocket.readyState !== WebSocket.OPEN) updateConnection("offline");
    showError(error.message, source);
  }
}

async function pollState() {
  if (state.pollInFlight) return;
  state.pollInFlight = true;
  try {
    const sample = await api("/state");
    renderState(sample);
    updateConnection("online", "http");
    /* ERR_LIVE, not a blanket clear: this runs 4 Hz and must not retract an
     * error some gesture put on screen a moment ago. It does still retract the
     * transient one raised below, which is what "recovered" means here. */
    clearError(ERR_LIVE);
  } catch (error) {
    updateConnection("offline");
    showError(error.message, ERR_LIVE);
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
    clearError(ERR_LIVE);
    void resyncHistory();
  };
  socket.onmessage = (event) => {
    if (state.liveSocket !== socket) return;
    try {
      const sample = JSON.parse(event.data);
      renderState(sample);
      updateConnection("online", "websocket");
      clearError(ERR_LIVE);
    } catch (error) {
      showError(`Live telemetry error: ${error.message}`, ERR_LIVE);
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

/* Push the browser clock to the device. The offset comes from tzOverride when
 * given, otherwise from the Time zone dropdown when populated, else the browser
 * offset. Updates the dropdown with the device's echoed offset on success. */
async function syncDeviceClock(tzOverride) {
  const now = new Date();
  const tz = Number.isFinite(Number(tzOverride))
    ? Number(tzOverride)
    : Number.isFinite(Number(el.tzOffset?.value))
      ? Number(el.tzOffset.value)
      : -now.getTimezoneOffset();
  const response = await api("/time", {
    method: "POST",
    body: JSON.stringify({ epochMs: now.getTime(), timezoneOffsetMinutes: tz }),
  });
  if (el.tzOffset) setTzOffsetSelect(el.tzOffset, response.timezoneOffsetMinutes);
  return response;
}

async function syncTime() {
  try {
    const response = await syncDeviceClock();
    renderState(response);
    showOk("Time synchronized");
  } catch (error) {
    showError(error.message === "clock_rejected"
      ? "Clock rejected: this computer's time disagrees with the device RTC by more than 5 min. If this computer's clock is right, pull the RTC battery for 2 s and Sync again."
      : error.message);
  }
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
  try { await syncDeviceClock(); } catch (_) {}
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
    return;
  }
  const kb = Math.round((media.sizeBytes ?? media.size ?? 0) / 1024);
  el.mediaStatus.textContent = `${media.name || "active.gif"} · ${kb} KB · ${media.playbackSupported ? "playing on AMOLED" : "ready"}`;
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
  /* User-sourced errors are deliberately sticky, so there has to be a way to
   * put one away that is not "perform another action". */
  on(el.errorBox, "click", () => clearError(ERR_USER));
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
    scheduleSparklineRender();
  });
}

updatePalette();
state.config = {
  psiMin: DEFAULT_PSI_MIN,
  psiMax: DEFAULT_PSI_MAX,
  psiOverboost: DEFAULT_PSI_OVERBOOST,
};
state.gaugeTarget = { psi: 0, peakPsi: 0, zone: "ATMO", demo: true };
wirePageControls();
wireControls();
wireDisplayToggles();
wireCalibration();
document.addEventListener("visibilitychange", () => {
  syncSensorPolling();
  if (!document.hidden) {
    scheduleGaugeRender();
    scheduleSparklineRender();
  }
});
syncSensorPolling();
if (IS_COCKPIT) {
  scheduleGaugeRender();
  scheduleSparklineRender();
}
populateTzSelect();
refreshAll(ERR_LIVE).finally(connectEvents);
