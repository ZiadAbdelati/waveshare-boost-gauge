/* Headless parity check for the neon web mirror.
 *
 * Extracts drawNeonGauge(), neonLitColor() and psiToSweep() out of web/app.js
 * and runs them against a canvas shim that records every draw call, then
 * asserts the recorded geometry against the firmware's own numbers. It is not a
 * pixel comparison - it checks the decisions the mirror makes: which segments
 * light, where zero lands, how many bulbs of each kind, and where the label
 * stack sits.
 *
 * Run: node tools/test_neon_web_parity.js
 */
const fs = require("fs");
const path = require("path");
const assert = require("assert");

const src = fs.readFileSync(path.join(__dirname, "..", "web", "app.js"), "utf8");
const css = fs.readFileSync(path.join(__dirname, "..", "web", "styles.css"), "utf8");
assert.ok(/@font-face\{[^}]*font-family:"SF Alien Encounters"[^}]*src:url\(data:font\//.test(css),
  "SF Alien Encounters must be inlined via @font-face");

function extract(name) {
  const start = src.indexOf(`function ${name}(`);
  assert.ok(start >= 0, `${name} not found in web/app.js`);
  let i = src.indexOf("{", start), depth = 0;
  for (let j = i; j < src.length; j++) {
    if (src[j] === "{") depth++;
    else if (src[j] === "}" && --depth === 0) return src.slice(start, j + 1);
  }
  throw new Error(`unbalanced braces in ${name}`);
}

const ARC_START = 135, ARC_RANGE = 270, DEG = Math.PI / 180;
/* Mirrors the app.js constant: ink runs 0.70 em above the baseline. */
const NEON_INK_EM = 0.70;
const clamp = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);
const degToRad = (d) => (d * Math.PI) / 180;
const hexToRgb = (h) => {
  const n = parseInt(h.replace("#", ""), 16);
  return [(n >> 16) & 255, (n >> 8) & 255, n & 255];
};

const calls = [];
const ctx = new Proxy({}, {
  get(_, k) {
    if (k === "canvas") return { width: 466, height: 466 };
    return (...a) => calls.push({ op: String(k), args: a, style: ctx._fill, stroke: ctx._stroke, lw: ctx._lw });
  },
  set(_, k, v) {
    if (k === "fillStyle") ctx._fill = v;
    if (k === "strokeStyle") ctx._stroke = v;
    if (k === "lineWidth") ctx._lw = v;
    calls.push({ op: "set:" + String(k), args: [v] });
    return true;
  },
});
function roundRectPath(x, y, w, h, r) { calls.push({ op: "roundRect", args: [x, y, w, h, r] }); }

const state = {
  neonLayout: 1,
  palette: { track: "#241038", muted: "#5A3A7A", vacuum: "#7B00FF", boost: "#FF2BD6", overboost: "#FF6A00" },
};
function psiRange() { return { psiMin: -15, psiMax: 10, psiOverboost: 8, zeroAngle: 236.25 }; }

/* Pull the bloom constants out of app.js rather than restating them, for the
 * same reason the firmware constants are read from boost_gauge.c below: a copy
 * here can agree with itself while the mirror drifts. */
function extractConstLine(name) {
  const m = new RegExp("^const .*\\b" + name + "\\b\\s*=.*$", "m").exec(src);
  assert.ok(m, `const ${name} not found in web/app.js`);
  return m[0];
}

const scope = { ARC_START, ARC_RANGE, DEG, NEON_INK_EM, clamp, degToRad, hexToRgb, ctx, roundRectPath, state, psiRange, calls };
const body = [
  extractConstLine("NEON_BLOOM"),
  extractConstLine("NEON_HALO_DIM"),
  extract("neonDim"),
  extract("psiToSweep"), extract("neonLitColor"), extract("drawNeonGauge"),
].join("\n");
const run = new Function(...Object.keys(scope), body + "\nreturn { drawNeonGauge, psiToSweep, neonLitColor };");
const api = run(...Object.values(scope));

function render(layout, psi, peak = 0) {
  calls.length = 0;
  state.neonLayout = layout;
  api.drawNeonGauge({ peakPsi: peak }, psi, { cx: 233, cy: 233, scale: 1 });
  return calls;
}

/* ctx.font and ctx.textBaseline are ordinary property sets in this shim, not
 * attributes of the fillText call itself - this replays the call log and
 * stamps each call with whichever font/baseline was active at that point, so
 * assertions can check what a given fillText actually drew with. */
function withTextState(callsArr) {
  let font = null, baseline = null;
  return callsArr.map((k) => {
    if (k.op === "set:font") font = String(k.args[0]);
    if (k.op === "set:textBaseline") baseline = k.args[0];
    return { ...k, font, baseline };
  });
}

{
  const c = render(1, 1);
  const fontSets = c.filter((k) => k.op === "set:font").map((k) => String(k.args[0]));
  assert.ok(fontSets.some((font) => font.includes('"SF Alien Encounters"')),
    "readout must request SF Alien Encounters");
}

/* --- the ring lights whole segments, indexed by floor at BOTH ends --------- */
const step = ARC_RANGE / 54;
const zeroAng = api.psiToSweep(0, ARC_START, ARC_START + ARC_RANGE);
assert.notStrictEqual(zeroAng, ARC_START + ARC_RANGE / 2, "zero must not be at sweep midpoint");
assert.ok((zeroAng - ARC_START) < (ARC_RANGE / 2), "vacuum share must differ from boost share");
assert.ok((zeroAng - ARC_START) / ARC_RANGE < 0.5 &&
  (ARC_START + ARC_RANGE - zeroAng) > (zeroAng - ARC_START), "boost side must be larger");
const zeroSeg = Math.floor((zeroAng - ARC_START) / step);
assert.strictEqual(zeroSeg, 20, `zero segment ${zeroSeg}`);

for (const psi of [-12, -3, 2.5, 8.5]) {
  const c = render(1, psi);
  const valAng = api.psiToSweep(psi, ARC_START, ARC_START + ARC_RANGE);
  const lo = Math.min(zeroAng, valAng), hi = Math.max(zeroAng, valAng);
  const first = Math.floor((lo - ARC_START) / step);
  const last = Math.floor((hi - ARC_START) / step);
  const drawn = new Set();
  for (const k of c) {
    if (k.op !== "arc") continue;
    /* Only the lit segments, which span step-2 degrees. The unlit track is one
     * full-sweep arc and the bulbs are tiny circles; both would otherwise be
     * mistaken for segment 0. */
    const span = (k.args[4] - k.args[3]) / DEG;
    if (Math.abs(span - (step - 2)) > 0.01) continue;
    const i = Math.round((k.args[3] / DEG - 1 - ARC_START) / step);
    if (i >= 0 && i < 54) drawn.add(i);
  }
  for (let i = first; i <= last; i++) {
    assert.ok(drawn.has(i), `psi ${psi}: segment ${i} in span ${first}..${last} not drawn`);
  }
  assert.ok(drawn.has(zeroSeg), `psi ${psi}: zero marker missing`);
  for (const i of drawn) {
    assert.ok(i === zeroSeg || (i >= first && i <= last),
      `psi ${psi}: segment ${i} drawn outside span ${first}..${last}`);
  }
}

/* --- the zero marker is drawn even with nothing lit ----------------------- */
{
  const c = render(1, 0);
  const white = c.filter((k) => k.op === "set:strokeStyle" && k.args[0] === "#ffffff");
  assert.ok(white.length >= 1, "zero marker missing at psi 0");
}

/* --- marquee: bulbs, zero tick, bar mapping ------------------------------- */
{
  const c = render(2, -12);
  const bulbs = c.filter((k) => k.op === "arc" && k.args[2] === 4);
  assert.strictEqual(bulbs.length, 72, `expected 72 bulbs, got ${bulbs.length}`);

  const fills = [];
  let cur = null;
  for (const k of c) {
    if (k.op === "set:fillStyle") cur = k.args[0];
    if (k.op === "arc" && k.args[2] === 4) fills.push(cur);
  }
  const accents = fills.filter((f) => f !== state.palette.track).length;
  assert.strictEqual(accents, 24, `expected 24 accent bulbs, got ${accents}`);

  const rects = c.filter((k) => k.op === "roundRect");
  const track = rects.find((k) => k.args[2] === 300);
  assert.ok(track, "bar track missing");
   const tick = rects.find((k) => k.args[2] === 7 && k.args[3] === 27);
  assert.ok(tick, "zero tick missing");
  /* Zero must land at the same fraction along the bar as it does around the
   * arc - this is the bug a plain psiMin..psiMax lerp reintroduces. */
  const tickCentre = tick.args[0] + 2 + 150;
  const arcFrac = (zeroAng - ARC_START) / ARC_RANGE;
  assert.ok(Math.abs(tickCentre / 300 - arcFrac) < 0.01,
    `zero tick at ${(tickCentre / 300).toFixed(4)} of the bar but ${arcFrac.toFixed(4)} of the arc`);

  /* The bar fill itself must still be drawn (firmware: draw_neon_live()
   * marquee branch, the accent-coloured lv_draw_rect over [x_lo, x_hi]). */
  const fill = rects.find((k) => k.args[3] === 16 && k.args[2] !== 300);
  assert.ok(fill, "bar fill missing");

  /* No glare: commit 0085465 removed the white highlight strip the firmware
   * used to draw over the bar fill. The mirror must not draw ANY white fill
   * over the bar other than the zero tick itself (NEON_BAR_TICK_W x
   * NEON_BAR_TICK_H = 7x27 - read live above), which is the only white
   * roundRect draw_neon_live()'s marquee branch still emits. */
  const whiteRects = [];
  let curFill = null;
  for (const k of c) {
    if (k.op === "set:fillStyle") curFill = k.args[0];
    if (k.op === "roundRect" && curFill === "#ffffff") whiteRects.push(k);
  }
  const nonTickWhite = whiteRects.filter((k) => !(k.args[2] === 7 && k.args[3] === 27));
  assert.strictEqual(nonTickWhite.length, 0,
    `expected no white fill over the bar besides the zero tick, found: ${JSON.stringify(nonTickWhite.map((k) => k.args))}`);
}

/* --- the label stack keeps one rhythm, marquee only shifts it ------------- */
for (const [layout, dy] of [[0, 0], [1, 0], [2, 20]]) {
  const c = render(layout, 1.0);
  const texts = c.filter((k) => k.op === "fillText");
  const psiRow = texts.find((k) => k.args[0] === "P S I");
  const peakRow = texts.find((k) => String(k.args[0]).startsWith("PEAK"));
  const rule = c.filter((k) => k.op === "moveTo" && k.args[0] === -62);
  /* Firmware draws "P S I" in a 200x30 box top-anchored at NEON_UNIT_Y - 15;
   * with base_line 0 and a 17 px line height (24 px * 0.70 em, rounded) its
   * ink runs 81..98 at dy=0 - ink BOTTOM at NEON_UNIT_Y + 2, not at
   * NEON_UNIT_Y itself. See the ink-clearance block below for the derivation. */
  assert.strictEqual(psiRow.args[2], 138 + dy, `layout ${layout} unit mark y`);
  assert.strictEqual(peakRow.args[2], 166 + dy, `layout ${layout} peak y`);
  assert.strictEqual(rule[0].args[1], 146 + dy, `layout ${layout} rule y`);
}

/* --- every SF Alien Encounters request must match the weight styles.css
   actually declares - a mismatched weight (or a missing one paired with a
   non-400 face) makes the browser silently shape the fallback font, which
   never gets corrected because canvas does not watch for webfont swaps. ---- */
{
  const faceMatch = /@font-face\{font-family:"SF Alien Encounters";font-style:italic;font-weight:(\d+)/.exec(css);
  assert.ok(faceMatch, "SF Alien Encounters @font-face weight not found in styles.css");
  const declaredWeight = faceMatch[1];
  const c = withTextState(render(1, -12));
  const requests = c.filter((k) => k.op === "set:font" && String(k.args[0]).includes('"SF Alien Encounters"'));
  assert.ok(requests.length >= 2, `expected at least 2 SF Alien Encounters font requests, got ${requests.length}`);
  for (const r of requests) {
    const spec = String(r.args[0]);
    const m = /(?:^|\s)(\d{3})(?=\s+\d+px\s+"SF Alien Encounters")/.exec(spec);
    if (m) {
      assert.strictEqual(m[1], declaredWeight,
        `font request "${spec}" asks for weight ${m[1]} but styles.css only declares weight ${declaredWeight} - the browser silently falls back to a system font`);
    } else {
      /* No weight token in the shorthand means the browser asks for "normal"
       * (400), which is only safe if the declared face IS weight 400. */
      assert.strictEqual(declaredWeight, "400",
        `font request "${spec}" has no weight token, which only matches a 400 @font-face declaration (styles.css declares ${declaredWeight})`);
    }
  }
}

/* --- the readout's ink TOP lands at NEON_READOUT_TOP / NEON_MARQUEE_READOUT_TOP,
   matching lv_draw_label()'s box-top anchoring. Canvas's "top" baseline anchors
   to the font's ascent metric instead, which sits above the ink for this face
   and is what made the readout draw low. -------------------------------------- */
{
  const READOUT_INK_EM = 0.70; /* SF Alien Encounters: glyphs run baseline..0.70em, base_line 0 */
  for (const [layout, expectedTop, fontPx] of [[0, -42, 118], [1, -42, 118], [2, -71, 130]]) {
    const c = withTextState(render(layout, -12));
    const digitCalls = c.filter((k) => k.op === "fillText" && k.font &&
      k.font.includes('"SF Alien Encounters"') && k.font.includes(`${fontPx}px`));
    assert.ok(digitCalls.length > 0, `layout ${layout}: no readout glyph draws found at ${fontPx}px`);
    for (const call of digitCalls) {
      assert.strictEqual(call.baseline, "alphabetic",
        `layout ${layout}: readout glyph "${call.args[0]}" drawn with baseline "${call.baseline}", must be "alphabetic" so ink (not font-ascent) top lands at readoutTop`);
      const inkTop = call.args[2] - fontPx * READOUT_INK_EM;
      assert.ok(Math.abs(inkTop - expectedTop) < 0.01,
        `layout ${layout}: readout glyph "${call.args[0]}" ink top ${inkTop.toFixed(2)} != expected ${expectedTop}`);
    }
  }
}

/* --- "P S I" ink must clear the separator rule below it, on all three
   layouts - this is the reported collision. ----------------------------------- */
{
  for (const [layout, dy] of [[0, 0], [1, 0], [2, 20]]) {
    const c = withTextState(render(layout, 1.0));
    const psiCall = c.find((k) => k.op === "fillText" && k.args[0] === "P S I");
    assert.ok(psiCall, `layout ${layout}: "P S I" not drawn`);
    assert.strictEqual(psiCall.baseline, "alphabetic",
      `layout ${layout}: "P S I" drawn with baseline "${psiCall.baseline}" - "middle" put its ink close enough to the rule for antialiasing to touch it`);
    /* base_line = 0 in the firmware's neon_label font means glyphs sit
     * exactly ON the baseline, so with an alphabetic baseline the y argument
     * IS the ink bottom. */
    const inkBottom = psiCall.args[2];
    const rule = c.find((k) => k.op === "moveTo" && k.args[0] === -62);
    assert.ok(rule, `layout ${layout}: rule not drawn`);
    const ruleY = rule.args[1];
    assert.ok(ruleY - inkBottom >= 4,
      `layout ${layout}: "P S I" ink bottom ${inkBottom} is only ${(ruleY - inkBottom).toFixed(2)}px above the rule at ${ruleY}`);
    assert.strictEqual(inkBottom, 138 + dy, `layout ${layout}: "P S I" ink bottom ${inkBottom} != expected ${138 + dy}`);
  }
}

/* --- peak never displays negative ---------------------------------------- */
{
  const c = render(1, -5, -3);
  const peak = c.find((k) => k.op === "fillText" && String(k.args[0]).startsWith("PEAK"));
  assert.strictEqual(peak.args[0], "PEAK 0.0", `peak clamp: ${peak.args[0]}`);
}

/* --- the bloom matches neon_lit() exactly -------------------------------- */
{
  /* Saturate 1.30 about luma, gain 1.92, then handle overflow by normalising
   * to peak 255 and lifting NEON_WHITE_LIFT of the clipped gain back as white.
   *
   * This reference used to clamp PER CHANNEL, which is what the mirror did -
   * so the two agreed, and the test stayed green while every saturated palette
   * entry pinned to the same corner of the colour cube and the zones converged.
   * Encoding the reference from the firmware's constants instead means the
   * test tracks neon_lit() rather than whatever app.js happens to do. */
  const cSrc = fs.readFileSync(path.join(__dirname, "..", "main", "boost_gauge.c"), "utf8");
  const fdef = (name) => {
    const m = new RegExp("^#define[ \\t]+" + name + "[ \\t]+([0-9.]+)f", "m").exec(cSrc);
    assert.ok(m, `#define ${name} not found in main/boost_gauge.c`);
    return Number(m[1]);
  };
  const BLOOM = fdef("NEON_BLOOM"), SAT = fdef("NEON_SAT"), LIFT = fdef("NEON_WHITE_LIFT");
  const ref = (hex, k = 1) => {
    const [r0, g0, b0] = hexToRgb(hex);
    const luma = 0.299 * r0 + 0.587 * g0 + 0.114 * b0;
    const gain = BLOOM * k;
    let v = [r0, g0, b0].map((c) => Math.max(0, (luma + (c - luma) * SAT) * gain));
    const peak = Math.max(...v);
    if (peak > 255) {
      const s = 255 / peak;
      v = v.map((c) => c * s);
      const w = (1 - s) * LIFT;
      v = v.map((c) => c + (255 - c) * w);
    }
    return `rgb(${v.map((c) => Math.round(c)).join(", ")})`;
  };
  for (const c of ["#7B00FF", "#FF2BD6", "#FF6A00", "#39FF14"]) {
    assert.strictEqual(api.neonLitColor(c), ref(c), `bloom mismatch for ${c}`);
    assert.strictEqual(api.neonLitColor(c, 0.55), ref(c, 0.55), `dim bloom mismatch for ${c}`);
  }

  /* The property the bloom exists to provide, asserted directly rather than
   * left implicit in the numbers above: the ring's bands must read dark ->
   * bright -> white from the inside out. Under the bare proportional clamp the
   * bloomed body came out DARKER than the raw palette behind it in 11 of the 12
   * palette/zone combinations, so the gradient ran backwards and the ring
   * collapsed to one flat colour on the panel. */
  const DIM = fdef("NEON_HALO_DIM");
  const lum = (s) => { const [r, g, b] = s.match(/\d+/g).map(Number); return 0.299 * r + 0.587 * g + 0.114 * b; };
  for (const hex of ["#7B00FF", "#FF2BD6", "#FF6A00", "#00E5FF", "#39FF14",
                     "#FFF000", "#FF00A0", "#0022FF", "#7A1220", "#FFC400"]) {
    const halo = lum(neonDimRef(hex, DIM));
    const bodyL = lum(api.neonLitColor(hex));
    assert.ok(bodyL > halo,
      `band order inverted for ${hex}: body luma ${bodyL.toFixed(1)} <= dimmed halo ${halo.toFixed(1)}`);
  }
  function neonDimRef(hex, k) {
    const [r, g, b] = hexToRgb(hex);
    return `rgb(${Math.round(r * k)}, ${Math.round(g * k)}, ${Math.round(b * k)})`;
  }
}

console.log("neon web parity: all assertions passed");

/* --- the three bands stack inward from ONE outer edge --------------------- */
/* LVGL's arc radius is the stroke's outer edge; canvas centres the stroke on
 * the path. Getting this wrong put the white cap in the middle of the band and
 * the segment read as a symmetrical stripe. */
{
  const c = render(1, -12);
  const bands = [];
  let stroke = null, lw = null;
  for (const k of c) {
    if (k.op === "set:strokeStyle") stroke = k.args[0];
    if (k.op === "set:lineWidth") lw = k.args[0];
    if (k.op === "arc" && lw > 4) bands.push({ r: k.args[2], w: lw, stroke });
  }
  assert.ok(bands.length >= 3, "expected stacked ring bands");
  for (const b of bands) {
     assert.ok(Math.abs((b.r + b.w / 2) - 228) < 0.001,
       `band w=${b.w} centred at r=${b.r} has outer edge ${b.r + b.w / 2}, expected 228`);
  }
  const widths = bands.map((b) => b.w);
  assert.ok(widths.includes(6), "white cap band missing");
  const cap = bands.find((b) => b.w === 6);
  const body = bands.find((b) => b.w === 30);
  const inner = bands.find((b) => b.w === 42);
  assert.ok(cap && body && inner, "expected 6/30/42 px bands");
  assert.ok(cap.r > body.r && body.r > inner.r,
    "bands must stack inward: cap outermost, then body, then the raw palette");
}

/* --- the sign shears the same way the italic digits lean ------------------ */
{
  const c = render(1, -12);
  /* Both bars are emitted as SUBPATHS of a single path, then filled once, so
   * that the glow shadow is cast by the pair rather than by each bar
   * separately - a per-bar shadow falls across the other bar and fills the
   * 2 px gap between them, which is the exact failure the panel's own first
   * sign glow had. So this looks for one fill carrying 8 points, not two
   * fills carrying 4 each. */
  let tris = [];
  let started = false, pts = [];
  for (const k of c) {
    if (k.op === "beginPath") { started = true; pts = []; continue; }
    if (!started) continue;
    if (k.op === "moveTo" || k.op === "lineTo") pts.push(k.args.slice());
    if (k.op === "fill" && pts.length === 8) {
      tris = [pts.slice(0, 4), pts.slice(4, 8)];
      started = false;
    }
  }
  assert.strictEqual(tris.length, 2, `expected 2 sign bars, got ${tris.length}`);
  /* Bars are emitted top first. The upper bar must sit RIGHT of the lower one,
   * matching a right-leaning italic. */
  const upperLeft = tris[0][3][0];
  const lowerLeft = tris[1][3][0];
  assert.ok(upperLeft > lowerLeft,
    `upper bar left edge ${upperLeft} must be right of lower ${lowerLeft}`);
  /* And within each bar the top edge leans right of its own bottom edge. */
  for (const t of tris) {
    assert.ok(t[0][0] > t[3][0], "bar top edge must lean right of its bottom edge");
  }
}

console.log("neon web parity: band stacking and sign shear verified");

/* --- tube: the zero marker matches the firmware's widened band (e400195) -- */
/* Firmware widened the tube zero marker to span the full band width -
 * NEON_TUBE_W (26) + NEON_HALO_EXTRA (12) = 38 - instead of the old bare
 * NEON_TUBE_W. Its outer edge must land on the same ring radius (NEON_R,
 * 228) as every other band, using the identical ringR - width/2
 * compensation the rest of the ring uses.
 *
 * It also spans 6 degrees now, not 4: the panel's lit run starts exactly at
 * zero and sweeps outward over this same band, so half the marker used to be
 * buried and the surviving half swapped sides at the zero crossing
 * (NEON_TUBE_ZERO_DEG). */
{
  const c = render(0, -12);
  let stroke = null, lw = null;
  const bands = [];
  for (const k of c) {
    if (k.op === "set:strokeStyle") stroke = k.args[0];
    if (k.op === "set:lineWidth") lw = k.args[0];
    if (k.op === "arc" && lw > 4) bands.push({ r: k.args[2], w: lw, stroke, a0: k.args[3], a1: k.args[4] });
  }
  /* The zero marker is the only white band spanning exactly 6 degrees
   * (zero - 3 .. zero + 3). The lit run's own outer (accent-coloured) band
   * shares the same 38 px width, so width alone cannot identify it. */
  const zeroMarker = bands.find((b) => b.stroke === "#ffffff" &&
    Math.abs((b.a1 - b.a0) / DEG - 6) < 0.01);
  assert.ok(zeroMarker, "tube zero marker band not found");
  assert.strictEqual(zeroMarker.w, 38,
    `tube zero marker width ${zeroMarker.w}, expected NEON_TUBE_W(26) + NEON_HALO_EXTRA(12) = 38`);
  assert.ok(Math.abs((zeroMarker.r + zeroMarker.w / 2) - 228) < 0.001,
    `tube zero marker outer edge ${zeroMarker.r + zeroMarker.w / 2}, expected ring radius 228`);
}

console.log("neon web parity: tube zero marker band verified");

/* --- the mirror's numbers ARE the firmware's numbers ---------------------- */
/* Every assertion above this point hard-codes the value it expects, so the
 * mirror and the panel could drift apart as long as this file was edited to
 * agree with web/app.js. They did drift, badly: the readout cells sat at 64/33
 * here against the panel's 88/34, the ring at 225 against 228, the fonts at
 * 108/154 against 118/130, and the label stack 40 px high. The mirror was
 * showing a materially tighter, smaller face than the glass ever did, and the
 * difference was read as a problem with the panel.
 *
 * So read the constants straight out of boost_gauge.c and check the mirror
 * against THOSE. This cannot drift silently: changing a #define without
 * updating web/app.js now fails here. */
{
  const cSrc = fs.readFileSync(path.join(__dirname, "..", "main", "boost_gauge.c"), "utf8");
  const def = (name) => {
    const m = new RegExp("^#define[ \\t]+" + name + "[ \\t]+\\(?(-?[0-9.]+)f?\\)?[ \\t]*(?:/\\*|$)", "m").exec(cSrc);
    assert.ok(m, `#define ${name} not found in main/boost_gauge.c`);
    return Number(m[1]);
  };
  const F = {
    R: def("NEON_R"), slot: def("NEON_SLOT_W"), dot: def("NEON_DOT_W"),
    mSlot: def("NEON_MARQUEE_SLOT_W"), mDot: def("NEON_MARQUEE_DOT_W"),
    font: def("NEON_FONT_PX"), mFont: def("NEON_MARQUEE_FONT_PX"),
    unitY: def("NEON_UNIT_Y"), ruleY: def("NEON_RULE_Y"), peakY: def("NEON_PEAK_Y"),
    stackDy: def("NEON_MARQUEE_STACK_DY"), bulbR: def("NEON_BULB_R"),
    zeroDeg: def("NEON_TUBE_ZERO_DEG"),
  };

  for (const [layout, dy, slot, dotw, fontPx] of [
    [0, 0, F.slot, F.dot, F.font],
    [1, 0, F.slot, F.dot, F.font],
    [2, F.stackDy, F.mSlot, F.mDot, F.mFont],
  ]) {
    const c = render(layout, 8.5);
    const fonts = c.filter((k) => k.op === "set:font").map((k) => k.args[0]);
    assert.ok(fonts.some((f) => f.includes(`${fontPx}px`)),
      `layout ${layout}: mirror never set a ${fontPx}px font (firmware NEON${layout === 2 ? "_MARQUEE" : ""}_FONT_PX); saw ${fonts.join(" | ")}`);

    /* "8.5" is digit, dot, digit - so the two fillText x positions either side
     * of the decimal are exactly one (slot + dot)/2 pitch apart. */
    const texts = c.filter((k) => k.op === "fillText" && /^[0-9.]$/.test(String(k.args[0])));
    assert.strictEqual(texts.length, 3, `layout ${layout}: expected 3 readout cells, got ${texts.length}`);
    const pitch = texts[2].args[1] - texts[0].args[1];
    assert.strictEqual(pitch, slot + dotw,
      `layout ${layout}: readout ones->tenths pitch ${pitch}, firmware SLOT_W+DOT_W = ${slot + dotw}`);

    const rows = c.filter((k) => k.op === "fillText");
    const psiRow = rows.find((k) => k.args[0] === "P S I");
    const peakRow = rows.find((k) => String(k.args[0]).startsWith("PEAK"));
    assert.strictEqual(psiRow.args[2], F.unitY + 2 + dy,
      `layout ${layout}: unit mark y vs NEON_UNIT_Y`);
    assert.strictEqual(peakRow.args[2], F.peakY + dy,
      `layout ${layout}: peak y vs NEON_PEAK_Y`);
    const rule = c.filter((k) => k.op === "moveTo");
    assert.ok(rule.some((k) => k.args[1] === F.ruleY + dy),
      `layout ${layout}: rule y vs NEON_RULE_Y (${F.ruleY + dy})`);
  }

  /* Ring radius: every band's outer edge is ringR, and the mirror compensates
   * canvas' centred stroke with ringR - width/2. */
  {
    const c = render(1, 5);
    let lw = null;
    const outer = [];
    for (const k of c) {
      if (k.op === "set:lineWidth") lw = k.args[0];
      if (k.op === "arc" && lw > 4) outer.push(k.args[2] + lw / 2);
    }
    assert.ok(outer.length > 0, "no ring bands recorded");
    for (const o of outer) {
      assert.ok(Math.abs(o - F.R) < 0.001,
        `ring band outer edge ${o}, firmware NEON_R = ${F.R}`);
    }
  }

  /* Bulb ring radius. */
  {
    const c = render(2, 5);
    const bulbs = c.filter((k) => k.op === "arc" && k.args[2] === 4);
    assert.strictEqual(bulbs.length, 72, `expected 72 bulbs, got ${bulbs.length}`);
    const r = Math.round(Math.hypot(bulbs[0].args[0], bulbs[0].args[1]));
    assert.strictEqual(r, F.bulbR, `bulb ring radius ${r}, firmware NEON_BULB_R = ${F.bulbR}`);
  }

  /* Tube zero marker half-width. */
  {
    const c = render(0, -12);
    let stroke = null, lw = null;
    let marker = null;
    for (const k of c) {
      if (k.op === "set:strokeStyle") stroke = k.args[0];
      if (k.op === "set:lineWidth") lw = k.args[0];
      if (k.op === "arc" && stroke === "#ffffff" && lw > 30) {
        const span = (k.args[4] - k.args[3]) / DEG;
        if (Math.abs(span - 2 * F.zeroDeg) < 0.01) marker = span;
      }
    }
    assert.ok(marker !== null,
      `tube zero marker spanning 2 x NEON_TUBE_ZERO_DEG (${2 * F.zeroDeg} deg) not found`);
  }
}

console.log("neon web parity: mirror matches boost_gauge.c constants");
