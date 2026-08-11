#!/usr/bin/env python3
"""Deterministic host model of the firmware's Neon Segments visible-change demand.

This tool PREDICTS DEMAND ONLY. It counts how often a 16 ms demo sample would
invalidate the Neon Segments readout / lit-segment ring / zone word on the
ESP32-S3 panel, and coalesces those change events into the panel's measured
16.753 ms refresh periods. It does NOT model rasterisation cost, LVGL draw-task
throughput, TE waits, regionDBuf, or any other renderer capacity. A "demand
cycle" here means "at least one visible component changed during a refresh
period", NOT "the panel rendered at 60 FPS". Numbers produced by this script
must never be presented as hardware measurements: they are host-side
predictions derived from source constants (main/boost_sim.c,
main/boost_gauge.c, main/boost_neon_geom.c) and the cadence contract in
AGENTS.md. Any output that quotes these numbers must say so.

Model contract (re-verified against the source before every run by
_verify_source_constants(); the run aborts if a constant drifted):
  * organic demo waveform: exact boost_sim.c envelope + flutter formula
  * constant-slew demo waveform: boost_sim.c fast-sweep triangle, 9.789 psi/s
  * 16 ms sample period (gauge update cadence)
  * default gauge range -15..10 psi, zeroAngle 236.25, ARC_START 135,
    ARC_RANGE 270, NEON_NSEG 54 (5 deg per segment)
  * boost_neon_lit_span() half-segment threshold and baked zero-segment
    exclusion, ported from boost_neon_geom.c
  * one-decimal readout rounding exactly as C lroundf() of |psi| * 10
  * zone thresholds: vacuum <= 0.05, boost > 0.05, overboost >= 8.0
  * changed sample ticks coalesced into 16.753 ms panel refresh periods
    (the measured TE period from the region-dbuf row of the AGENTS ledger)

The firmware computes in C `float`, so the waveform, sweep mapping and lit-span
math are rounded to IEEE float32 at each operation (f32()). The remaining
precision gap versus the device is libm sinf(): Python's double-precision
math.sin() rounded to float32 is within ~1 ulp of the firmware's libm, which
can only flip a decision for a sample that sits within ~1 ulp of a threshold
(segment boundary, 0.05/8.0 zone edge, or a .x5 readout carry) - bounded and
documented as a residual mismatch risk, not silent.
"""

import argparse
import json
import math
import re
import statistics
import struct
import sys
from pathlib import Path

# --------------------------------------------------------------------------
# C float32 semantics: the firmware stores every intermediate in `float`.
# --------------------------------------------------------------------------
def f32(x):
    """Round a Python float to IEEE-754 binary32 (the C `float` type)."""
    return struct.unpack('f', struct.pack('f', float(x)))[0]


def c_lroundf_nonneg(x):
    """C lroundf(): round-half-away-from-zero to nearest int. x is nonnegative."""
    return int(math.floor(float(x) + 0.5))


# --------------------------------------------------------------------------
# Waveform constants, copied verbatim from main/boost_sim.c.
# _verify_source_constants() aborts if any of these drift from the source.
# --------------------------------------------------------------------------
PSI_MIN = -14.5
PSI_MAX = 9.6
FAST_SWEEP_SLEW_PSI_PER_S = 9.789
FAST_SWEEP_PERIOD_S = 2.0 * (PSI_MAX - PSI_MIN) / FAST_SWEEP_SLEW_PSI_PER_S
ORGANIC_PERIOD_S = 7.5
ENV_DC = 0.55
ENV_AMP = 0.45
ENV_PHASE_OFF = 0.4
NORM_DC = 0.5
NORM_BIAS_MUL = 0.92
NORM_BIAS_ADD = 0.08
FLUTTER_1_AMP = 0.18
FLUTTER_1_FREQ = 17.3
FLUTTER_2_AMP = 0.08
FLUTTER_2_FREQ = 31.1

# --------------------------------------------------------------------------
# Gauge defaults, copied from main/boost_gauge.c (default range and dial).
# --------------------------------------------------------------------------
S_PSI_MIN = -15.0
S_PSI_MAX = 10.0
S_ZERO_ANGLE = 236.25
ARC_START = 135.0
ARC_END = 45.0
ARC_RANGE = 270.0
NEON_NSEG = 54
S_PSI_OVERBOOST = 8.0
ZONE_EPS = 0.05   # psi > 0.05 -> boost (neon_zone_id()/neon_zone_rgb())

# --------------------------------------------------------------------------
# Cadence inputs (AGENTS.md cadence contract / measured panel period).
# --------------------------------------------------------------------------
SAMPLE_S = 0.016          # 16 ms gauge update cadence
PANEL_S = 0.016753        # measured TE period, region-dbuf ledger row


# --------------------------------------------------------------------------
# Waveforms, mirroring main/boost_sim.c::boost_sim_tick().
# --------------------------------------------------------------------------
def sim_organic_psi(t):
    """Organic demo waveform: layered sines + flutter, clamped, f32-rounded."""
    t = f32(t)
    omega = f32(f32(f32(2.0) * f32(math.pi)) / f32(ORGANIC_PERIOD_S))
    phase = f32(t * omega)
    envelope = f32(ENV_DC + f32(ENV_AMP * f32(math.sin(f32(f32(phase * 0.5) + ENV_PHASE_OFF)))))
    norm = f32(NORM_DC + f32(NORM_DC * f32(math.sin(phase))))
    norm = f32(norm * envelope)
    norm = f32(f32(norm * NORM_BIAS_MUL) + NORM_BIAS_ADD)
    psi = f32(f32(PSI_MIN) + f32(f32(PSI_MAX - PSI_MIN) * norm))
    psi = f32(psi + f32(f32(FLUTTER_1_AMP * f32(math.sin(f32(t * FLUTTER_1_FREQ)))) +
                        f32(FLUTTER_2_AMP * f32(math.sin(f32(t * FLUTTER_2_FREQ))))))
    if psi < PSI_MIN:
        return f32(PSI_MIN)
    if psi > PSI_MAX:
        return f32(PSI_MAX)
    return psi


def sim_fast_psi(t):
    """Constant-slew symmetric triangle, 9.789 psi/s, full PSI_MIN..PSI_MAX span."""
    t = f32(t)
    period = f32(f32(f32(2.0) * f32(PSI_MAX - PSI_MIN)) / f32(FAST_SWEEP_SLEW_PSI_PER_S))
    tt = f32(math.fmod(t, period))
    half = f32(period * 0.5)
    frac = f32(tt / half) if tt < half else f32(2.0 - f32(tt / half))
    return f32(f32(PSI_MIN) + f32(f32(PSI_MAX - PSI_MIN) * frac))


# --------------------------------------------------------------------------
# Sweep mapping: psi_to_sweep() from main/boost_gauge.c (default range).
# --------------------------------------------------------------------------
def psi_to_sweep(psi, a0, a1):
    psi = f32(min(max(psi, S_PSI_MIN), S_PSI_MAX))
    span = f32(a1 - a0)
    zero_at = f32(a0 + f32(f32(f32(S_ZERO_ANGLE - ARC_START) / f32(ARC_RANGE)) * span))
    if psi < 0.0:
        d = f32(0.0 - S_PSI_MIN)
        t = f32(f32(psi - S_PSI_MIN) / d) if d > 0.0 else f32(1.0)
        return f32(a0 + f32(t * f32(zero_at - a0)))
    t = f32(psi / S_PSI_MAX) if S_PSI_MAX > 0.0 else f32(0.0)
    return f32(zero_at + f32(t * f32(a1 - zero_at)))


def neon_seg_index(angle):
    """Segment index carrying an angle, or -1 (boost_gauge.c::neon_seg_index())."""
    step = f32(ARC_RANGE / NEON_NSEG)
    i = int(math.floor(f32((angle - ARC_START) / step)))
    return i if 0 <= i < NEON_NSEG else -1


# --------------------------------------------------------------------------
# Lit-span and painted-segment helpers, ported from boost_neon_geom.c.
# --------------------------------------------------------------------------
def lit_span(a_zero, a_value, a_start, span, nseg):
    """boost_neon_lit_span(): floor-indexed whole-segment run, half-segment
    threshold, clamped. Returns (first, last) or None when nothing is lit."""
    if nseg <= 0 or span <= 0.0:
        return None
    step = f32(span / nseg)
    lo, hi = (a_zero, a_value) if a_zero <= a_value else (a_value, a_zero)
    if f32(hi - lo) < f32(step * 0.5):
        return None
    i0 = int(math.floor(f32((lo - a_start) / step)))
    i1 = int(math.floor(f32((hi - a_start) / step)))
    i0 = max(i0, 0)
    i1 = min(i1, nseg - 1)
    if i1 < i0:
        return None
    return (i0, i1)


def _zero_seg(a_zero, a_start, span, nseg):
    """Baked zero-marker segment, clamped like boost_neon_lit_span()."""
    step = f32(span / nseg)
    z = int(math.floor(f32((a_zero - a_start) / step)))
    return max(0, min(z, nseg - 1))


def _painted(a_zero, a_value, a_start, span, nseg):
    """Painted (non-zero-marker) segment set of a zero-rooted lit run."""
    rng = lit_span(a_zero, a_value, a_start, span, nseg)
    if rng is None:
        return set()
    first, last = rng
    z = _zero_seg(a_zero, a_start, span, nseg)
    if z == first and first == last:
        return set()
    if z == first:
        return set(range(first + 1, last + 1))
    if z == last:
        return set(range(first, last))
    return set(range(first, last + 1))


def seg_diff(a_zero, a_old, a_new, a_start, span, nseg):
    """boost_neon_seg_diff(): symmetric difference of the painted (baked-zero-
    excluded) old and new segment sets - the segments whose painted state
    changed. Returns the set of changed segment indices."""
    if nseg <= 0 or span <= 0.0:
        return set()
    return _painted(a_zero, a_old, a_start, span, nseg) ^ \
           _painted(a_zero, a_new, a_start, span, nseg)


# --------------------------------------------------------------------------
# Zone and readout visible state.
# --------------------------------------------------------------------------
def zone_id(psi):
    """neon_zone_id(): 2 overboost, 1 boost, 0 vacuum."""
    return 2 if psi >= S_PSI_OVERBOOST else (1 if psi > ZONE_EPS else 0)


def readout_tt(psi):
    """lroundf(fabsf(psi) * 10.0f) - the one-decimal digit content."""
    return c_lroundf_nonneg(f32(f32(abs(psi)) * 10.0))


# --------------------------------------------------------------------------
# Source-constant verification: abort if any modelled constant drifted.
# --------------------------------------------------------------------------
def _verify_source_constants(root):
    sim = (root / 'main' / 'boost_sim.c').read_text(encoding='utf-8')
    gauge = (root / 'main' / 'boost_gauge.c').read_text(encoding='utf-8')
    geom = (root / 'main' / 'boost_neon_geom.c').read_text(encoding='utf-8')
    checks = [
        (sim, r'PSI_MIN\s*=\s*-14\.5f', 'boost_sim.c: PSI_MIN = -14.5f'),
        (sim, r'PSI_MAX\s*=\s*9\.6f', 'boost_sim.c: PSI_MAX = 9.6f'),
        (sim, r'FAST_SWEEP_SLEW_PSI_PER_S\s+9\.789f',
         'boost_sim.c: FAST_SWEEP_SLEW_PSI_PER_S = 9.789f'),
        (sim, r'0\.55f\s*\+\s*0\.45f\s*\*\s*sinf',
         'boost_sim.c: envelope 0.55 + 0.45*sin'),
        (sim, r'0\.4f\b', 'boost_sim.c: envelope phase offset 0.4'),
        (sim, r'0\.5f\s*\+\s*0\.5f\s*\*\s*sinf', 'boost_sim.c: norm 0.5 + 0.5*sin'),
        (sim, r'\*\s*0\.92f\s*\+\s*0\.08f', 'boost_sim.c: norm bias *0.92 + 0.08'),
        (sim, r'0\.18f\s*\*\s*sinf\(t\s*\*\s*17\.3f\)\s*\+\s*0\.08f\s*\*\s*sinf\(t\s*\*\s*31\.1f\)',
         'boost_sim.c: flutter 0.18*sin(17.3t) + 0.08*sin(31.1t)'),
        (sim, r'M_PI\s*/\s*7\.5f', 'boost_sim.c: organic period 7.5 s'),
        (gauge, r'DEFAULT_PSI_MIN\s*\(\s*-15\.0f\s*\)', 'boost_gauge.c: DEFAULT_PSI_MIN -15.0'),
        (gauge, r'DEFAULT_PSI_MAX\s*\(\s*10\.0f\s*\)', 'boost_gauge.c: DEFAULT_PSI_MAX 10.0'),
        (gauge, r'DEFAULT_ZERO_ANGLE\s+236\.25f', 'boost_gauge.c: DEFAULT_ZERO_ANGLE 236.25'),
        (gauge, r'ARC_START\s+135\b', 'boost_gauge.c: ARC_START 135'),
        (gauge, r'ARC_RANGE\s+270\b', 'boost_gauge.c: ARC_RANGE 270'),
        (gauge, r'NEON_NSEG\s+54\b', 'boost_gauge.c: NEON_NSEG 54'),
        (gauge, r'DEFAULT_PSI_OVERBOOST\s*\(\s*8\.0f\s*\)',
         'boost_gauge.c: DEFAULT_PSI_OVERBOOST 8.0'),
        (geom, r'step\s*\*\s*0\.5f', 'boost_neon_geom.c: half-segment threshold'),
    ]
    for src, pattern, name in checks:
        if not re.search(pattern, src):
            sys.exit('ERROR: source constant drifted from the model contract: %s\n'
                     'Refusing to run - fix the model or the source.' % name)
    return True


# --------------------------------------------------------------------------
# Model run: 16 ms samples -> component changes -> 16.753 ms refresh periods.
# --------------------------------------------------------------------------
def run_waveform(seconds, fast_sweep):
    peak = 0.0
    prev_psi = None
    prev_zone = prev_tt = prev_sign = None
    prev_peak_idx = prev_peak_zone = prev_peak_in_run = None

    a_zero = psi_to_sweep(0.0, ARC_START, ARC_START + ARC_RANGE)

    periods = {}      # period_idx -> {'readout','segments','zone' : bool}
    sec_cycles = {}   # sec_idx -> demand cycles attributed to that second
    sec_comp = {}     # sec_idx -> per-component cycle counts
    sec_vel_sum = {}
    sec_vel_n = {}

    n_samples = int(math.ceil(seconds / SAMPLE_S))
    for i in range(n_samples):
        t = i * SAMPLE_S
        psi = sim_fast_psi(t) if fast_sweep else sim_organic_psi(t)
        peak = max(peak, psi)
        sec_idx = int(t)

        if prev_psi is not None:
            vel = abs(psi - prev_psi) / SAMPLE_S
            sec_vel_sum[sec_idx] = sec_vel_sum.get(sec_idx, 0.0) + vel
            sec_vel_n[sec_idx] = sec_vel_n.get(sec_idx, 0) + 1

            zone = zone_id(psi)
            tt = readout_tt(psi)
            sign = (psi < 0.0) and (tt != 0)

            zone_change = (zone != prev_zone)
            readout_change = (tt != prev_tt) or (sign != prev_sign)

            a_old = psi_to_sweep(prev_psi, ARC_START, ARC_START + ARC_RANGE)
            a_new = psi_to_sweep(psi, ARC_START, ARC_START + ARC_RANGE)
            changed = seg_diff(a_zero, a_old, a_new, ARC_START, ARC_RANGE, NEON_NSEG)
            old_painted = bool(_painted(a_zero, a_old, ARC_START, ARC_RANGE, NEON_NSEG))
            new_painted = bool(_painted(a_zero, a_new, ARC_START, ARC_RANGE, NEON_NSEG))
            side_flip = (prev_psi < 0.0) != (psi < 0.0)

            # Peak tell-tale, drawn as a segment overlay on the ring.
            peak_value = max(peak, 0.0)
            peak_idx = neon_seg_index(psi_to_sweep(peak_value, ARC_START,
                                                   ARC_START + ARC_RANGE)) if peak_value > 0.2 else -1
            peak_zone = zone_id(peak_value)
            pspan = lit_span(a_zero, a_new, ARC_START, ARC_RANGE, NEON_NSEG)
            peak_in_run = (pspan is not None and pspan[0] <= peak_idx <= pspan[1])
            peak_changed = (peak_idx != prev_peak_idx or
                            peak_zone != prev_peak_zone or
                            peak_in_run != prev_peak_in_run)

            # Segments visibly change when the XOR of painted runs is non-empty,
            # or a zone/side flip repaints a run that actually has lit segments
            # (a zone flip with both runs empty recolours only the word/readout).
            segments_change = (len(changed) > 0 or peak_changed or
                               (side_flip or zone_change) and (old_painted or new_painted))

            if readout_change or segments_change or zone_change:
                d = periods.setdefault(int(t / PANEL_S),
                                       {'readout': False, 'segments': False, 'zone': False})
                d['readout'] = d['readout'] or readout_change
                d['segments'] = d['segments'] or segments_change
                d['zone'] = d['zone'] or zone_change

            prev_zone, prev_tt, prev_sign = zone, tt, sign
            prev_peak_idx, prev_peak_zone, prev_peak_in_run = peak_idx, peak_zone, peak_in_run
        else:
            prev_zone = zone_id(psi)
            prev_tt = readout_tt(psi)
            prev_sign = (psi < 0.0) and (prev_tt != 0)
            peak_value = max(peak, 0.0)
            prev_peak_idx = neon_seg_index(psi_to_sweep(peak_value, ARC_START,
                                                        ARC_START + ARC_RANGE)) if peak_value > 0.2 else -1
            prev_peak_zone = zone_id(peak_value)
            pspan = lit_span(a_zero, psi_to_sweep(psi, ARC_START, ARC_START + ARC_RANGE),
                             ARC_START, ARC_RANGE, NEON_NSEG)
            prev_peak_in_run = (pspan is not None and pspan[0] <= prev_peak_idx <= pspan[1])

        prev_psi = psi

    total_periods = int(math.floor(seconds / PANEL_S)) + 1
    comp = {'readout': 0, 'segments': 0, 'zone': 0}
    cycles = 0
    for p in range(total_periods):
        d = periods.get(p)
        if d is None:
            continue
        if d['readout'] or d['segments'] or d['zone']:
            cycles += 1
            sec = int(p * PANEL_S)
            sec_cycles[sec] = sec_cycles.get(sec, 0) + 1
            for k in comp:
                if d[k]:
                    comp[k] += 1

    all_sec = list(range(int(seconds)))
    per_sec = [sec_cycles.get(s, 0) for s in all_sec]
    velocities = []
    for s in all_sec:
        vsum = sec_vel_sum.get(s, 0.0)
        n = sec_vel_n.get(s, 0)
        velocities.append(vsum / n if n else 0.0)

    return {
        'duration_s': seconds,
        'samples': n_samples,
        'refresh_periods': total_periods,
        'demand_cycles_total': cycles,
        'demand_fraction_of_periods': cycles / total_periods,
        'overall_per_second': _stats(per_sec),
        'slowest_velocity_quartile': _quartile(per_sec, velocities, low=True),
        'fastest_velocity_quartile': _quartile(per_sec, velocities, low=False),
        'component_cycles': comp,
        'mean_velocity_psi_per_s': sum(velocities) / len(velocities) if velocities else 0.0,
    }


def _stats(vals):
    return {
        'min': min(vals),
        'median': round(statistics.median(vals), 2),
        'max': max(vals),
    }


def _quartile(per_sec, velocities, low):
    n = len(per_sec)
    if n == 0:
        return None
    k = int(math.ceil(n / 4.0))
    order = sorted(range(n), key=lambda s: velocities[s])
    idx = order[:k] if low else order[-k:]
    vmean = sum(velocities[s] for s in idx) / len(idx)
    res = _stats([per_sec[s] for s in idx])
    res['mean_velocity_psi_per_s'] = round(vmean, 3)
    return res


# --------------------------------------------------------------------------
# Reporting.
# --------------------------------------------------------------------------
def _fmt_stats(st, indent):
    return ('%smin %s | median %s | max %s' %
            (indent, st['min'], st['median'], st['max']))


def _fmt_waveform(label, wf):
    lines = []
    lines.append('[%s]' % label)
    lines.append('  duration / samples / refresh periods : %.1f s / %d / %d'
                 % (wf['duration_s'], wf['samples'], wf['refresh_periods']))
    lines.append('  demand cycles total                   : %d (%.1f%% of refresh periods)'
                 % (wf['demand_cycles_total'], wf['demand_fraction_of_periods'] * 100.0))
    lines.append('  per-second demand cycles              : ' +
                 _fmt_stats(wf['overall_per_second'], ''))
    sq, fq = wf['slowest_velocity_quartile'], wf['fastest_velocity_quartile']
    lines.append('  slowest-velocity quartile             : ' +
                 _fmt_stats(sq, '') +
                 '   (mean |dpsi/dt| %.3f psi/s)' % sq['mean_velocity_psi_per_s'])
    lines.append('  fastest-velocity quartile             : ' +
                 _fmt_stats(fq, '') +
                 '   (mean |dpsi/dt| %.3f psi/s)' % fq['mean_velocity_psi_per_s'])
    lines.append('  component cycles (coalesced)          : ' +
                 'readout %d | segments %d | zone %d'
                 % (wf['component_cycles']['readout'],
                    wf['component_cycles']['segments'],
                    wf['component_cycles']['zone']))
    lines.append('  mean |dpsi/dt| overall                : %.3f psi/s'
                 % wf['mean_velocity_psi_per_s'])
    return '\n'.join(lines)


def main(argv=None):
    parser = argparse.ArgumentParser(
        description='Host-side prediction of Neon Segments visible-change '
                    'demand from the boost_sim.c demo waveforms. PREDICTS '
                    'DEMAND, not renderer capacity; not a hardware measurement.')
    parser.add_argument('--seconds', type=float, default=120,
                        help='duration of each waveform run in seconds (default 120)')
    parser.add_argument('--json', action='store_true',
                        help='emit JSON on stdout instead of the text report')
    args = parser.parse_args(argv)

    root = Path(__file__).resolve().parent.parent
    if not (root / 'main' / 'boost_sim.c').is_file():
        sys.exit('ERROR: repo root not found next to this script (%s)' % root)
    _verify_source_constants(root)

    organic = run_waveform(args.seconds, fast_sweep=False)
    fast = run_waveform(args.seconds, fast_sweep=True)

    if args.json:
        out = {
            'model_meta': {
                'disclaimer': ('HOST PREDICTION of visible-change demand; '
                               'not renderer capacity and not a hardware measurement.'),
                'sample_s': SAMPLE_S,
                'panel_s': PANEL_S,
                'psi_min': S_PSI_MIN,
                'psi_max': S_PSI_MAX,
                'zero_angle': S_ZERO_ANGLE,
                'arc_start': ARC_START,
                'arc_range': ARC_RANGE,
                'nseg': NEON_NSEG,
                'overboost': S_PSI_OVERBOOST,
                'zone_eps': ZONE_EPS,
                'fast_slew_psi_per_s': FAST_SWEEP_SLEW_PSI_PER_S,
                'source_verified': True,
            },
            'organic': organic,
            'constant_slew': fast,
        }
        print(json.dumps(out, indent=2, sort_keys=True))
        return 0

    print('Neon Segments visible-change demand model')
    print('HOST PREDICTION ONLY - predicts render/refresh DEMAND from demo')
    print('waveforms, NOT renderer capacity; NOT a hardware measurement.')
    print('')
    print('source constants verified against main/boost_sim.c, main/boost_gauge.c,')
    print('main/boost_neon_geom.c: OK')
    print('')
    print(_fmt_waveform('organic demo waveform (boost_sim.c envelope + flutter)', organic))
    print('')
    print(_fmt_waveform('constant-slew demo waveform (boost_sim.c fast sweep, '
                        '9.789 psi/s)', fast))
    print('')
    print('Interpretation: demand cycles are the number of 16.753 ms panel refresh')
    print('periods in which at least one component (readout / segments / zone)')
    print('visibly changed. Idle periods need no render. Do not report these')
    print('numbers as hardware FPS or renderer capacity.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
