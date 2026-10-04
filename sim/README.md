# Boost Gauge Desktop Simulator

Headless LVGL 9 simulator for the same UI code that runs on the Waveshare board.

## Requirements

- CMake, a host C compiler
- Python 3 + Pillow (for PNG conversion and the browser control panel)
- LVGL sources in `../managed_components/lvgl__lvgl`
  (created automatically by the first ESP-IDF build)
- SDL2 (`libsdl2-dev`) is **optional** - only the `--window` mode needs it; the
  default, `--screenshot`, `--audit`, `--tpms`, `--qr-test` and `--stream` modes
  render into memory and need no display.

## Build

```bash
cmake -S sim -B sim/build
cmake --build sim/build -j"$(nproc)"
```

## Headless screenshots (default)

```bash
./sim/build/boost_gauge_sim --screenshot preview/sim
python3 sim/raw_to_png.py preview/sim
```

Or:

```bash
cmake --build sim/build --target sim-screenshots
```

Outputs:
- `preview/sim/gauge_vac.png`
- `preview/sim/gauge_atmo.png`
- `preview/sim/gauge_boost.png`
- `preview/sim/gauge_over.png`
- `preview/sim/gauge_sheet.png`
- `preview/sim/gauge_sweep.gif`

## Interactive control panel (`--stream` + `tools/sim_panel.py`)

`--stream` renders the live gauge continuously and writes one binary frame per
render cycle to **stdout** while reading commands from **stdin**, one per line.
All firmware logs and diagnostics go to **stderr**, so the byte stream can never
be corrupted by a `printf`. `tools/sim_panel.py` wraps it in an HTTP server with
a browser page (live gauge image + controls), no SDL2 required:

```bash
# from the repository root, after the build above
python3 tools/sim_panel.py
# open http://127.0.0.1:8787/
```

Options:

```bash
python3 tools/sim_panel.py --port 8787 --sim sim/build/boost_gauge_sim \
    --shot-dir preview/panel
```

The page streams frames as `multipart/x-mixed-replace` (a plain
`<img src="/stream">` updates with no JS video code) and offers theme, unit
(psi/bar/kPa), neon layout/font/preset, Vault needle/tail, the Dyno Cell
background, a pressure number + slider with a "follow the demo waveform"
toggle, the organic and fast-sweep waveforms, the boost/TPMS page selector with
the TPMS mock scenario, and a "Save screenshot" button. Screenshots are written
to `preview/panel/<theme>-<unit>-<psi>.png`. The panel restarts the sim if the
subprocess dies and shuts it down on Ctrl-C.

**The panel also restarts the sim when the BINARY CHANGES**, so `cmake --build
sim/build` is picked up without touching the panel — the page's status line
shows the sim's build time and an `auto-restarted Nx after a rebuild` note when
it happens. This matters more than it looks: a panel whose sim child predates
the last build keeps streaming the OLD firmware, and a stale render is
indistinguishable from a change that did not work. `tools/tests/test_sim_panel.py`
drives a real panel and bumps the binary's mtime to keep that honest.

`--stream` can also be driven by hand; the framing is
`"BGFR" | uint32 width | uint32 height | uint32 seq | RGBA pixels`
(little-endian, 466x466, RGBA is LVGL ARGB8888 = B,G,R,A bytes), so a reader
must drain stdout continuously (a frame is ~850 KB, larger than the pipe
buffer):

```bash
printf 'theme neon\nunit bar\npsi 12\ndemo off\nquit\n' | \
    ./sim/build/boost_gauge_sim --stream > /tmp/frames.bin
```

Commands map to the same firmware entry points the settings UI drives:
`theme <dyno-cell|vault-tec|night-city|big-digit|neon>`, `unit <psi|bar|kPa>`,
`psi <float>` (freezes the sweep), `demo <on|off>`, `sweep <organic|fast>`,
`layout <tube|segments|marquee>`, `neonfont <0|1>`, `preset <0..3>`,
`page <boost|tpms>`, `tpms <normal|stale|disconnected>`, `needle <red|green>`,
`tail <on|off>`, `dynoblack <on|off>`, `quit`.

## Windowed mode

```bash
# on a machine with a display
./sim/build/boost_gauge_sim --window

# headless LXC
xvfb-run -a ./sim/build/boost_gauge_sim --window
```

## Architecture

| File | Role |
|------|------|
| `../main/boost_gauge.c` | Shared LVGL UI (firmware + sim) |
| `../main/boost_sim.c` | Shared demo pressure waveform |
| `sim/main.c` | Host entry: headless FB, SDL window, or `--stream` panel backend |
| `sim/lv_conf.h` | Host LVGL config (SDL, fonts, snapshot) |
| `../tools/sim_panel.py` | Browser control panel over `--stream` (stdlib + Pillow) |
