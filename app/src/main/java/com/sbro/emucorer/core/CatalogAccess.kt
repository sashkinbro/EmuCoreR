package com.sbro.emucorer.core

import android.content.Context
import com.sbro.emucorer.BuildConfig
import java.net.HttpURLConnection

object CatalogAccess {
    private const val CHEAT_REPO_PREFIX =
        "https://raw.githubusercontent.com/sashkinbro/EmuCoreR-Cheat/"
    private const val TEXTURE_RELEASE_PREFIX =
        "https://github.com/sashkinbro/EmuCoreR-Textures/releases/download/"

    val workerBaseUrl: String
        get() = BuildConfig.CATALOG_WORKER_URL.trim().trimEnd('/')

    val isConfigured: Boolean
        get() = workerBaseUrl.startsWith("https://")

    fun textureCatalogUrls(): List<String> = catalogUrls("textures/catalog.json")

    fun cheatCatalogUrls(): List<String> = catalogUrls("cheats/catalog.json")

    fun authorize(connection: HttpURLConnection, context: Context, url: String) {
        if (!isConfigured || !url.startsWith("$workerBaseUrl/")) return
        val key = BuildConfig.FEATURE_KEY
        if (key.isBlank() || !FeatureGate.hasValidKey()) return
        connection.setRequestProperty("X-EmuCoreR-Key", key)
        connection.setRequestProperty("X-EmuCoreR-Package", context.packageName)
    }

    fun rewriteDownloadUrl(url: String): String {
        if (!isConfigured) return url
        if (url.startsWith(CHEAT_REPO_PREFIX)) {
            return "$workerBaseUrl/v1/cheats/file/" + url.removePrefix(CHEAT_REPO_PREFIX)
        }
        if (url.startsWith(TEXTURE_RELEASE_PREFIX)) {
            return "$workerBaseUrl/v1/textures/asset/" + url.removePrefix(TEXTURE_RELEASE_PREFIX)
        }
        return url
    }

    private fun catalogUrls(path: String): List<String> =
        if (isConfigured) listOf("$workerBaseUrl/v1/$path") else emptyList()
}
