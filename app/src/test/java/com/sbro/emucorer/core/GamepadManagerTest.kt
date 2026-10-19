package com.sbro.emucorer.core

import android.view.KeyEvent
import com.sbro.emucorer.data.GamepadBindingRules
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class GamepadManagerTest {
    @Test
    fun explicitAssignmentMovesSelectedControllerToPlayerOne() {
        val assignments = GamepadManager.assignConnectedGamepadSlots(
            previousAssignments = emptyMap(),
            connectedDeviceIds = listOf(41, 72),
            singleGamepadReplacesTouch = true,
            deviceKeysByDeviceId = mapOf(41 to "xbox", 72 to "mangmi"),
            padDeviceKeys = mapOf(0 to "mangmi")
        )

        assertEquals(linkedMapOf(72 to 0, 41 to 1), assignments)
    }

    @Test
    fun explicitAssignmentOverridesExternalControllerPriority() {
        val assignments = GamepadManager.assignConnectedGamepadSlots(
            previousAssignments = emptyMap(),
            connectedDeviceIds = listOf(41, 72),
            singleGamepadReplacesTouch = true,
            preferExternalGamepadAsPlayerOne = true,
            externalDeviceIds = setOf(72),
            deviceKeysByDeviceId = mapOf(41 to "built-in", 72 to "external"),
            padDeviceKeys = mapOf(0 to "built-in")
        )

        assertEquals(linkedMapOf(41 to 0, 72 to 1), assignments)
    }

    @Test
    fun explicitPlayerTwoAssignmentKeepsOtherControllerOnPlayerOne() {
        val assignments = GamepadManager.assignConnectedGamepadSlots(
            previousAssignments = emptyMap(),
            connectedDeviceIds = listOf(41, 72),
            singleGamepadReplacesTouch = true,
            deviceKeysByDeviceId = mapOf(41 to "a", 72 to "b"),
            padDeviceKeys = mapOf(1 to "a")
        )

        assertEquals(linkedMapOf(72 to 0, 41 to 1), assignments)
    }

    @Test
    fun ignoredControllerIsExcludedFromSlots() {
        val assignments = GamepadManager.assignConnectedGamepadSlots(
            previousAssignments = emptyMap(),
            connectedDeviceIds = listOf(41, 72),
            singleGamepadReplacesTouch = true,
            deviceKeysByDeviceId = mapOf(41 to "xbox", 72 to "mangmi"),
            ignoredDeviceKeys = setOf("xbox")
        )

        assertEquals(linkedMapOf(72 to 0), assignments)
    }

    @Test
    fun explicitAssignmentOfDisconnectedControllerFallsBackToAutomatic() {
        val assignments = GamepadManager.assignConnectedGamepadSlots(
            previousAssignments = emptyMap(),
            connectedDeviceIds = listOf(41, 72),
            singleGamepadReplacesTouch = true,
            deviceKeysByDeviceId = mapOf(41 to "a", 72 to "b"),
            padDeviceKeys = mapOf(0 to "disconnected")
        )

        assertEquals(linkedMapOf(41 to 0, 72 to 1), assignments)
    }

    @Test
    fun ignoredExplicitControllerFallsBackToRemainingDevice() {
        val assignments = GamepadManager.assignConnectedGamepadSlots(
            previousAssignments = emptyMap(),
            connectedDeviceIds = listOf(41, 72),
            singleGamepadReplacesTouch = true,
            deviceKeysByDeviceId = mapOf(41 to "a", 72 to "b"),
            padDeviceKeys = mapOf(0 to "a"),
            ignoredDeviceKeys = setOf("a")
        )

        assertEquals(linkedMapOf(72 to 0), assignments)
    }

    @Test
    fun explicitPlayerOneAssignmentWinsInTouchPlusGamepadMode() {
        val assignments = GamepadManager.assignConnectedGamepadSlots(
            previousAssignments = emptyMap(),
            connectedDeviceIds = listOf(41),
            singleGamepadReplacesTouch = false,
            deviceKeysByDeviceId = mapOf(41 to "xbox"),
            padDeviceKeys = mapOf(0 to "xbox")
        )

        assertEquals(linkedMapOf(41 to 0), assignments)
    }

    @Test
    fun stickDirectionActionsAreMappableToPhysicalButtons() {
        val actions = GamepadManager.mappableButtonActions().associateBy { it.id }

        assertEquals(110, actions.getValue("left_stick_up").padKey)
        assertEquals(112, actions.getValue("left_stick_down").padKey)
        assertEquals(113, actions.getValue("left_stick_left").padKey)
        assertEquals(111, actions.getValue("left_stick_right").padKey)
        assertEquals(120, actions.getValue("right_stick_up").padKey)
        assertEquals(122, actions.getValue("right_stick_down").padKey)
        assertEquals(123, actions.getValue("right_stick_left").padKey)
        assertEquals(121, actions.getValue("right_stick_right").padKey)

        val bindings = GamepadBindingRules.assign(
            emptyMap(),
            "left_stick_up",
            KeyEvent.KEYCODE_DPAD_UP
        )
        assertEquals(
            "left_stick_up",
            GamepadManager.resolveMappedActionIdForKeyCode(KeyEvent.KEYCODE_DPAD_UP, bindings)
        )
    }

    @Test
    fun bindingAStickDirectionReplacesTheButtonOnItsOldAction() {
        val bindings = GamepadBindingRules.assign(
            mapOf("square" to KeyEvent.KEYCODE_BUTTON_X),
            "right_stick_right",
            KeyEvent.KEYCODE_BUTTON_X
        )

        assertNull(bindings["square"])
        assertEquals(
            "right_stick_right",
            GamepadManager.resolveMappedActionIdForKeyCode(KeyEvent.KEYCODE_BUTTON_X, bindings)
        )
    }
}
