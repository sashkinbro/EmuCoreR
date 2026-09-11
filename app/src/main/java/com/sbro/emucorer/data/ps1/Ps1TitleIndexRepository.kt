package com.sbro.emucorer.data.ps1

import android.content.Context
import java.util.Locale
import kotlinx.serialization.json.Json

/**
 * Offline serial -> English title index generated from the Redump/No-Intro
 * PlayStation metadata bundled with libretro-database.
 */
class Ps1TitleIndexRepository(context: Context) {
    private val appContext = context.applicationContext
    private val json = Json { ignoreUnknownKeys = true }

    fun titleForSerial(serial: String?): String? {
        val key = normalizeSerial(serial) ?: return null
        return loadIndex()[key]
    }

    /**
     * Best-effort reverse lookup: recovers a disc serial from a game title when
     * the filename does not contain one. Used by the library so cover art can
     * still be downloaded for conventionally named dumps.
     */
    fun serialForTitle(title: String?, regionHint: Char? = null): String? {
        val key = normalizeTitle(title) ?: return null
        val candidates = loadReverseIndex()[key] ?: return null
        if (candidates.isEmpty()) return null
        if (regionHint != null) {
            candidates.firstOrNull { serialSortKey(it) == regionHint }?.let { return it }
        }
        return candidates.first()
    }

    private fun loadReverseIndex(): Map<String, List<String>> {
        cachedReverseIndex?.let { return it }
        synchronized(cacheLock) {
            cachedReverseIndex?.let { return it }
            val reverse = LinkedHashMap<String, MutableList<String>>()
            loadIndex().forEach { (serial, title) ->
                val key = normalizeTitle(title) ?: return@forEach
                reverse.getOrPut(key) { mutableListOf() }.add(serial)
            }
            // Prefer NTSC-U, then Europe, then Japan, then anything else.
            val ordered = reverse.mapValues { (_, serials) ->
                serials.sortedBy { REGION_PRIORITY.indexOf(serialSortKey(it)).let { rank -> if (rank < 0) REGION_PRIORITY.size else rank } }
            }
            cachedReverseIndex = ordered
            return ordered
        }
    }

    private fun normalizeTitle(title: String?): String? {
        val normalized = title
            ?.lowercase(Locale.ROOT)
            ?.replace(Regex("""\[[^]]*]|\([^)]*\)"""), " ")
            ?.replace(Regex("""\b(disc|disk|cd|dvd)\s*\d+\b"""), " ")
            ?.replace(Regex("""\b(usa|us|europe|eur|japan|jpn|asia|korea|world|rev\s*[a-z0-9]+|beta|demo|proto)\b"""), " ")
            ?.replace(Regex("""[^a-z0-9]+"""), " ")
            ?.replace(Regex("""\s+"""), " ")
            ?.trim()
            .orEmpty()
        return normalized.takeIf { it.length >= 3 }
    }

    /** Region sort key derived from the serial prefix, or '?' when unknown. */
    private fun serialSortKey(serial: String): Char {
        val compact = serial.uppercase(Locale.ROOT).replace(Regex("[^A-Z0-9]"), "")
        return when {
            compact.startsWith("SLUS") || compact.startsWith("SCUS") || compact.startsWith("PBPX") -> 'U'
            compact.startsWith("SLES") || compact.startsWith("SCES") || compact.startsWith("SLED") -> 'E'
            compact.startsWith("SLPS") || compact.startsWith("SCPS") || compact.startsWith("SLPM") ||
                compact.startsWith("SCPM") || compact.startsWith("SIPS") -> 'J'
            else -> '?'
        }
    }

    private fun loadIndex(): Map<String, String> {
        cachedIndex?.let { return it }
        synchronized(cacheLock) {
            cachedIndex?.let { return it }
            val parsed = runCatching {
                appContext.assets.open(ASSET_PATH).bufferedReader().use { reader ->
                    json.decodeFromString<Map<String, String>>(reader.readText())
                }
            }.getOrDefault(emptyMap())
            cachedIndex = parsed
            return parsed
        }
    }

    private fun normalizeSerial(serial: String?): String? {
        val normalized = serial
            ?.trim()
            ?.uppercase(Locale.ROOT)
            ?.replace(Regex("[^A-Z0-9]"), "")
            .orEmpty()
        return normalized.takeIf { it.length >= 6 }
    }

    private companion object {
        private const val ASSET_PATH = "catalog/ps1_titles.json"
        private val cacheLock = Any()
        private val REGION_PRIORITY = listOf('U', 'E', 'J')

        @Volatile
        private var cachedIndex: Map<String, String>? = null

        @Volatile
        private var cachedReverseIndex: Map<String, List<String>>? = null
    }
}
