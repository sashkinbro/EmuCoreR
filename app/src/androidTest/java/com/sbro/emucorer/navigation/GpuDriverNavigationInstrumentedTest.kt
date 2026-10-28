package com.sbro.emucorer.navigation

import android.content.Context
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.ui.Modifier
import androidx.compose.ui.test.junit4.v2.createComposeRule
import androidx.compose.ui.test.onNodeWithContentDescription
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.rememberNavController
import androidx.navigation.NavHostController
import androidx.test.core.app.ApplicationProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import com.sbro.emucorer.R
import com.sbro.emucorer.ui.common.gpuDriverEnterTransition
import com.sbro.emucorer.ui.common.gpuDriverExitTransition
import com.sbro.emucorer.ui.settings.GpuDriverScreen
import org.junit.Rule
import org.junit.Assert.assertEquals
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class GpuDriverNavigationInstrumentedTest {
    @get:Rule val compose = createComposeRule()

    @Test fun topBarBackKeepsDriverScreenDuringExitTransition() {
        val context = ApplicationProvider.getApplicationContext<Context>()
        lateinit var navController: NavHostController
        compose.setContent {
            MaterialTheme {
                navController = rememberNavController()
                NavHost(navController, startDestination = "settings") {
                    composable("settings") {
                        Box(Modifier.fillMaxSize()) {
                            Button(onClick = { navController.navigate("drivers") }) { Text("Open drivers") }
                        }
                    }
                    composable(
                        "drivers",
                        enterTransition = { gpuDriverEnterTransition() },
                        popExitTransition = { gpuDriverExitTransition() }
                    ) {
                        GpuDriverScreen(onBackClick = { navController.popBackStack() })
                    }
                }
            }
        }
        compose.mainClock.autoAdvance = false
        try {
            compose.onNodeWithText("Open drivers").performClick()
            compose.mainClock.advanceTimeByFrame()
            compose.mainClock.advanceTimeBy(100)
            compose.onNodeWithText(context.getString(R.string.settings_gpu_driver_manager_title)).assertExists()
            compose.mainClock.advanceTimeBy(400)
            compose.onNodeWithContentDescription(context.getString(R.string.back)).performClick()
            compose.mainClock.advanceTimeByFrame()
            compose.mainClock.advanceTimeBy(30)
            compose.runOnIdle { assertEquals("settings", navController.currentDestination?.route) }
            compose.onNodeWithText(context.getString(R.string.settings_gpu_driver_manager_title)).assertExists()
            compose.mainClock.advanceTimeBy(1_000)
            compose.mainClock.advanceTimeByFrame()
            compose.onNodeWithText(context.getString(R.string.settings_gpu_driver_manager_title)).assertDoesNotExist()
            compose.onNodeWithText("Open drivers").assertExists()
        } finally {
            compose.mainClock.autoAdvance = true
        }
    }
}
