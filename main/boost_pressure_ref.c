#include "boost_pressure_ref.h"

#include "boost_neon_geom.h"      /* boost_readout_display_psi: the ONE fold band */
#include "boost_theme.h"          /* the persisted display mode */
#include "boost_tpms_protocol.h"  /* BOOST_TPMS_KPA_TO_PSI: the ONE kpa->psi factor */

static float s_atmosphere_kpa = BOOST_STANDARD_ATM_KPA;
/* Sim/test override; <= 0 means "use the published atmospheric baseline". */
static float s_override_kpa;

bool boost_pressure_ref_absolute(void)
{
    return boost_theme_pressure_absolute();
}

float boost_pressure_ref_atmosphere_kpa(void)
{
    return (s_override_kpa > 0.0f) ? s_override_kpa : s_atmosphere_kpa;
}

float boost_pressure_ref_psi(void)
{
    return boost_pressure_ref_atmosphere_kpa() * BOOST_TPMS_KPA_TO_PSI;
}

void boost_pressure_ref_update(float ambient_kpa, bool ambient_is_fallback)
{
    /* `ambient_kpa > 0.0f` is also the NaN guard: a comparison against NaN is
     * false, so a corrupt reading lands on the standard atmosphere. */
    if (ambient_is_fallback || !(ambient_kpa > 0.0f)) {
        s_atmosphere_kpa = BOOST_STANDARD_ATM_KPA;
    } else {
        s_atmosphere_kpa = ambient_kpa;
    }
}

void boost_pressure_ref_set_atmosphere_kpa(float kpa)
{
    s_override_kpa = (kpa > 0.0f) ? kpa : 0.0f;
}

float boost_pressure_ref_display(float gauge_psi, bool fold_deadband)
{
    if (fold_deadband) gauge_psi = boost_readout_display_psi(gauge_psi);
    if (!boost_theme_pressure_absolute()) return gauge_psi;
    return gauge_psi + boost_pressure_ref_psi();
}

float boost_pressure_ref_from_psi(boost_unit_t unit, float gauge_psi, bool fold_deadband)
{
    return boost_units_from_psi(unit, boost_pressure_ref_display(gauge_psi, fold_deadband));
}

void boost_pressure_ref_format(boost_unit_t unit, float gauge_psi, bool fold_deadband,
                               char *out, size_t out_len)
{
    /* The fold already happened inside boost_pressure_ref_display(). */
    boost_units_format(unit, boost_pressure_ref_display(gauge_psi, fold_deadband),
                       false, out, out_len);
}

void boost_pressure_ref_format_tick(boost_unit_t unit, float gauge_psi,
                                    char *out, size_t out_len)
{
    /* Dial numerals never fold (they are not a live reading). */
    boost_units_format_tick(unit, boost_pressure_ref_display(gauge_psi, false), out, out_len);
}
