package com.sbro.emucorer.data

import android.content.Context
import com.sbro.emucorer.core.EmulatorStorage
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream
import java.net.HttpURLConnection
import java.net.URL
import java.util.Locale
import java.util.UUID
import java.util.zip.ZipFile

enum class PatchDatabaseInstallStage {
    DOWNLOADING,
    INSTALLING
}

data class PatchDatabaseInstallProgress(
    val stage: PatchDatabaseInstallStage,
    val downloadedBytes: Long = 0L,
    val totalBytes: Long = 0L
) {
    val fraction: Float?
        get() = totalBytes.takeIf { it > 0L }?.let { total ->
            (downloadedBytes.toDouble() / total.toDouble()).toFloat().coerceIn(0f, 1f)
        }
}

/**
 * Downloads the community patch archive and caches its patch files inside the
 * emulator data `patches` directory so the core can pick them up by serial.
 *
 * Only entries from the archive's `patches/` directory are extracted; cheat
 * files are ignored. Files that were installed by a previous run but are no
 * longer present in the archive are removed, while patches added manually by
 * the user are never touched.
 */
class PatchDatabaseDownloader(private val context: Context) {
    private val preferences = AppPreferences(context)
    private val stateFile = File(EmulatorStorage.appStateDir(context), "patch-database.json")

    fun patchesDirectory(): File =
        EmulatorStorage.patchesDir(context, preferences.getEmulatorDataPathSync())

    fun installedPatchCount(): Int {
        val recorded = readInstalledFiles().size
        return if (recorded > 0) recorded else countPatchFiles()
    }

    fun hasConfiguredSource(): Boolean = effectiveSourceUrl() != null

    fun effectiveSourceUrl(): String? {
        val custom = preferences.getPatchDatabaseCustomUrlSync()?.takeIf { it.isNotBlank() }
        return if (preferences.getPatchDatabaseUseOfficialSync()) OFFICIAL_ARCHIVE_URL else custom
    }

    fun download(
        onProgress: (PatchDatabaseInstallProgress) -> Unit = {}
    ): Result<Int> = runCatching {
        val source = effectiveSourceUrl() ?: error("No patch source configured")
        val archive = File(context.cacheDir, "patch-database-${UUID.randomUUID()}.zip")
        try {
            downloadArchive(source, archive, onProgress)
            onProgress(PatchDatabaseInstallProgress(PatchDatabaseInstallStage.INSTALLING))
            installArchive(source, archive)
        } finally {
            archive.delete()
        }
    }

    private fun downloadArchive(
        source: String,
        destination: File,
        onProgress: (PatchDatabaseInstallProgress) -> Unit
    ) {
        val connection = (URL(source).openConnection() as HttpURLConnection).apply {
            connectTimeout = 20_000
            readTimeout = 30_000
            instanceFollowRedirects = true
            setRequestProperty("User-Agent", "EmuCoreR-Android")
        }
        try {
            require(connection.responseCode in 200..299) { "HTTP ${connection.responseCode}" }
            val totalBytes = connection.contentLengthLong.coerceAtLeast(0L)
            require(totalBytes == 0L || totalBytes <= MAX_ARCHIVE_BYTES) { "Patch archive is too large" }
            onProgress(
                PatchDatabaseInstallProgress(
                    stage = PatchDatabaseInstallStage.DOWNLOADING,
                    totalBytes = totalBytes
                )
            )
            connection.inputStream.use { input ->
                FileOutputStream(destination).use { output ->
                    val buffer = ByteArray(64 * 1024)
                    var total = 0L
                    var lastReportedAtNanos = 0L
                    while (true) {
                        val read = input.read(buffer)
                        if (read < 0) break
                        total += read
                        require(total <= MAX_ARCHIVE_BYTES) { "Patch archive is too large" }
                        output.write(buffer, 0, read)
                        val now = System.nanoTime()
                        if (now - lastReportedAtNanos >= PROGRESS_UPDATE_INTERVAL_NANOS) {
                            onProgress(
                                PatchDatabaseInstallProgress(
                                    stage = PatchDatabaseInstallStage.DOWNLOADING,
                                    downloadedBytes = total,
                                    totalBytes = totalBytes
                                )
                            )
                            lastReportedAtNanos = now
                        }
                    }
                    output.fd.sync()
                    onProgress(
                        PatchDatabaseInstallProgress(
                            stage = PatchDatabaseInstallStage.DOWNLOADING,
                            downloadedBytes = total,
                            totalBytes = totalBytes.takeIf { it > 0L } ?: total
                        )
                    )
                }
            }
        } finally {
            connection.disconnect()
        }
    }

    private fun installArchive(source: String, archive: File): Int {
        val directory = patchesDirectory().apply { mkdirs() }
        val previousFiles = readInstalledFiles()
        val installedFiles = linkedSetOf<String>()
        var extractedBytes = 0L
        ZipFile(archive).use { zip ->
            val entries = zip.entries().toList().filterNot { it.isDirectory }
            require(entries.size <= MAX_ARCHIVE_ENTRIES) { "Too many files in patch archive" }
            entries.forEach { entry ->
                val path = entry.name.removePrefix("./").trimStart('/')
                val relative = when {
                    path.startsWith("patches/") -> path.removePrefix("patches/")
                    else -> path.substringAfterLast("/patches/", missingDelimiterValue = "")
                }
                if (relative.isBlank() || relative.contains('/')) return@forEach
                val extension = relative.substringAfterLast('.', "").lowercase(Locale.US)
                if (extension !in SUPPORTED_PATCH_EXTENSIONS) return@forEach
                val name = relative.replace(Regex("[^A-Za-z0-9._-]"), "_")
                if (name.isBlank()) return@forEach
                val output = File(directory, name)
                require(output.canonicalFile.parentFile == directory.canonicalFile) {
                    "Unsafe archive path"
                }
                zip.getInputStream(entry).use { input ->
                    output.outputStream().use { sink ->
                        val buffer = ByteArray(64 * 1024)
                        while (true) {
                            val read = input.read(buffer)
                            if (read < 0) break
                            extractedBytes += read
                            require(extractedBytes <= MAX_EXTRACTED_BYTES) {
                                "Expanded patch database is too large"
                            }
                            sink.write(buffer, 0, read)
                        }
                    }
                }
                installedFiles += name
            }
        }
        require(installedFiles.isNotEmpty()) { "No patch files in archive" }
        previousFiles
            .filterNot(installedFiles::contains)
            .forEach { name -> File(directory, name).takeIf(File::isFile)?.delete() }
        writeInstalledFiles(source, installedFiles)
        return installedFiles.size
    }

    private fun countPatchFiles(): Int = patchesDirectory()
        .listFiles { file ->
            file.isFile && file.extension.lowercase(Locale.US) in SUPPORTED_PATCH_EXTENSIONS
        }
        ?.size
        ?: 0

    private fun readInstalledFiles(): List<String> {
        if (!stateFile.exists()) return emptyList()
        return runCatching {
            val json = JSONObject(stateFile.readText())
            val files = json.optJSONArray("files") ?: return emptyList()
            buildList {
                for (index in 0 until files.length()) {
                    val name = files.optString(index)
                    if (name.isNotBlank()) add(name)
                }
            }
        }.getOrDefault(emptyList())
    }

    private fun writeInstalledFiles(source: String, files: Set<String>) {
        runCatching {
            stateFile.parentFile?.mkdirs()
            val json = JSONObject()
                .put("source", source)
                .put("installedAtEpochMs", System.currentTimeMillis())
                .put("files", JSONArray(files.toList().sorted()))
            stateFile.writeText(json.toString())
        }
    }

    companion object {
        const val OFFICIAL_ARCHIVE_URL =
            "https://github.com/duckstation/chtdb/archive/refs/heads/master.zip"
        const val OFFICIAL_SOURCE_PAGE_URL = "https://github.com/duckstation/chtdb"

        private val SUPPORTED_PATCH_EXTENSIONS = setOf("cht", "pnach")
        private const val MAX_ARCHIVE_BYTES = 128L * 1024L * 1024L
        private const val MAX_EXTRACTED_BYTES = 256L * 1024L * 1024L
        private const val MAX_ARCHIVE_ENTRIES = 20_000
        private const val PROGRESS_UPDATE_INTERVAL_NANOS = 100_000_000L
    }
}
