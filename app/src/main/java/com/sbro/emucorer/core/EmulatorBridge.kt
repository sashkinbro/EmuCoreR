
package com.sbro.emucorer.core

import android.content.Context
import android.graphics.Bitmap
import android.os.Handler
import android.os.Looper
import android.os.ParcelFileDescriptor
import android.util.Log
import android.view.PixelCopy
import android.view.Surface
import com.sbro.emucorer.BuildConfig
import com.sbro.emucorer.data.AppPreferences
import com.sbro.emucorer.network.NetPlaySession
import com.sbro.emucorer.network.RemotePlaySession
import com.sbro.emucorer.data.DisplayCrop
import com.sbro.emucorer.ui.common.invalidateCoverImage
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.launch
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull
import java.io.File
import java.io.FileOutputStream
import java.lang.ref.WeakReference
import java.util.Locale
import kotlin.coroutines.resume

object EmulatorBridge {
    private const val TAG = "EmulatorBridge"
    const val AUTO_RENDERER = RendererDefaults.AUTO
    const val OPENGL_RENDERER = RendererDefaults.OPENGL
    const val VULKAN_RENDERER = RendererDefaults.VULKAN

    private val aspectRatioSettingValues = mapOf(
        0 to "Stretch",
        1 to "Auto 4:3/3:2",
        2 to "4:3",
        3 to "16:9",
        4 to "10:7"
    )

    // Card types the bridge derives from the memory-card slot assignment itself,
    // so a stale persisted value must not override the current assignment.
    private val AUTO_MANAGED_CARD_TYPES = setOf("Shared", "None")

    private val serialDispatcher = Dispatchers.IO.limitedParallelism(1)
    private val serialScope = CoroutineScope(SupervisorJob() + serialDispatcher)
    private val rendererSwitchMutex = Mutex()

    @Volatile
    var isNativeLoaded: Boolean = false
        private set

    @Volatile
    private var isVmActive: Boolean = false

    @Volatile
    private var lastSurface: Surface? = null

    @Volatile
    private var lastSurfaceWidth: Int = 0

    @Volatile
    private var lastSurfaceHeight: Int = 0

    @Volatile
    private var surfaceEventVersion: Long = 0

    private val _presentationSurfaceGeneration = MutableStateFlow(0L)
    val presentationSurfaceGeneration: StateFlow<Long> =
        _presentationSurfaceGeneration.asStateFlow()
    val runtimeFailure: StateFlow<RuntimeFailure?> get() = NativeApp.runtimeFailure

    @Volatile
    private var shutdownRequested: Boolean = false

    private var contextRef: WeakReference<Context>? = null
    private val settingsCache = HashMap<String, String>()
    private var audioVolumeSetting: Int = AudioDefaults.VOLUME_DEFAULT
    private var audioMutedSetting: Boolean = false

    init {
        isNativeLoaded = NativeApp.hasNativeCore
        if (isNativeLoaded) {
            Log.i(TAG, "${NativeApp.loadedCoreLibraryName} is ready")
        } else {
            Log.e(TAG, "${NativeApp.loadedCoreLibraryName} is unavailable")
        }
    }

    private data class RuntimeOp(val kind: String, val fields: List<String>)
    private data class PreparedMetadataPath(
        val path: String,
        val descriptor: ParcelFileDescriptor? = null
    )

    private fun settingOp(section: String, key: String, type: String, value: String) =
        RuntimeOp("setting", listOf(section, key, type, value))

    private fun upscaleOp(value: Float) = RuntimeOp("upscale", listOf(value.toString()))

    private fun frameSkipOp(value: Int) = RuntimeOp("frame_skip", listOf(value.coerceIn(0, 4).toString()))

    private fun normalizeAspectRatio(type: Int): Int {
        return if (type in aspectRatioSettingValues.keys) type else 1
    }

    private fun aspectOp(type: Int) = RuntimeOp(
        "aspect",
        normalizeAspectRatio(type).let { listOf(it.toString(), aspectRatioSettingValues.getValue(it)) }
    )

    private fun memoryCardSlotOp(slot: Int, fileName: String?) = RuntimeOp(
        "memory_card_slot",
        listOf(slot.toString(), fileName.orEmpty())
    )

    /**
     * Resolves a memory-card file name stored in preferences to the absolute
     * path inside the app's emulator data root, so the core opens the same
     * image the memory card manager shows.
     */
    private fun resolveMemoryCardPath(fileName: String): String? {
        val name = fileName.trim()
        if (name.isEmpty()) return null
        val direct = File(name)
        if (direct.isAbsolute) return direct.absolutePath
        val context = getContext() ?: return null
        val root = EmulatorStorage.memoryCardsDir(
            context,
            AppPreferences(context).getEmulatorDataPathSync()
        )
        return File(root, name).absolutePath
    }

    private fun rendererExecutionOps(renderer: Int): List<RuntimeOp> {
        val resolved = normalizeRenderer(renderer)
        return listOf(
            settingOp("EmuCore/GS", "Renderer", "int", resolved.toString())
        )
    }

    private suspend fun <T> runSerial(block: () -> T): T = withContext(serialDispatcher) { block() }

    private fun launchSerial(block: suspend () -> Unit) {
        serialScope.launch { block() }
    }

    private suspend fun performRuntimeOps(ops: List<RuntimeOp>): Boolean {
        if (!isNativeLoaded || ops.isEmpty()) return true
        return runSerial {
            var succeeded = true
            NativeApp.beginSettingsBatch()
            try {
                ops.forEach { op ->
                    when (op.kind) {
                        "setting" -> {
                            val section = op.fields.getOrNull(0) ?: return@forEach
                            val key = op.fields.getOrNull(1) ?: return@forEach
                            val type = op.fields.getOrNull(2) ?: return@forEach
                            val value = op.fields.getOrNull(3) ?: return@forEach
                            if (!applyAudioOutputSetting(section, key, value) &&
                                !NativeApp.setSetting(section, key, type, value)
                            ) succeeded = false
                        }
                        "upscale" -> {
                            val value = op.fields.firstOrNull()?.toFloatOrNull() ?: return@forEach
                            NativeApp.renderUpscalemultiplier(normalizeUpscale(value))
                        }
                        "frame_skip" -> {
                            val value = op.fields.firstOrNull()?.toIntOrNull() ?: return@forEach
                            NativeApp.setFrameSkip(value.coerceIn(0, 4))
                        }
                        "aspect" -> {
                            val type = op.fields.firstOrNull()?.toIntOrNull() ?: return@forEach
                            NativeApp.setAspectRatio(type)
                        }
                        "memory_card_slot" -> {
                            val slot = op.fields.getOrNull(0)?.toIntOrNull() ?: return@forEach
                            val slotIndex = slot.coerceIn(1, 2)
                            val fileName = op.fields.getOrNull(1).orEmpty()
                            val hasCard = fileName.isNotBlank()
                            if (!NativeApp.setSetting("MemoryCards", "Slot${slotIndex}_Enable", "bool", hasCard.toString()))
                                succeeded = false
                            if (!NativeApp.setSetting("MemoryCards", "Slot${slotIndex}_Filename", "string", fileName))
                                succeeded = false
                            // The bundled SwanStation core opens memory cards through
                            // the libretro host interface, so translate the app's slot
                            // assignment into an explicit image path plus the matching
                            // shared-card type option.
                            val resolvedPath = resolveMemoryCardPath(fileName)
                            NativeApp.setMemoryCardPath(slotIndex - 1, resolvedPath)
                            val cardTypeKey = "swanstation_MemoryCards_Card${slotIndex}Type"
                            val storedCardType = SwanStationOptions.value(cardTypeKey)
                            if (storedCardType == null || storedCardType in AUTO_MANAGED_CARD_TYPES) {
                                NativeApp.setCoreOption(
                                    cardTypeKey,
                                    if (resolvedPath != null) "Shared" else "None"
                                )
                            }
                        }
                    }
                }
            } finally {
                NativeApp.endSettingsBatch()
            }
            succeeded
        }
    }

    /**
     * Volume and mute are applied to the Kotlin PCM path because the core API
     * has no gain stage.
     */
    private fun applyAudioOutputSetting(section: String, key: String, value: String): Boolean {
        if (section != "Audio/Output") return false
        when (key) {
            "Volume" -> {
                val volume = value.toIntOrNull() ?: return false
                audioVolumeSetting = volume.coerceIn(AudioDefaults.VOLUME_MIN, AudioDefaults.VOLUME_MAX)
            }
            "Mute" -> {
                audioMutedSetting = value.toBooleanStrictOrNull() ?: return false
            }
            "AudioOutputLatencyMs" -> {
                val milliseconds = value.toIntOrNull() ?: return false
                NativeApp.setAudioOutputLatencyMs(
                    milliseconds.coerceIn(
                        AudioDefaults.OUTPUT_LATENCY_MS_MIN,
                        AudioDefaults.OUTPUT_LATENCY_MS_MAX
                    )
                )
                return true
            }
            "MinimalOutputLatency" -> {
                val enabled = value.toBooleanStrictOrNull() ?: return false
                NativeApp.setAudioLowLatency(enabled)
                return true
            }
            else -> return false
        }
        NativeApp.setAudioOutputGain(audioVolumeSetting, audioMutedSetting)
        return true
    }

    private fun rendererName(renderer: Int): String = when (renderer) {
        AUTO_RENDERER -> "Vulkan"
        OPENGL_RENDERER -> "OpenGL"
        RendererDefaults.SOFTWARE -> "Software"
        VULKAN_RENDERER -> "Vulkan"
        else -> "Unknown($renderer)"
    }

    internal fun normalizeRenderer(renderer: Int): Int {
        return RendererDefaults.normalizeAndroidRenderer(renderer)
    }

    fun getMaxUpscaleMultiplier(renderer: Int): Int {
        if (!isNativeLoaded) return UPSCALE_MAX.toInt()
        val resolvedRenderer = normalizeRenderer(renderer)
        return runCatching { NativeApp.getMaxUpscaleMultiplier(resolvedRenderer) }
            .getOrDefault(UPSCALE_MAX.toInt())
            .coerceAtLeast(UPSCALE_MIN.toInt())
    }

    fun initializeOnce(context: Context) {
        contextRef = WeakReference(context)
        if (!isNativeLoaded) {
            Log.e(TAG, "initializeOnce skipped: native library is not loaded")
            return
        }

        try {
            NativeApp.initializeOnce(context.applicationContext)
            if (BuildConfig.DEBUG) {
                val jitSmokeOk = runCatching { NativeApp.runJitExecutableMemorySmokeTest() }.getOrDefault(false)
                Log.i(TAG, "JIT executable-memory smoke result=$jitSmokeOk")
            }
            val (preferEnglishTitles, emulatorDataPath) = runBlocking {
                val preferences = AppPreferences(context.applicationContext)
                preferences.preferEnglishGameTitles.first() to preferences.getEmulatorDataPathSync()
            }
            NativeApp.setSetting("UI", "PreferEnglishGameTitles", "bool", preferEnglishTitles.toString())
            NativeApp.reloadDataRoot(emulatorDataPath ?: "")
            Log.i(TAG, "initializeOnce completed")
        } catch (error: Exception) {
            Log.e(TAG, "initializeOnce failed", error)
            CrashLogger.logError(TAG, "initializeOnce FAILED", error)
        }
    }

    fun getContext(): Context? = contextRef?.get()

    suspend fun applyRuntimeConfig(
        biosPath: String?,
        emulatorDataPath: String? = null,
        renderer: Int,
        upscaleMultiplier: Float,
        aspectRatio: Int = 1,
        displayCrop: DisplayCrop = DisplayCrop.None,
        audioVolume: Int = AudioDefaults.VOLUME_DEFAULT,
        audioMuted: Boolean = false,
        audioOutputLatencyMs: Int = AudioDefaults.OUTPUT_LATENCY_MS_DEFAULT,
        audioMinimalOutputLatency: Boolean = AudioDefaults.MINIMAL_OUTPUT_LATENCY_DEFAULT,
        enableFastBoot: Boolean = true,
        frameSkip: Int = 0,
        frameLimitEnabled: Boolean = true,
        targetFps: Int = 0,
        textureFiltering: Int = 0,
        shaderChainEnabled: Boolean = false,
        shaderChainPreset: String = "",
        widescreenPatches: Boolean = false,
        memoryCardSlot1: String? = null,
        memoryCardSlot2: String? = null,
        enableIcacheEmulation: Boolean = false,
        cdReadAhead: Int = 0,
        enableCddaAudio: Boolean = true,
        multitapMode: Int = 0
    ) = withContext(serialDispatcher) {
        if (!isNativeLoaded) return@withContext

        val context = getContext() ?: return@withContext
        val resolvedRenderer = normalizeRenderer(renderer)
        val preparedBios = DocumentPathResolver.prepareBiosSelection(context, biosPath)
        val resolvedBiosPath = preparedBios?.directoryPath
            ?: biosPath?.let(DocumentPathResolver::resolveDirectoryPath)
        val preferredBiosFile = preparedBios?.fileName
            ?: DocumentPathResolver.findPreferredBiosFileName(resolvedBiosPath)
        // Keep the native layer on the same data root as the runtime directories;
        // saves and memory cards previously ignored the configured location.
        NativeApp.reloadDataRoot(emulatorDataPath ?: "")
        // The bundled core reads replacement textures from the app's texture
        // root (it appends the running game code), so the texture manager and
        // the emulator share a single directory.
        NativeApp.setTextureReplacementsPathOverride(
            EmulatorStorage.texturesDir(context, emulatorDataPath).absolutePath
        )
        NativeApp.setCrashContextString("emu_renderer_name", rendererName(resolvedRenderer))
        NativeApp.logCrashBreadcrumb(
            "applyRuntimeConfig renderer=${rendererName(resolvedRenderer)}($resolvedRenderer) upscale=$upscaleMultiplier aspect=$aspectRatio fastBoot=$enableFastBoot"
        )
        val prefs = AppPreferences(context)
        val padVibrationEnabled = prefs.padVibration.first()
        val textureReplacementsEnabled = prefs.textureReplacementsEnabled.first()
        val textureReplacementsPrecache = prefs.textureReplacementsPrecache.first()
        val runtimeApplied = performRuntimeOps(
            buildList {
                addAll(rendererExecutionOps(resolvedRenderer))
                add(upscaleOp(upscaleMultiplier))
                add(aspectOp(aspectRatio))
                add(settingOp("Audio/Output", "Volume", "int", AudioDefaults.coerceVolume(audioVolume).toString()))
                add(settingOp("Audio/Output", "Mute", "bool", audioMuted.toString()))
                add(settingOp("Audio/Output", "AudioOutputLatencyMs", "int", AudioDefaults.coerceOutputLatencyMs(audioOutputLatencyMs).toString()))
                add(settingOp("Audio/Output", "MinimalOutputLatency", "bool", audioMinimalOutputLatency.toString()))
                add(settingOp("Folders", "Bios", "string", resolvedBiosPath.orEmpty()))
                add(memoryCardSlotOp(1, memoryCardSlot1))
                add(memoryCardSlotOp(2, memoryCardSlot2))
                add(settingOp("Filenames", "BIOS", "string", preferredBiosFile.orEmpty()))
                add(settingOp("EmuCore", "EnableFastBoot", "bool", enableFastBoot.toString()))
                add(settingOp("EmuCore/GS", "FrameLimitEnable", "bool", frameLimitEnabled.toString()))
                add(settingOp("EmuCore/GS", "TargetFps", "int", targetFps.coerceIn(0, 120).toString()))
                add(frameSkipOp(frameSkip))
                add(settingOp("EmuCore/GS", "filter", "int", textureFiltering.toString()))
                add(settingOp("EmuCore/GS", "ShaderChainEnabled", "bool", (shaderChainEnabled && shaderChainPreset.isNotBlank()).toString()))
                add(settingOp("EmuCore/GS", "ShaderChainPreset", "string", shaderChainPreset.trim()))
                add(settingOp("EmuCore/GS", "LoadTextureReplacements", "bool", textureReplacementsEnabled.toString()))
                add(settingOp("EmuCore/GS", "PrecacheTextureReplacements", "bool", textureReplacementsPrecache.toString()))
                add(settingOp("EmuCore", "EnableWideScreenPatches", "bool", widescreenPatches.toString()))
                add(settingOp("EmuCoreR", "BiosSource", "string", biosPath.orEmpty()))
                add(settingOp("EmuCoreR", "Renderer", "int", resolvedRenderer.toString()))
                add(settingOp("EmuCoreR", "UpscaleMultiplier", "float", upscaleMultiplier.toString()))
                add(settingOp("InputSources", "PadVibration", "bool", padVibrationEnabled.toString()))
                add(settingOp("EmuCoreR/CPU", "enableIcacheEmulation", "bool", enableIcacheEmulation.toString()))
                add(settingOp("EmuCoreR/CPU", "cdReadAhead", "int", cdReadAhead.toString()))
                add(settingOp("EmuCoreR/Audio", "enableCddaAudio", "bool", enableCddaAudio.toString()))
                add(settingOp("EmuCoreR/Input", "multitapMode", "int", multitapMode.toString()))
            }
        )
        // Crop is applied by the frontend presenter, not the core option set,
        // so it is pushed straight to the native bridge.
        NativeApp.setDisplayCrop(displayCrop.sanitized())
        if (runtimeApplied) {
            settingsCache["EmuCore/GS:Renderer"] = resolvedRenderer.toString()
        }
    }

    suspend fun setMemoryCardAssignments(slot1: String?, slot2: String?) {
        performRuntimeOps(
            listOf(
                memoryCardSlotOp(1, slot1),
                memoryCardSlotOp(2, slot2)
            )
        )
    }

    suspend fun startEmulation(
        path: String,
        saveStateIdentityPath: String? = null,
        allowBiosBoot: Boolean = false
    ): Boolean {
        if (!isNativeLoaded) {
            Log.e(TAG, "startEmulation skipped: native library is not loaded")
            return false
        }
        if (path.isBlank() && !allowBiosBoot) {
            Log.e(TAG, "startEmulation rejected blank game path")
            return false
        }
        // Save-state files are named after the path the user launched, not the
        // core's prepared/materialized path, so writes and listings agree.
        NativeApp.setSaveStateIdentityPath(saveStateIdentityPath ?: path)
        val isExeExecutable = when {
            path.substringAfterLast('.', "").let { it.equals("exe", true) || it.equals("psexe", true) || it.equals("cpe", true) } -> true
            path.startsWith("content://") -> {
                val context = getContext()
                val displayName = context?.let { DocumentPathResolver.getDisplayName(it, path) }.orEmpty()
                displayName.substringAfterLast('.', "").let {
                    it.equals("exe", true) || it.equals("psexe", true) || it.equals("cpe", true)
                }
            }
            else -> false
        }
        val pathType = when {
            path.startsWith("content://") -> "content"
            path.isBlank() -> "bios"
            else -> "file"
        }
        NativeApp.logCrashBreadcrumb("startEmulation requested pathType=$pathType vmActive=$isVmActive")
        Log.i(TAG, "startEmulation requested pathType=$pathType vmActive=$isVmActive")

        return BackupSessionGate.start(active = { isVmActive }) {
            getContext()?.let { com.sbro.emucorer.data.drive.DriveBackupArchive(it).recoverPending() }
            runSerial {
                isVmActive = true
                shutdownRequested = false
                var result = try {
                    NativeApp.logCrashBreadcrumb("startEmulation entering native runVMThread")
                    NativeApp.runVMThread(path)
                } catch (error: Exception) {
                    NativeApp.logCrashBreadcrumb("startEmulation exception before native start returned")
                    Log.e(TAG, "startEmulation native call failed", error)
                    false
                }
                if (result && !allowBiosBoot && !isExeExecutable && !path.isBlank()) {
                    // The bundled core silently boots the BIOS when a disc image
                    // cannot be opened. Surface that as a failed launch instead of
                    // leaving the user on a misleading BIOS screen.
                    if (!NativeApp.hasDiscMedia()) {
                        NativeApp.logCrashBreadcrumb("disc image failed to mount; aborting launch")
                        Log.w(TAG, "Disc image could not be mounted; aborting $pathType launch")
                        runCatching { NativeApp.shutdown() }
                        result = false
                    }
                }
                isVmActive = NativeApp.hasOwnedVm()
                if (!isVmActive) {
                    DocumentPathResolver.releasePreparedLaunchHandles()
                }
                NativeApp.logCrashBreadcrumb("startEmulation finished result=$result")
                Log.i(TAG, "startEmulation finished result=$result")
                result
            }
        }
    }

    suspend fun pause() {
        if (!isNativeLoaded || !isVmActive) return
        runSerial {
            try {
                NativeApp.pause()
            } catch (_: Exception) { }
        }
    }

    suspend fun resume() {
        if (!isNativeLoaded || !isVmActive) return
        runSerial {
            try {
                rebindSurface()
                NativeApp.resume()
            } catch (_: Exception) { }
        }
    }

    suspend fun startJitProfiler() {
        if (!isNativeLoaded || !isVmActive) return
        runSerial {
            try {
                NativeApp.startJitProfiler()
            } catch (_: Exception) { }
        }
    }

    suspend fun stopJitProfiler() {
        if (!isNativeLoaded || !isVmActive) return
        runSerial {
            try {
                NativeApp.stopJitProfiler()
            } catch (_: Exception) { }
        }
    }

    suspend fun isJitProfilerActive(): Boolean {
        if (!isNativeLoaded || !isVmActive) return false
        return runSerial {
            try {
                NativeApp.isJitProfilerActive()
            } catch (_: Exception) {
                false
            }
        }
    }

    suspend fun startHangTrace() {
        if (!isNativeLoaded || !isVmActive) return
        runSerial {
            try {
                NativeApp.startHangTrace()
            } catch (_: Exception) { }
        }
    }

    suspend fun stopHangTrace() {
        if (!isNativeLoaded || !isVmActive) return
        runSerial {
            try {
                NativeApp.stopHangTrace()
            } catch (_: Exception) { }
        }
    }

    suspend fun isHangTraceActive(): Boolean {
        if (!isNativeLoaded || !isVmActive) return false
        return runSerial {
            try {
                NativeApp.isHangTraceActive()
            } catch (_: Exception) {
                false
            }
        }
    }

    suspend fun shutdown() {
        if (!isNativeLoaded) return
        runSerial {
            if (shutdownRequested) return@runSerial
            shutdownRequested = true
            try {
                NativeApp.shutdown()
                isVmActive = false
                DocumentPathResolver.releasePreparedLaunchHandles()
            } finally {
                // A failed teardown must retain ownership and its descriptors.
                shutdownRequested = false
            }
        }
        // A failed native shutdown must never unlock memory-card backup while the VM still owns it.
        if (!runCatching { NativeApp.hasValidVm() }.getOrDefault(true)) {
            BackupSessionGate.stopped()
            getContext()?.let { com.sbro.emucorer.data.drive.DriveBackupWork.afterGame(it) }
        }
    }

    fun hasValidVm(): Boolean {
        if (!isNativeLoaded) return false
        return try {
            NativeApp.hasValidVm()
        } catch (_: Exception) {
            false
        }
    }

    fun isVmActive(): Boolean {
        // A status read never performs teardown. The native handle can outlive
        // a failed worker and still needs its audio/session/descriptors released.
        return isVmActive || (isNativeLoaded && NativeApp.hasOwnedVm())
    }

    fun getPadRumble(port: Int): FloatArray? {
        if (!isNativeLoaded) return null
        return try {
            NativeApp.getPadRumble(port)
        } catch (_: Exception) {
            null
        }
    }

    fun getGameTitle(path: String): String = getGameMetadata(path).title

    /**
     * [readDiscMetadata] opens the disc image to read its real serial. Bulk
     * library scans pass false: opening every image would make the first scan
     * dramatically slower, and the filename/title-index serial is enough to
     * list the library.
     */
    fun getGameMetadata(path: String, readDiscMetadata: Boolean = true): GameMetadata {
        val inferredMetadata = when {
            path.startsWith("content://") -> {
                val context = getContext()
                val displayName = context?.let { DocumentPathResolver.getDisplayName(it, path) } ?: path
                parseMetadataFromName(displayName)
            }
            else -> parseMetadataFromName(File(path).nameWithoutExtension)
        }
        val extensionSource = if (path.startsWith("content://")) {
            getContext()?.let { DocumentPathResolver.getDisplayName(it, path) } ?: path
        } else {
            path
        }
        val extension = extensionSource.substringAfterLast('.', "").lowercase()

        if (!readDiscMetadata || !isNativeLoaded) return inferredMetadata
        if (isVmActive) return inferredMetadata
        if (extension == "elf") return inferredMetadata

        val preparedPath = prepareMetadataPathForNative(path) ?: return inferredMetadata
        Log.i(TAG, "getGameMetadata native lookup path=$path vmActive=$isVmActive ext=$extension")
        return try {
            val rawTitle = NativeApp.getGameTitle(preparedPath.path).orEmpty()
            Log.i(TAG, "getGameMetadata native result=$rawTitle")
            val segments = rawTitle.split('|')
            val nativeTitle = segments.getOrNull(0).orEmpty()
            val nativeSerial = segments.getOrNull(1)?.takeIf { it.isNotBlank() }
            val nativeSerialWithCrc = segments.getOrNull(2)?.takeIf { it.isNotBlank() }
            if (isFdMetadataArtifact(preparedPath.path, nativeTitle, nativeSerial)) {
                return inferredMetadata
            }
            val title = nativeTitle
                .takeIf { it.isNotBlank() }
                ?.let { normalizeNativeGameTitle(it, inferredMetadata.title) }
                ?: inferredMetadata.title
            GameMetadata(
                title = title,
                serial = nativeSerial ?: inferredMetadata.serial,
                serialWithCrc = nativeSerialWithCrc ?: inferredMetadata.serialWithCrc
            )
        } catch (_: Exception) {
            inferredMetadata
        } finally {
            preparedPath.descriptor?.close()
        }
    }

    private fun prepareMetadataPathForNative(path: String): PreparedMetadataPath? {
        if (!path.startsWith("content://")) return PreparedMetadataPath(path)
        val context = getContext() ?: return null
        val directPath = DocumentPathResolver.resolveFilePath(context, path)
            ?.let(::File)
            ?.takeIf { it.isFile && it.canRead() }
            ?.absolutePath
        if (!directPath.isNullOrBlank()) return PreparedMetadataPath(directPath)

        return PreparedMetadataPath(path)
    }

    private fun isFdMetadataArtifact(path: String, nativeTitle: String, nativeSerial: String?): Boolean {
        if (!path.startsWith("/proc/self/fd/")) return false
        if (!nativeSerial.isNullOrBlank()) return false
        val trimmedTitle = nativeTitle.substringBefore('|').trim()
        return trimmedTitle.isBlank() || trimmedTitle.all(Char::isDigit)
    }

    private fun normalizeNativeGameTitle(rawTitle: String, fallbackTitle: String): String {
        return cleanGameDisplayTitle(rawTitle, fallbackTitle)
    }

    fun cleanGameDisplayTitle(rawTitle: String?, fallbackNameOrPath: String? = null): String {
        val fallbackDisplay = fallbackNameOrPath
            ?.takeIf { it.isNotBlank() }
            ?.let { value ->
                if (value.startsWith("content://")) {
                    getContext()?.let { context -> DocumentPathResolver.getDisplayName(context, value) }
                        ?: DocumentPathResolver.normalizeDisplayName(value)
                } else {
                    DocumentPathResolver.normalizeDisplayName(value)
                }
            }
            .orEmpty()
        val fallbackTitle = parseMetadataFromName(fallbackDisplay).title
        val trimmed = rawTitle.orEmpty().trim()
        val source = if (trimmed.isBlank() || looksLikeStoragePath(trimmed)) {
            fallbackDisplay.ifBlank { trimmed }
        } else {
            trimmed
        }
        val normalized = if (looksLikeStoragePath(source)) {
            DocumentPathResolver.normalizeDisplayName(source)
        } else {
            source
        }
        return parseMetadataFromName(normalized).title.ifBlank { fallbackTitle }
    }

    private fun looksLikeStoragePath(value: String): Boolean {
        val trimmed = value.trim()
        return trimmed.contains("%2F", ignoreCase = true) ||
            trimmed.contains("%3A", ignoreCase = true) ||
            trimmed.startsWith("content://", ignoreCase = true) ||
            trimmed.startsWith("primary:", ignoreCase = true) ||
            trimmed.startsWith("home:", ignoreCase = true) ||
            trimmed.startsWith("raw:", ignoreCase = true) ||
            trimmed.contains("/storage/", ignoreCase = true)
    }

    fun parseMetadataFromName(rawName: String): GameMetadata {
        val ext = rawName.substringAfterLast('.', "").lowercase()
        val cleanName = if (ext in setOf("iso", "bin", "cue", "img", "mdf", "gz", "cso", "zso", "chd", "elf")) {
            rawName.substringBeforeLast('.').trim()
        } else {
            rawName.trim()
        }
        val serial = extractSerialFromName(cleanName)
        val title = cleanName
            .replace(Regex("""(?i)\b([A-Z]{4})[-_. ]?(\d{3})[-_. ]?(\d{2})\b"""), " ")
            .replace(Regex("""(?i)\b([A-Z]{4})[-_. ]?(\d{5})\b"""), " ")
            .replace(Regex("""\[[^]]*]|\([^)]*\)"""), " ")
            .replace(Regex("""\b(disc|disk|cd|dvd)\s*\d+\b""", RegexOption.IGNORE_CASE), " ")
            .replace('_', ' ')
            .replace(Regex("""\s+"""), " ")
            .trim()
            .ifBlank { cleanName }
        return GameMetadata(title = title, serial = serial, serialWithCrc = serial)
    }

    private fun extractSerialFromName(value: String): String? {
        val normalized = value.uppercase(Locale.ROOT)
        val fullPattern = Regex("""\b([A-Z]{4})[-_. ]?(\d{3})[-_. ]?(\d{2})\b""")
        val compactPattern = Regex("""\b([A-Z]{4})[-_. ]?(\d{5})\b""")
        return fullPattern.find(normalized)?.let { match ->
            "${match.groupValues[1]}-${match.groupValues[2]}${match.groupValues[3]}"
        } ?: compactPattern.find(normalized)?.let { match ->
            "${match.groupValues[1]}-${match.groupValues[2]}"
        }
    }

    suspend fun saveState(slot: Int): Boolean {
        if (!isNativeLoaded || !isVmActive) return false
        val saved = runSerial {
            try {
                NativeApp.saveStateToSlot(slot)
            } catch (_: Exception) {
                false
            }
        }
        if (saved) {
            val savePath = runCatching { NativeApp.getCurrentSaveStatePath(slot) }.getOrNull()
            if (!savePath.isNullOrBlank()) {
                captureSaveStatePreview(savePath)
            }
        }
        return saved
    }

    private suspend fun captureSaveStatePreview(savePath: String) {
        val surface = lastSurface
        val width = lastSurfaceWidth
        val height = lastSurfaceHeight
        if (surface == null || !surface.isValid || width <= 0 || height <= 0) return
        val bitmap = withContext(Dispatchers.Main) {
            runCatching {
                val frame = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
                val copied = suspendCancellableCoroutine { continuation ->
                    try {
                        PixelCopy.request(
                            surface,
                            frame,
                            { result ->
                                if (continuation.isActive) {
                                    continuation.resume(result == PixelCopy.SUCCESS)
                                }
                            },
                            Handler(Looper.getMainLooper())
                        )
                    } catch (_: Throwable) {
                        if (continuation.isActive) continuation.resume(false)
                    }
                }
                if (copied) {
                    frame
                } else {
                    frame.recycle()
                    null
                }
            }.getOrNull()
        } ?: return
        val preview = bitmap
        withContext(Dispatchers.IO) {
            runCatching {
                FileOutputStream("$savePath.png").use { output ->
                    preview.compress(Bitmap.CompressFormat.PNG, 100, output)
                }
                invalidateCoverImage("$savePath.png")
            }
            preview.recycle()
        }
    }

    suspend fun loadState(slot: Int): Boolean {
        if (!isNativeLoaded || !isVmActive) return false
        return runSerial {
            try {
                val success = NativeApp.loadStateFromSlot(slot)
                if (success) {
                    runCatching { rebindSurface() }
                    runCatching { NativeApp.resume() }
                }
                success
            } catch (_: Exception) {
                false
            }
        }
    }

    /**
     * Replaces the mounted disc without restarting the VM. Native code performs
     * the disc mutation on the CPU thread and restores the previous image on failure.
     */
    suspend fun changeDisc(path: String): Boolean {
        if (!isNativeLoaded || !isVmActive || path.isBlank()) return false
        return runSerial {
            try {
                NativeApp.logCrashBreadcrumb("disc swap requested pathType=${path.substringBefore(':', "file")}")
                NativeApp.changeDisc(path)
            } catch (error: Exception) {
                Log.e(TAG, "Disc swap failed", error)
                false
            }
        }
    }

    suspend fun setRenderer(gpuType: Int): Boolean {
        return rendererSwitchMutex.withLock {
            val resolvedRenderer = normalizeRenderer(gpuType)
            val cacheKey = "EmuCore/GS:Renderer"
            val rendererChanged = settingsCache[cacheKey] != resolvedRenderer.toString()
            NativeApp.logCrashBreadcrumb(
                "renderer change requested renderer=${rendererName(resolvedRenderer)}($resolvedRenderer) vmActive=$isVmActive"
            )
            Log.i(TAG, "Renderer change requested: ${rendererName(resolvedRenderer)}($resolvedRenderer) vmActive=$isVmActive")
            val switched = performRuntimeOps(rendererExecutionOps(resolvedRenderer))
            if (!switched) return@withLock false

            settingsCache[cacheKey] = resolvedRenderer.toString()
            if (isVmActive && rendererChanged) {
                // The software presenter reads frames through ANativeWindow_lock,
                // which leaves the Surface's BufferQueue connected through the
                // CPU API until the Surface is destroyed. EGL and Vulkan cannot
                // attach to an already-connected BufferQueue, so a hardware
                // session started on the same Surface fails and silently falls
                // back. Recreate the SurfaceView and wait until the replacement
                // Surface is bound before the single session restart.
                if (CoreRuntime.isActiveRendererSoftware() && resolvedRenderer != RendererDefaults.SOFTWARE) {
                    recreatePresentationSurfaceAndWait()
                }
                // The core re-negotiates the renderer only on boot, so the
                // in-game switch is a state-preserving session restart.
                val restarted = runSerial { NativeApp.restartRenderer(resolvedRenderer) }
                isVmActive = NativeApp.hasOwnedVm()
                if (!restarted) {
                    Log.e(TAG, "Renderer restart failed for ${rendererName(resolvedRenderer)}")
                    return@withLock false
                }
            }
            true
        }
    }

    /**
     * Recreates the SurfaceView and waits until the replacement Surface is
     * bound to [CoreRuntime]. Used before a hardware renderer takes over a
     * Surface that was previously presented through the CPU buffer API.
     */
    private suspend fun recreatePresentationSurfaceAndWait(timeoutMs: Long = 5000L): Boolean {
        val previousSurface = lastSurface
        _presentationSurfaceGeneration.value += 1
        val recreated = withTimeoutOrNull(timeoutMs) {
            var ready = false
            while (!ready) {
                val current = CoreRuntime.currentSurface()
                ready = current != null && current !== previousSurface && current.isValid
                if (!ready) delay(16)
            }
            true
        } ?: false
        if (!recreated) Log.w(TAG, "Timed out waiting for the recreated presentation surface")
        return recreated
    }

    suspend fun setUpscaleMultiplier(multiplier: Float) {
        val normalized = normalizeUpscale(multiplier)
        performRuntimeOps(listOf(upscaleOp(normalized)))
    }

    suspend fun setAspectRatio(type: Int) {
        performRuntimeOps(listOf(aspectOp(type)))
    }

    suspend fun setDisplayCrop(value: DisplayCrop) {
        NativeApp.setDisplayCrop(value.sanitized())
    }

    suspend fun setFrameLimitEnabled(enabled: Boolean) {
        if (!isNativeLoaded) return
        val value = enabled.toString()
        val cacheKey = "EmuCore/GS:FrameLimitEnable"
        if (settingsCache[cacheKey] == value) return
        runSerial {
            NativeApp.setFrameLimitEnabled(enabled)
        }
        settingsCache[cacheKey] = value
    }

    suspend fun setFrameSkip(value: Int) {
        performRuntimeOps(listOf(frameSkipOp(value)))
    }

    suspend fun setTargetFps(targetFps: Int) {
        setSetting("EmuCore/GS", "TargetFps", "int", targetFps.coerceIn(0, 120).toString())
    }

    fun setPadButton(padIndex: Int, index: Int, range: Int, pressed: Boolean) {
        if (!isNativeLoaded) return
        try {
            if (RemotePlaySession.forwardGuestButton(index, range, pressed)) return
            val mappedPadIndex = NetPlaySession.mapAndSendLocalButton(padIndex, index, range, pressed)
            NativeApp.setPadButton(mappedPadIndex, index, range, pressed)
        } catch (_: Exception) { }
    }

    fun resetKeyStatus() {
        if (!isNativeLoaded || !isVmActive) return
        launchSerial {
            if (!isVmActive || !runCatching { NativeApp.hasValidVm() }.getOrDefault(false)) return@launchSerial
            try {
                NativeApp.resetKeyStatus()
            } catch (_: Exception) { }
        }
    }

    fun resetPadState(padIndex: Int) {
        if (!isNativeLoaded || !isVmActive) return
        launchSerial {
            if (!isVmActive || !runCatching { NativeApp.hasValidVm() }.getOrDefault(false)) return@launchSerial
            try {
                NativeApp.resetPadState(padIndex)
            } catch (_: Exception) { }
        }
    }

    suspend fun setPadVibration(enabled: Boolean) {
        setSetting("InputSources", "PadVibration", "bool", enabled.toString())
    }

    fun onSurfaceCreated(generation: Long) {
        if (!isNativeLoaded) return
        if (generation != _presentationSurfaceGeneration.value) return
        Log.i(TAG, "onSurfaceCreated: generation=$generation")
        NativeApp.setCrashContextString("emu_surface_state", "created")
        NativeApp.logCrashBreadcrumb("surfaceCreated")
        launchSerial {
            try {
                NativeApp.onNativeSurfaceCreated()
                Log.i(TAG, "onSurfaceCreated: native callback done")
            } catch (e: Exception) {
                Log.e(TAG, "onSurfaceCreated: native callback failed", e)
            }
        }
    }

    fun onSurfaceChanged(surface: Surface, width: Int, height: Int, generation: Long) {
        if (!isNativeLoaded) return
        if (generation != _presentationSurfaceGeneration.value) {
            Log.i(TAG, "Ignoring stale surfaceChanged generation=$generation")
            return
        }
        val eventVersion = ++surfaceEventVersion
        Log.i(TAG, "onSurfaceChanged: width=$width height=$height valid=${surface.isValid} generation=$generation eventVersion=$eventVersion")
        lastSurface = surface
        lastSurfaceWidth = width
        lastSurfaceHeight = height
        NativeApp.setCrashContextString("emu_surface_state", "changed")
        NativeApp.setCrashContextInt("emu_surface_width", width)
        NativeApp.setCrashContextInt("emu_surface_height", height)
        NativeApp.setCrashContextBool("emu_surface_valid", surface.isValid)
        NativeApp.logCrashBreadcrumb("surfaceChanged width=$width height=$height valid=${surface.isValid}")
        launchSerial {
            if (surfaceEventVersion != eventVersion) {
                Log.w(TAG, "onSurfaceChanged: eventVersion mismatch ($eventVersion != $surfaceEventVersion), skipping")
                return@launchSerial
            }
            try {
                NativeApp.onNativeSurfaceChanged(surface, width, height)
                Log.i(TAG, "onSurfaceChanged: native callback done")
            } catch (e: Exception) {
                Log.e(TAG, "onSurfaceChanged: native callback failed", e)
            }
        }
    }

    fun onSurfaceDestroyed(generation: Long) {
        if (!isNativeLoaded) return
        if (generation != _presentationSurfaceGeneration.value) {
            Log.i(TAG, "Ignoring stale surfaceDestroyed generation=$generation")
            return
        }
        val oldVersion = surfaceEventVersion
        ++surfaceEventVersion
        lastSurface = null
        lastSurfaceWidth = 0
        lastSurfaceHeight = 0
        Log.i(TAG, "onSurfaceDestroyed: called, version $oldVersion -> $surfaceEventVersion")
        NativeApp.setCrashContextString("emu_surface_state", "destroyed")
        NativeApp.logCrashBreadcrumb("surfaceDestroyed")
        runCatching { NativeApp.pause() }
        Log.i(TAG, "onSurfaceDestroyed: pause done, calling native destroy")
        // Android invalidates the BufferQueue as soon as this callback returns.
        // Detach GS synchronously so it cannot keep presenting to an abandoned Surface.
        try {
            NativeApp.onNativeSurfaceDestroyed()
            Log.i(TAG, "onSurfaceDestroyed: native destroy done")
        } catch (e: Exception) {
            Log.e(TAG, "onSurfaceDestroyed: native destroy failed", e)
        }
    }

    private fun rebindSurface() {
        val surface = lastSurface ?: return
        val width = lastSurfaceWidth
        val height = lastSurfaceHeight
        if (!surface.isValid || width <= 0 || height <= 0) {
            Log.w(TAG, "rebindSurface: skipped (valid=${surface.isValid} w=$width h=$height)")
            return
        }
        Log.i(TAG, "rebindSurface: width=$width height=$height")
        NativeApp.logCrashBreadcrumb("rebindSurface width=$width height=$height")
        try {
            NativeApp.onNativeSurfaceChanged(surface, width, height)
        } catch (_: Exception) { }
    }

    suspend fun setSetting(section: String, key: String, type: String, value: String) {
        if (!isNativeLoaded) return
        val cacheKey = "$section:$key"
        if (settingsCache[cacheKey] == value) return
        if (performRuntimeOps(listOf(settingOp(section, key, type, value))))
            settingsCache[cacheKey] = value
    }

    suspend fun reloadPatches() {
        if (!isNativeLoaded) return
        runSerial { NativeApp.reloadPatches() }
    }

    fun getSetting(section: String, key: String, type: String): String? {
        if (!isNativeLoaded) return null
        return try {
            NativeApp.getSetting(section, key, type)
        } catch (_: Exception) {
            null
        }
    }

}
