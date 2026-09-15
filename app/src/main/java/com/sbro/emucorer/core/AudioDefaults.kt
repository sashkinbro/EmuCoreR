package com.sbro.emucorer.core

/** Audio defaults used by the Android frontend and sanitized before reaching the core. */
object AudioDefaults {
    const val VOLUME_DEFAULT = 100
    const val VOLUME_MIN = 0
    const val VOLUME_MAX = 100

    const val INTERPOLATION_NEAREST = 0
    const val INTERPOLATION_LINEAR = 1
    const val INTERPOLATION_GAUSSIAN = 2
    const val INTERPOLATION_CUBIC = 3
    const val INTERPOLATION_DEFAULT = INTERPOLATION_GAUSSIAN

    const val SYNC_DISABLED = 0
    const val SYNC_TIME_STRETCH = 1
    const val SYNC_DEFAULT = SYNC_TIME_STRETCH

    const val LIGHTWEIGHT_SPU2_DEFAULT = false

    // Audio backend selection (Android-specific)
    // BACKEND_AAUDIO  → maps to AudioBackend::SDL in core (android_aaudio_stream.cpp)
    // BACKEND_OPENSLES → maps to AudioBackend::OpenSLES (android_opensles_stream.cpp)
    const val BACKEND_AAUDIO = 0
    const val BACKEND_OPENSLES = 1
    const val BACKEND_DEFAULT = BACKEND_AAUDIO

    const val BUFFER_MS_DEFAULT = 150
    const val BUFFER_MS_MIN = 10
    const val BUFFER_MS_MAX = 500

    // Capacity ceiling for the shared-mode AAudio buffer. The stream is
    // resized to the device burst afterwards, so this mostly bounds how far
    // the audio clock may lag; 30 ms keeps a margin for slower devices without
    // adding audible delay. The separate 100 ms time-stretch buffer is
    // unchanged.
    const val OUTPUT_LATENCY_MS_DEFAULT = 30
    const val OUTPUT_LATENCY_MS_MIN = 1
    const val OUTPUT_LATENCY_MS_MAX = 500
    // Prefer the platform's low latency output path. Devices that cannot give
    // a fast mixer fall back to the shared path inside the native stream open,
    // so enabling this by default is safe.
    const val MINIMAL_OUTPUT_LATENCY_DEFAULT = true

    fun coerceVolume(value: Int): Int = value.coerceIn(VOLUME_MIN, VOLUME_MAX)

    fun coerceInterpolation(value: Int): Int = when (value) {
        INTERPOLATION_NEAREST,
        INTERPOLATION_LINEAR,
        INTERPOLATION_GAUSSIAN,
        INTERPOLATION_CUBIC -> value
        else -> INTERPOLATION_DEFAULT
    }

    fun interpolationCoreName(value: Int): String = when (coerceInterpolation(value)) {
        INTERPOLATION_NEAREST -> "Nearest"
        INTERPOLATION_LINEAR -> "Linear"
        INTERPOLATION_CUBIC -> "Cubic"
        else -> "Gaussian"
    }

    fun coerceSyncMode(value: Int): Int = when (value) {
        SYNC_DISABLED, SYNC_TIME_STRETCH -> value
        else -> SYNC_DEFAULT
    }

    fun syncModeCoreName(value: Int): String = when (coerceSyncMode(value)) {
        SYNC_DISABLED -> "Disabled"
        else -> "TimeStretch"
    }

    fun effectiveInterpolation(value: Int, lightweightSpu2: Boolean): Int =
        if (lightweightSpu2) INTERPOLATION_LINEAR else coerceInterpolation(value)

    fun effectiveSyncMode(value: Int, lightweightSpu2: Boolean): Int =
        if (lightweightSpu2) SYNC_DISABLED else coerceSyncMode(value)

    fun coerceBackend(value: Int): Int = when (value) {
        BACKEND_AAUDIO, BACKEND_OPENSLES -> value
        else -> BACKEND_DEFAULT
    }

    /** Returns the core AudioBackend enum string name for the given frontend backend index. */
    fun backendCoreName(value: Int): String = when (coerceBackend(value)) {
        BACKEND_OPENSLES -> "OpenSLES"
        else -> "SDL" // SDL on Android = AAudio via android_aaudio_stream.cpp
    }

    fun coerceBufferMs(value: Int): Int = value.coerceIn(BUFFER_MS_MIN, BUFFER_MS_MAX)

    fun coerceOutputLatencyMs(value: Int): Int =
        value.coerceIn(OUTPUT_LATENCY_MS_MIN, OUTPUT_LATENCY_MS_MAX)
}
