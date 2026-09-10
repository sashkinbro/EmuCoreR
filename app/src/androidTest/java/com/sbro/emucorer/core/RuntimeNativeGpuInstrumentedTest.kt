// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: LicenseRef-EmuCoreR-Proprietary
package com.sbro.emucorer.core

import android.graphics.PixelFormat
import android.media.ImageReader
import android.os.Handler
import android.os.HandlerThread
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

/** Actual CoreRuntime worker + native AAudio/GPU executors, with an owned ROM.
 * Does not change DataStore, real BIOS, memory cards or app GPU defaults.
 */
@RunWith(AndroidJUnit4::class)
class RuntimeNativeGpuInstrumentedTest {
    @Test
    fun explicitNativeConfigurationExecutesOnStartupAndAcrossLiveModeChanges() {
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        val context = instrumentation.targetContext
        assertFalse(CoreRuntime.hasSession())
        val previous = CoreRuntime.settings.toMap()
        val previousRenderer = previous["EmuCoreR:Renderer"]
            ?: previous["EmuCore/GS:Renderer"]
            ?: RendererDefaults.defaultForHardware().toString()
        val rom = File.createTempFile("native-runtime-", ".bin", context.cacheDir)
        val initial = File.createTempFile("native-initial-", ".rstate", context.cacheDir)
        val snapshot = File.createTempFile("native-snapshot-", ".rstate", context.cacheDir)
        val consumer = HandlerThread("NativeRuntime-ImageConsumer").apply { start() }
        val handler = Handler(consumer.looper)
        fun onUi(block: () -> Unit) = instrumentation.runOnMainSync(block)
        fun saveBytes(): ByteArray {
            assertTrue(CoreRuntime.saveState(snapshot.absolutePath))
            return snapshot.readBytes()
        }
        try {
            rom.writeBytes(drawingRom())
            CoreRuntime.settings.clear()
            CoreRuntime.initialize(context)
            assertTrue(CoreRuntime.updateSetting("Folders", "Bios", rom.parent!!))
            assertTrue(CoreRuntime.updateSetting("Filenames", "BIOS", rom.name))
            assertTrue(CoreRuntime.updateSetting("EmuCoreR/GPU", "PGXP", "false"))
            // Prepare the pre-draw guest state without advancing any cycles.
            val bridge = CoreRuntime.bridge
            val reference = bridge.createSession()
            assertTrue(reference != 0L)
            try {
                assertEquals(0, bridge.loadBiosBytes(reference, rom.readBytes()))
                assertEquals(0, bridge.setGeometryPrecision(reference, false))
                assertEquals(0, bridge.saveState(reference, initial.absolutePath))
            } finally {
                bridge.destroySession(reference)
            }
            for (renderer in listOf(RendererDefaults.VULKAN, RendererDefaults.OPENGL)) {
                assertTrue(CoreRuntime.updateSetting("EmuCoreR", "Renderer", renderer.toString()))
                assertTrue(CoreRuntime.updateSetting("EmuCoreR/GPU", "HardwareExecution", "true"))
                val frames = AtomicInteger()
                ImageReader.newInstance(96, 96, PixelFormat.RGBA_8888, 3).use { reader ->
                    reader.setOnImageAvailableListener({ source ->
                        source.acquireLatestImage()?.use { frames.incrementAndGet() }
                    }, handler)
                    onUi { CoreRuntime.attachSurface(reader.surface, 96, 96) }
                    fun runUntilPresented() {
                        val before = frames.get()
                        CoreRuntime.resume()
                        val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(15)
                        while (frames.get() < before + 3 && System.nanoTime() < deadline) Thread.sleep(5)
                        CoreRuntime.pause()
                        assertNull(CoreRuntime.failure.value)
                        assertTrue("renderer $renderer stopped presenting", frames.get() >= before + 3)
                    }
                    try {
                        assertTrue(CoreRuntime.start("", biosOnly = true))
                        runUntilPresented()
                        assertTrue("startup did not bind the native executor for $renderer",
                            CoreRuntime.gpuBackendSubmissions() > 0L)
                        val accepted = CoreRuntime.settings.toMap()
                        val beforeRejected = saveBytes()
                        assertFalse(CoreRuntime.updateSetting("EmuCoreR/GPU", "PGXP", "true"))
                        assertFalse(CoreRuntime.updateSetting("EmuCoreR", "Renderer", RendererDefaults.SOFTWARE.toString()))
                        assertFalse(CoreRuntime.updateSetting("EmuCoreR/GPU", "HardwareExecution", "TRUE"))
                        assertEquals(accepted, CoreRuntime.settings.toMap())
                        assertArrayEquals("rejected config mutated guest state", beforeRejected, saveBytes())

                        assertTrue(CoreRuntime.updateSetting("EmuCoreR/GPU", "HardwareExecution", "false"))
                        val softwareStart = CoreRuntime.gpuBackendSubmissions()
                        assertTrue(CoreRuntime.loadState(initial.absolutePath))
                        runUntilPresented()
                        assertEquals("software execution still submitted native batches",
                            softwareStart, CoreRuntime.gpuBackendSubmissions())

                        assertTrue(CoreRuntime.updateSetting("EmuCoreR/GPU", "HardwareExecution", "true"))
                        val nativeStart = CoreRuntime.gpuBackendSubmissions()
                        assertTrue(CoreRuntime.loadState(initial.absolutePath))
                        runUntilPresented()
                        assertTrue("live native enable did not execute new draws",
                            CoreRuntime.gpuBackendSubmissions() > nativeStart)
                    } finally {
                        CoreRuntime.shutdown()
                        onUi { CoreRuntime.detachSurface() }
                        reader.setOnImageAvailableListener(null, null)
                        val drained = CountDownLatch(1)
                        handler.post { drained.countDown() }
                        assertTrue(drained.await(5, TimeUnit.SECONDS))
                    }
                }
                assertFalse(CoreRuntime.hasSession())
            }
        } finally {
            CoreRuntime.shutdown()
            CoreRuntime.detachSurface()
            CoreRuntime.updateSetting("EmuCoreR/GPU", "HardwareExecution", "false")
            CoreRuntime.updateSetting("EmuCoreR", "Renderer", previousRenderer)
            CoreRuntime.settings.clear()
            CoreRuntime.settings.putAll(previous)
            consumer.quitSafely()
            consumer.join(5_000)
            rom.delete()
            initial.delete()
            snapshot.delete()
        }
    }

    private fun drawingRom(): ByteArray {
        val words = mutableListOf(0x3c08bf80)
        fun write(value: Int, offset: Int) {
            words += 0x3c090000 or (value ushr 16)
            words += 0x35290000 or (value and 0xffff)
            words += 0xad090000.toInt() or offset
        }
        write(0x03000000, 0x1814)
        write(0x08000001, 0x1814)
        write(0xe3000000.toInt(), 0x1810)
        write(0xe407ffff.toInt(), 0x1810)
        write(0x601838c0, 0x1810) // Flat rectangle exercises native GP0 rasterization.
        write(0, 0x1810)
        write(320 or (240 shl 16), 0x1810)
        // Subsequent cycles keep changing RAM, so paused-state equality is real.
        words += listOf(0x3c08a000, 0x24090000, 0x25290001, 0xad090000.toInt(), 0x1000fffd, 0)
        return ByteBuffer.allocate(512 * 1024).order(ByteOrder.LITTLE_ENDIAN).apply {
            words.forEach { putInt(it) }
        }.array()
    }
}
