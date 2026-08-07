"""Size and dirty-area budget for a pre-rendered-sprite neon face.

Everything here comes from the shipped artefacts: glyph boxes are parsed out of
the generated lv_font_conv C files, ring geometry from the constants in
boost_gauge.c. Nothing is estimated except the glow margin, which is swept.
"""
import re, math, sys, pathlib

ROOT = pathlib.Path(sys.argv[1])

def glyph_boxes(path):
    txt = (ROOT / path).read_text(errors="ignore")
    body = txt.split("glyph_dsc[] = {", 1)[1].split("};", 1)[0]
    out = []
    for m in re.finditer(r"\.box_w = (\d+), \.box_h = (\d+)", body):
        w, h = int(m.group(1)), int(m.group(2))
        if w and h:
            out.append((w, h))
    return out

BIG = glyph_boxes("main/fonts/neon_big.c")      # 108 px, tube + segments
HUGE = glyph_boxes("main/fonts/neon_huge.c")    # 154 px, marquee
LABEL = glyph_boxes("main/fonts/neon_label.c")  # 24 px, zone word

# --- ring geometry, from boost_gauge.c -------------------------------------
ARC_START, ARC_RANGE = 135.0, 270.0
R_OUT, SEG_W, HALO = 182, 30, 12
NSEG, GAP = 54, 2.0
R_IN = R_OUT - (SEG_W + HALO)
STEP = ARC_RANGE / NSEG

def sector_box(a0, a1, r_in, r_out):
    """Exact axis-aligned bounds of an annular sector, including the axis
    crossings the arc actually passes through."""
    xs, ys = [], []
    for a in (a0, a1):
        for r in (r_in, r_out):
            t = math.radians(a)
            xs.append(r * math.cos(t)); ys.append(r * math.sin(t))
    for axis in (0, 90, 180, 270, 360):
        if a0 <= axis <= a1:
            t = math.radians(axis)
            xs.append(r_out * math.cos(t)); ys.append(r_out * math.sin(t))
    return max(xs) - min(xs), max(ys) - min(ys)

def ring_sprites(margin):
    tot, boxes = 0, []
    for i in range(NSEG):
        a0 = ARC_START + i * STEP + GAP / 2
        a1 = a0 + STEP - GAP
        w, h = sector_box(a0, a1, R_IN, R_OUT)
        w = math.ceil(w) + 2 * margin
        h = math.ceil(h) + 2 * margin
        boxes.append((w, h)); tot += w * h
    return tot, boxes

def fmt(n):
    return f"{n/1024:9.1f} KB" if n < 1024 * 1024 else f"{n/1048576:9.2f} MB"

print("glyph counts: big=%d huge=%d label=%d" % (len(BIG), len(HUGE), len(LABEL)))
print("largest big  box: %dx%d" % max(BIG, key=lambda b: b[0] * b[1]))
print("largest huge box: %dx%d" % max(HUGE, key=lambda b: b[0] * b[1]))
print()

print("%-8s %12s %12s %12s %12s %12s" %
      ("margin", "readout108", "readout154", "ring54", "label", "TOTAL A8"))
for margin in (8, 12, 16, 24, 32):
    r108 = sum((w + 2 * margin) * (h + 2 * margin) for w, h in BIG)
    r154 = sum((w + 2 * margin) * (h + 2 * margin) for w, h in HUGE)
    ring, _ = ring_sprites(margin)
    lab = sum((w + 2 * margin) * (h + 2 * margin) for w, h in LABEL)
    tot = r108 + r154 + ring + lab
    print("%-8d %12s %12s %12s %12s %12s" %
          (margin, fmt(r108), fmt(r154), fmt(ring), fmt(lab), fmt(tot)))
print()

# --- dirty area per changed element ----------------------------------------
print("dirty area for ONE changed element (px), vs what ships today")
print("%-8s %14s %14s %14s" % ("margin", "digit@108", "digit@154", "1 ring seg"))
for margin in (8, 12, 16, 24, 32):
    dw, dh = max(BIG, key=lambda b: b[0] * b[1])
    hw, hh = max(HUGE, key=lambda b: b[0] * b[1])
    _, boxes = ring_sprites(margin)
    seg = max(w * h for w, h in boxes)
    print("%-8d %14d %14d %14d" %
          (margin, (dw + 2 * margin) * (dh + 2 * margin),
           (hw + 2 * margin) * (hh + 2 * margin), seg))
print()
print("shipping today: digit@108 cell = 152x104 = %d, digit@154 cell = 180x136 = %d"
      % (152 * 104, 180 * 136))
print()

# --- how many sprites overlap one changed sprite ---------------------------
# Segment pitch along the ring at the body's mid radius.
pitch = math.radians(STEP) * (R_OUT - (SEG_W + HALO) / 2)
print("ring segment pitch at mid radius: %.1f px" % pitch)
for margin in (8, 12, 16, 24, 32):
    n = 1 + 2 * math.ceil(margin / pitch)
    print("  margin %2d -> %d sprites overlap any point (redraw factor)" % (margin, n))

print()
print("=== marquee-specific + revised label plan ===")
# Zone word as 3 whole-word sprites (they glow); P S I / PEAK are muted, no glow.
ZONE_W, ZONE_H = 200, 17          # OVERBOOST at 24 px, generous
BAR_CAP_W, BAR_H = 16, 16         # two end caps; middle is a 1-px repeat column
BULB, TICK_W, TICK_H = 9, 4, 36
for margin in (8, 12, 16):
    zone = 3 * (ZONE_W + 2 * margin) * (ZONE_H + 2 * margin)
    bar = 2 * (BAR_CAP_W + 2 * margin) * (BAR_H + 2 * margin) \
        + (1 + 2 * margin) * (BAR_H + 2 * margin)
    bulb = (BULB + 2 * margin) ** 2
    tick = (TICK_W + 2 * margin) * (TICK_H + 2 * margin)
    print("  margin %2d: zone %s bar %s bulb %s tick %s" %
          (margin, fmt(zone), fmt(bar), fmt(bulb), fmt(tick)))

print()
print("=== A8 + recolor vs pre-tinted RGB565 (3 zones), margin 12 ===")
r108 = sum((w + 24) * (h + 24) for w, h in BIG)
r154 = sum((w + 24) * (h + 24) for w, h in HUGE)
ring, _ = ring_sprites(12)
zone = 3 * (ZONE_W + 24) * (ZONE_H + 24)
core = r108 + r154 + ring + zone
print("  A8, 1 byte/px, recolor at draw time : %s" % fmt(core))
print("  RGB565 pre-tinted, 2 B/px x 3 zones : %s" % fmt(core * 2 * 3))
print("  RGB565 + A8 alpha (RGB565A8), 3 B/px x 3 zones: %s" % fmt(core * 3 * 3))
print()
APP_USED, APP_SIZE = 2125296, 0x400000
print("  app partition %s, used %s, free %s"
      % (fmt(APP_SIZE), fmt(APP_USED), fmt(APP_SIZE - APP_USED)))
