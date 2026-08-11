#include "boost_sim.h"

#include <stdint.h>
#include <math.h>

#ifdef ESP_PLATFORM
#include "esp_timer.h"
#else
#include <time.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Demo range stays inside the default face (-15..10) so the dial is exercised. */
static const float PSI_MIN = -14.5f;
static const float PSI_MAX = 9.6f;

static float s_peak;
static int64_t s_t0_us;
static bool s_fast_sweep;

/*
 * Slope magnitude (psi/s) for the fast-sweep triangle. Matches the sine-
 * envelope waveform's own measured peak TREND |dpsi/dt| (noise terms
 * excluded), computed offline by numerically differentiating the exact
 * formula below at dt=1e-4 over multiple full envelope cycles: peak was
 * 9.789 psi/s, occurring near the psi=0 crossing. Re-derive with
 * tools/bench_fast_motion.py constants if PSI_MIN/PSI_MAX or the
 * envelope shape ever change here - this is not re-derived automatically.
 */
#define FAST_SWEEP_SLEW_PSI_PER_S 9.789f
#define FAST_SWEEP_PERIOD_S \
    (2.0f * (PSI_MAX - PSI_MIN) / FAST_SWEEP_SLEW_PSI_PER_S)

static int64_t now_us(void)
{
#ifdef ESP_PLATFORM
    return esp_timer_get_time();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + (int64_t)ts.tv_nsec / 1000LL;
#endif
}

void boost_sim_init(void)
{
    s_peak = 0.0f;
    s_t0_us = now_us();
    s_fast_sweep = false;
}

void boost_sim_reset_peak(void)
{
    s_peak = 0.0f;
}

void boost_sim_set_fast_sweep(bool enabled)
{
    s_fast_sweep = enabled;
}

bool boost_sim_fast_sweep(void)
{
    return s_fast_sweep;
}

boost_sample_t boost_sim_tick(void)
{
    const int64_t now = now_us();
    const float t = (float)(now - s_t0_us) * 1e-6f;

    float psi;

    if (s_fast_sweep) {
        /* Constant-slew symmetric triangle, full PSI_MIN..PSI_MAX span, no
         * flutter - a sustained, clean isolation of the envelope waveform's
         * own peak trend slew rate (see FAST_SWEEP_SLEW_PSI_PER_S above),
         * for measuring fast-motion render cadence without waiting on or
         * hand-picking the brief fast segments of the organic sweep. */
        const float period = FAST_SWEEP_PERIOD_S;
        const float tt = fmodf(t, period);
        const float half = period * 0.5f;
        const float frac = (tt < half) ? (tt / half) : (2.0f - tt / half);
        psi = PSI_MIN + (PSI_MAX - PSI_MIN) * frac;
    } else {
        /*
         * Idle → spool → pull → lift: layered sines so the needle
         * doesn't look like a pure triangle generator.
         * Period ~7.5 s full cycle.
         */
        const float phase = t * (2.0f * (float)M_PI / 7.5f);
        const float envelope = 0.55f + 0.45f * sinf(phase * 0.5f + 0.4f);
        float norm = 0.5f + 0.5f * sinf(phase);
        norm = norm * envelope;
        /* Bias toward positive boost so the gauge spends time "on boost". */
        norm = norm * 0.92f + 0.08f;

        psi = PSI_MIN + (PSI_MAX - PSI_MIN) * norm;

        /* Tiny high-frequency flutter (manifold noise feel). */
        psi += 0.18f * sinf(t * 17.3f) + 0.08f * sinf(t * 31.1f);
    }

    if (psi < PSI_MIN) {
        psi = PSI_MIN;
    } else if (psi > PSI_MAX) {
        psi = PSI_MAX;
    }

    if (psi > s_peak) {
        s_peak = psi;
    }

    boost_sample_t s = {
        .psi = psi,
        .peak_psi = s_peak,
        .demo = true,
    };
    return s;
}
