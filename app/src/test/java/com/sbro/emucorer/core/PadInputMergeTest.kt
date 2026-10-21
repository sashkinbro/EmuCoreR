package com.sbro.emucorer.core

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Test

class PadInputMergeTest {
    private val crossBit = 14
    private val circleBit = 13

    private fun releasedSource(buttonMask: Int = PadInputMerge.ALL_RELEASED): IntArray =
        intArrayOf(buttonMask, buttonMask)

    @Test
    fun releasingKeyboardDoesNotClearAGamepadButtonStillHeld() {
        val legacy = PadInputMerge.applyButton(PadInputMerge.ALL_RELEASED, crossBit, true)
        val keyboard = releasedSource(
            PadInputMerge.applyButton(PadInputMerge.ALL_RELEASED, crossBit, true)
        )
        val mouse = releasedSource()
        val sources = arrayOf(keyboard, mouse)

        val whileBothPressed = PadInputMerge.effectiveButtons(
            legacy,
            PadInputMerge.ALL_RELEASED,
            sources,
            0
        )
        assertEquals(0, whileBothPressed and (1 shl crossBit))

        keyboard[0] = PadInputMerge.applyButton(keyboard[0], crossBit, false)
        val afterKeyboardRelease = PadInputMerge.effectiveButtons(
            legacy,
            PadInputMerge.ALL_RELEASED,
            sources,
            0
        )
        assertEquals(0, afterKeyboardRelease and (1 shl crossBit))

        val releasedLegacy = PadInputMerge.applyButton(legacy, crossBit, false)
        val afterGamepadRelease = PadInputMerge.effectiveButtons(
            releasedLegacy,
            PadInputMerge.ALL_RELEASED,
            sources,
            0
        )
        assertNotEquals(0, afterGamepadRelease and (1 shl crossBit))
    }

    @Test
    fun oneSourceReleaseLeavesAnotherSourcePressActive() {
        val legacy = PadInputMerge.ALL_RELEASED
        val keyboard = releasedSource(
            PadInputMerge.applyButton(PadInputMerge.ALL_RELEASED, crossBit, true)
        )
        val mouse = releasedSource(
            PadInputMerge.applyButton(PadInputMerge.ALL_RELEASED, crossBit, true)
        )
        val sources = arrayOf(keyboard, mouse)

        val press = PadInputMerge.effectiveButtons(
            legacy,
            PadInputMerge.ALL_RELEASED,
            sources,
            0
        )
        assertEquals(0, press and (1 shl crossBit))

        keyboard[0] = PadInputMerge.applyButton(keyboard[0], crossBit, false)
        val afterKeyboardRelease = PadInputMerge.effectiveButtons(
            legacy,
            PadInputMerge.ALL_RELEASED,
            sources,
            0
        )
        assertEquals(0, afterKeyboardRelease and (1 shl crossBit))

        mouse[0] = PadInputMerge.applyButton(mouse[0], crossBit, false)
        val afterBothReleased = PadInputMerge.effectiveButtons(
            legacy,
            PadInputMerge.ALL_RELEASED,
            sources,
            0
        )
        assertNotEquals(0, afterBothReleased and (1 shl crossBit))
    }

    @Test
    fun unrelatedButtonsStayIndependentPerSource() {
        val legacy = PadInputMerge.applyButton(PadInputMerge.ALL_RELEASED, circleBit, true)
        val keyboard = releasedSource(
            PadInputMerge.applyButton(PadInputMerge.ALL_RELEASED, crossBit, true)
        )
        val sources = arrayOf(keyboard, releasedSource())

        val effective = PadInputMerge.effectiveButtons(
            legacy,
            PadInputMerge.ALL_RELEASED,
            sources,
            0
        )

        assertEquals(0, effective and (1 shl crossBit))
        assertEquals(0, effective and (1 shl circleBit))
    }

    @Test
    fun strongestAnalogValueSurvivesOtherSourceRelease() {
        val legacyAxes = Array(2) { IntArray(8) }
        val keyboardAxes = Array(2) { IntArray(8) }
        keyboardAxes[0][1] = 255
        val sourceAxes = arrayOf(legacyAxes, keyboardAxes)

        assertEquals(
            255,
            PadInputMerge.strongestHalfAxis(120, sourceAxes, 0, 1)
        )

        keyboardAxes[0][1] = 0
        assertEquals(
            120,
            PadInputMerge.strongestHalfAxis(120, sourceAxes, 0, 1)
        )
    }
}
