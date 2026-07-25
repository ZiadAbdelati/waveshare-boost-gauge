# MAP Atmosphere Calibration Design

**Date:** 2026-07-25

## Goal

Correct the GM 12223861 three-bar MAP conversion, compensate for the fixed
5.20 V sensor supply, and let the driver manually align the MAP sensor to the
BMP280 atmospheric reference while the engine is off. The calibration must
survive reboot, remain inspectable from Settings, and never run automatically.

## Accuracy model

The BMP280 is the atmospheric reference. Its pressure range and absolute
accuracy are better suited to measuring ambient pressure than an unbranded
three-bar automotive MAP clone. The MAP sensor remains the runtime manifold
pressure source because the BMP280 cannot cover boost pressure.

The nominal GM 12223861 curve is defined by 0.619 V at 40 kPa and 4.818 V at
304 kPa when powered from 5.00 V:

```text
normalized_volts = measured_volts * 5.00 / configured_supply_volts
nominal_map_kpa = 62.8721124 * normalized_volts + 1.08216242
corrected_map_kpa = nominal_map_kpa + saved_offset_kpa
gauge_kpa = corrected_map_kpa - current_bmp_kpa
```

The configured MAP supply defaults to 5.20 V and is editable in Settings.
Supply normalization corrects the expected ratiometric span change. The
one-point atmospheric calibration stores an additive pressure offset because
one known pressure can identify zero error but cannot independently identify
sensor gain. Recalibration replaces the prior offset; offsets never compound.
If the configured supply voltage changes after calibration, the firmware
recomputes the offset from the stored reference MAP voltage and BMP pressure
before persisting the updated record. This preserves the same atmospheric
reference without carrying an offset calculated under the old normalization.

## Calibration workflow

Settings contains a **MAP atmosphere calibration** panel with:

- configured MAP supply voltage;
- live ADS1115 voltage;
- nominal and corrected MAP absolute pressure;
- live BMP280 atmospheric pressure;
- saved offset in kPa and PSI;
- calibration state and reference values;
- a **Calibrate MAP to ATM** button.

Pressing the button shows a confirmation that the engine must be off, the gauge
must remain powered, and the MAP port and BMP280 must both be exposed to the
same atmosphere.

The firmware then observes sensor-task snapshots for approximately two seconds.
It requires fresh successful ADS1115 and BMP280 samples, multiple BMP updates,
finite and plausible pressures, stable readings, and a candidate correction no
larger than plus or minus 10 kPa. Boot-time presence flags, the standard
atmosphere fallback, and stale BMP values are not acceptable calibration
references.

The candidate is:

```text
offset_kpa = average_bmp_kpa - average_nominal_map_kpa
```

The firmware writes a versioned sensor-owned NVS record before activating the
new offset. A persistence failure leaves the prior live and stored calibration
unchanged. A successful calibration resets the real-sensor peak so a peak
computed with the previous conversion does not remain visible.

The calibration record uses a separate key in the existing `boost` namespace
instead of extending `boost_config_t`; changing that blob would otherwise
require another migration and could discard existing gauge settings. The
record includes the schema/transfer version, offset, configured supply voltage,
reference MAP voltage, reference nominal MAP pressure, reference BMP pressure,
and sample count.

## Firmware and API

Sensor acquisition remains the single owner of I²C traffic. Calibration reads
published snapshots rather than issuing I²C transactions from an HTTP handler.
The sample state gains explicit ADS and BMP freshness information so stale or
synthetic data cannot be calibrated.

The API adds:

- `GET /api/v1/sensors/calibration` for live diagnostics, saved calibration,
  freshness, and configured supply voltage;
- `POST /api/v1/sensors/calibration` to validate, persist, and activate a new
  atmospheric offset;
- a Settings save path for the supply voltage that persists it without running
  atmospheric calibration.

Errors use the existing JSON `{ "error": "machine_code" }` convention. Missing
or stale sensors, unstable readings, excessive correction, and persistence
failure produce distinct errors and do not change the active calibration.
The persistent mutation remains manual and trusted-LAN only; page load never
triggers calibration.

Calibration diagnostics use the dedicated endpoint rather than enlarging the
high-rate `/state` and WebSocket payload. This avoids overflowing the smaller
WebSocket JSON buffer and lets Settings display real sensor state even while
demo mode supplies the gauge.

## I²C and cadence cleanup

The external sensor bus remains on GPIO18 SCL and GPIO17 SDA at 100 kHz.
ADS1115 and BMP280 traffic occupies only a small fraction of that bandwidth.
The sensor task changes from 20 ms to 16 ms, providing approximately 62.5 fresh
MAP updates per second to match the display path. The ADS1115 remains at
250 samples per second. BMP sampling may remain divided down because atmosphere
changes slowly, but every successful BMP update records freshness for
calibration.

Keep the durable I²C improvements:

- supported in-place `i2c_master_bus_reset()` recovery;
- the bus-administration mutex that prevents reset and scan overlap;
- four-consecutive-ACK filtering for reported scan addresses;
- recovery counters and the ordinary live address scanner.

Remove the temporary diagnosis-only behavior:

- manual GPIO boot probes at unused address `0x55`;
- fixed repeated `0x48` and `0x76` probe counters;
- the onboard touch-bus `0x5A` control probe and BSP dependency;
- boot diagnostic structures and JSON fields.

The historical bench investigation belongs in troubleshooting documentation,
while the README and regression ledger must state the final architecture and
the confirmed root cause: the replaced ADS1115 assembly, not a required
400 kHz-to-100 kHz compatibility workaround.

## Night City theme

The Night City physical gauge and browser mirror replace the synthetic
`MAP ... kPa` corner text with `ATM ... kPa` sourced directly from a fresh
BMP280 reading. When no fresh BMP reading exists, they show `ATM --kPa` rather
than presenting the 101.325 kPa fallback as a measurement. Other themes are
unchanged.

## Testing and acceptance

Automated or host-side tests cover:

- 0.619 V at 5.00 V maps to 40 kPa;
- 4.818 V at 5.00 V maps to 304 kPa;
- a 5.20 V ratiometric reading normalizes to its 5.00 V equivalent;
- the observed 1.5741 V / 98.57 kPa case at a configured 5.20 V supply produces
  approximately 96.24 kPa nominal pressure and a +2.33 kPa offset;
- recalibration replaces rather than accumulates the offset;
- missing, stale, unstable, implausible, and over-limit readings preserve the
  previous calibration;
- invalid or failed NVS persistence preserves the previous calibration;
- Settings and the mock API expose and update the agreed diagnostics;
- Night City shows measured ATM and the unavailable placeholder correctly.

The web sources are regenerated into the embedded C assets and the simulator is
used to inspect Night City. The ESP-IDF firmware must build successfully.

After OTA flashing, hardware acceptance requires:

1. both `0x48` and `0x76` remain discoverable;
2. engine-off calibration succeeds and the gauge settles near zero PSI;
3. calibration and the 5.20 V setting survive reboot;
4. recalibration does not compound the prior offset;
5. calibration is rejected when the BMP280 is disconnected or stale;
6. both sensors run without faults or recoveries during a sustained soak;
7. the 30-second physical display cadence guard reports a median of at least
   60 FPS with no display transport errors;
8. Night City shows the BMP280 atmosphere value.

