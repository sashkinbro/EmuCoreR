// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: LicenseRef-EmuCoreR-Proprietary
package com.sbro.emucorer.core

import android.view.Surface

/**
 * JNI surface over the bundled SwanStation libretro frontend.
 *
 * The native side drives the core through the libretro API (video, audio,
 * input, environment) and keeps the same per-frame PCM + pad contract the
 * Kotlin runtime expects from the previous core.
 */
class NativeCoreBridge {
    companion object {
        init {
            System.loadLibrary("emucorer_jni")
        }
    }

    external fun apiVersion(): Int

    // ---------------------------------------------------------------------
    // Lifecycle / configuration.
    // ---------------------------------------------------------------------
    external fun nativeInit(systemDir: String, saveDir: String, coreAssetsDir: String)
    external fun createSession(): Long
    external fun destroySession(handle: Long)
    external fun nativeSetOption(key: String, value: String)
    external fun nativeGetOption(key: String): String?
    /** Frontend post-processing effect derived from the selected shader preset. */
    external fun nativeSetShaderEffect(effect: Int)

    /** RetroArch (.slangp) shader chain executed through librashader. */
    external fun nativeSetShaderPreset(path: String, enabled: Boolean)

    // ---------------------------------------------------------------------
    // Content.
    // ---------------------------------------------------------------------
    external fun loadBios(handle: Long, path: String): Int
    /** Boots the core with no content into the PlayStation BIOS. */
    external fun loadBiosOnly(handle: Long): Int
    external fun loadDisc(handle: Long, path: String): Int
    external fun loadDiscFd(handle: Long, fd: Int, offset: Long, size: Long): Int
    external fun reset(handle: Long): Int

    /** Advances one guest frame and returns interleaved 44.1 kHz stereo PCM. */
    external fun runFrame(handle: Long): ShortArray?
    external fun setSurface(handle: Long, surface: Surface?, renderer: Int): Int

    // ---------------------------------------------------------------------
    // Input.
    // ---------------------------------------------------------------------
    external fun setPadButtons(handle: Long, port: Int, activeLowButtons: Int)
    external fun setPadAnalog(handle: Long, port: Int, lx: Int, ly: Int, rx: Int, ry: Int)
    external fun setPadAnalogMode(handle: Long, port: Int, enabled: Boolean)
    /** Bit 16 analog mode, bits 8..15 large motor, bits 0..7 small motor. */
    external fun getPadState(handle: Long, port: Int): Int

    // ---------------------------------------------------------------------
    // Save states / memory cards.
    // ---------------------------------------------------------------------
    external fun saveState(handle: Long, path: String): Int
    external fun loadState(handle: Long, path: String): Int
    external fun createMemoryCard(path: String): Int

    // ---------------------------------------------------------------------
    // Diagnostics.
    // ---------------------------------------------------------------------
    external fun getSystemInfo(): String
    external fun getDiagnostics(): String
    external fun getDisplayRect(handle: Long): IntArray?
    external fun getAvInfo(handle: Long): LongArray?

    /** Human-readable core name/version used by statistics and the About screen. */
    fun coreName(): String? = "SwanStation"
    fun coreVersion(): String? = getSystemInfo().removePrefix("SwanStation ").trim()

    // Compatibility surface used by the app layer. The libretro frontend owns
    // AAudio buffering and cheats are applied through retro_cheat_set.
    fun setAudioBufferMs(@Suppress("UNUSED_PARAMETER") milliseconds: Int) = Unit
    fun loadCheats(@Suppress("UNUSED_PARAMETER") path: String) = Unit
    fun clearCheats() = Unit

    // ---------------------------------------------------------------------
    // AAudio output (consumed by NativeAudioPcmSink).
    // ---------------------------------------------------------------------
    external fun createAudioOutput(): Long
    external fun destroyAudioOutput(handle: Long)
    external fun startAudioOutput(handle: Long): Int
    external fun pauseAudioOutput(handle: Long): Int
    external fun flushAudioOutput(handle: Long): Int
    /** Nonblocking: accepted shorts, or negative status. */
    external fun writeAudioOutput(handle: Long, samples: ShortArray, offset: Int, count: Int): Int
    /** state, error, sample rate, burst, queued, accepted, callback, silence frames. */
    external fun audioOutputStats(handle: Long): LongArray?

    // ---------------------------------------------------------------------
    // Disc metadata is not exposed by the libretro core; the library layer
    // falls back to filename-derived titles when this returns null.
    // ---------------------------------------------------------------------
    fun getDiscMetadata(@Suppress("UNUSED_PARAMETER") path: String): String? = null

    fun getDiscMetadataFd(
        @Suppress("UNUSED_PARAMETER") fd: Int,
        @Suppress("UNUSED_PARAMETER") offset: Long,
        @Suppress("UNUSED_PARAMETER") size: Long
    ): String? = null

    // ---------------------------------------------------------------------
    // Legacy self-test surface. The bespoke EmuCoreR core was replaced by
    // SwanStation, so these report availability instead of running suites.
    // ---------------------------------------------------------------------
    fun getHostInfo(): String = getSystemInfo()
    private fun unavailable(@Suppress("UNUSED_PARAMETER") name: String): String =
        "SwanStation core: '$name' self-test is not available for the libretro core"

    fun runSmoke(): String = unavailable("smoke")
    fun runCpuTests(): String = unavailable("cpu")
    fun runIrqTimerTests(): String = unavailable("irq-timer")
    fun runDmaTests(): String = unavailable("dma")
    fun runGteTests(): String = unavailable("gte")
    fun runGpuTests(): String = unavailable("gpu")
    fun runSpuMdecTests(): String = unavailable("spu-mdec")
    fun runCdromSioTests(): String = unavailable("cdrom-sio")
    fun runJitTests(): String = unavailable("jit")
    fun runBiosTests(): String = unavailable("bios")
    fun runOptimizedTests(): String = unavailable("optimized")
    fun runRegressionTests(): String = unavailable("regression")
    fun runFinalTests(): String = unavailable("final")
    fun runDiscLoaderTests(): String = unavailable("disc-loader")
    fun runAsyncDiscTests(): String = unavailable("async-disc")
    fun runSavestateFileTests(): String = unavailable("savestate")
    fun runBootTests(): String = unavailable("boot")
    fun runGamesBootTests(): String = unavailable("games-boot")
    fun runHostThreadTests(): String = unavailable("host-thread")
}
