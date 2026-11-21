package com.sbro.emucorer.core

import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

class StringsLocalizationTest {
    private val requiredKeyboardMouseKeys = listOf(
        "settings_keyboard_mouse_section",
        "settings_keyboard_input",
        "settings_keyboard_input_desc",
        "settings_keyboard_target_player",
        "settings_keyboard_mapping_title",
        "settings_keyboard_mapping_desc",
        "settings_mouse_input",
        "settings_mouse_input_desc",
        "settings_mouse_target_player",
        "settings_mouse_mapping_title",
        "settings_mouse_mapping_desc",
        "settings_keyboard_mapping_listening_title",
        "settings_keyboard_mapping_listening_desc",
        "settings_mouse_mapping_listening_title",
        "settings_mouse_mapping_listening_desc",
        "settings_keyboard_connected",
        "settings_mouse_connected",
        "settings_mouse_aim_mode",
        "settings_mouse_aim_off",
        "settings_mouse_aim_right_stick",
        "settings_mouse_aim_left_stick",
        "settings_mouse_aim_mode_desc",
        "settings_mouse_sensitivity",
        "settings_mouse_sensitivity_desc",
        "settings_mouse_vertical_sensitivity",
        "settings_mouse_smoothing",
        "settings_mouse_smoothing_desc",
        "settings_mouse_deadzone",
        "settings_mouse_wheel_pulse",
        "settings_mouse_invert_x",
        "settings_mouse_invert_x_desc",
        "settings_mouse_invert_y",
        "settings_mouse_invert_y_desc",
        "settings_mouse_capture",
        "settings_mouse_capture_desc",
        "settings_mouse_left_handed",
        "settings_mouse_left_handed_desc",
        "settings_mouse_wheel",
        "settings_mouse_wheel_off",
        "settings_mouse_wheel_dpad",
        "settings_mouse_wheel_triggers",
        "settings_mouse_wheel_left_stick",
        "settings_mouse_wheel_right_stick",
        "settings_mouse_wheel_shoulders",
        "settings_keyboard_mouse_mapping_reset_title",
        "settings_keyboard_mouse_mapping_reset_desc",
        "settings_keyboard_mouse_global_note",
        "settings_keyboard_mouse_game_desc",
        "settings_keyboard_mouse_use_global",
        "settings_keyboard_mouse_use_global_desc",
        "settings_keyboard_mouse_source_keyboard",
        "settings_keyboard_mouse_source_mouse",
        "settings_gamepad_mapping_use_global_desc",
        "emulation_shaders_tab",
        "emulation_shaders_title",
        "emulation_shaders_pack_title",
        "emulation_shaders_pack_hint",
        "emulation_shaders_pack_ready",
        "emulation_shaders_source_global",
        "emulation_shaders_source_game",
        "emulation_shaders_global_value",
        "emulation_shaders_help_scope",
        "emulation_shaders_picker_title",
        "emulation_shaders_search",
        "emulation_shaders_empty",
        "emulation_shaders_enable",
        "emulation_shaders_help_enable",
        "settings_game_menu_section_shaders"
    )

    private val requiredPs1UiKeys = listOf(
        "ps1ui_main_menu",
        "ps1ui_menu_games",
        "ps1ui_menu_memory_card",
        "ps1ui_menu_settings",
        "ps1ui_boot_subtitle",
        "ps1ui_hint_select",
        "ps1ui_hint_exit",
        "ps1ui_hint_back",
        "ps1ui_hint_tab",
        "ps1ui_hint_change",
        "ps1ui_btn_start",
        "ps1ui_btn_continue",
        "ps1ui_btn_options",
        "ps1ui_loading_games",
        "ps1ui_no_games",
        "ps1ui_disc",
        "ps1ui_discs",
        "ps1ui_quick_saves_lbl",
        "ps1ui_mc_tab_all",
        "ps1ui_mc_tab_game",
        "ps1ui_mc_tab_cards",
        "ps1ui_mc_slot1",
        "ps1ui_mc_slot2",
        "ps1ui_mc_empty_slot",
        "ps1ui_mc_available_cards",
        "ps1ui_mc_blocks_title",
        "ps1ui_saves_word",
        "ps1ui_mc_free_block",
        "ps1ui_loading_saves",
        "ps1ui_mc_no_saves",
        "ps1ui_mc_empty_block",
        "ps1ui_mc_load",
        "ps1ui_mc_delete",
        "ps1ui_mc_exit",
        "ps1ui_theme_classic",
        "ps1ui_theme_psone",
        "ps1ui_theme_slate",
        "ps1ui_aspect_auto",
        "ps1ui_aspect_16_9",
        "ps1ui_aspect_4_3",
        "ps1ui_orbs_normal",
        "ps1ui_orbs_calm",
        "ps1ui_orbs_static",
        "ps1ui_scan_subtle",
        "ps1ui_scan_classic",
        "ps1ui_boot_full",
        "ps1ui_boot_fast",
        "ps1ui_on",
        "ps1ui_off",
        "ps1ui_btn_cross",
        "ps1ui_btn_circle",
        "ps1ui_time_24h",
        "ps1ui_time_12h",
        "ps1ui_open",
        "ps1ui_view",
        "ps1ui_reset",
        "ps1ui_set_aspect",
        "ps1ui_set_theme",
        "ps1ui_set_orbs",
        "ps1ui_set_scanlines",
        "ps1ui_set_boot_anim",
        "ps1ui_set_key_tone",
        "ps1ui_set_ambient",
        "ps1ui_set_confirm_btn",
        "ps1ui_set_touch",
        "ps1ui_set_touch_guide",
        "ps1ui_set_time_fmt",
        "ps1ui_set_date_fmt",
        "ps1ui_set_keep_screen",
        "ps1ui_set_sysinfo",
        "ps1ui_set_restore",
        "ps1ui_launching_save",
        "ps1ui_launching_disc",
        "ps1ui_exit_title",
        "ps1ui_exit_msg",
        "ps1ui_del_title",
        "ps1ui_del_msg",
        "ps1ui_restore_title",
        "ps1ui_restore_msg",
        "ps1ui_yes",
        "ps1ui_no",
        "ps1ui_opt_start",
        "ps1ui_opt_continue",
        "ps1ui_opt_saves",
        "ps1ui_opt_info",
        "ps1ui_opt_settings",
        "ps1ui_select_disc",
        "ps1ui_info_title",
        "ps1ui_info_serial",
        "ps1ui_info_size",
        "ps1ui_info_discs",
        "ps1ui_close",
        "ps1ui_sys_app",
        "ps1ui_sys_device",
        "ps1ui_sys_bios",
        "ps1ui_sys_storage",
        "ps1ui_touch_guide_title",
        "ps1ui_touch_tip_1",
        "ps1ui_touch_tip_2",
        "ps1ui_touch_tip_3",
        "ps1ui_touch_tip_4",
        "ps1ui_got_it",
        "ps1ui_slot_auto",
        "ps1ui_slot_fmt",
        "ps1ui_card_formatted",
        "ps1ui_bios_ready",
        "ps1ui_bios_missing"
    )

    @Test
    fun `keyboard and mouse strings exist in every locale`() {
        val resDir = resolveResDir()
        val localeDirs = resDir
            .listFiles { file -> file.isDirectory && file.name.startsWith("values") }
            .orEmpty()
            .filter { dir -> File(dir, "strings.xml").isFile }
        assertTrue("Expected at least 18 locale string files", localeDirs.size >= 18)
        localeDirs.forEach { dir ->
            val content = File(dir, "strings.xml").readText()
            requiredKeyboardMouseKeys.forEach { key ->
                assertTrue("Missing $key in ${dir.name}/strings.xml", content.contains("name=\"$key\""))
            }
            requiredPs1UiKeys.forEach { key ->
                assertTrue("Missing $key in ${dir.name}/strings.xml", content.contains("name=\"$key\""))
            }
        }
    }

    @Test
    fun `shader format placeholders are preserved in every locale`() {
        val resDir = resolveResDir()
        val localeDirs = resDir
            .listFiles { file -> file.isDirectory && file.name.startsWith("values") }
            .orEmpty()
            .filter { dir -> File(dir, "strings.xml").isFile }
        val expectedPlaceholders = mapOf(
            "emulation_shaders_pack_ready" to "%1\$d",
            "emulation_shaders_global_value" to "%1\$s",
            "ps1ui_slot_fmt" to "%1\$d"
        )
        assertTrue("Expected at least 18 locale string files", localeDirs.size >= 18)
        localeDirs.forEach { dir ->
            val content = File(dir, "strings.xml").readText()
            expectedPlaceholders.forEach { (key, placeholder) ->
                val value = Regex(
                    "<string name=\"$key\">(.*?)</string>",
                    setOf(RegexOption.DOT_MATCHES_ALL)
                ).find(content)?.groupValues?.getOrNull(1)
                assertTrue("Missing $key in ${dir.name}/strings.xml", value != null)
                assertTrue(
                    "$key in ${dir.name}/strings.xml must keep the $placeholder placeholder",
                    value.orEmpty().contains(placeholder)
                )
            }
        }
    }

    private fun resolveResDir(): File {
        val candidates = listOf(
            File("src/main/res"),
            File("app/src/main/res"),
            File(System.getProperty("user.dir").orEmpty(), "src/main/res"),
            File(System.getProperty("user.dir").orEmpty(), "app/src/main/res")
        )
        return candidates.firstOrNull { it.isDirectory }
            ?: error("Could not locate the Android res directory")
    }
}
