package com.sbro.emucorer.ui.emulation

import com.sbro.emucorer.data.PerformanceOverlayMetrics
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class PerformanceOverlayLayoutTest {

    @Test
    fun versionHeaderIsShownOnlyWhenVersionMetricIsSelected() {
        val header = "EmuCoreR-1.4|119|v3.0"
        val shown = buildPerformanceOverlayLayout("FPS: 60", PerformanceOverlayMetrics.VERSION, header)
        val hidden = buildPerformanceOverlayLayout("FPS: 60", PerformanceOverlayMetrics.FPS, header)

        assertEquals(listOf(header), shown.mainLines)
        assertEquals(listOf("FPS:60"), hidden.mainLines)
    }

    @Test
    fun blankHeaderNeverAddsALine() {
        val layout = buildPerformanceOverlayLayout("FPS: 60", PerformanceOverlayMetrics.ALL, " ")

        assertEquals(listOf("FPS:60"), layout.mainLines)
    }

    @Test
    fun versionMetricIsPartOfTheDefaultMask() {
        assertTrue(PerformanceOverlayMetrics.isEnabled(PerformanceOverlayMetrics.DEFAULT, PerformanceOverlayMetrics.VERSION))
        assertFalse(PerformanceOverlayMetrics.isEnabled(PerformanceOverlayMetrics.DEFAULT, PerformanceOverlayMetrics.AUDIO))
    }
}
