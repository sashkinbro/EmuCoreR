package com.sbro.emucorer.ui.emulation

import android.content.Intent
import android.view.KeyEvent
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.test.junit4.v2.createEmptyComposeRule
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.performTouchInput
import androidx.compose.ui.unit.dp
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.sbro.emucorer.R
import com.sbro.emucorer.RuntimeTestActivity
import com.sbro.emucorer.data.AppPreferences
import com.sbro.emucorer.data.CustomTouchControl
import com.sbro.emucorer.data.CustomTouchControlPressMode
import com.sbro.emucorer.data.TouchControlPressEffect
import com.sbro.emucorer.data.TouchControlVisualStyle
import org.junit.Assert.assertEquals
import org.junit.After
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class TouchButtonGroupInstrumentedTest {
    @get:Rule val compose = createEmptyComposeRule()
    private var activity: RuntimeTestActivity? = null

    @Before fun launchActivity() {
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        activity = instrumentation.startActivitySync(
            Intent(instrumentation.targetContext, RuntimeTestActivity::class.java)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        ) as RuntimeTestActivity
    }

    @After fun finishActivity() {
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        activity?.let { instrumentation.runOnMainSync { it.finish() } }
        instrumentation.waitForIdleSync()
    }

    private fun setContent(content: @Composable () -> Unit) {
        InstrumentationRegistry.getInstrumentation().runOnMainSync {
            requireNotNull(activity).setContent(content = content)
        }
    }

    private val pressed = linkedSetOf<String>()
    private val transitions = mutableListOf<Pair<String, Boolean>>()
    private val visible = mutableStateOf(true)
    private val pressedKeys = mutableSetOf<Int>()
    private val runtimePrimaryAction = mutableStateOf("square")

    private fun showRuntimeComboButtons(toggleCombo: Boolean = false) {
        setContent {
            if (visible.value) {
                OnScreenControls(
                    modifier = Modifier.size(180.dp, 120.dp).testTag("buttons"),
                    scaleFactor = 1f,
                    touchscreenRightStick = false,
                    dpadOffset = 0f to 0f,
                    lstickOffset = 0f to 0f,
                    rstickOffset = 0f to 0f,
                    actionOffset = 0f to 0f,
                    lbtnOffset = 0f to 0f,
                    rbtnOffset = 0f to 0f,
                    centerOffset = 0f to 0f,
                    controlLayouts = AppPreferences.defaultOverlayControlLayouts()
                        .mapValues { (_, layout) -> layout.copy(visible = false) },
                    customControls = listOf(
                        CustomTouchControl(id = "square", actionId = "square",
                            positionX = 0.2f, positionY = 0.5f, widthDp = 80, heightDp = 80),
                        CustomTouchControl(id = "combo", actionId = runtimePrimaryAction.value, secondaryActionId = "triangle",
                            pressMode = if (toggleCombo) CustomTouchControlPressMode.TOGGLE else CustomTouchControlPressMode.HOLD,
                            positionX = 0.6f, positionY = 0.5f, widthDp = 80, heightDp = 80)
                    ),
                    racingMode = false,
                    onToggleLeftInputMode = {},
                    onPadInput = { key, _, down ->
                        if (down) pressedKeys.add(key) else pressedKeys.remove(key)
                    },
                    respectSystemInsets = false
                )
            }
        }
    }

    private fun assertKeys(vararg keys: Int) {
        compose.runOnIdle { assertEquals(keys.toSet(), pressedKeys.toSet()) }
    }

    private fun showButtons(toggle: Boolean = false, fourButtons: Boolean = false) {
        setContent {
            if (visible.value) {
                val ids = if (fourButtons) listOf("square", "triangle", "circle", "cross")
                    else listOf("square", "triangle")
                TouchButtonGroup(
                    specs = ids.mapIndexed { index, id ->
                        TouchButtonSpec(
                            id = id,
                            drawableRes = R.drawable.ic_controller_square_button,
                            width = 80.dp,
                            height = 80.dp,
                            x = if (index == 0 || fourButtons) 20.dp else 60.dp,
                            y = 20.dp,
                            shape = CircleShape,
                            tapToHold = toggle,
                            onPressChange = { down ->
                                transitions += id to down
                                if (down) pressed.add(id) else pressed.remove(id)
                            }
                        )
                    },
                    inputWidth = 180.dp,
                    inputHeight = 120.dp,
                    touchscreenRightStick = false,
                    touchscreenRightStickSensitivity = 1f,
                    invertRightStick = false,
                    invertRightStickHorizontal = false,
                    onPadInput = { _, _, _ -> },
                    visualStyle = TouchControlVisualStyle.CLASSIC,
                    pressEffect = TouchControlPressEffect.GROW,
                    onTouchHaptic = {},
                    modifier = Modifier.testTag("buttons")
                )
            }
        }
    }

    // Coordinates are relative to the 180 x 120 dp input group, independent of density.
    private fun point(x: Float, y: Float) = Offset(
        compose.onNodeWithTag("buttons").fetchSemanticsNode().size.width * x / 180f,
        compose.onNodeWithTag("buttons").fetchSemanticsNode().size.height * y / 120f
    )

    private fun assertPressed(vararg ids: String) {
        compose.runOnIdle { assertEquals(ids.toSet(), pressed.toSet()) }
    }

    @Test fun overlapPressesBothButtonsAndReleasesBothOnUp() {
        showButtons()
        val overlap = point(80f, 60f)
        compose.onNodeWithTag("buttons").performTouchInput { down(overlap) }
        assertPressed("square", "triangle")
        compose.onNodeWithTag("buttons").performTouchInput { up() }
        assertPressed()
    }

    @Test fun slidingIntoAndOutOfOverlapKeepsCommonButtonHeld() {
        showButtons()
        val square = point(40f, 60f)
        val overlap = point(80f, 60f)
        val triangle = point(120f, 60f)
        val outside = point(160f, 60f)
        compose.onNodeWithTag("buttons").performTouchInput { down(square) }
        assertPressed("square")
        compose.onNodeWithTag("buttons").performTouchInput { moveTo(overlap) }
        assertPressed("square", "triangle")
        compose.onNodeWithTag("buttons").performTouchInput { moveTo(triangle) }
        assertPressed("triangle")
        compose.onNodeWithTag("buttons").performTouchInput { moveTo(outside) }
        assertPressed()
        compose.onNodeWithTag("buttons").performTouchInput { up() }
        compose.runOnIdle {
            assertEquals(listOf("square" to true, "triangle" to true,
                "square" to false, "triangle" to false), transitions)
        }
    }

    @Test fun releasingOneFingerDoesNotReleaseButtonsHeldByAnother() {
        showButtons()
        val overlap = point(80f, 60f)
        val square = point(40f, 60f)
        compose.onNodeWithTag("buttons").performTouchInput {
            down(0, overlap)
            down(1, square)
        }
        assertPressed("square", "triangle")
        compose.onNodeWithTag("buttons").performTouchInput { up(0) }
        assertPressed("square")
        compose.onNodeWithTag("buttons").performTouchInput { up(1) }
        assertPressed()
        compose.runOnIdle {
            assertEquals(1, transitions.count { it == "square" to true })
            assertEquals(1, transitions.count { it == "square" to false })
        }
    }

    @Test fun fullyOverlappingFaceButtonsAllPressTogether() {
        showButtons(fourButtons = true)
        val overlap = point(60f, 60f)
        compose.onNodeWithTag("buttons").performTouchInput { down(overlap) }
        assertPressed("square", "triangle", "circle", "cross")
        compose.onNodeWithTag("buttons").performTouchInput { up() }
        assertPressed()
    }

    @Test fun cancelReleasesEveryOverlappingButton() {
        showButtons()
        val overlap = point(80f, 60f)
        compose.onNodeWithTag("buttons").performTouchInput { down(overlap) }
        assertPressed("square", "triangle")
        compose.onNodeWithTag("buttons").performTouchInput { cancel() }
        assertPressed()
    }

    @Test fun removingControlsReleasesEveryOverlappingButton() {
        showButtons()
        val overlap = point(80f, 60f)
        compose.onNodeWithTag("buttons").performTouchInput { down(overlap) }
        assertPressed("square", "triangle")
        compose.runOnIdle { visible.value = false }
        assertPressed()
    }

    @Test fun overlappingStickyButtonsToggleOnlyOnRelease() {
        showButtons(toggle = true)
        val overlap = point(80f, 60f)
        compose.onNodeWithTag("buttons").performTouchInput { down(overlap) }
        assertPressed()
        compose.onNodeWithTag("buttons").performTouchInput { up() }
        assertPressed("square", "triangle")
        compose.onNodeWithTag("buttons").performTouchInput { down(overlap); up() }
        assertPressed()
    }

    @Test fun slidingOntoStickyButtonDoesNotToggleIt() {
        showButtons(toggle = true)
        val square = point(40f, 60f)
        val overlap = point(80f, 60f)
        compose.onNodeWithTag("buttons").performTouchInput {
            down(square)
            moveTo(overlap)
            up()
        }
        assertPressed("square")
    }

    @Test fun leavingOverlapKeepsSharedComboActionPressed() {
        showRuntimeComboButtons()
        val overlap = point(80f, 60f)
        val combo = point(120f, 60f)
        compose.onNodeWithTag("buttons").performTouchInput { down(overlap) }
        assertKeys(KeyEvent.KEYCODE_BUTTON_X, KeyEvent.KEYCODE_BUTTON_Y)
        compose.onNodeWithTag("buttons").performTouchInput { moveTo(combo) }
        assertKeys(KeyEvent.KEYCODE_BUTTON_X, KeyEvent.KEYCODE_BUTTON_Y)
        compose.onNodeWithTag("buttons").performTouchInput { up() }
        assertKeys()
    }

    @Test fun releasingOneControlKeepsSharedComboActionHeldByAnotherFinger() {
        showRuntimeComboButtons()
        val square = point(40f, 60f)
        val combo = point(120f, 60f)
        compose.onNodeWithTag("buttons").performTouchInput {
            down(0, square)
            down(1, combo)
        }
        assertKeys(KeyEvent.KEYCODE_BUTTON_X, KeyEvent.KEYCODE_BUTTON_Y)
        compose.onNodeWithTag("buttons").performTouchInput { up(0) }
        assertKeys(KeyEvent.KEYCODE_BUTTON_X, KeyEvent.KEYCODE_BUTTON_Y)
        compose.onNodeWithTag("buttons").performTouchInput { up(1) }
        assertKeys()
    }

    @Test fun changingBindingReleasesLatchedActionsBeforeReusingControl() {
        showRuntimeComboButtons(toggleCombo = true)
        val combo = point(120f, 60f)
        compose.onNodeWithTag("buttons").performTouchInput { down(combo); up() }
        assertKeys(KeyEvent.KEYCODE_BUTTON_X, KeyEvent.KEYCODE_BUTTON_Y)
        compose.runOnIdle { runtimePrimaryAction.value = "circle" }
        assertKeys()
        compose.onNodeWithTag("buttons").performTouchInput { down(combo); up() }
        assertKeys(KeyEvent.KEYCODE_BUTTON_B, KeyEvent.KEYCODE_BUTTON_Y)
        compose.runOnIdle { visible.value = false }
        assertKeys()
    }
}
