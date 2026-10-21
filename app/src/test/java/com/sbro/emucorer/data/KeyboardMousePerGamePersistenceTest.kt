package com.sbro.emucorer.data

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class KeyboardMousePerGamePersistenceTest {
    @Test
    fun keyboardMouseOverrideAddsAndRemovesItsSelectiveKey() {
        val profile = PerGameSettings(
            gameKey = "game.iso",
            gameTitle = "PS1 game",
            renderer = 14,
            providedKeys = setOf("renderer")
        )
        val settings = KeyboardMouseSettings(
            keyboardEnabled = true,
            mouseEnabled = true,
            keyboardBindingsByPad = mapOf(0 to mapOf("cross" to 39)),
            mouseBindingsByPad = mapOf(1 to mapOf("circle" to 1))
        )

        val added = profile.withKeyboardMouseSettings(settings)
        val cleared = added.withKeyboardMouseSettings(null)

        assertEquals(settings, added.keyboardMouse)
        assertEquals(setOf("renderer", "keyboardMouse"), added.providedKeys)
        assertNull(cleared.keyboardMouse)
        assertEquals(setOf("renderer"), cleared.providedKeys)
        assertEquals(14, cleared.renderer)
    }

    @Test
    fun oldFullProfilesKeepFullProfileSemanticsAfterKeyboardMouseChange() {
        val oldProfile = PerGameSettings(
            gameKey = "old.iso",
            gameTitle = "Old game",
            providedKeys = null
        )

        assertNull(oldProfile.withKeyboardMouseSettings(KeyboardMouseSettings(keyboardEnabled = true)).providedKeys)
        assertNull(oldProfile.withKeyboardMouseSettings(null).providedKeys)
    }

    @Test
    fun keyboardMouseSettingsSurviveThePerGameJsonRoundTrip() {
        val settings = KeyboardMouseSettings(
            keyboardEnabled = false,
            mouseEnabled = true,
            keyboardTargetPad = 1,
            mouseTargetPad = 1,
            keyboardBindingsByPad = mapOf(1 to mapOf("cross" to 62)),
            mouseBindingsByPad = mapOf(1 to mapOf("circle" to 2)),
            mouseAimMode = AppPreferences.MOUSE_AIM_RIGHT_STICK,
            mouseSensitivity = 150,
            mouseVerticalSensitivity = 80,
            mouseSmoothing = 25,
            mouseAimDeadzone = 5,
            mouseInvertX = true,
            mouseInvertY = true,
            mouseCaptureCursor = false,
            mouseLeftHanded = true,
            mouseWheelMode = AppPreferences.MOUSE_WHEEL_TRIGGERS,
            mouseWheelPulseMs = 120
        )
        val profile = PerGameSettings(
            gameKey = "game.iso",
            gameTitle = "PS1 game",
            keyboardMouse = settings
        )

        val parsed = profile.toJson().toPerGameSettings()

        assertEquals(settings, parsed.keyboardMouse)
    }

    @Test
    fun profilesWithoutKeyboardMouseDoNotWriteTheKey() {
        val profile = PerGameSettings(
            gameKey = "game.iso",
            gameTitle = "PS1 game"
        )

        assertEquals(false, profile.toJson().has("keyboardMouse"))
    }
}
