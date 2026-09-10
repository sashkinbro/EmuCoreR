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

        @Volatile
        private var cachedIndex: Map<String, String>? = null
    }
}
