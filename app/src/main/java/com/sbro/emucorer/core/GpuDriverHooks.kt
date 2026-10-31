// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: GPL-3.0-or-later
package com.sbro.emucorer.core

import android.content.Context
import android.os.Build
import android.util.Log
import com.sbro.emucorer.BuildConfig
import java.io.File
import java.util.zip.ZipFile

/**
 * adrenotools loads its hooks (libmain_hook.so, libhook_impl.so, ...) through a
 * linker namespace rooted at a directory, but the app keeps every native
 * library stored inside the APK (useLegacyPackaging = false: no install-time
 * unpacking of ~38 MB of libraries). The four hook libraries are only ~70 KB
 * together, so they are copied out of the APK into app storage on first use and
 * that directory is handed to the Vulkan driver loader.
 *
 * The custom driver itself is already read from app storage by adrenotools, so
 * this keeps custom Vulkan drivers working without extracting the whole
 * library payload at install time.
 */
internal object GpuDriverHooks {

    private const val TAG = "GpuDriverHooks"
    private const val STAMP_FILE = ".hooks-version"

    internal val LIBRARIES = listOf(
        "libmain_hook.so",
        "libhook_impl.so",
        "libfile_redirect_hook.so",
        "libgsl_alloc_hook.so"
    )

    /**
     * Returns the directory that contains the hook libraries, or null when they
     * are unavailable. Uses the installer-provided library directory when the
     * build extracts libraries (legacy packaging), otherwise unpacks the hooks
     * from the APK.
     */
    fun ensureAvailable(context: Context): String? {
        val installedDirectory = context.applicationInfo.nativeLibraryDir
        if (!installedDirectory.isNullOrBlank() && File(installedDirectory, "libmain_hook.so").isFile) {
            return installedDirectory
        }

        val target = File(context.filesDir, "adrenotools-hooks")
        val apkFiles = buildList {
            context.applicationInfo.sourceDir?.let(::File)?.let(::add)
            context.applicationInfo.splitSourceDirs?.forEach { path -> path?.let { add(File(it)) } }
        }
        val version = buildString {
            append(BuildConfig.VERSION_CODE)
            for (apk in apkFiles) append('|').append(apk.length()).append('@').append(apk.lastModified())
        }

        val stamp = File(target, STAMP_FILE)
        val current = stamp.isFile &&
            runCatching { stamp.readText().trim() }.getOrNull() == version &&
            LIBRARIES.all { File(target, it).isFile }
        if (current) return target.absolutePath

        val extracted = runCatching { extract(apkFiles, target) }.getOrElse { error ->
            Log.w(TAG, "Unable to extract the Vulkan driver hooks", error)
            false
        }
        if (!extracted) return null
        runCatching { stamp.writeText(version) }
        return target.absolutePath
    }

    private fun extract(apkFiles: List<File>, target: File): Boolean {
        if (!target.isDirectory && !target.mkdirs()) {
            Log.w(TAG, "Unable to create ${target.absolutePath}")
            return false
        }
        val abi = Build.SUPPORTED_ABIS.firstOrNull() ?: "arm64-v8a"
        val pending = LIBRARIES.toMutableSet()
        for (apk in apkFiles) {
            if (pending.isEmpty()) break
            if (!apk.isFile) continue
            ZipFile(apk).use { zip ->
                for (name in pending.toList()) {
                    val entry = zip.getEntry("lib/$abi/$name") ?: continue
                    val destination = File(target, name)
                    zip.getInputStream(entry).use { input ->
                        destination.outputStream().use { output -> input.copyTo(output) }
                    }
                    // dlopen only needs the library to be readable.
                    destination.setReadable(true, false)
                    pending.remove(name)
                }
            }
        }
        if (pending.isNotEmpty()) {
            Log.w(TAG, "Hook libraries are missing from the APK: $pending")
            return false
        }
        return true
    }
}
