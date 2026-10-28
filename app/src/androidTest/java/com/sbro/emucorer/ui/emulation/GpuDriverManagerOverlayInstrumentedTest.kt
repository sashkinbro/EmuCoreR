package com.sbro.emucorer.ui.emulation

import android.content.Intent
import androidx.activity.compose.setContent
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.test.getUnclippedBoundsInRoot
import androidx.compose.ui.test.junit4.v2.createEmptyComposeRule
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithContentDescription
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performTouchInput
import androidx.compose.ui.test.click
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.ViewCompat
import com.sbro.emucorer.R
import com.sbro.emucorer.RuntimeTestActivity
import com.sbro.emucorer.ui.settings.GpuDriverScreen
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class GpuDriverManagerOverlayInstrumentedTest {
    @get:Rule val compose = createEmptyComposeRule()

    @Test fun overlayCoversMenuAndStaysPresentDuringBackAnimation() {
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        val activity = instrumentation.startActivitySync(
            Intent(instrumentation.targetContext, RuntimeTestActivity::class.java)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        ) as RuntimeTestActivity
        val visible = mutableStateOf(false)
        var menuClicks = 0
        try {
            instrumentation.runOnMainSync {
                activity.setContent {
                    MaterialTheme {
                        Box(Modifier.fillMaxSize().testTag("root")) {
                            Box(Modifier.fillMaxSize().clickable { menuClicks++ }.testTag("menu"))
                            GpuDriverManagerOverlay(visible.value, onBack = { visible.value = false }) {
                                Box(Modifier.fillMaxSize().testTag("manager")) { Text("Drivers") }
                            }
                        }
                    }
                }
            }
            compose.mainClock.autoAdvance = false
            compose.runOnIdle { visible.value = true }
            compose.mainClock.advanceTimeByFrame()
            compose.onNodeWithTag("manager", useUnmergedTree = true).assertExists()
            compose.mainClock.advanceTimeBy(400)
            assertEquals(
                compose.onNodeWithTag("root").getUnclippedBoundsInRoot(),
                compose.onNodeWithTag("manager", useUnmergedTree = true).getUnclippedBoundsInRoot()
            )
            compose.onNodeWithTag("manager", useUnmergedTree = true).performTouchInput { click(center) }
            compose.runOnIdle { assertEquals(0, menuClicks) }

            instrumentation.runOnMainSync { activity.onBackPressedDispatcher.onBackPressed() }
            compose.mainClock.advanceTimeByFrame()
            compose.mainClock.advanceTimeBy(40)
            compose.onNodeWithTag("manager", useUnmergedTree = true).assertExists()
            compose.mainClock.advanceTimeBy(350)
            compose.onNodeWithTag("manager", useUnmergedTree = true).assertDoesNotExist()
        } finally {
            compose.mainClock.autoAdvance = true
            instrumentation.runOnMainSync { activity.finish() }
        }
    }

    @Test fun realManagerKeepsImmersiveModeAndReturnsToGameMenu() {
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        val activity = instrumentation.startActivitySync(
            Intent(instrumentation.targetContext, RuntimeTestActivity::class.java)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        ) as RuntimeTestActivity
        val visible = mutableStateOf(true)
        try {
            instrumentation.runOnMainSync {
                WindowCompat.getInsetsController(activity.window, activity.window.decorView)
                    .hide(WindowInsetsCompat.Type.systemBars())
                activity.setContent {
                    MaterialTheme {
                        Box(Modifier.fillMaxSize()) {
                            Text("Game menu")
                            GpuDriverManagerOverlay(visible.value, onBack = { visible.value = false }) {
                                GpuDriverScreen(onBackClick = { visible.value = false }, onSelectDriverForGame = {})
                            }
                        }
                    }
                }
            }
            compose.onNodeWithText(activity.getString(R.string.settings_gpu_driver_manager_title)).assertExists()
            compose.runOnIdle {
                val insets = ViewCompat.getRootWindowInsets(activity.window.decorView)
                assertFalse("Navigation bar reappeared over the game", insets!!.isVisible(WindowInsetsCompat.Type.navigationBars()))
            }
            compose.onNodeWithContentDescription(activity.getString(R.string.back)).performClick()
            compose.onNodeWithText("Game menu").assertExists()
            compose.onNodeWithText(activity.getString(R.string.settings_gpu_driver_manager_title)).assertDoesNotExist()
        } finally {
            instrumentation.runOnMainSync { activity.finish() }
        }
    }
}
