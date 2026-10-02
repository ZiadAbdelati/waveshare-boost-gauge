package com.boostgauge.app.ui

import kotlin.math.roundToInt
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

/**
 * Presentation-only pressure unit (contract v1). Canonical wire and sensor
 * data are always PSI — the raw sensor kPa diagnostics are a different domain
 * and never convert — so this enum exists purely for the display and input
 * boundaries.
 *
 * Two deliberate byte-compat choices:
 *  - [suffix] stays lowercase "psi" (not the gauge-face "PSI") so the default
 *    rendering is byte-identical to the pre-unit build on both companion apps.
 *  - PSI keeps each call site's historical decimals via [format]'s
 *    `psiDecimals`; the contract's per-unit decimals apply to bar/kPa.
 */
enum class PressureUnit(
    /** Wire token carried by the /themes `pressureUnit` field. */
    val wire: String,
    /**
     * Native readout suffix. PSI stays lowercase "psi" (the contract's "PSI"
     * is the panel unit mark) so the default rendering is byte-identical.
     */
    val suffix: String,
    /** Decimals for converted units; PSI keeps per-site precision instead. */
    val decimals: Int,
    private val factor: Double,
) {
    PSI("psi", "psi", 1, 1.0),
    BAR("bar", "bar", 2, 0.0689475729),
    KPA("kPa", "kPa", 0, 6.89475729);

    /** Missing-reading placeholder matching this unit's precision. */
    val placeholder: String
        get() = when (decimals) {
            2 -> "--.--"
            0 -> "--"
            else -> "--.-"
        }

    /** Canonical PSI -> this unit (display boundary). */
    fun fromPsi(psi: Double): Double = psi * factor

    /** This unit -> canonical PSI (input boundary); exact inverse of [fromPsi]. */
    fun toPsi(value: Double): Double = value / factor

    /**
     * Formats a canonical PSI value in this unit. While the unit is PSI the
     * call site's historical [psiDecimals] are kept; bar/kPa always use
     * [decimals].
     */
    fun format(psi: Double, psiDecimals: Int = decimals): String =
        Format.fmt(fromPsi(psi), if (this == PSI) psiDecimals else decimals)

    /** Chart tick numerals: PSI keeps the historical 0/decimal rule, converted units use their decimals. */
    fun formatTick(psiValue: Double): String = when {
        this != PSI -> Format.fmt(fromPsi(psiValue), decimals)
        psiValue == 0.0 -> "0"
        psiValue == psiValue.roundToInt().toDouble() -> Format.fmt(psiValue, 0)
        else -> Format.fmt(psiValue, 1)
    }

    companion object {
        val default: PressureUnit = PSI

        /** Absent/unknown wire values fall back to PSI (the firmware default). */
        fun fromWire(wire: String?): PressureUnit =
            entries.firstOrNull { it.wire.equals(wire, ignoreCase = true) } ?: default
    }
}

/**
 * Process-wide mirror of the device's /themes `pressureUnit`. The device owns
 * the setting; this mirrors it so every screen renders the same unit without
 * each view model fetching /themes. Written only from an adopted /themes
 * payload (settings load/save fold-back, dashboard refresh).
 */
class PressureUnitState(initial: PressureUnit = PressureUnit.default) {
    private val _unit = MutableStateFlow(initial)
    val unit: StateFlow<PressureUnit> = _unit.asStateFlow()

    fun apply(wire: String?) {
        _unit.value = PressureUnit.fromWire(wire)
    }
}
