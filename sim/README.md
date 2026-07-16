# Boost Gauge Desktop Simulator

Headless LVGL 9 simulator for the same UI code that runs on the Waveshare board.

## Requirements

- CMake, a host C compiler
- SDL2 (`libsdl2-dev`)
- Python 3 + Pillow (for PNG conversion)
- LVGL sources in `../managed_components/lvgl__lvgl`  
  (created automatically by the first ESP-IDF build)

On this headless LXC we also use `xvfb-run` only for the optional `--window` mode.

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
| `sim/main.c` | Host entry: headless FB or SDL window |
| `sim/lv_conf.h` | Host LVGL config (SDL, fonts, snapshot) |
