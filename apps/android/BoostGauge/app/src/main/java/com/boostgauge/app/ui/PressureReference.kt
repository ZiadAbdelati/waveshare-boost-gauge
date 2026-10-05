package com.boostgauge.app.ui

import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

/** Standard-atmosphere reference (kPa) used when the ambient read is missing or non-positive. */
const val STANDARD_ATMOSPHERE_KPA = 101.325

/** kPa -> psi (exact reference-contract factor). */
private const val PSI_PER_KPA = 0.145037738

/**
 * Process-wide mirror of the device's /themes `pressureAbsolute`. Unlike the
 * unit (which /state also carries), the reference has no /state field, so a
 * `/themes` payload is the only adoption path.
 *
 * `false` = relative (gauge) psi — today's behaviour; `true` = absolute,
 * i.e. displayed = gauge + the ambient reference. Canonical wire/geometry
 * values stay gauge PSI; the reference is added only when a value becomes text.
 */
class PressureReferenceState(initial: Boolean = false) {
    private val _absolute = MutableStateFlow(initial)
    val absolute: StateFlow<Boolean> = _absolute.asStateFlow()

    fun apply(value: Boolean) {
        if (_absolute.value != value) _absolute.value = value
    }
}

/**
 * Boost-side display psi under [absolute]: gauge psi unchanged in relative
 * mode, or gauge + the reference in absolute mode. [ambientKpa] is the EXISTING
 * `/state.sensors.ambientKpa`; missing or non-positive values fall back to the
 * standard atmosphere (101.325 kPa -> 14.6959 psi). Pure — callers pass only
 * the numeral they render; no wire, threshold, comparison or geometry value is
 * ever routed through this.
 */
fun boostDisplayPsi(gaugePsi: Double, ambientKpa: Double?, absolute: Boolean): Double {
    if (!absolute) return gaugePsi
    val kpa = ambientKpa?.takeIf { it > 0.0 } ?: STANDARD_ATMOSPHERE_KPA
    return gaugePsi + kpa * PSI_PER_KPA
}
