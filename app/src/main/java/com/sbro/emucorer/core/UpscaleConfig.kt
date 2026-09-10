package com.sbro.emucorer.core

import kotlin.math.roundToInt

const val UPSCALE_MIN = 1.0f
const val UPSCALE_MAX = 10.0f

private const val UPSCALE_STEP = 0.25f
private const val UPSCALE_NATIVE_MULTIPLIER = 1.0f
private const val UPSCALE_MAX_MULTIPLIER = UPSCALE_MAX

fun normalizeUpscale(value: Float, maxMultiplier: Int = UPSCALE_MAX_MULTIPLIER.roundToInt()): Float {
    val max = maxMultiplier.coerceAtLeast(UPSCALE_NATIVE_MULTIPLIER.roundToInt())
        .coerceAtMost(UPSCALE_MAX_MULTIPLIER.roundToInt())
        .toFloat()
    val stepped = (value / UPSCALE_STEP).roundToInt() * UPSCALE_STEP
    return stepped.coerceIn(UPSCALE_NATIVE_MULTIPLIER, max)
}

fun upscaleMultiplierValue(value: Float): Int = upscaleMultiplierKey(normalizeUpscale(value))

fun upscaleMultiplierKey(value: Float): Int = (normalizeUpscale(value) * 100f).roundToInt()

fun upscaleKeyToMultiplier(value: Int): Float = normalizeUpscale(value.toFloat() / 100f)

fun formatUpscaleLabel(value: Float, nativeLabel: String): String {
    val normalized = normalizeUpscale(value)
    return when {
        normalized == UPSCALE_NATIVE_MULTIPLIER -> nativeLabel
        normalized == normalized.roundToInt().toFloat() -> "${normalized.roundToInt()}x"
        else -> "${"%.2f".format(java.util.Locale.US, normalized)}x"
    }
}

fun buildUpscaleOptions(nativeLabel: String, maxMultiplier: Int = UPSCALE_MAX_MULTIPLIER.roundToInt()): List<Pair<Int, String>> {
    val max = maxMultiplier.coerceAtLeast(UPSCALE_NATIVE_MULTIPLIER.roundToInt())
        .coerceAtMost(UPSCALE_MAX_MULTIPLIER.roundToInt())
    // The PS1 core only supports native and the 2x enhanced-resolution buffer.
    // Do not offer fractional multipliers that would silently behave like 2x.
    if (max <= 2) {
        val options = mutableListOf(upscaleMultiplierKey(UPSCALE_NATIVE_MULTIPLIER) to nativeLabel)
        if (max >= 2) {
            options += upscaleMultiplierKey(2f) to formatUpscaleLabel(2f, nativeLabel)
        }
        return options
    }
    val steps = ((max.toFloat() - UPSCALE_NATIVE_MULTIPLIER) / UPSCALE_STEP).roundToInt()
    return (0..steps).map { index ->
        val multiplier = UPSCALE_NATIVE_MULTIPLIER + (index * UPSCALE_STEP)
        upscaleMultiplierKey(multiplier) to formatUpscaleLabel(multiplier, nativeLabel)
    }
}
