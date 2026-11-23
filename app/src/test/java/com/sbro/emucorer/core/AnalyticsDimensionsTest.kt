package com.sbro.emucorer.core

import org.junit.Assert.assertEquals
import org.junit.Test

class AnalyticsDimensionsTest {
    @Test
    fun sanitizersMapUnknownValuesToUnknown() {
        assertEquals("game", AnalyticsDimensions.sanitizeLaunchType("game"))
        assertEquals("unknown", AnalyticsDimensions.sanitizeLaunchType("/sdcard/x.iso"))
        assertEquals("native_start", AnalyticsDimensions.sanitizeFailureReason("native_start"))
        assertEquals("unknown", AnalyticsDimensions.sanitizeFailureReason("boom"))
        assertEquals("load", AnalyticsDimensions.sanitizeSaveStateAction("load"))
        assertEquals("unknown", AnalyticsDimensions.sanitizeSaveStateAction("delete"))
    }

    @Test
    fun rendererAndUpscaleMapping() {
        assertEquals("vulkan", AnalyticsDimensions.renderer(RendererDefaults.VULKAN))
        assertEquals("opengl", AnalyticsDimensions.renderer(RendererDefaults.OPENGL))
        assertEquals("software", AnalyticsDimensions.renderer(RendererDefaults.SOFTWARE))
        assertEquals("unknown", AnalyticsDimensions.renderer(99))
        assertEquals("native", AnalyticsDimensions.upscale(1f))
        assertEquals("native", AnalyticsDimensions.upscale(Float.NaN))
        assertEquals("2x", AnalyticsDimensions.upscale(2f))
        assertEquals("3x", AnalyticsDimensions.upscale(3f))
        assertEquals("4x_plus", AnalyticsDimensions.upscale(6f))
    }

    @Test
    fun durationBuckets() {
        assertEquals("under_1m", AnalyticsDimensions.durationBucket(59_999L))
        assertEquals("1_5m", AnalyticsDimensions.durationBucket(60_000L))
        assertEquals("5_15m", AnalyticsDimensions.durationBucket(5 * 60_000L))
        assertEquals("15_30m", AnalyticsDimensions.durationBucket(15 * 60_000L))
        assertEquals("30_60m", AnalyticsDimensions.durationBucket(30 * 60_000L))
        assertEquals("over_60m", AnalyticsDimensions.durationBucket(60 * 60_000L))
    }
}
