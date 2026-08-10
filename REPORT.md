# Vault-Tec optimization — attempt 2 (opt/vault-tec)

Task: reduce vault-tec organic-demo cost / worst cycles without any visual change.
Host audit is the gate: `sim\build\boost_gauge_sim.exe --audit --theme vault-tec --seconds 25`.

## Protocol log

### 1. Baseline (unmodified HEAD aafcfcf)
Command:
`& C:\Users\aliab\boost-gauge-vault-opt\sim\build\boost_gauge_sim.exe --audit --theme vault-tec --seconds 25`

Result (baseline_audit.log):
- render cycles: 1517 of 1550 samples
- cycle ms: p50 0.55, p90 0.70, max 1.23, over-16ms 0
- top-5 px cycles: (30114) (17411) (17411) (17107) (17002)
- flushed px/cycle: mean **10786**, max **30114**
- throughput: 0.660 Mpx/s at 62.5 Hz
- severe mismatches: **0 px**
- stale-pixel check: 517 compares, 6 with any mismatch, total 6 px, worst 1 px
  (all six are isolated 1-step AA seams at needle-edge radii — the documented
  baseline condition; ledger: "six isolated one-step AA seams over 517 comparisons")
- exit code 4 (stale_frames != 0) — same as the accepted baseline in the ledger.

Baseline accepted: 0 severe, 6 isolated 1-step AA seams, mean 10786.

### 2. Change 1: per-slice needle invalidation pad (tapered wedge)
Only `main/boost_gauge.c` (vault region) touched.

Rationale: `invalidate_vault_needle()` applied one flat pad
`VAULT_NEEDLE_HALFW + 2 = 9 px` to all three slices of the swept needle, but
the wedge is tapered — half-width shrinks linearly from 7 px at the base to
2.5 px at the tip. The corner bbox of each slice only spans the swept
centerline, so the flat pad over-covered the outer slices' air by up to
~3 px/side (slice 2's ink is <=4 px wide at its inner radius). The pad now uses
the local half-width at the slice's inner radius `hw(ra) + 2` (same 2 px AA
margin as before), shared through a new `VAULT_NEEDLE_TIP_HALF` constant that
the draw path's `tipw` also uses, so draw and invalidation cannot drift.

- Slice 0 (base): hw = 7 -> pad 9 (unchanged, hub fold untouched: effective
  hub coverage stays HUB_R + 2).
- Slice 1: pad 9 -> 7.5
- Slice 2: pad 9 -> 6
- `tipw` in `draw_vault_needle()` now reads `VAULT_NEEDLE_TIP_HALF` (2.5f,
  value-identical).

Build: `ninja -C sim\build` -> [1/2] compile boost_gauge.c, [2/2] link, no errors.
Audit: running (audit1.log).

