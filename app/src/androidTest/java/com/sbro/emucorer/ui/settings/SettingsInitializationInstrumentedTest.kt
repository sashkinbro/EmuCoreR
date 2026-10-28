package com.sbro.emucorer.ui.settings

import android.app.Application
import androidx.lifecycle.ViewModelStore
import androidx.test.core.app.ApplicationProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.sbro.emucorer.data.SettingsSnapshot
import com.sbro.emucorer.ui.theme.ThemeMode
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class SettingsInitializationInstrumentedTest {
    @Test fun driverRefreshDoesNotOverwriteNewSettings() {
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        val entered = CountDownLatch(1)
        val release = CountDownLatch(1)
        val store = ViewModelStore()
        lateinit var model: SettingsViewModel
        instrumentation.runOnMainSync {
            model = SettingsViewModel(ApplicationProvider.getApplicationContext<Application>()) {
                if (Thread.currentThread().name == "test-driver-refresh") {
                    entered.countDown()
                    check(release.await(10, TimeUnit.SECONDS))
                }
                emptyList()
            }
            store.put("settings", model)
        }
        try {
            runBlocking { withTimeout(15_000) { model.uiState.first { it.isLoaded } } }
            val refresh = Thread({ model.refreshInstalledGpuDrivers() }, "test-driver-refresh")
            refresh.start()
            assertTrue("Driver refresh did not start", entered.await(10, TimeUnit.SECONDS))
            instrumentation.runOnMainSync {
                model.applySettingsSnapshot(SettingsSnapshot(themeMode = ThemeMode.DARK))
            }
            release.countDown()
            refresh.join(10_000)
            assertTrue("Driver refresh did not finish", !refresh.isAlive)
            assertTrue("Settings became unloaded", model.uiState.value.isLoaded)
            assertEquals(ThemeMode.DARK, model.uiState.value.themeMode)
        } finally {
            release.countDown()
            instrumentation.runOnMainSync { store.clear() }
        }
    }
}
