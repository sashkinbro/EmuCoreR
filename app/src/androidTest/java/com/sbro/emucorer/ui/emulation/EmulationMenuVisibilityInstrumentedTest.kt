package com.sbro.emucorer.ui.emulation

import android.content.Intent
import android.graphics.Bitmap
import android.graphics.Color
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.size
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.State
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.test.assertTextEquals
import androidx.compose.ui.test.junit4.v2.createEmptyComposeRule
import androidx.compose.ui.test.onAllNodesWithContentDescription
import androidx.compose.ui.test.onNodeWithContentDescription
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.unit.dp
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.sbro.emucorer.RuntimeTestActivity
import com.sbro.emucorer.ui.common.GameCoverArt
import com.sbro.emucorer.ui.common.clearCoverImageMemoryCache
import java.io.File
import org.junit.After
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class EmulationMenuVisibilityInstrumentedTest {
    @get:Rule val compose = createEmptyComposeRule()
    private var activity: RuntimeTestActivity? = null
    private lateinit var cover: File

    @Before fun launchMenu() {
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        cover = File.createTempFile("menu-cover", ".png", instrumentation.targetContext.cacheDir)
        val bitmap = Bitmap.createBitmap(32, 32, Bitmap.Config.ARGB_8888)
        try {
            bitmap.eraseColor(Color.RED)
            cover.outputStream().use { bitmap.compress(Bitmap.CompressFormat.PNG, 100, it) }
        } finally {
            bitmap.recycle()
        }
        activity = instrumentation.startActivitySync(
            Intent(instrumentation.targetContext, RuntimeTestActivity::class.java)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        ) as RuntimeTestActivity
    }

    @After fun finishMenu() {
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        activity?.let { instrumentation.runOnMainSync { it.finish() } }
        instrumentation.waitForIdleSync()
        if (::cover.isInitialized) cover.delete()
        clearCoverImageMemoryCache()
    }

    private fun showMenu(state: State<EmulationUiState>) {
        InstrumentationRegistry.getInstrumentation().runOnMainSync {
            requireNotNull(activity).setContent {
                MaterialTheme {
                    EmulationMenuVisibility(state.value, showControlsEditor = false) { menuState ->
                        Column(Modifier.testTag("menu")) {
                            GameCoverArt(
                                coverPath = menuState.currentGameCoverPath,
                                fallbackTitle = menuState.currentGameTitle,
                                modifier = Modifier.size(64.dp)
                            )
                            Text(menuState.currentGameTitle, Modifier.testTag("menu-title"))
                        }
                    }
                }
            }
        }
        compose.waitUntil(timeoutMillis = 5_000) {
            compose.onAllNodesWithContentDescription("Tekken").fetchSemanticsNodes().isNotEmpty()
        }
    }

    @Test fun shutdownKeepsCoverAndTitleUntilMenuExitAnimationFinishes() {
        val state = mutableStateOf(EmulationUiState(
            showMenu = true,
            currentGameTitle = "Tekken",
            currentGameCoverPath = cover.absolutePath
        ))
        showMenu(state)
        compose.mainClock.autoAdvance = false
        compose.runOnIdle {
            state.value = state.value.copy(
                showMenu = false,
                currentGameTitle = "",
                currentGameCoverPath = null
            )
        }
        compose.mainClock.advanceTimeByFrame()
        compose.mainClock.advanceTimeBy(100)

        compose.onNodeWithContentDescription("Tekken").assertExists()
        compose.onNodeWithTag("menu-title").assertTextEquals("Tekken")

        compose.mainClock.advanceTimeBy(500)
        compose.onNodeWithTag("menu").assertDoesNotExist()
    }

    @Test fun reopeningMenuUsesCurrentGameRatherThanClosingSnapshot() {
        val state = mutableStateOf(EmulationUiState(
            showMenu = true,
            currentGameTitle = "Tekken",
            currentGameCoverPath = cover.absolutePath
        ))
        showMenu(state)
        compose.mainClock.autoAdvance = false
        compose.runOnIdle { state.value = state.value.copy(showMenu = false) }
        compose.mainClock.advanceTimeByFrame()
        compose.mainClock.advanceTimeBy(100)
        compose.runOnIdle { state.value = state.value.copy(showMenu = true, currentGameTitle = "Soul Blade") }
        compose.mainClock.advanceTimeBy(500)

        compose.onNodeWithContentDescription("Soul Blade").assertExists()
        compose.onNodeWithTag("menu-title").assertTextEquals("Soul Blade")
    }
}
