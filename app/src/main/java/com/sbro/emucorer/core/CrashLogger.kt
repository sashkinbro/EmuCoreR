package com.sbro.emucorer.core

import android.content.Context
import android.os.Build
import android.util.Log
import com.sbro.emucorer.BuildConfig
import java.io.File
import java.io.FileOutputStream
import java.io.PrintWriter
import java.io.StringWriter
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale


/**
 * Crash and startup-failure log.
 *
 * Two kinds of problems have to be visible in release builds:
 *  - Java exceptions that kill the process (uncaught exception handler), and
 *  - native faults in the core, the GPU driver or libvulkan, which never reach
 *    the Java handler. Those are captured by the native signal handlers that
 *    [NativeCoreBridge.nativeInstallCrashHandler] installs, and the emulator
 *    startup stages recorded through [stage] show how far the launch got.
 */
object CrashLogger {

    private const val TAG = "CrashLogger"
    private const val LOG_DIR = "logs"
    private const val LOG_FILE = "crash.log"
    private const val NATIVE_STAGE_FILE = "last-session.log"
    private const val SESSION_MARKER_FILE = "session.active"
    private const val MAX_LOG_SIZE_BYTES = 5 * 1024 * 1024L  // 5 MB
    private const val KEEP_BYTES_AFTER_ROTATION = 4 * 1024 * 1024  // keep last 4 MB

    @Volatile
    private var logFile: File? = null

    @Volatile
    private var logDirectory: File? = null

    @Volatile
    private var previousHandler: Thread.UncaughtExceptionHandler? = null

    private val dateFmt = SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US)

    fun init(context: Context) {
        val directory = resolveLogDirectory(context)
        logDirectory = directory
        logFile = directory?.let { File(it, LOG_FILE) }
        reportPreviousRun(directory)
        installUncaughtExceptionHandler()
        writeStartupLine(context)
        installNativeCrashHandler(directory)
    }

    fun logError(tag: String, message: String, throwable: Throwable? = null) {
        val body = buildString {
            append("[$tag] $message")
            if (throwable != null) {
                append("\n")
                append(throwable.stackTraceString())
            }
        }
        writeEntry("ERROR", body)
    }

    /**
     * Records an emulator startup stage. Always written (including release
     * builds) so a failed or crashed launch leaves a trail behind, but it must
     * only be called on launch boundaries, never per frame.
     */
    fun stage(name: String, detail: String = "") {
        val line = if (detail.isBlank()) name else "$name — $detail"
        writeEntry("STAGE", line)
        runCatching { NativeCoreBridge().nativeMarkStage(line) }
    }

    fun markSessionStart(label: String) {
        val directory = logDirectory ?: return
        runCatching {
            File(directory, SESSION_MARKER_FILE)
                .writeText("[${dateFmt.format(Date())}] $label\n")
        }
    }

    fun markSessionEnd() {
        val directory = logDirectory ?: return
        runCatching { File(directory, SESSION_MARKER_FILE).delete() }
    }

    fun logInfo(tag: String, message: String) {
        if (!BuildConfig.DEBUG) return
        writeEntry("INFO", "[$tag] $message")
    }

    fun logContext(key: String, value: Any?) {
        if (!BuildConfig.DEBUG) return
        writeEntry("CTX", "$key = $value")
    }

    // ─── Private helpers ──────────────────────────────────────────────────────

    private fun resolveLogDirectory(context: Context): File? {
        return try {
            val dir = File(context.getExternalFilesDir(null) ?: context.filesDir, LOG_DIR)
            if (!dir.exists()) dir.mkdirs()
            dir
        } catch (e: Exception) {
            Log.e(TAG, "Failed to resolve log file", e)
            null
        }
    }

    private fun installNativeCrashHandler(directory: File?) {
        if (directory == null) return
        runCatching { NativeCoreBridge().nativeInstallCrashHandler(directory.absolutePath) }
            .onFailure { Log.w(TAG, "Native crash handler unavailable", it) }
    }

    /**
     * A session marker that survived the previous process means the runtime was
     * killed without a clean shutdown (native fault, OOM kill, force stop).
     * The native stage trail is then the only evidence of how far the launch
     * got, so it is folded into crash.log before being cleared.
     */
    private fun reportPreviousRun(directory: File?) {
        if (directory == null) return
        val marker = File(directory, SESSION_MARKER_FILE)
        val stages = File(directory, NATIVE_STAGE_FILE)
        if (marker.exists()) {
            val detail = runCatching { marker.readText().trim() }.getOrDefault("")
            val trail = runCatching { stages.readText().trim() }.getOrDefault("")
            val body = buildString {
                append("emulator session ended without a clean shutdown ")
                append("(native crash, OOM kill or force stop)")
                if (detail.isNotBlank()) append("\n").append(detail)
                if (trail.isNotBlank()) append("\nlast recorded stages:\n").append(trail)
            }
            writeEntry("PREVIOUS-RUN", body)
        }
        runCatching { marker.delete() }
        runCatching { stages.delete() }
    }

    private fun writeStartupLine(context: Context) {
        val appVersion = runCatching {
            val info = context.packageManager.getPackageInfo(context.packageName, 0)
            // getLongVersionCode() is API 28+, but minSdk is 26.
            val code = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
                info.longVersionCode
            } else {
                @Suppress("DEPRECATION")
                info.versionCode.toLong()
            }
            "${info.versionName} ($code)"
        }.getOrDefault("?")

        val device = "${Build.MANUFACTURER} ${Build.MODEL}"
        val androidVer = "${Build.VERSION.RELEASE} (API ${Build.VERSION.SDK_INT})"
        val abi = Build.SUPPORTED_ABIS.firstOrNull() ?: "?"
        val pageSize = runCatching { AndroidNativeCoreSelector.runtimePageSizeBytes() }.getOrDefault(0L)
        val coreVariant = if (pageSize == 16_384L) "16k" else "standard"

        writeEntry(
            "START",
            "APP START — v$appVersion | Android $androidVer | $device | $abi | " +
                "page=${pageSize}B core=$coreVariant"
        )
    }

    private fun installUncaughtExceptionHandler() {
        previousHandler = Thread.getDefaultUncaughtExceptionHandler()
        Thread.setDefaultUncaughtExceptionHandler { thread, throwable ->
            try {
                writeEntry(
                    "CRASH",
                    "Thread: ${thread.name}\n${throwable.stackTraceString()}",
                    sync = true
                )
            } catch (_: Throwable) {
                // Don't let logger crash prevent the original crash from propagating
            }
            previousHandler?.uncaughtException(thread, throwable)
        }
    }

    private fun writeEntry(level: String, body: String, sync: Boolean = false) {
        val file = logFile ?: return
        val timestamp = dateFmt.format(Date())
        val line = "[$timestamp] $level: $body\n"

        try {
            rotateIfNeeded(file)
            FileOutputStream(file, /* append = */ true).use { stream ->
                stream.write(line.toByteArray())
                stream.flush()
                if (sync) runCatching { stream.fd.sync() }
            }
        } catch (e: Exception) {
            Log.e(TAG, "Failed to write log entry", e)
        }
    }

    private fun rotateIfNeeded(file: File) {
        if (!file.exists() || file.length() < MAX_LOG_SIZE_BYTES) return
        try {
            val content = file.readBytes()
            // Keep only the last KEEP_BYTES_AFTER_ROTATION bytes to avoid trimming mid-line chaos
            val trimStart = (content.size - KEEP_BYTES_AFTER_ROTATION).coerceAtLeast(0)
            // Find the next newline after trimStart so we start on a clean line
            var safeStart = trimStart
            while (safeStart < content.size && content[safeStart] != '\n'.code.toByte()) safeStart++
            safeStart++ // skip the newline itself

            val rotationHeader = "=== LOG ROTATED at ${dateFmt.format(Date())} (old entries trimmed) ===\n"
            file.writeBytes(rotationHeader.toByteArray() + content.copyOfRange(safeStart.coerceAtMost(content.size), content.size))
        } catch (e: Exception) {
            Log.e(TAG, "Failed to rotate log", e)
        }
    }

    private fun Throwable.stackTraceString(): String {
        val sw = StringWriter()
        printStackTrace(PrintWriter(sw))
        return sw.toString().trimEnd()
    }
}
