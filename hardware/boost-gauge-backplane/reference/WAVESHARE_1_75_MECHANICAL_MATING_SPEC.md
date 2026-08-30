# Waveshare ESP32-S3-Touch-AMOLED-1.75
# Mechanical Mating Specification for a 46 mm Rear Daughterboard

**Target board:** Waveshare ESP32-S3-Touch-AMOLED-1.75, standard round PCB  
**Purpose:** Provide fixed mechanical datums for a custom PCB that mounts directly behind the Waveshare PCB and mates to its 8-pin H2 expansion header.

This document is intended to supplement the main `CUSTOM_PCB_TSCIRCUIT_AGENT_SPEC.md` so that the PCB-design agent does **not** need to research or estimate the Waveshare board geometry.

---

## 1. Source of truth

The dimensions below are taken from the official Waveshare dimension drawing for the **ESP32-S3-Touch-AMOLED-1.75** and cross-checked against the official Waveshare schematic/hardware reference.

Important:

- Use the **standard ESP32-S3-Touch-AMOLED-1.75** geometry.
- Do **not** use the later `1.75C` board geometry.
- Do **not** use the protective-case `-B` outer dimensions as the PCB outline.
- The PCB itself is **46.00 mm diameter**.
- The display/touch assembly is larger than the PCB; that larger diameter is not the daughterboard PCB diameter.

---

# 2. Canonical coordinate system

Use the following coordinate system for all daughterboard mechanical work.

Viewed from the **rear/component side of the Waveshare PCB**:

- Board center = **(0.00, 0.00) mm**
- `+X` = toward the **USB-C connector / right side**
- `-X` = toward the **PWR/BOOT button / left side**
- `+Y` = toward the **speaker/battery connectors / top**
- `-Y` = toward the **H2 8-pin expansion header / bottom**

The daughterboard outline is centered on the same origin.

```text
                         +Y
                         ^
                         |
                 TOP MOUNT
                    (0,20.50)

        PWR side         |          USB-C side
          -X <-----------+-----------> +X
                         |
                         |
          LOWER LEFT     |      LOWER RIGHT
           MOUNT         |         MOUNT
      (-13.75,-14.70)    |    (+13.75,-14.70)
                         |
                    H2 HEADER
                  y = -18.68
                         |
                         v
                         -Y
```

---

# 3. PCB outline

## Required daughterboard outline

| Parameter | Value |
|---|---:|
| PCB shape | Circle |
| PCB diameter | **46.00 mm** |
| PCB radius | **23.00 mm** |
| PCB center | **(0.00, 0.00) mm** |

Therefore the nominal bare-PCB limits are:

```text
X = -23.00 ... +23.00 mm
Y = -23.00 ... +23.00 mm
```

Do not use the approximately 48.96 mm display/front-glass diameter as the PCB outline.

---

# 4. Three primary mounting points

The Waveshare assembly has three primary brass mounting/standoff locations.

Use these centers exactly.

| Mount | X (mm) | Y (mm) |
|---|---:|---:|
| Top | **0.00** | **+20.50** |
| Lower-left | **-13.75** | **-14.70** |
| Lower-right | **+13.75** | **-14.70** |

## CAD placement

```text
MOUNT_TOP      = (  0.00, +20.50 )
MOUNT_BOTTOM_L = (-13.75, -14.70 )
MOUNT_BOTTOM_R = (+13.75, -14.70 )
```

The lower two centers are therefore:

- **27.50 mm apart horizontally**
- on the same horizontal centerline
- **14.70 mm below board center**

The top mount is:

- centered on `X = 0`
- **20.50 mm above board center**

## Mounting hardware callout

The official drawing calls out the mounting feature as:

```text
M2.00-H3.5
```

Treat the three gold/brass mounting locations as the assembly's M2-class mechanical attachment points.

### Daughterboard recommendation

For the new PCB, use **non-plated clearance holes rather than tapped PCB holes** unless the chosen stack hardware specifically requires otherwise.

Recommended daughterboard hole:

```text
Ø2.2 mm NPTH
```

This gives normal clearance for an M2 screw while retaining accurate alignment.

If the actual stack will use shoulder hardware, press-fit hardware, or the Waveshare brass bosses directly, adjust the daughterboard drill diameter to the selected fastener datasheet — **do not move the hole centers**.

---

# 5. H2 8-pin expansion header geometry

## Header type

```text
Reference: H2
Rows:      1
Pins:      8
Pitch:     2.54 mm
Orientation: horizontal
```

The H2 row is centered on the board's vertical axis.

### Header row centerline

```text
Y = -18.68 mm
```

### Header X center

```text
X = 0.00 mm
```

Because there are 8 pins at 2.54 mm pitch, the pin array is symmetric around `X=0`.

The pin-center X coordinates are:

```text
-8.89
-6.35
-3.81
-1.27
+1.27
+3.81
+6.35
+8.89
```

All pins use:

```text
Y = -18.68 mm
```

---

# 6. Exact H2 pin-center coordinates and signals

The official schematic numbers H2 from **pin 1 = VBUS** through **pin 8 = GPIO16**.

Using the canonical Waveshare rear/component-side view defined above:

| H2 pin | Signal | X (mm) | Y (mm) |
|---:|---|---:|---:|
| 8 | GPIO16 | **-8.89** | **-18.68** |
| 7 | GPIO18 | **-6.35** | **-18.68** |
| 6 | GPIO17 | **-3.81** | **-18.68** |
| 5 | GPIO43 / U0TXD | **-1.27** | **-18.68** |
| 4 | GPIO44 / U0RXD | **+1.27** | **-18.68** |
| 3 | 3V3 | **+3.81** | **-18.68** |
| 2 | GND | **+6.35** | **-18.68** |
| 1 | VBUS | **+8.89** | **-18.68** |

Equivalent pin-number order from left to right in the **Waveshare rear/component-side view**:

```text
LEFT                                                       RIGHT / USB-C
 H2-8      H2-7      H2-6      H2-5      H2-4      H2-3      H2-2      H2-1
 GPIO16    GPIO18    GPIO17    U0TXD     U0RXD      3V3       GND       VBUS
```

---

# 7. Critical mirroring warning for the daughterboard

This is the most likely mechanical/electrical mistake.

The coordinates above describe the Waveshare board when viewed from its **rear/component side**.

The daughterboard will sit behind it with the daughterboard's **mating face facing the Waveshare rear face**.

Therefore:

- physical hole centers stay coincident in assembly coordinates
- H2 pin centers stay coincident in assembly coordinates
- but a PCB viewed from its own outward side can appear horizontally mirrored relative to the Waveshare rear view

## Mandatory validation rule

Do **not** decide header orientation merely by saying "leftmost pin is GPIO16" while looking at the daughterboard from an arbitrary side.

Instead validate electrically:

```text
Daughterboard mating contact -> Waveshare H2 pin 1 -> VBUS
Daughterboard mating contact -> Waveshare H2 pin 2 -> GND
Daughterboard mating contact -> Waveshare H2 pin 3 -> 3V3
...
Daughterboard mating contact -> Waveshare H2 pin 8 -> GPIO16
```

Before fabrication, make a face-to-face assembly rendering or printout and verify pin 1 physically lands on **VBUS**.

---

# 8. Recommended H2 daughterboard footprint

Use a standard:

```text
1x8
2.54 mm pitch
through-hole
```

A mating **female socket header** is usually preferable on the daughterboard if the Waveshare board is fitted with male pins.

However, confirm what is physically installed on the actual display board before ordering connectors.

Recommended plated drill / pad values for a typical 0.64 mm square header pin:

```text
Finished drill: 1.0 mm nominal
Copper pad:     ~1.7–2.0 mm nominal
Pitch:          2.54 mm
```

The exact drill and pad dimensions must follow the selected mating connector's datasheet.

**The pin-center coordinates and pitch must not change.**

---

# 9. Mechanical dimension summary

Use this block directly in CAD/tscircuit work.

```text
BOARD
  shape: circle
  diameter: 46.00 mm
  center: (0.00, 0.00)

PRIMARY MOUNTS
  top:          (  0.00, +20.50)
  lower-left:   (-13.75, -14.70)
  lower-right:  (+13.75, -14.70)

  recommended daughterboard drill:
    Ø2.2 mm NPTH for M2 clearance
    (adjust drill for actual chosen hardware only)

H2
  1x8
  pitch: 2.54 mm
  row center: (0.00, -18.68)

  pin 8 GPIO16:       (-8.89, -18.68)
  pin 7 GPIO18:       (-6.35, -18.68)
  pin 6 GPIO17:       (-3.81, -18.68)
  pin 5 GPIO43/TX:    (-1.27, -18.68)
  pin 4 GPIO44/RX:    (+1.27, -18.68)
  pin 3 3V3:          (+3.81, -18.68)
  pin 2 GND:          (+6.35, -18.68)
  pin 1 VBUS:         (+8.89, -18.68)
```

---

# 10. Useful official envelope dimensions

These are not needed to define the daughterboard PCB outline, but they are useful for stack/enclosure checks.

| Feature | Official dimension |
|---|---:|
| Waveshare PCB | **Ø46.00 mm** |
| Front display visible/body reference | **43.76 mm** |
| Front assembly reference | **44.16 mm** |
| Maximum front circular glass/bezel envelope | **48.96 mm** |
| Rear PCB nominal radius | **23.00 mm** |
| USB-C side hardware extends beyond the 46 mm PCB circle | approximately to **X = +23.80 mm** in the official rear-view dimensioning |

The custom rear PCB should remain Ø46.00 mm unless there is a deliberate reason to extend beyond the Waveshare PCB.

---

# 11. USB-C access / keepout requirement

The Waveshare USB-C connector is on the `+X` side of the board.

Even though the daughterboard is a separate rear PCB, do not place tall daughterboard parts where they prevent a USB-C plug from entering the Waveshare connector.

At minimum:

- keep the immediate `+X` perimeter around the USB-C connector mechanically open
- perform a 3D/side-view check with a realistic USB-C cable plug
- do not assume the 46 mm daughterboard edge alone guarantees plug clearance

The Waveshare dimension drawing shows the USB-C hardware extending slightly beyond the nominal 23.00 mm PCB radius.

---

# 12. Inter-board height

The Waveshare dimension drawing gives several overall board/display thickness references, but it does **not** define the required custom daughterboard separation.

Do not choose the daughterboard spacer/header height solely from the nominal PCB thickness.

The PCB agent must select the final separation based on:

1. the tallest Waveshare rear-side component beneath the daughterboard
2. selected H2 socket/header stack height
3. screw/standoff arrangement
4. solder-joint protrusion
5. daughterboard component placement

Recommended design policy:

```text
All tall daughterboard components -> outward/back face
Display-facing daughterboard face -> header + low-profile passives only where possible
```

---

# 13. Secondary small holes/features

The official Waveshare drawing also contains a `2*1.80` hole/features callout.

These are **not required as the three primary daughterboard attachment datums defined above** unless the final mechanical stack deliberately uses them.

Do not invent matching holes for them merely because they appear in the Waveshare drawing.

If they are later required for an enclosure or alignment feature, import/inspect the official Waveshare 3D model and add them as a separate mechanical feature.

For the direct daughterboard design, the required registration features are:

- the three primary mount centers
- the H2 connector

---

# 14. tscircuit implementation requirement

Create a reusable form-factor component, for example:

```text
WaveshareEsp32S3TouchAmoled175Backplane
```

It should instantiate:

- Ø46.00 mm circular PCB outline
- three mounting holes at the coordinates in this document
- H2 footprint at the coordinates in this document
- a visible board-center datum
- a USB-C access keepout marker on the +X side

Do not hand-place these separately in every revision.

Use numeric constants in one location, e.g.:

```ts
const WS_BOARD_DIAMETER = 46.00

const WS_MOUNTS = [
  { x:  0.00,  y:  20.50 },
  { x: -13.75, y: -14.70 },
  { x:  13.75, y: -14.70 },
]

const WS_H2_Y = -18.68
const WS_H2_PITCH = 2.54

const WS_H2 = [
  { pin: 8, signal: "GPIO16", x: -8.89, y: -18.68 },
  { pin: 7, signal: "GPIO18", x: -6.35, y: -18.68 },
  { pin: 6, signal: "GPIO17", x: -3.81, y: -18.68 },
  { pin: 5, signal: "GPIO43_TX", x: -1.27, y: -18.68 },
  { pin: 4, signal: "GPIO44_RX", x:  1.27, y: -18.68 },
  { pin: 3, signal: "3V3", x:  3.81, y: -18.68 },
  { pin: 2, signal: "GND", x:  6.35, y: -18.68 },
  { pin: 1, signal: "VBUS", x:  8.89, y: -18.68 },
]
```

Adapt the syntax to the installed/current tscircuit API; the coordinates themselves are the mechanical contract.

---

# 15. Fabrication validation

Before ordering the daughterboard:

## 1:1 paper check

Export the daughterboard mechanical layer at **100% scale**.

Verify with calipers:

```text
circle diameter:          46.00 mm
bottom mount spacing:     27.50 mm
top mount Y from center:  20.50 mm
bottom mount Y:          -14.70 mm
H2 pitch:                  2.54 mm
H2 row Y:                -18.68 mm
```

## Physical overlay check

If possible:

1. print the daughterboard outline at 1:1
2. place the actual Waveshare PCB over the print
3. verify the three mount centers
4. verify all eight H2 pin centers
5. verify pin-1/VBUS orientation
6. only then order PCBs

A cheap paper overlay catches a mirrored H2 footprint much more reliably than visual inspection of two separate PCB renders.

---

# 16. Final mechanical acceptance criteria

The backplane mechanical design is acceptable only if all are true:

- [ ] PCB is a **46.00 mm diameter circle**
- [ ] Board origin is at the circle center
- [ ] Top mount center is `(0.00, +20.50)`
- [ ] Lower-left mount center is `(-13.75, -14.70)`
- [ ] Lower-right mount center is `(+13.75, -14.70)`
- [ ] Lower mount X spacing is **27.50 mm**
- [ ] H2 is **1x8, 2.54 mm pitch**
- [ ] H2 row center is `(0.00, -18.68)`
- [ ] H2 pin centers use the coordinate table above
- [ ] H2 pin 1 mates to **VBUS**
- [ ] H2 pin 8 mates to **GPIO16**
- [ ] Daughterboard orientation has been checked face-to-face, not just in separate top views
- [ ] USB-C plug access remains possible
- [ ] Tall parts are on the outward daughterboard face
- [ ] A 1:1 mechanical overlay has been checked before fabrication

---

# 17. Reference notes

Primary references used to prepare this mechanical specification:

- Waveshare official `ESP32-S3-Touch-AMOLED-1.75` dimension drawing
- Waveshare official `ESP32-S3-Touch-AMOLED-1.75` schematic
- Waveshare official hardware reference for H2 pin numbering and signals
- Waveshare product documentation for the 2.54 mm 8-pin header and board variant identification

If Waveshare later releases a hardware revision with a changed dimension drawing, the revision-specific official drawing supersedes this document.

