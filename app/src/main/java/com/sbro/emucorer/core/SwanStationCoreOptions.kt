// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: LicenseRef-EmuCoreR-Proprietary
package com.sbro.emucorer.core

import android.content.Context
import android.util.Log
import org.json.JSONObject

/**
 * Full catalogue of the SwanStation libretro core options, generated from the
 * core's own `libretro_core_options.h`. This is the single source of truth used
 * by the settings UI, the per-game manager and the in-game menu.
 */
object SwanStationCoreOptions {
    private const val TAG = "SwanStationCoreOptions"
    private const val ASSET_PATH = "catalog/swanstation_options.json"

    data class Choice(val value: String, val label: String)

    data class Option(
        val key: String,
        val label: String,
        val description: String,
        val category: String,
        val choices: List<Choice>,
        val defaultValue: String,
    ) {
        /** The option's short key without the `swanstation_` prefix. */
        val shortKey: String get() = key.removePrefix("swanstation_")
    }

    data class Category(val key: String, val label: String, val description: String)

    @Volatile
    private var categories: List<Category> = emptyList()

    @Volatile
    private var options: List<Option> = emptyList()

    @Volatile
    private var optionsByKey: Map<String, Option> = emptyMap()

    @Volatile
    private var loaded: Boolean = false

    fun load(context: Context) {
        if (loaded) return
        synchronized(this) {
            if (loaded) return
            val app = context.applicationContext
            val language = app.resources.configuration.locales[0].language.lowercase()
            val candidates = listOf(
                "catalog/swanstation_options_$language.json",
                "catalog/swanstation_options.json",
            )
            for (path in candidates) {
                val text = runCatching {
                    app.assets.open(path).bufferedReader().use { it.readText() }
                }.getOrNull() ?: continue
                runCatching { parse(text) }
                    .onFailure { Log.e(TAG, "Unable to parse $path", it) }
                    .onSuccess { loaded = true; return }
            }
            loaded = true
        }
    }

    private fun parse(json: String) {
        val root = JSONObject(json)
        val categoryArray = root.optJSONArray("categories")
        val parsedCategories = ArrayList<Category>()
        if (categoryArray != null) {
            for (i in 0 until categoryArray.length()) {
                val item = categoryArray.optJSONObject(i) ?: continue
                parsedCategories += Category(
                    key = item.optString("key"),
                    label = item.optString("label"),
                    description = item.optString("description"),
                )
            }
        }
        val optionArray = root.optJSONArray("options")
        val parsedOptions = ArrayList<Option>()
        if (optionArray != null) {
            for (i in 0 until optionArray.length()) {
                val item = optionArray.optJSONObject(i) ?: continue
                val choicesArray = item.optJSONArray("choices")
                val choices = ArrayList<Choice>()
                if (choicesArray != null) {
                    for (j in 0 until choicesArray.length()) {
                        val choice = choicesArray.optJSONObject(j) ?: continue
                        choices += Choice(
                            value = choice.optString("value"),
                            label = choice.optString("label").ifBlank { choice.optString("value") },
                        )
                    }
                }
                val key = item.optString("key")
                if (key.isBlank()) continue
                parsedOptions += Option(
                    key = key,
                    label = item.optString("label").ifBlank { key },
                    description = item.optString("description"),
                    category = item.optString("category"),
                    choices = choices,
                    defaultValue = item.optString("default")
                        .ifBlank { choices.firstOrNull()?.value.orEmpty() },
                )
            }
        }
        categories = parsedCategories
        options = parsedOptions
        optionsByKey = parsedOptions.associateBy { it.key }
    }

    fun isLoaded(): Boolean = loaded

    fun categories(): List<Category> = categories

    fun all(): List<Option> = options

    fun forCategory(categoryKey: String): List<Option> = options.filter { it.category == categoryKey }

    fun option(key: String): Option? = optionsByKey[key]

    fun categoryLabel(categoryKey: String): String =
        categories.firstOrNull { it.key == categoryKey }?.label ?: categoryKey

    /**
     * Keys that already have a dedicated control in the app UI (renderer,
     * resolution, aspect, crop, fast boot, PGXP, widescreen, rumble, texture
     * replacements, BIOS selection, memory cards). They must not be rendered
     * again by the generic option rows.
     */
    private val managedKeys = setOf(
        "swanstation_GPU_Renderer",
        "swanstation_GPU_ResolutionScale",
        "swanstation_Display_AspectRatio",
        "swanstation_Display_CropMode",
        "swanstation_BIOS_PatchFastBoot",
        "swanstation_BIOS_PathNTSCJ",
        "swanstation_BIOS_PathNTSCU",
        "swanstation_BIOS_PathPAL",
        "swanstation_GPU_PGXPEnable",
        "swanstation_GPU_WidescreenHack",
        "swanstation_Controller_EnableRumble",
        "swanstation_ControllerPorts_MultitapMode",
        "swanstation_TextureReplacements_EnableVRAMWriteReplacements",
        "swanstation_TextureReplacements_PreloadTextures",
        "swanstation_Main_ApplyGameSettings",
        "swanstation_MemoryCards_Card1Type",
        "swanstation_MemoryCards_Card2Type",
        "swanstation_MemoryCards_UsePlaylistTitle",
        "swanstation_GPU_ShaderPrecompile",
        "swanstation_CPU_RecompilerICache",
        "swanstation_CDROM_MuteCDAudio",
    )

    /** Graphics tab: visual enhancements and display geometry. */
    fun graphicsOptions(): List<Option> =
        (forCategory("enhancement") + forCategory("display")).filterNot { it.key in managedKeys }

    /** Emulation tab: console behaviour and advanced CPU/CD-ROM settings. */
    fun emulationOptions(): List<Option> =
        (forCategory("console") + forCategory("advanced")).filterNot { it.key in managedKeys }

    /** Controls tab: controller ports and memory cards. */
    fun controlsOptions(): List<Option> =
        forCategory("port").filterNot { it.key in managedKeys }

    fun resolutionScaleChoices(): List<Choice> =
        option("swanstation_GPU_ResolutionScale")?.choices.orEmpty()

    fun cropModeChoices(): List<Choice> =
        option("swanstation_Display_CropMode")?.choices.orEmpty()
}
