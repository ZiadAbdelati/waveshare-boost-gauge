# MAP sensor bus and calibration

## Sensor bus

The dedicated sensor bus is I2C port 0: **SDA=GPIO17**, **SCL=GPIO18**. It remains separate from the Waveshare BSP bus on GPIO15/14 and avoids the UART-default GPIO43/44 pads.

- ADS1115 is `0x48` with ADDR grounded; the 5 V GM 12223861 three-bar MAP signal is read on A0.
- BMP280 is `0x76` with SDO grounded and must remain on 3.3 V logic.
- `SENS_I2C_HZ` in `main/boost_sensors.c` is **100 kHz**. The run passes through a MOSFET level shifter with 4.7 kΩ pull-ups, whose rise times do not comfortably support 400 kHz in a vehicle. It is not an API, UI, NVS, or Kconfig setting.
- The sensor task runs at **16 ms**, matching the cadence contract. ADS1115 is read every loop (62.5 Hz MAP), BMP280 every tenth (6.25 Hz ambient).
- `GET /api/v1/sensors/scan` uses ESP-IDF `i2c_master_probe()`, which probes at 100 kHz independently of `SENS_I2C_HZ`, and requires four consecutive ACKs before reporting an address. That filter exists because a sweep through *changing* addresses was observed to invent one-off phantom addresses on an electrically empty bus.

Bench history for the bus failures that preceded this is in `docs/troubleshooting/i2c-sensor-bus.md`. The short version: the bus was restored by replacing the ADS1115 assembly. GPIO17/18 were never damaged, the RX/TX remap was never needed, and the 400 kHz → 100 kHz reduction was **not** the fix.

## MAP conversion (GM 12223861)

The GM 12223861 curve is defined at a **5.00 V** supply by 0.619 V → 40 kPa and 4.818 V → 304 kPa. The sensor is ratiometric, so a supply that is not 5.00 V is normalized out before the transfer function is applied:

```text
normalized_volts = map_volts * 5.00 / supply_volts
nominal_kpa      = 62.8721124 * normalized_volts + 1.08216242
corrected_kpa    = nominal_kpa + calibration.offset_kpa
gauge_kpa        = corrected_kpa - ambient_kpa      (BMP280, live)
```

The configured supply defaults to **5.20 V** and is editable in Settings. Because gauge pressure subtracts the *current* BMP280 reading, weather and altitude drift correct themselves; only sensor error needs calibrating.

## Calibration

Calibration is **manual and never automatic.** With the engine off and both the MAP port and the BMP280 open to the same atmosphere, Settings → *Calibrate MAP to ATM* observes ~2 s of samples and stores `offset_kpa = mean(bmp) - mean(nominal)`. It refuses stale, unstable, implausible, or over-limit readings, and a failure never disturbs the existing calibration. Recalibration **replaces** the offset — nominal is always recomputed from raw volts, so offsets cannot compound.

One known pressure identifies zero error but cannot independently identify sensor gain, so the correction is deliberately **additive in kPa**: exact at atmosphere (the reading that must be right), degrading gracefully toward full boost. `ref_map_volts` is stored so a second calibration point could be added later without a schema migration. Do not convert this to a multiplicative fit on one point.

Supply voltage and the calibration record live under their own NVS keys (`map_vsup`, `map_cal`) in the `boost` namespace rather than inside `boost_config_t`, so growing them never risks discarding existing gauge settings. Editing the supply after calibrating re-derives the offset from the stored raw reference instead of stacking a correction on the old normalization. Note: a badly wrong supply entry is absorbed into a correspondingly large offset — the gauge still reads zero at atmosphere but is mis-scaled under boost, so check the displayed offset after changing supply and recalibrate.

Calibration diagnostics live on `GET /api/v1/sensors/calibration`, deliberately *not* on `/state` or the WebSocket payload: that path runs at 62.5 Hz into a smaller JSON buffer, and Settings must be able to show real sensor state even while demo mode is driving the gauge.
