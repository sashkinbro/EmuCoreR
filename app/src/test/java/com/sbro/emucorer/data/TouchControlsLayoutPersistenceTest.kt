package com.sbro.emucorer.data

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Test

class TouchControlsLayoutPersistenceTest {
    private val layout = TouchControlsLayoutProfile(
        stickScale = 130,
        controlLayouts = AppPreferences.defaultOverlayControlLayouts(130)
    )

    @Test
    fun `toggle swaps the left stick with its dedicated d-pad`() {
        val dpadToggled = layout.toggleStick(AppPreferences.STICK_TOGGLE_LEFT)

        assertFalse(dpadToggled.controlLayouts.getValue("left_stick").visible)
        assertTrue(dpadToggled.controlLayouts.getValue("dpad_toggle").visible)

        val stickRestored = dpadToggled.toggleStick(AppPreferences.STICK_TOGGLE_LEFT)

        assertTrue(stickRestored.controlLayouts.getValue("left_stick").visible)
        assertFalse(stickRestored.controlLayouts.getValue("dpad_toggle").visible)
    }

    @Test
    fun `toggle never touches the main d-pad or the editor extra d-pad`() {
        val result = layout.toggleStick(AppPreferences.STICK_TOGGLE_LEFT)

        listOf("dpad_up", "dpad_down", "dpad_left", "dpad_right").forEach { id ->
            assertEquals(
                layout.controlLayouts.getValue(id).visible,
                result.controlLayouts.getValue(id).visible
            )
        }
        assertEquals(
            layout.controlLayouts.getValue("dpad_cluster").visible,
            result.controlLayouts.getValue("dpad_cluster").visible
        )
    }

    @Test
    fun `toggle target can be normalized to either stick`() {
        val stickShown = layout.toggleStick(AppPreferences.STICK_TOGGLE_RIGHT)

        assertTrue(stickShown.controlLayouts.getValue("right_stick").visible)
        assertFalse(stickShown.controlLayouts.getValue("dpad_toggle").visible)
        assertTrue(stickShown.controlLayouts.getValue("left_stick").visible)

        val dpadShown = stickShown.toggleStick(AppPreferences.STICK_TOGGLE_RIGHT)

        assertFalse(dpadShown.controlLayouts.getValue("right_stick").visible)
        assertTrue(dpadShown.controlLayouts.getValue("dpad_toggle").visible)
        assertTrue(dpadShown.controlLayouts.getValue("left_stick").visible)
    }

    @Test
    fun `new override creates a layout-only per-game profile`() {
        val result = null.withTouchControlsLayout(
            gameKey = "game.iso",
            gameTitle = "Game",
            gameSerial = "SLUS-12345",
            layout = layout
        )

        assertEquals(setOf(PER_GAME_TOUCH_CONTROLS_LAYOUT_KEY), result.providedKeys)
        assertSame(layout, result.touchControlsLayout)
        assertEquals("SLUS-12345", result.gameSerial)
    }

    @Test
    fun `layout is added without losing selective profile keys`() {
        val existing = PerGameSettings(
            gameKey = "game.iso",
            gameTitle = "Old title",
            renderer = 14,
            providedKeys = setOf("renderer")
        )

        val result = existing.withTouchControlsLayout(
            gameKey = existing.gameKey,
            gameTitle = "New title",
            gameSerial = null,
            layout = layout
        )

        assertEquals(setOf("renderer", PER_GAME_TOUCH_CONTROLS_LAYOUT_KEY), result.providedKeys)
        assertEquals(14, result.renderer)
        assertEquals("New title", result.gameTitle)
        assertSame(layout, result.touchControlsLayout)
    }

    @Test
    fun `layout keeps full profile semantics and existing serial`() {
        val existing = PerGameSettings(
            gameKey = "game.iso",
            gameTitle = "Game",
            gameSerial = "SLES-00001",
            providedKeys = null
        )

        val result = existing.withTouchControlsLayout(
            gameKey = existing.gameKey,
            gameTitle = existing.gameTitle,
            gameSerial = null,
            layout = layout
        )

        assertNull(result.providedKeys)
        assertEquals("SLES-00001", result.gameSerial)
        assertSame(layout, result.touchControlsLayout)
    }

    @Test
    fun `custom controls override creates a custom-only profile`() {
        val library = CustomTouchControlLibrary(
            controls = listOf(
                CustomTouchControl(
                    id = "combo",
                    name = "Combo",
                    actionId = "cross",
                    secondaryActionId = "l1"
                )
            )
        )

        val result = null.withCustomTouchControls(
            gameKey = "game.iso",
            gameTitle = "Game",
            gameSerial = "SLUS-12345",
            library = library
        )

        assertEquals(setOf(PER_GAME_CUSTOM_TOUCH_CONTROLS_KEY), result.providedKeys)
        assertEquals("combo", result.customTouchControls?.controls?.single()?.id)
        assertEquals("l1", result.customTouchControls?.controls?.single()?.secondaryActionId)
        assertNull(result.touchControlsLayout)
    }

    @Test
    fun `custom controls are added without losing layout keys`() {
        val existing = PerGameSettings(
            gameKey = "game.iso",
            gameTitle = "Game",
            touchControlsLayout = layout,
            providedKeys = setOf("renderer", PER_GAME_TOUCH_CONTROLS_LAYOUT_KEY)
        )

        val result = existing.withCustomTouchControls(
            gameKey = existing.gameKey,
            gameTitle = existing.gameTitle,
            gameSerial = null,
            library = CustomTouchControlLibrary(
                controls = listOf(CustomTouchControl(id = "extra", name = "Extra"))
            )
        )

        assertEquals(
            setOf("renderer", PER_GAME_TOUCH_CONTROLS_LAYOUT_KEY, PER_GAME_CUSTOM_TOUCH_CONTROLS_KEY),
            result.providedKeys
        )
        assertSame(layout, result.touchControlsLayout)
        assertEquals("extra", result.customTouchControls?.controls?.single()?.id)
    }

    @Test
    fun `reset deletes a layout-only profile`() {
        val existing = PerGameSettings(
            gameKey = "game.iso",
            gameTitle = "Game",
            touchControlsLayout = layout,
            providedKeys = setOf(PER_GAME_TOUCH_CONTROLS_LAYOUT_KEY)
        )

        assertNull(existing.withoutTouchControlsLayout())
    }

    @Test
    fun `reset clears both layout and custom control overrides`() {
        val existing = PerGameSettings(
            gameKey = "game.iso",
            gameTitle = "Game",
            touchControlsLayout = layout,
            customTouchControls = CustomTouchControlLibrary(
                controls = listOf(CustomTouchControl(id = "extra", name = "Extra"))
            ),
            providedKeys = setOf(
                PER_GAME_TOUCH_CONTROLS_LAYOUT_KEY,
                PER_GAME_CUSTOM_TOUCH_CONTROLS_KEY
            )
        )

        assertNull(existing.withoutTouchControlsLayout())
    }

    @Test
    fun `reset keeps unrelated keys but drops custom controls`() {
        val existing = PerGameSettings(
            gameKey = "game.iso",
            gameTitle = "Game",
            renderer = 14,
            touchControlsLayout = layout,
            customTouchControls = CustomTouchControlLibrary(
                controls = listOf(CustomTouchControl(id = "extra", name = "Extra"))
            ),
            providedKeys = setOf(
                "renderer",
                PER_GAME_TOUCH_CONTROLS_LAYOUT_KEY,
                PER_GAME_CUSTOM_TOUCH_CONTROLS_KEY
            )
        )

        val result = requireNotNull(existing.withoutTouchControlsLayout())

        assertEquals(setOf("renderer"), result.providedKeys)
        assertNull(result.touchControlsLayout)
        assertNull(result.customTouchControls)
    }

    @Test
    fun `reset preserves unrelated selective overrides`() {
        val existing = PerGameSettings(
            gameKey = "game.iso",
            gameTitle = "Game",
            renderer = 14,
            touchControlsLayout = layout,
            providedKeys = setOf("renderer", PER_GAME_TOUCH_CONTROLS_LAYOUT_KEY)
        )

        val result = requireNotNull(existing.withoutTouchControlsLayout())

        assertEquals(setOf("renderer"), result.providedKeys)
        assertEquals(14, result.renderer)
        assertNull(result.touchControlsLayout)
    }

    @Test
    fun `reset preserves a full game profile`() {
        val existing = PerGameSettings(
            gameKey = "game.iso",
            gameTitle = "Game",
            touchControlsLayout = layout,
            providedKeys = null
        )

        val result = requireNotNull(existing.withoutTouchControlsLayout())

        assertNull(result.providedKeys)
        assertNull(result.touchControlsLayout)
    }

    @Test
    fun `global snapshot converts without losing layout values`() {
        val snapshot = OverlayLayoutSnapshot(
            dpadOffset = 0.1f to 0.2f,
            stickScale = 120,
            controlLayouts = AppPreferences.defaultOverlayControlLayouts(120)
        )

        val result = snapshot.toTouchControlsLayoutProfile()

        assertEquals(snapshot.dpadOffset, result.dpadOffset)
        assertEquals(120, result.stickScale)
        assertTrue(result.controlLayouts.isNotEmpty())
    }
}
