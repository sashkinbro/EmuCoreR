package com.sbro.emucorer.data

object PerformanceOverlayMetrics {
    const val FPS = 1 shl 0
    const val SPEED = 1 shl 2
    const val TARGET = 1 shl 3
    const val RENDERER = 1 shl 4
    const val FRAME_TIME = 1 shl 6
    const val RESOLUTION = 1 shl 8
    // Keep the bit positions stable so existing user selections migrate without
    // resetting the whole overlay configuration.
    const val HOST_CPU = 1 shl 13
    const val HOST_GPU = 1 shl 14
    const val AUDIO = 1 shl 15

    const val VERSION = 1 shl 16

    const val ALL = VERSION or FPS or SPEED or TARGET or RENDERER or FRAME_TIME or RESOLUTION or
        HOST_CPU or HOST_GPU or AUDIO

    // Audio remains opt-in; the default mask only contains metrics the runtime
    // currently publishes (FPS/Speed, renderer+resolution, frame time, host
    // CPU/GPU names and load).
    const val DEFAULT = VERSION or FPS or SPEED or RENDERER or FRAME_TIME or RESOLUTION or HOST_CPU or HOST_GPU

    fun sanitize(mask: Int): Int = mask and ALL

    fun isEnabled(mask: Int, metric: Int): Boolean = sanitize(mask) and metric != 0
}
