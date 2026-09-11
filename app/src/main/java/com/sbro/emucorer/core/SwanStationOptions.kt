package com.sbro.emucorer.core

import android.content.Context
import android.content.SharedPreferences
import androidx.annotation.StringRes
import com.sbro.emucorer.R

/** Settings tab an option belongs to. Mirrors the app's settings tabs. */
enum class SsCategory {
    GRAPHICS,
    EMULATION,
    AUDIO,
    CONTROLS,
    DISPLAY,
}

data class SsChoice(@StringRes val labelRes: Int, val value: String)

data class SsOption(
    val key: String,
    @StringRes val labelRes: Int,
    @StringRes val descriptionRes: Int,
    val category: SsCategory,
    val choices: List<SsChoice>,
    val default: String? = null,
) {
    val defaultValue: String get() = default ?: choices.first().value
}

/**
 * Curated SwanStation core options exposed through the app settings. Labels and
 * help text come from localized string resources. Values are pushed to the
 * libretro core before every launch and persisted in a dedicated
 * SharedPreferences file so the settings UI stays synchronous.
 */
object SwanStationOptions {
    private const val PREFS_NAME = "swanstation_core_options"
    private var prefs: SharedPreferences? = null
    private val cache = HashMap<String, String>()

    private fun enabledDisabled() = listOf(
        SsChoice(R.string.ss_choice_enabled, "true"),
        SsChoice(R.string.ss_choice_disabled, "false"),
    )

    val all: List<SsOption> by lazy {
        listOf(
            // ------------------------------------------------------------ Graphics
            SsOption(
                "swanstation_GPU_TextureFilter", R.string.ss_opt_texture_filter,
                R.string.ss_opt_texture_filter_desc, SsCategory.GRAPHICS,
                listOf(
                    SsChoice(R.string.ss_choice_nearest, "Nearest"),
                    SsChoice(R.string.ss_choice_bilinear, "Bilinear"),
                    SsChoice(R.string.ss_choice_bilinear_binalpha, "BilinearBinAlpha"),
                    SsChoice(R.string.ss_choice_jinc2, "JINC2"),
                    SsChoice(R.string.ss_choice_jinc2_binalpha, "JINC2BinAlpha"),
                    SsChoice(R.string.ss_choice_xbr, "xBR"),
                    SsChoice(R.string.ss_choice_xbr_binalpha, "xBRBinAlpha"),
                ),
                default = "Nearest",
            ),
            SsOption(
                "swanstation_GPU_ScaledDithering", R.string.ss_opt_scaled_dithering,
                R.string.ss_opt_scaled_dithering_desc, SsCategory.GRAPHICS, enabledDisabled(),
            ),
            SsOption(
                "swanstation_GPU_TrueColor", R.string.ss_opt_true_color,
                R.string.ss_opt_true_color_desc, SsCategory.GRAPHICS, enabledDisabled(),
            ),
            SsOption(
                "swanstation_GPU_DisableInterlacing", R.string.ss_opt_disable_interlacing,
                R.string.ss_opt_disable_interlacing_desc, SsCategory.GRAPHICS, enabledDisabled(),
            ),
            SsOption(
                "swanstation_GPU_ForceNTSCTimings", R.string.ss_opt_force_ntsc,
                R.string.ss_opt_force_ntsc_desc, SsCategory.GRAPHICS, enabledDisabled(),
            ),
            SsOption(
                "swanstation_GPU_UseSoftwareRendererForReadbacks", R.string.ss_opt_sw_readbacks,
                R.string.ss_opt_sw_readbacks_desc, SsCategory.GRAPHICS, enabledDisabled(),
            ),

            // ------------------------------------------------------------- Display
            SsOption(
                "swanstation_Display_AspectRatio", R.string.ss_opt_aspect_ratio,
                R.string.ss_opt_aspect_ratio_desc, SsCategory.DISPLAY,
                listOf(
                    SsChoice(R.string.ss_choice_aspect_auto, "Auto"),
                    SsChoice(R.string.ss_choice_aspect_4_3, "4:3"),
                    SsChoice(R.string.ss_choice_aspect_16_9, "16:9"),
                    SsChoice(R.string.ss_choice_aspect_10_7, "10:7"),
                    SsChoice(R.string.ss_choice_aspect_custom, "Custom"),
                ),
                default = "Auto",
            ),
            SsOption(
                "swanstation_Display_CropMode", R.string.ss_opt_crop_mode,
                R.string.ss_opt_crop_mode_desc, SsCategory.DISPLAY,
                listOf(
                    SsChoice(R.string.ss_choice_crop_overscan, "Overscan"),
                    SsChoice(R.string.ss_choice_crop_borders, "Borders"),
                    SsChoice(R.string.ss_choice_crop_all, "All"),
                ),
                default = "Overscan",
            ),
            SsOption(
                "swanstation_Display_ShowOSDMessages", R.string.ss_opt_osd_messages,
                R.string.ss_opt_osd_messages_desc, SsCategory.DISPLAY, enabledDisabled(),
            ),

            // ----------------------------------------------------------- Emulation
            SsOption(
                "swanstation_Console_Region", R.string.ss_opt_console_region,
                R.string.ss_opt_console_region_desc, SsCategory.EMULATION,
                listOf(
                    SsChoice(R.string.ss_choice_auto_detect, "Auto"),
                    SsChoice(R.string.ss_choice_region_ntsc_j, "NTSC-J"),
                    SsChoice(R.string.ss_choice_region_ntsc_u, "NTSC-U"),
                    SsChoice(R.string.ss_choice_region_pal, "PAL"),
                ),
                default = "Auto",
            ),
            SsOption(
                "swanstation_CPU_ExecutionMode", R.string.ss_opt_cpu_mode,
                R.string.ss_opt_cpu_mode_desc, SsCategory.EMULATION,
                listOf(
                    SsChoice(R.string.ss_choice_cpu_recompiler, "Recompiler"),
                    SsChoice(R.string.ss_choice_cpu_cached_interp, "CachedInterpreter"),
                    SsChoice(R.string.ss_choice_cpu_interp, "Interpreter"),
                ),
                default = "Recompiler",
            ),
            SsOption(
                "swanstation_CPU_FastmemMode", R.string.ss_opt_fastmem,
                R.string.ss_opt_fastmem_desc, SsCategory.EMULATION,
                listOf(
                    SsChoice(R.string.ss_choice_disabled, "Disabled"),
                    SsChoice(R.string.ss_choice_enabled, "Enabled"),
                    SsChoice(R.string.ss_choice_fastmem_mmap, "MMap"),
                ),
                default = "MMap",
            ),
            SsOption(
                "swanstation_CPU_RecompilerICache", R.string.ss_opt_icache,
                R.string.ss_opt_icache_desc, SsCategory.EMULATION, enabledDisabled(),
            ),
            SsOption(
                "swanstation_CDROM_ReadThread", R.string.ss_opt_cdrom_read_thread,
                R.string.ss_opt_cdrom_read_thread_desc, SsCategory.EMULATION, enabledDisabled(),
            ),
            SsOption(
                "swanstation_CDROM_RegionCheck", R.string.ss_opt_cdrom_region_check,
                R.string.ss_opt_cdrom_region_check_desc, SsCategory.EMULATION, enabledDisabled(),
            ),
            SsOption(
                "swanstation_CDROM_ReadaheadSectors", R.string.ss_opt_cdrom_readahead,
                R.string.ss_opt_cdrom_readahead_desc, SsCategory.EMULATION,
                listOf(
                    SsChoice(R.string.ss_choice_off, "0"),
                    SsChoice(R.string.ss_choice_value_8, "8"),
                    SsChoice(R.string.ss_choice_value_16, "16"),
                    SsChoice(R.string.ss_choice_value_32, "32"),
                ),
                default = "8",
            ),

            // --------------------------------------------------------------- Audio
            SsOption(
                "swanstation_Audio_FastHook", R.string.ss_opt_audio_fast_hook,
                R.string.ss_opt_audio_fast_hook_desc, SsCategory.AUDIO, enabledDisabled(),
            ),

            // ------------------------------------------------------------ Controls
            SsOption(
                "swanstation_Controller_EnableRumble", R.string.ss_opt_rumble,
                R.string.ss_opt_rumble_desc, SsCategory.CONTROLS, enabledDisabled(),
            ),
            SsOption(
                "swanstation_Controller_AnalogCombo", R.string.ss_opt_analog_combo,
                R.string.ss_opt_analog_combo_desc, SsCategory.CONTROLS,
                listOf(
                    SsChoice(R.string.ss_choice_disabled, "0"),
                    SsChoice(R.string.ss_choice_analog_combo_l1r1l3r3, "1"),
                    SsChoice(R.string.ss_choice_analog_combo_full, "2"),
                ),
                default = "0",
            ),
            SsOption(
                "swanstation_Controller1_ForceAnalog", R.string.ss_opt_force_analog,
                R.string.ss_opt_force_analog_desc, SsCategory.CONTROLS, enabledDisabled(),
            ),
        )
    }

    fun forCategory(category: SsCategory): List<SsOption> = all.filter { it.category == category }

    fun value(key: String): String? = cache[key]

    /** All persisted core option overrides (curated + full catalogue). */
    fun persistedEntries(): Map<String, String> = HashMap(cache)

    fun set(key: String, value: String) {
        cache[key] = value
        prefs?.edit()?.putString(key, value)?.apply()
    }

    fun effectiveValue(option: SsOption): String = cache[option.key] ?: option.defaultValue

    fun initialize(context: Context) {
        prefs = context.applicationContext.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        cache.clear()
        prefs?.all?.forEach { (key, value) -> (value as? String)?.let { cache[key] = it } }
    }
}
