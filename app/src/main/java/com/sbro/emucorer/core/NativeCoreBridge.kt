// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: GPL-3.0-or-later
package com.sbro.emucorer.core

import android.view.Surface

/**
 * JNI surface over the bundled SwanStation libretro frontend.
 *
 * The native side drives the core through the libretro API (video, audio,
 * input, environment); the frame loop and pad state stay on the Kotlin side.
 */
class NativeCoreBridge {
    companion object {
        private const val STANDARD_LIBRARY_NAME = "emucorer_jni"

        init {
            val selected = AndroidNativeCoreSelector.selectedLibraryName()
            val selectedLoaded = runCatching { System.loadLibrary(selected) }.isSuccess
            if (!selectedLoaded && selected != STANDARD_LIBRARY_NAME) {
                // The 16 KiB variant ships with release builds; debug builds on a
                // 16 KiB device fall back to the standard core instead of failing.
                System.loadLibrary(STANDARD_LIBRARY_NAME)
            }
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

    /**
     * Returns the path of a memory card the core just flushed, or null when no
     * write happened since the previous call.
     */
    external fun pollMemoryCardEvent(): String?

    /** Runs one guest frame; audio is pulled by the output stream callback. */
    external fun runFrame(handle: Long)
    /**
     * Creates/rebinds the hardware renderer context on the calling thread.
     * Must be invoked from the frame worker so GL state stays thread-affine.
     */
    external fun ensureHardwareContext(): Boolean
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
    /**
     * Presenter destination rect in window pixels as
     * `{left, top, right, bottom}` (null until a window is attached).
     */
    external fun getPresentRect(): FloatArray?
    external fun getAvInfo(handle: Long): LongArray?
    /** Emulated vertical refresh in Hz, used for audio-synced frame pacing. */
    external fun getFrameRate(handle: Long): Double

    /** Human-readable core name/version used by statistics and the About screen. */
    fun coreName(): String? = "SwanStation"
    fun coreVersion(): String? = getSystemInfo().removePrefix("SwanStation ").trim()

    /** Loads active GameShark-style codes from a PCSX `.cht` container. */
    external fun loadCheats(path: String)
    external fun clearCheats()

    /** Binds an explicit memory-card image to a slot (null or blank disables it). */
    external fun setMemoryCardPath(slot: Int, path: String?)

    /**
     * Sets the base directory replacement textures are loaded from; the core
     * appends the running game code (null or blank restores the core default).
     */
    external fun setTextureReplacementsPathOverride(path: String?)

    // ---------------------------------------------------------------------
    // RetroAchievements (rcheevos). The client lives in native code; Kotlin
    // polls JSON state/events and persists the account token.
    // ---------------------------------------------------------------------
    external fun achievementsSetEnabled(enabled: Boolean)
    external fun achievementsSetHardcore(enabled: Boolean)
    external fun achievementsSetUnofficial(enabled: Boolean)
    external fun achievementsSetEncore(enabled: Boolean)
    external fun achievementsLoginWithPassword(user: String, password: String): String?
    external fun achievementsLoginWithToken(user: String, token: String): String?
    external fun achievementsLogout()
    external fun achievementsLoadGame(path: String)
    external fun achievementsUnloadGame()
    external fun achievementsPump()
    external fun achievementsStateJson(): String
    external fun achievementsAchievementsJson(): String
    external fun achievementsPollEventsJson(): String

    /** True when the running session has a disc image mounted. */
    external fun hasDiscMedia(handle: Long): Boolean

    // ---------------------------------------------------------------------
    // AAudio output tuning (applied when the next stream is opened).
    // ---------------------------------------------------------------------
    external fun setAudioOutputLatencyMs(milliseconds: Int)
    external fun setAudioLowLatency(enabled: Boolean)
    /** Frontend presentation frame skip (0..4). */
    external fun setFrameSkip(frames: Int)
    /** Display crop in source pixels, applied before aspect-ratio scaling. */
    external fun setDisplayCrop(left: Int, top: Int, right: Int, bottom: Int)

    // ---------------------------------------------------------------------
    // AAudio output (owned by NativeAudioOutput).
    // ---------------------------------------------------------------------
    external fun createAudioOutput(): Long
    external fun destroyAudioOutput(handle: Long)
    external fun startAudioOutput(handle: Long): Int
    external fun pauseAudioOutput(handle: Long): Int
    external fun flushAudioOutput(handle: Long): Int
    /** Empties the shared ring so a new session cannot replay old frames. */
    external fun resetAudioQueue()
    /** Linear gain in 0..1 applied on the output callback thread. */
    external fun setAudioGain(gain: Float)
    /** Frames queued for the output; negative when the stream needs recovery. */
    external fun audioOutputBufferedFrames(handle: Long): Int
    /** Queue level the frame loop keeps the output at for audio-synced pacing. */
    external fun audioOutputPacingHighWaterFrames(handle: Long): Int
    /** state, error, sample rate, burst, queued, accepted, callback, silence frames. */
    external fun audioOutputStats(handle: Long): LongArray?

    // ---------------------------------------------------------------------
    // Disc metadata read straight from the image (SYSTEM.CNF). The library
    // layer falls back to filename-derived titles when this returns null.
    // ---------------------------------------------------------------------
    external fun getDiscMetadata(path: String): String?

    external fun getDiscMetadataFd(fd: Int, offset: Long, size: Long): String?
}
