package com.sbro.emucorer.ui.settings

import android.content.Context
import androidx.compose.ui.test.getUnclippedBoundsInRoot
import androidx.compose.ui.test.junit4.v2.createComposeRule
import androidx.compose.ui.test.onAllNodesWithText
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.test.core.app.ApplicationProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import com.sbro.emucorer.R
import com.sbro.emucorer.ui.theme.EmuCoreRTheme
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class GpuDriverSettingsEntryInstrumentedTest {
    @get:Rule val compose = createComposeRule()

    @Test fun driverEntryFollowsRendererAndOpensManager() {
        val context = ApplicationProvider.getApplicationContext<Context>()
        val renderer = context.getString(R.string.settings_renderer)
        val driver = context.getString(R.string.settings_gpu_driver_manager_title)
        var opens = 0
        compose.setContent {
            EmuCoreRTheme {
                SettingsScreen(initialTab = "graphics", onOpenGpuDriverManager = { opens++ })
            }
        }
        compose.waitUntil(timeoutMillis = 15_000) {
            compose.onAllNodesWithText(driver).fetchSemanticsNodes().isNotEmpty()
        }
        val rendererBounds = compose.onNodeWithText(renderer).getUnclippedBoundsInRoot()
        val driverBounds = compose.onNodeWithText(driver).getUnclippedBoundsInRoot()
        assertTrue("Driver entry must follow renderer", driverBounds.top > rendererBounds.bottom)
        compose.onNodeWithText(driver).performClick()
        compose.runOnIdle { assertEquals(1, opens) }
    }
}
