package com.sbro.emucorer.core

import android.content.Context
import android.content.SharedPreferences
import androidx.annotation.StringRes
import com.sbro.emucorer.R

/** Settings tab an option belongs to. Mirrors the app's settings tabs. */
enum class EcrCategory {
    GRAPHICS,
    EMULATION,
    AUDIO,
    CONTROLS,
    DISPLAY,
}

data class EcrChoice(@StringRes val labelRes: Int, val value: String)

data class EcrOption(
    val key: String,
    @StringRes val labelRes: Int,
    @StringRes val descriptionRes: Int,
    val category: EcrCategory,
    val choices: List<EcrChoice>,
    val default: String? = null,
) {
    val defaultValue: String get() = default ?: choices.first().value
}

/**
 * Curated ECR PlayStation 1 core options exposed through the app settings.
 * Labels and help text come from localized string resources. Values are pushed
 * to the `libecr` core before every launch and persisted in a dedicated
 * SharedPreferences file so the settings UI stays synchronous.
 */
object EcrOptions {
    internal const val PREFS_NAME = "ecr_core_options"
    internal const val LEGACY_PREFS_NAME = "swanstation_core_options"
    internal const val KEY_PREFIX = "ecr_"
    internal const val LEGACY_KEY_PREFIX = "swanstation_"
    private const val MIGRATION_MARKER_KEY = "__migrated_from_swanstation"
    private var prefs: SharedPreferences? = null
    private val cache = HashMap<String, String>()

    fun normalizeOptionKey(key: String): String =
        if (key.startsWith(LEGACY_KEY_PREFIX)) KEY_PREFIX + key.removePrefix(LEGACY_KEY_PREFIX) else key

    private fun enabledDisabled() = listOf(
        EcrChoice(R.string.ecr_choice_enabled, "true"),
        EcrChoice(R.string.ecr_choice_disabled, "false"),
    )

    val all: List<EcrOption> by lazy {
        listOf(
            // ------------------------------------------------------------ Graphics
            EcrOption(
                "ecr_GPU_TextureFilter", R.string.ecr_opt_texture_filter,
                R.string.ecr_opt_texture_filter_desc, EcrCategory.GRAPHICS,
                listOf(
                    EcrChoice(R.string.ecr_choice_nearest, "Nearest"),
                    EcrChoice(R.string.ecr_choice_bilinear, "Bilinear"),
                    EcrChoice(R.string.ecr_choice_bilinear_binalpha, "BilinearBinAlpha"),
                    EcrChoice(R.string.ecr_choice_jinc2, "JINC2"),
                    EcrChoice(R.string.ecr_choice_jinc2_binalpha, "JINC2BinAlpha"),
                    EcrChoice(R.string.ecr_choice_xbr, "xBR"),
                    EcrChoice(R.string.ecr_choice_xbr_binalpha, "xBRBinAlpha"),
                ),
                default = "Nearest",
            ),
            EcrOption(
                "ecr_GPU_ScaledDithering", R.string.ecr_opt_scaled_dithering,
                R.string.ecr_opt_scaled_dithering_desc, EcrCategory.GRAPHICS, enabledDisabled(),
            ),
            EcrOption(
                "ecr_GPU_TrueColor", R.string.ecr_opt_true_color,
                R.string.ecr_opt_true_color_desc, EcrCategory.GRAPHICS, enabledDisabled(),
            ),
            EcrOption(
                "ecr_GPU_DisableInterlacing", R.string.ecr_opt_disable_interlacing,
                R.string.ecr_opt_disable_interlacing_desc, EcrCategory.GRAPHICS, enabledDisabled(),
            ),
            EcrOption(
                "ecr_GPU_ForceNTSCTimings", R.string.ecr_opt_force_ntsc,
                R.string.ecr_opt_force_ntsc_desc, EcrCategory.GRAPHICS, enabledDisabled(),
            ),
            EcrOption(
                "ecr_GPU_UseSoftwareRendererForReadbacks", R.string.ecr_opt_sw_readbacks,
                R.string.ecr_opt_sw_readbacks_desc, EcrCategory.GRAPHICS, enabledDisabled(),
            ),

            // ------------------------------------------------------------- Display
            EcrOption(
                "ecr_Display_AspectRatio", R.string.ecr_opt_aspect_ratio,
                R.string.ecr_opt_aspect_ratio_desc, EcrCategory.DISPLAY,
                listOf(
                    EcrChoice(R.string.ecr_choice_aspect_auto, "Auto"),
                    EcrChoice(R.string.ecr_choice_aspect_4_3, "4:3"),
                    EcrChoice(R.string.ecr_choice_aspect_16_9, "16:9"),
                    EcrChoice(R.string.ecr_choice_aspect_10_7, "10:7"),
                    EcrChoice(R.string.ecr_choice_aspect_custom, "Custom"),
                ),
                default = "Auto",
            ),
            EcrOption(
                "ecr_Display_CropMode", R.string.ecr_opt_crop_mode,
                R.string.ecr_opt_crop_mode_desc, EcrCategory.DISPLAY,
                listOf(
                    EcrChoice(R.string.ecr_choice_crop_overscan, "Overscan"),
                    EcrChoice(R.string.ecr_choice_crop_borders, "Borders"),
                ),
                default = "Overscan",
            ),
            EcrOption(
                "ecr_Display_ShowOSDMessages", R.string.ecr_opt_osd_messages,
                R.string.ecr_opt_osd_messages_desc, EcrCategory.DISPLAY, enabledDisabled(),
            ),

            // ----------------------------------------------------------- Emulation
            EcrOption(
                "ecr_Console_Region", R.string.ecr_opt_console_region,
                R.string.ecr_opt_console_region_desc, EcrCategory.EMULATION,
                listOf(
                    EcrChoice(R.string.ecr_choice_auto_detect, "Auto"),
                    EcrChoice(R.string.ecr_choice_region_ntsc_j, "NTSC-J"),
                    EcrChoice(R.string.ecr_choice_region_ntsc_u, "NTSC-U"),
                    EcrChoice(R.string.ecr_choice_region_pal, "PAL"),
                ),
                default = "Auto",
            ),
            EcrOption(
                "ecr_CPU_ExecutionMode", R.string.ecr_opt_cpu_mode,
                R.string.ecr_opt_cpu_mode_desc, EcrCategory.EMULATION,
                listOf(
                    EcrChoice(R.string.ecr_choice_cpu_recompiler, "Recompiler"),
                    EcrChoice(R.string.ecr_choice_cpu_cached_interp, "CachedInterpreter"),
                    EcrChoice(R.string.ecr_choice_cpu_interp, "Interpreter"),
                ),
                default = "Recompiler",
            ),
            EcrOption(
                "ecr_CPU_FastmemMode", R.string.ecr_opt_fastmem,
                R.string.ecr_opt_fastmem_desc, EcrCategory.EMULATION,
                listOf(
                    EcrChoice(R.string.ecr_choice_disabled, "Disabled"),
                    EcrChoice(R.string.ecr_choice_enabled, "Enabled"),
                    EcrChoice(R.string.ecr_choice_fastmem_mmap, "MMap"),
                ),
                default = "MMap",
            ),
            EcrOption(
                "ecr_CPU_RecompilerICache", R.string.ecr_opt_icache,
                R.string.ecr_opt_icache_desc, EcrCategory.EMULATION, enabledDisabled(),
            ),
            EcrOption(
                "ecr_CDROM_ReadThread", R.string.ecr_opt_cdrom_read_thread,
                R.string.ecr_opt_cdrom_read_thread_desc, EcrCategory.EMULATION, enabledDisabled(),
            ),
            EcrOption(
                "ecr_CDROM_RegionCheck", R.string.ecr_opt_cdrom_region_check,
                R.string.ecr_opt_cdrom_region_check_desc, EcrCategory.EMULATION, enabledDisabled(),
            ),
            EcrOption(
                "ecr_CDROM_ReadaheadSectors", R.string.ecr_opt_cdrom_readahead,
                R.string.ecr_opt_cdrom_readahead_desc, EcrCategory.EMULATION,
                listOf(
                    EcrChoice(R.string.ecr_choice_off, "0"),
                    EcrChoice(R.string.ecr_choice_value_8, "8"),
                    EcrChoice(R.string.ecr_choice_value_16, "16"),
                    EcrChoice(R.string.ecr_choice_value_32, "32"),
                ),
                default = "8",
            ),

            // --------------------------------------------------------------- Audio
            EcrOption(
                "ecr_Audio_FastHook", R.string.ecr_opt_audio_fast_hook,
                R.string.ecr_opt_audio_fast_hook_desc, EcrCategory.AUDIO, enabledDisabled(),
            ),

            // ------------------------------------------------------------ Controls
            EcrOption(
                "ecr_Controller_EnableRumble", R.string.ecr_opt_rumble,
                R.string.ecr_opt_rumble_desc, EcrCategory.CONTROLS, enabledDisabled(),
            ),
            EcrOption(
                "ecr_Controller_AnalogCombo", R.string.ecr_opt_analog_combo,
                R.string.ecr_opt_analog_combo_desc, EcrCategory.CONTROLS,
                listOf(
                    EcrChoice(R.string.ecr_choice_disabled, "0"),
                    EcrChoice(R.string.ecr_choice_analog_combo_l1r1l3r3, "1"),
                    EcrChoice(R.string.ecr_choice_analog_combo_full, "2"),
                ),
                default = "0",
            ),
            EcrOption(
                "ecr_Controller1_ForceAnalog", R.string.ecr_opt_force_analog,
                R.string.ecr_opt_force_analog_desc, EcrCategory.CONTROLS, enabledDisabled(),
            ),
        )
    }

    fun forCategory(category: EcrCategory): List<EcrOption> = all.filter { it.category == category }

    fun value(key: String): String? = cache[normalizeOptionKey(key)]

    /** All persisted core option overrides (curated + full catalogue). */
    fun persistedEntries(): Map<String, String> = HashMap(cache)

    fun set(key: String, value: String) {
        val normalizedKey = normalizeOptionKey(key)
        cache[normalizedKey] = value
        prefs?.edit()?.putString(normalizedKey, value)?.apply()
    }

    /** Drops a persisted override so the option falls back to its default. */
    fun remove(key: String): Boolean {
        val normalizedKey = normalizeOptionKey(key)
        val removed = cache.remove(normalizedKey) != null
        val editor = prefs?.edit()
        var touched = false
        if (prefs?.contains(normalizedKey) == true) {
            editor?.remove(normalizedKey)
            touched = true
        }
        if (key != normalizedKey && prefs?.contains(key) == true) {
            editor?.remove(key)
            touched = true
        }
        if (touched) editor?.apply()
        return removed || touched
    }

    internal fun migrateEntriesMap(
        legacyEntries: Map<String, *>,
        existingTargetEntries: Map<String, *>
    ): Map<String, String> {
        val migrated = LinkedHashMap<String, String>()
        existingTargetEntries.forEach { (key, value) ->
            if (!key.startsWith("__") && value is String) {
                val normalized = normalizeOptionKey(key)
                // Prefer already-migrated ecr_* keys over legacy keys within target.
                if (normalized == key || !migrated.containsKey(normalized)) {
                    migrated[normalized] = value
                }
            }
        }
        legacyEntries.forEach { (key, value) ->
            if (!key.startsWith("__") && value is String) {
                val normalized = normalizeOptionKey(key)
                if (!migrated.containsKey(normalized)) {
                    migrated[normalized] = value
                }
            }
        }
        return migrated
    }

    fun initialize(context: Context) {
        val appContext = context.applicationContext
        val targetPrefs = appContext.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        prefs = targetPrefs
        if (!targetPrefs.getBoolean(MIGRATION_MARKER_KEY, false)) {
            val legacyPrefs = appContext.getSharedPreferences(LEGACY_PREFS_NAME, Context.MODE_PRIVATE)
            val migrated = migrateEntriesMap(legacyPrefs.all, targetPrefs.all)
            val editor = targetPrefs.edit()
            targetPrefs.all.keys.filter { it.startsWith(LEGACY_KEY_PREFIX) }.forEach { editor.remove(it) }
            migrated.forEach { (key, value) -> editor.putString(key, value) }
            editor.putBoolean(MIGRATION_MARKER_KEY, true)
            editor.commit()
        }
        cache.clear()
        targetPrefs.all.forEach { (key, value) ->
            if (!key.startsWith("__") && value is String) {
                cache[normalizeOptionKey(key)] = value
            }
        }
    }
}
