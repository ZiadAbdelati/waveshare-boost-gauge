package com.boostgauge.app.ui

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class PressureReferenceTest {

    @Test
    fun relativeLeavesGaugeUntouched() {
        assertEquals(3.42, boostDisplayPsi(3.42, 101.325, absolute = false), 1e-9)
        assertEquals(-2.0, boostDisplayPsi(-2.0, null, absolute = false), 1e-9)
    }

    @Test
    fun absoluteAddsAmbientReference() {
        // absolute = gauge + ambientKpa * 0.145037738 (exact contract factor).
        assertEquals(1.0 + 101.325 * 0.145037738, boostDisplayPsi(1.0, 101.325, absolute = true), 1e-9)
    }

    @Test
    fun absoluteFallsBackToStandardAtmosphereWhenAmbientMissingOrNonPositive() {
        val standard = 101.325 * 0.145037738 // 14.6959 psi
        assertEquals(0.0 + standard, boostDisplayPsi(0.0, null, absolute = true), 1e-9)
        assertEquals(0.0 + standard, boostDisplayPsi(0.0, 0.0, absolute = true), 1e-9)
        assertEquals(0.0 + standard, boostDisplayPsi(0.0, -5.0, absolute = true), 1e-9)
    }

    @Test
    fun stateMirrorsTheAdoptedFlag() {
        val state = PressureReferenceState()
        assertFalse(state.absolute.value)
        state.apply(true)
        assertTrue(state.absolute.value)
    }
}
