"""Neon theme mockups for boost-gauge. Renders 466x466 faces at 4 psi states.

Honours the firmware's real geometry: DISP_SIZE 466, ARC_START 135, ARC_RANGE 270,
zero notch at 236.25 deg, psi range -15..+10, overboost at +8.
"""
import math, os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
from PIL import Image, ImageDraw, ImageFont, ImageFilter

SS = 3                      # supersample factor
D = 466
S = D * SS
CX = CY = S / 2

ARC_START, ARC_RANGE = 135.0, 270.0
ZERO_ANGLE = 236.25
PSI_MIN, PSI_MAX, PSI_OVER = -15.0, 10.0, 8.0

ALIEN = os.path.join(ROOT, "main", "fonts", "SFAlienEncounters-Italic.ttf")
COND  = "C:/Windows/Fonts/bahnschrift.ttf"
MONO  = "C:/Windows/Fonts/consola.ttf"

# --- palettes: zone ramp + the structural track/muted that harmonise with it
def rgb(h):
    return ((h >> 16) & 255, (h >> 8) & 255, h & 255)


PALETTES = {
    "violet": dict(name="VIOLET DRIVE", vacuum=0x8B3DFF, boost=0xFF2BD6,
                   over=0xFF6A00, track=0x241038, muted=0x5A3A7A),
    "miami":  dict(name="MIAMI", vacuum=0x00E5FF, boost=0xFF2BD6,
                   over=0xFF1466, track=0x10222E, muted=0x3F6E80),
    "toxic":  dict(name="TOXIC", vacuum=0x39FF14, boost=0xFFF000,
                   over=0xFF00A0, track=0x12300A, muted=0x4C7A2E),
}
PAL = PALETTES["violet"]
TEXT = (0xFF, 0xFF, 0xFF)


def VACUUMc():    return rgb(PAL["vacuum"])
def BOOSTc():     return rgb(PAL["boost"])
def OVERBOOSTc(): return rgb(PAL["over"])
def MUTEDc():     return rgb(PAL["muted"])
def TRACKc():     return rgb(PAL["track"])


def zone_color(psi):
    if psi >= PSI_OVER: return OVERBOOSTc()
    if psi > 0.05:      return BOOSTc()
    return VACUUMc()


def zone_name(psi):
    if psi >= PSI_OVER: return "OVERBOOST"
    if psi > 0.05:      return "BOOST"
    return "VACUUM"


def psi_to_angle(psi):
    """Mirror of psi_to_angle(): vacuum and boost scale independently."""
    psi = max(PSI_MIN, min(PSI_MAX, psi))
    if psi < 0:
        t = psi / PSI_MIN                     # 0 at atmo -> 1 at full vacuum
        return ARC_START + (1 - t) * (ZERO_ANGLE - ARC_START)
    t = psi / PSI_MAX
    return ZERO_ANGLE + t * (ARC_START + ARC_RANGE - ZERO_ANGLE)


def pt(angle_deg, r):
    a = math.radians(angle_deg)
    return CX + r * math.cos(a), CY + r * math.sin(a)


def font(path, px):
    return ImageFont.truetype(path, int(px * SS))


def glow(layer, radius, gain=1.0):
    """Blur a layer to fake an emissive bloom (baked into the face in firmware)."""
    g = layer.filter(ImageFilter.GaussianBlur(radius * SS))
    if gain != 1.0:
        g = Image.eval(g, lambda v: min(255, int(v * gain)))
    return g


def add(base, layer):
    return Image.fromarray(
        __import__("numpy").clip(
            __import__("numpy").asarray(base, dtype="int16")
            + __import__("numpy").asarray(layer, dtype="int16"), 0, 255
        ).astype("uint8"))


def new_layer():
    return Image.new("RGB", (S, S), (0, 0, 0))


def draw_text_c(d, xy, text, f, fill, anchor="mm"):
    d.text((xy[0], xy[1]), text, font=f, fill=fill, anchor=anchor)


# --- SF Alien Encounters ships NO hyphen and NO plus (both are zero-contour
#     blank glyphs). The sign is therefore drawn as a shape, in the typeface's
#     own slice rhythm: bars 8/128 of the size, period 10.4/128, 12 deg italic.
def sliced_minus(d, cx, cy, size, fill, bars=2):
    period = size * 10.4 / 128.0
    bar_h = size * 8.0 / 128.0
    w = size * 0.34
    skew = math.tan(math.radians(12.0))
    top = cy - (bars * period - (period - bar_h)) / 2
    for i in range(bars):
        y0 = top + i * period
        y1 = y0 + bar_h
        dx = (cy - (y0 + y1) / 2) * skew
        d.polygon([(cx - w / 2 + dx + bar_h * skew, y0),
                   (cx + w / 2 + dx + bar_h * skew, y0),
                   (cx + w / 2 + dx, y1),
                   (cx - w / 2 + dx, y1)], fill=fill)


# --- fixed-width digit slots (the font is NOT tabular: '1' is 409/1000 vs
#     '0' at 780, so live digits must sit in constant-width centred cells) ---
def draw_slotted(d, cx, cy, psi, f, fill, slot_w, dot_w, size, gap_f=0.26):
    neg = psi < 0
    v = abs(psi)
    tens = int(v) // 10
    ones = int(v) % 10
    tenths = int(round((v - int(v)) * 10)) % 10
    cells = []
    if tens: cells.append(("%d" % tens, slot_w))
    cells.append(("%d" % ones, slot_w))
    cells.append((".", dot_w))
    cells.append(("%d" % tenths, slot_w))
    total = sum(w for _, w in cells)
    x = cx - total / 2
    for ch, w in cells:
        d.text((x + w / 2, cy), ch, font=f, fill=fill, anchor="mm")
        x += w
    if neg:
        sliced_minus(d, cx - total / 2 - slot_w * gap_f, cy, size, fill)


def round_mask():
    m = Image.new("L", (S, S), 0)
    ImageDraw.Draw(m).ellipse([0, 0, S - 1, S - 1], fill=255)
    return m


def finish(img):
    img = img.resize((D, D), Image.LANCZOS)
    m = round_mask().resize((D, D), Image.LANCZOS)
    out = Image.new("RGB", (D, D), (0, 0, 0))
    out.paste(img, (0, 0), m)
    return out


# =========================================================================
# A — NEON TUBE : one continuous glass tube, lit from the zero notch
# =========================================================================
def face_tube(psi, peak):
    col = zone_color(psi)
    base = new_layer()
    gl = new_layer()      # everything that blooms
    dg = ImageDraw.Draw(base)
    dgl = ImageDraw.Draw(gl)

    R = 182
    W = 26

    # --- baked: unlit glass tube + ticks + labels
    box = [CX - R * SS, CY - R * SS, CX + R * SS, CY + R * SS]
    dg.arc(box, ARC_START, ARC_START + ARC_RANGE, fill=TRACKc(), width=int(W * SS))
    dg.arc(box, ARC_START, ARC_START + ARC_RANGE, fill=(0x3A, 0x1C, 0x55),
           width=int(2 * SS))

    LABELLED = (-15, -10, -5, 0, 5, 10)
    for v in (-15, -12.5, -10, -7.5, -5, -2.5, 0, 2, 4, 5, 6, 8, 10):
        a = psi_to_angle(v)
        major = v in LABELLED
        r0, r1 = (R + 16, R + 26) if major else (R + 18, R + 24)
        x0, y0 = pt(a, r0 * SS); x1, y1 = pt(a, r1 * SS)
        c = TEXT if v == 0 else MUTEDc()
        dgl.line([x0, y0, x1, y1], fill=c, width=int((4 if major else 2) * SS))
        if v in LABELLED:
            lx, ly = pt(a, (R + 38) * SS)
            draw_text_c(dgl, (lx, ly), "%d" % v, font(COND, 18),
                        TEXT if v == 0 else MUTEDc())

    # zero notch: a bright bar through the tube
    a0 = psi_to_angle(0)
    x0, y0 = pt(a0, (R - W / 2 - 6) * SS); x1, y1 = pt(a0, (R + W / 2 + 6) * SS)
    dgl.line([x0, y0, x1, y1], fill=TEXT, width=int(5 * SS))

    # --- live: the lit run of tube, faked bloom = 3 opaque strokes
    a_val = psi_to_angle(psi)
    lo, hi = (a_val, a0) if psi < 0 else (a0, a_val)
    if abs(hi - lo) > 0.4:
        halo = tuple(int(c * 0.35) for c in col)
        mid = tuple(int(c * 0.75) for c in col)
        dgl.arc(box, lo, hi, fill=halo, width=int((W + 16) * SS))
        dgl.arc(box, lo, hi, fill=mid, width=int(W * SS))
        dgl.arc(box, lo, hi, fill=col, width=int((W - 12) * SS))
        dgl.arc(box, lo, hi, fill=(255, 255, 255), width=int(3 * SS))

    # peak tell-tale
    ap = psi_to_angle(peak)
    px0, py0 = pt(ap, (R + W / 2 + 2) * SS); px1, py1 = pt(ap, (R + W / 2 + 12) * SS)
    dgl.line([px0, py0, px1, py1], fill=zone_color(peak), width=int(5 * SS))

    # --- readout
    fbig = font(ALIEN, 104)
    draw_slotted(dgl, CX, CY - 6 * SS, psi, fbig, col, 62 * SS, 32 * SS, 104 * SS)
    draw_text_c(dgl, (CX, CY + 54 * SS), "P S I", font(ALIEN, 24), MUTEDc())
    draw_text_c(dgl, (CX, CY - 84 * SS), "N E O N D R I V E", font(ALIEN, 22), MUTEDc())
    draw_text_c(dgl, (CX, CY + 96 * SS), zone_name(psi), font(ALIEN, 28), col)
    draw_text_c(dgl, (CX, CY + 132 * SS), "PK %.1f    MAP %dkPa" % (peak, 101 + psi * 6.895),
                font(MONO, 16), MUTEDc())

    out = add(base, gl)
    out = add(out, glow(gl, 7, 0.85))
    out = add(out, glow(gl, 20, 0.45))
    return finish(out)


# =========================================================================
# B — NEON SEGMENT RING : 56 discrete tube segments, VU-meter fill
# =========================================================================
NSEG = 56


def face_segments(psi, peak):
    col = zone_color(psi)
    base = new_layer()
    gl = new_layer()
    dg = ImageDraw.Draw(base)
    dgl = ImageDraw.Draw(gl)

    R = 182
    W = 30
    gap = 1.6
    step = ARC_RANGE / NSEG
    a_val = psi_to_angle(psi)
    a0 = psi_to_angle(0)
    a_peak = psi_to_angle(peak)
    box = [CX - R * SS, CY - R * SS, CX + R * SS, CY + R * SS]

    peak_idx = int((a_peak - ARC_START) / step)
    for i in range(NSEG):
        s = ARC_START + i * step + gap / 2
        e = s + step - gap
        mid = (s + e) / 2
        lit = (a0 <= mid <= a_val) or (a_val <= mid <= a0)
        if lit:
            c = zone_color(psi if psi >= 0 else -1)
            # brighten toward the leading edge
            k = 1.0
            dgl.arc(box, s, e, fill=tuple(int(v * 0.3 * k) for v in c),
                    width=int((W + 14) * SS))
            dgl.arc(box, s, e, fill=c, width=int(W * SS))
            dgl.arc(box, s, e, fill=(255, 255, 255), width=int(6 * SS))
        elif i == peak_idx and peak > 0.2:
            pc = zone_color(peak)
            dgl.arc(box, s, e, fill=pc, width=int(W * SS))
        else:
            dg.arc(box, s, e, fill=TRACKc(), width=int(W * SS))

    # zero notch gap marker outside the ring
    x0, y0 = pt(a0, (R + W / 2 + 2) * SS); x1, y1 = pt(a0, (R + W / 2 + 14) * SS)
    dgl.line([x0, y0, x1, y1], fill=TEXT, width=int(5 * SS))

    for v in (-15, -10, -5, 0, 5, 10):
        a = psi_to_angle(v)
        lx, ly = pt(a, (R + W / 2 + 23) * SS)
        draw_text_c(dgl, (lx, ly), str(v), font(COND, 18),
                    TEXT if v == 0 else MUTEDc())

    fbig = font(ALIEN, 104)
    draw_slotted(dgl, CX, CY - 2 * SS, psi, fbig, col, 62 * SS, 32 * SS, 104 * SS)
    draw_text_c(dgl, (CX, CY - 82 * SS), zone_name(psi), font(ALIEN, 26), col)
    draw_text_c(dgl, (CX, CY + 58 * SS), "P S I", font(ALIEN, 24), MUTEDc())
    dgl.line([CX - 62 * SS, CY + 82 * SS, CX + 62 * SS, CY + 82 * SS],
             fill=MUTEDc(), width=int(1.5 * SS))
    draw_text_c(dgl, (CX, CY + 104 * SS), "PEAK %.1f" % peak, font(MONO, 17), MUTEDc())

    out = add(base, gl)
    out = add(out, glow(gl, 6, 0.9))
    out = add(out, glow(gl, 18, 0.5))
    return finish(out)


# =========================================================================
# C — NEON MARQUEE : no ring. Sign-board typography + a linear bar.
# =========================================================================
def face_marquee(psi, peak):
    col = zone_color(psi)
    base = new_layer()
    gl = new_layer()
    dg = ImageDraw.Draw(base)
    dgl = ImageDraw.Draw(gl)

    # baked marquee bulb border
    Rb = 216
    for i in range(72):
        a = 360 * i / 72
        x, y = pt(a, Rb * SS)
        on = (i % 6) < 2
        c = tuple(int(v * 0.55) for v in col) if on else (0x2A, 0x14, 0x40)
        r = 4.0 * SS
        (dgl if on else dg).ellipse([x - r, y - r, x + r, y + r], fill=c)

    # baked sign plate edge
    dg.arc([CX - 196 * SS, CY - 196 * SS, CX + 196 * SS, CY + 196 * SS],
           0, 360, fill=(0x30, 0x18, 0x48), width=int(2 * SS))

    draw_text_c(dgl, (CX, CY - 132 * SS), "MANIFOLD", font(ALIEN, 30), MUTEDc())

    fbig = font(ALIEN, 156)
    draw_slotted(dgl, CX, CY - 26 * SS, psi, fbig, col, 94 * SS, 48 * SS, 156 * SS,
                 gap_f=0.12)

    # linear neon bar: bidirectional from centre-zero
    BW, BH = 300, 16
    bx0 = CX - BW / 2 * SS
    by = CY + 76 * SS
    zx = bx0 + BW * SS * (0 - PSI_MIN) / (PSI_MAX - PSI_MIN)
    dg.rounded_rectangle([bx0, by - BH / 2 * SS, bx0 + BW * SS, by + BH / 2 * SS],
                         radius=BH / 2 * SS, fill=TRACKc())
    vx = bx0 + BW * SS * (max(PSI_MIN, min(PSI_MAX, psi)) - PSI_MIN) / (PSI_MAX - PSI_MIN)
    lo, hi = min(zx, vx), max(zx, vx)
    if hi - lo > 2 * SS:
        dgl.rounded_rectangle([lo, by - BH / 2 * SS, hi, by + BH / 2 * SS],
                              radius=BH / 2 * SS, fill=col)
        dgl.rounded_rectangle([lo, by - BH / 2 * SS + 3 * SS, hi, by - BH / 2 * SS + 6 * SS],
                              radius=2 * SS, fill=(255, 255, 255))
    dgl.line([zx, by - 18 * SS, zx, by + 18 * SS], fill=TEXT, width=int(4 * SS))
    pxx = bx0 + BW * SS * (max(PSI_MIN, min(PSI_MAX, peak)) - PSI_MIN) / (PSI_MAX - PSI_MIN)
    dgl.line([pxx, by - 14 * SS, pxx, by + 14 * SS], fill=zone_color(peak),
             width=int(3 * SS))

    for v in (-15, -10, -5, 0, 5, 10):
        tx = bx0 + BW * SS * (v - PSI_MIN) / (PSI_MAX - PSI_MIN)
        dgl.line([tx, by + BH / 2 * SS + 4 * SS, tx, by + BH / 2 * SS + 10 * SS],
                 fill=TEXT if v == 0 else MUTEDc(), width=int(2 * SS))
        draw_text_c(dgl, (tx, by + 26 * SS), str(v), font(COND, 16),
                    TEXT if v == 0 else MUTEDc())
    draw_text_c(dgl, (CX, by + 60 * SS), zone_name(psi), font(ALIEN, 28), col)
    draw_text_c(dgl, (CX, by + 92 * SS), "PSI   PEAK %.1f" % peak, font(MONO, 17), MUTEDc())

    out = add(base, gl)
    out = add(out, glow(gl, 7, 0.9))
    out = add(out, glow(gl, 22, 0.5))
    return finish(out)


STATES = [(-12.0, 0.0), (0.0, 0.0), (8.5, 8.5), (19.5, 19.5)]

OUT = os.path.join(ROOT, "preview", "neon_mock")
os.makedirs(OUT, exist_ok=True)

FACES = (("A_tube", face_tube), ("B_segments", face_segments),
         ("C_marquee", face_marquee))

# 1. each layout at 4 states, in the default palette
for name, fn in FACES:
    sheet = Image.new("RGB", (D * 2 + 24, D * 2 + 24), (0x0D, 0x0D, 0x11))
    for i, (psi, peak) in enumerate(STATES):
        im = fn(psi, peak)
        im.save("%s/%s_%d.png" % (OUT, name, i))
        sheet.paste(im, ((i % 2) * (D + 8) + 8, (i // 2) * (D + 8) + 8))
    sheet.save("%s/%s_sheet.png" % (OUT, name))
    print("wrote", name)

# 2. palette comparison: one layout (B), 3 palettes x 3 telling states
PSTATES = [(-12.0, 0.0), (4.5, 4.5), (9.2, 9.2)]
cols, rows = len(PSTATES), len(PALETTES)
sheet = Image.new("RGB", (D * cols + 8 * (cols + 1), D * rows + 8 * (rows + 1)),
                  (0x0D, 0x0D, 0x11))
for r, key in enumerate(("violet", "miami", "toxic")):
    PAL = PALETTES[key]
    globals()["PAL"] = PAL
    for cidx, (psi, peak) in enumerate(PSTATES):
        im = face_segments(psi, peak)
        im.save("%s/pal_%s_%d.png" % (OUT, key, cidx))
        sheet.paste(im, (cidx * (D + 8) + 8, r * (D + 8) + 8))
sheet.save("%s/palettes_sheet.png" % OUT)
print("wrote palettes")
