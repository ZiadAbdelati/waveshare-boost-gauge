#include "boost_sim.h"

#include <math.h>

#include "esp_timer.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Demo range roughly matches a GM 3-bar MAP on a street turbo. */
static const float PSI_MIN = -14.5f;
static const float PSI_MAX = 22.0f;

static float s_peak;
static int64_t s_t0_us;

void boost_sim_init(void)
{
    s_peak = 0.0f;
    s_t0_us = esp_timer_get_time();
}

void boost_sim_reset_peak(void)
{
    s_peak = 0.0f;
}

boost_sample_t boost_sim_tick(void)
{
    const int64_t now = esp_timer_get_time();
    const float t = (float)(now - s_t0_us) * 1e-6f;

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

    float psi = PSI_MIN + (PSI_MAX - PSI_MIN) * norm;

    /* Tiny high-frequency flutter (manifold noise feel). */
    psi += 0.18f * sinf(t * 17.3f) + 0.08f * sinf(t * 31.1f);

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
