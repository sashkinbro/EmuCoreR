package com.sbro.emucorer.data

import android.content.Context
import android.util.Log
import com.sbro.emucorer.core.CatalogAccess
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import java.io.File
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
import java.util.Locale
import java.util.concurrent.ConcurrentHashMap

data class RemoteTexturePart(
    val downloadUrl: String,
    val sizeBytes: Long,
    val sha256: String
)

data class RemoteTexturePack(
    val id: String,
    val name: String,
    val gameTitle: String,
    val serials: List<String>,
    val version: String,
    val authors: List<String>,
    val credits: String,
    val description: String,
    val downloadUrl: String,
    val sourceUrl: String,
    val license: String,
    val sizeBytes: Long,
    val sha256: String,
    val fileCount: Int,
    val previewUrls: List<String>,
    val parts: List<RemoteTexturePart> = emptyList()
)

data class RemoteCheatPack(
    val id: String,
    val title: String,
    val serials: List<String>,
    val crc: String,
    val authors: List<String>,
    val description: String,
    val downloadUrl: String,
    val sourceUrl: String,
    val sourceName: String,
    val license: String,
    val blockCount: Int
)

data class RemoteCatalogResult<T>(
    val entries: List<T>,
    val fromCache: Boolean,
    val error: Throwable? = null,
    val cacheHit: Boolean = false
)

class RemoteContentCatalogRepository(context: Context) {
    private val appContext = context.applicationContext
    private val json = Json { ignoreUnknownKeys = true }
    private val cacheDir = File(appContext.filesDir, "remote-content").apply { mkdirs() }

    fun loadTextureCatalog(forceRefresh: Boolean = false): RemoteCatalogResult<RemoteTexturePack> = loadCatalog(
        urls = CatalogAccess.textureCatalogUrls(),
        cacheFile = File(cacheDir, "textures-v1.json"),
        parser = ::parseTextureCatalog,
        forceRefresh = forceRefresh
    )

    fun loadCheatCatalog(forceRefresh: Boolean = false): RemoteCatalogResult<RemoteCheatPack> = loadCatalog(
        urls = CatalogAccess.cheatCatalogUrls(),
        cacheFile = File(cacheDir, "cheats-v1.json"),
        parser = ::parseCheatCatalog,
        forceRefresh = forceRefresh
    )

    fun downloadCheatText(pack: RemoteCheatPack): String {
        require(pack.downloadUrl.isHttpsUrl()) { "Cheat download must use HTTPS" }
        val bytes = fetchBytes(pack.downloadUrl, MAX_CHEAT_BYTES)
        val text = bytes.toString(Charsets.UTF_8).removePrefix("\uFEFF")
        require(
            text.lineSequence().any { line ->
                val trimmed = line.trim()
                if (trimmed.isEmpty() || trimmed.startsWith("//") || trimmed.startsWith("#")) {
                    return@any false
                }
                val candidate = trimmed.substringBefore("//").substringBefore("#").trim()
                if (candidate.isEmpty()) {
                    return@any false
                }
                candidate.startsWith("patch=", ignoreCase = true) ||
                    candidate.startsWith("dpatch=", ignoreCase = true) ||
                    RAW_CHEAT_CODE_REGEX.matchEntire(candidate) != null ||
                    LIBRETRO_CHEAT_CODE_REGEX.containsMatchIn(candidate)
            }
        ) { "Downloaded file does not contain supported cheat codes" }
        return text
    }

    private fun <T> loadCatalog(
        urls: List<String>,
        cacheFile: File,
        parser: (String) -> List<T>,
        forceRefresh: Boolean
    ): RemoteCatalogResult<T> {
        val cachedEntries = if (cacheFile.isFile) {
            runCatching { parser(cacheFile.readText()) }
                .getOrNull()
                ?.takeIf { it.isNotEmpty() }
        } else {
            null
        }
        if (!forceRefresh && cachedEntries != null) {
            val cacheAge = (System.currentTimeMillis() - cacheFile.lastModified()).coerceAtLeast(0L)
            if (cacheAge <= CATALOG_CACHE_TTL_MS) {
                // A fresh cache is the normal fast path, not an offline fallback warning.
                // The screen only schedules a background refresh when the network was
                // not consulted recently, so an app-start prefetch is not repeated by
                // the first screen that opens right after.
                val lastFetch = lastNetworkFetchAt[cacheFile.name] ?: 0L
                val cacheHit = System.currentTimeMillis() - lastFetch > CATALOG_REFRESH_COOLDOWN_MS
                return RemoteCatalogResult(cachedEntries, fromCache = false, cacheHit = cacheHit)
            }
        }
        var lastError: Throwable? = null
        urls.forEach { url ->
            val result = runCatching { fetchCatalog(url, cacheFile, parser) }
            result.getOrNull()?.let { entries ->
                lastNetworkFetchAt[cacheFile.name] = System.currentTimeMillis()
                return RemoteCatalogResult(entries, fromCache = false)
            }
            lastError = result.exceptionOrNull()
            Log.w(TAG, "Catalog fetch failed for $url", lastError)
        }
        if (cachedEntries != null) {
            return RemoteCatalogResult(cachedEntries, fromCache = true, error = lastError, cacheHit = true)
        }
        return RemoteCatalogResult(emptyList(), fromCache = false, error = lastError)
    }

    private fun <T> fetchCatalog(url: String, cacheFile: File, parser: (String) -> List<T>): List<T> {
        var lastError: Throwable? = null
        for (attempt in 1..CATALOG_FETCH_ATTEMPTS) {
            val result = runCatching {
                val raw = fetchBytes(url, MAX_CATALOG_BYTES).toString(Charsets.UTF_8)
                val parsed = parser(raw)
                require(parsed.isNotEmpty()) { "Remote catalog is empty" }
                // A failed cache write must not fail the load: the entries are
                // already in memory and the next load simply tries again.
                runCatching { writeAtomically(cacheFile, raw) }
                parsed
            }
            result.getOrNull()?.let { return it }
            lastError = result.exceptionOrNull()
            val error = lastError
            if (error != null && error.isPermanentHttpFailure()) break
            if (attempt < CATALOG_FETCH_ATTEMPTS) {
                Log.w(TAG, "Catalog attempt $attempt/$CATALOG_FETCH_ATTEMPTS failed for $url", error)
                Thread.sleep(CATALOG_FETCH_RETRY_DELAY_MS * attempt)
            }
        }
        throw lastError ?: IOException("Catalog fetch failed")
    }

    private fun parseTextureCatalog(raw: String): List<RemoteTexturePack> {
        val root = json.parseToJsonElement(raw).jsonObject
        require(root.int("schemaVersion") == 1) { "Unsupported texture catalog version" }
        return root.array("entries").mapNotNull { element ->
            val item = element.jsonObject
            runCatching {
                RemoteTexturePack(
                    id = item.requiredString("id"),
                    name = item.requiredString("name"),
                    gameTitle = item.requiredString("gameTitle"),
                    serials = item.stringList("serials").mapNotNull(::normalizeSerial).distinct(),
                    version = item.requiredString("version"),
                    authors = item.stringList("authors").filter(String::isNotBlank),
                    credits = item.string("credits"),
                    description = item.string("description"),
                    downloadUrl = CatalogAccess.rewriteDownloadUrl(item.requiredString("downloadUrl").requireHttps()),
                    sourceUrl = item.requiredString("sourceUrl").requireHttps(),
                    license = item.string("license"),
                    sizeBytes = item.long("sizeBytes"),
                    sha256 = item.requiredString("sha256").uppercase(Locale.US),
                    fileCount = item.int("fileCount"),
                    previewUrls = item.stringList("previewUrls").filter(String::isHttpsUrl),
                    parts = item.array("parts").map { partElement ->
                        val part = partElement.jsonObject
                        RemoteTexturePart(
                            downloadUrl = CatalogAccess.rewriteDownloadUrl(part.requiredString("downloadUrl").requireHttps()),
                            sizeBytes = part.long("sizeBytes"),
                            sha256 = part.requiredString("sha256").uppercase(Locale.US)
                        )
                    }
                ).also { pack ->
                    require(pack.serials.isNotEmpty())
                    require(pack.authors.isNotEmpty())
                    require(pack.sizeBytes in 1..MAX_TEXTURE_ARCHIVE_BYTES)
                    require(pack.sha256.matches(Regex("[0-9A-F]{64}")))
                    require(pack.fileCount > 0)
                    require(pack.parts.isEmpty() || pack.parts.sumOf(RemoteTexturePart::sizeBytes) == pack.sizeBytes)
                    pack.parts.forEach { part ->
                        require(part.sizeBytes in 1..MAX_TEXTURE_PART_BYTES)
                        require(part.sha256.matches(Regex("[0-9A-F]{64}")))
                    }
                }
            }.getOrNull()
        }.distinctBy(RemoteTexturePack::id)
    }

    private fun parseCheatCatalog(raw: String): List<RemoteCheatPack> {
        val root = json.parseToJsonElement(raw).jsonObject
        require(root.int("schemaVersion") == 1) { "Unsupported cheat catalog version" }
        return root.array("entries").mapNotNull { element ->
            val item = element.jsonObject
            runCatching {
                RemoteCheatPack(
                    id = item.requiredString("id"),
                    title = item.requiredString("title"),
                    serials = item.stringList("serials").mapNotNull(::normalizeSerial).distinct(),
                    crc = item.string("crc").uppercase(Locale.US),
                    authors = item.stringList("authors").filter(String::isNotBlank),
                    description = item.string("description"),
                    downloadUrl = CatalogAccess.rewriteDownloadUrl(item.requiredString("downloadUrl").requireHttps()),
                    sourceUrl = item.requiredString("sourceUrl").requireHttps(),
                    sourceName = item.requiredString("sourceName"),
                    license = item.string("license"),
                    blockCount = item.int("blockCount")
                ).also { pack ->
                    require(pack.crc.isEmpty() || pack.crc.matches(Regex("[0-9A-F]{8}")))
                    require(pack.authors.isNotEmpty())
                    require(pack.blockCount > 0)
                }
            }.getOrNull()
        }.distinctBy(RemoteCheatPack::id)
    }

    private fun fetchBytes(url: String, maxBytes: Long): ByteArray {
        require(url.isHttpsUrl()) { "Only HTTPS downloads are allowed" }
        val connection = openConnection(url)
        return try {
            val contentLength = connection.contentLengthLong
            if (contentLength > maxBytes) throw IOException("Response is too large")
            connection.inputStream.buffered().use { input ->
                val output = java.io.ByteArrayOutputStream()
                val buffer = ByteArray(DEFAULT_BUFFER_SIZE)
                var total = 0L
                while (true) {
                    val read = input.read(buffer)
                    if (read < 0) break
                    total += read
                    if (total > maxBytes) throw IOException("Response is too large")
                    output.write(buffer, 0, read)
                }
                output.toByteArray()
            }
        } finally {
            connection.disconnect()
        }
    }

    private fun openConnection(url: String): HttpURLConnection {
        val connection = URL(url).openConnection() as HttpURLConnection
        connection.connectTimeout = 10_000
        connection.readTimeout = 20_000
        connection.instanceFollowRedirects = true
        connection.setRequestProperty("Accept", "application/json, application/octet-stream, text/plain, */*")
        connection.setRequestProperty("User-Agent", "EmuCoreR-Android")
        CatalogAccess.authorize(connection, appContext, url)
        connection.connect()
        if (connection.responseCode !in 200..299) {
            val code = connection.responseCode
            connection.disconnect()
            throw IOException("HTTP $code")
        }
        return connection
    }

    private fun writeAtomically(target: File, contents: String) {
        synchronized(CACHE_LOCK) {
            target.parentFile?.mkdirs()
            // A unique temp name keeps two concurrent writers (prefetch plus a
            // screen load) from clobbering each other's partial file.
            val temp = File(target.parentFile, "${target.name}.${System.nanoTime()}.tmp")
            try {
                temp.writeText(contents)
                if (!temp.renameTo(target)) {
                    temp.copyTo(target, overwrite = true)
                }
            } finally {
                temp.delete()
            }
        }
    }

    private companion object {
        const val TAG = "RemoteContentCatalog"
        const val MAX_CATALOG_BYTES = 8L * 1024L * 1024L
        const val MAX_CHEAT_BYTES = 2L * 1024L * 1024L
        const val MAX_TEXTURE_ARCHIVE_BYTES = 16L * 1024L * 1024L * 1024L
        const val MAX_TEXTURE_PART_BYTES = 2L * 1024L * 1024L * 1024L
        const val CATALOG_CACHE_TTL_MS = 6L * 60L * 60L * 1000L
        const val CATALOG_FETCH_ATTEMPTS = 3
        const val CATALOG_FETCH_RETRY_DELAY_MS = 600L
        const val CATALOG_REFRESH_COOLDOWN_MS = 60_000L

        val CACHE_LOCK = Any()
        val lastNetworkFetchAt = ConcurrentHashMap<String, Long>()

        val RAW_CHEAT_CODE_REGEX = Regex("[0-9A-Fa-f]{8}[\\s:+-]+[0-9A-Fa-f]{1,8}")
        val LIBRETRO_CHEAT_CODE_REGEX = Regex("^cheat\\d+_code\\s*=", RegexOption.IGNORE_CASE)
    }
}

private fun JsonObject.array(name: String): JsonArray = get(name)?.jsonArray ?: JsonArray(emptyList())
private fun JsonObject.string(name: String): String = get(name)?.jsonPrimitive?.content.orEmpty()
private fun JsonObject.requiredString(name: String): String = string(name).trim().also { require(it.isNotEmpty()) }
private fun JsonObject.int(name: String): Int = get(name)?.jsonPrimitive?.content?.toIntOrNull() ?: 0
private fun JsonObject.long(name: String): Long = get(name)?.jsonPrimitive?.content?.toLongOrNull() ?: 0L
private fun JsonObject.stringList(name: String): List<String> =
    get(name)?.jsonArray?.mapNotNull { it.jsonPrimitive.content.trim().takeIf(String::isNotEmpty) }.orEmpty()

private fun String.requireHttps(): String = trim().also { require(it.isHttpsUrl()) }
private fun String.isHttpsUrl(): Boolean = startsWith("https://", ignoreCase = true)

private val HTTP_STATUS_REGEX = Regex("HTTP (\\d{3})")

// A 4xx response (except 408/429) means retrying the same request cannot help.
private fun Throwable.isPermanentHttpFailure(): Boolean {
    val code = message?.let { HTTP_STATUS_REGEX.find(it)?.groupValues?.get(1)?.toIntOrNull() } ?: return false
    return code in 400..499 && code != 408 && code != 429
}

private fun normalizeSerial(value: String): String? {
    val compact = value.trim().uppercase(Locale.US).replace(Regex("[-_ ]"), "")
    if (!compact.matches(Regex("[A-Z]{4}[0-9]{5}"))) return null
    return "${compact.take(4)}-${compact.drop(4)}"
}
