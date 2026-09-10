package com.sbro.emucorer

import android.app.Application
import com.sbro.emucorer.core.CrashLogger
import com.sbro.emucorer.core.EmulatorBridge
import com.sbro.emucorer.discord.DiscordIntegration
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob

class EmuCoreRApp : Application() {
    internal val applicationScope = CoroutineScope(SupervisorJob() + Dispatchers.IO)

    override fun onCreate() {
        super.onCreate()
        // The Discord SDK helper must not initialize the emulator-side application graph.
        if (Application.getProcessName().endsWith(":discord")) return
        // CrashLogger must be the very first thing — it catches crashes in all subsequent init steps
        CrashLogger.init(this)
        EmulatorBridge.initializeOnce(this)
        DiscordIntegration.initialize(this)
    }
}
