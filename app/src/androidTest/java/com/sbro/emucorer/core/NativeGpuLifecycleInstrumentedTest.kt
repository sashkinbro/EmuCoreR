// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: LicenseRef-EmuCoreR-Proprietary
package com.sbro.emucorer.core

import android.graphics.PixelFormat
import android.media.ImageReader
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.Callable
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

/** Tests the shipped JNI library, real Android queues and a persistent frame worker.
 * Synthetic project-owned MIPS ROM only: no private BIOS/game or user preferences.
 * This is bridge lifecycle coverage, not a claim about Activity/Compose lifecycle.
 */
@RunWith(AndroidJUnit4::class)
class NativeGpuLifecycleInstrumentedTest {
    private val bridge = NativeCoreBridge()

    @Test
    fun absentSessionReportsInvalidNativeExecutionRequest() {
        assertEquals(7, bridge.stateHashSchemaVersion())
        assertNotEquals(0, bridge.setHardwareGpuExecution(0L, true))
        assertNotEquals(0, bridge.setHardwareGpuExecution(0L, false))
        assertEquals("Invalid session", bridge.getGpuExecutionError(0L))
        assertEquals(0L, bridge.getGpuBackendSubmissions(0L))
        assertEquals(-1, bridge.getPresentationStatus(0L))
        assertEquals("Invalid session", bridge.getPresentationError(0L))
    }

    @Test
    fun nativeExecutionSurvivesSurfaceRecreationAndRendererChanges() {
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        val state = File.createTempFile("gpu-lifecycle-", ".rstate", instrumentation.targetContext.cacheDir)
        val snapshot = File.createTempFile("gpu-snapshot-", ".rstate", instrumentation.targetContext.cacheDir)
        val worker = Executors.newSingleThreadExecutor()
        val handle = bridge.createSession()
        assertNotEquals(0L, handle)
        fun <T> onWorker(block: () -> T): T = worker.submit(Callable(block)).get(30, TimeUnit.SECONDS)
        fun onUi(block: () -> Unit) = instrumentation.runOnMainSync(block)
        fun saveBytes(): ByteArray {
            assertEquals(0, bridge.saveState(handle, snapshot.absolutePath))
            return snapshot.readBytes()
        }
        try {
            assertEquals(0, bridge.loadBiosBytes(handle, drawingRom()))
            assertEquals(0, bridge.setGeometryPrecision(handle, false))
            // A reproducible pre-execution checkpoint: each backend must execute
            // the ROM itself, not merely present a software-produced image.
            assertEquals(0, bridge.saveState(handle, state.absolutePath))
            var oracle: IntArray? = null
            var oracleState: ByteArray? = null
            for (renderer in listOf(0, 1, 2, 1, 0)) {
                assertEquals(0, bridge.setHardwareGpuExecution(handle, false))
                onUi { assertEquals(0, bridge.setSurface(handle, null, renderer)) }
                assertEquals(0, bridge.loadState(handle, state.absolutePath))
                if (renderer != 0) {
                    assertEquals(0, bridge.setHardwareGpuExecution(handle, true))
                    val before = saveBytes()
                    assertNotEquals(0, bridge.setGeometryPrecision(handle, true))
                    assertArrayEquals("rejected PGXP must preserve state", before, saveBytes())
                }
                val submittedBefore = bridge.getGpuBackendSubmissions(handle)
                for ((width, height) in listOf(64 to 64, 160 to 120, 96 to 128)) {
                    ImageReader.newInstance(width, height, PixelFormat.RGBA_8888, 2).use { reader ->
                        onUi { assertEquals(0, bridge.setSurface(handle, reader.surface, renderer)) }
                        try {
                            val pcm = onWorker { bridge.runFrame(handle) }
                            assertNotNull("frame failed: ${bridge.getGpuExecutionError(handle)}", pcm)
                            assertEquals("", bridge.getGpuExecutionError(handle))
                            assertEquals(1, bridge.getPresentationStatus(handle))
                            assertEquals("", bridge.getPresentationError(handle))
                            assertPresented(reader, width, height)
                            val pixels = requireNotNull(bridge.getVramRgba(handle, 320, 240))
                            if (oracle == null) oracle = pixels else assertArrayEquals(oracle, pixels)
                            if (width == 64) {
                                val afterFrame = saveBytes()
                                if (oracleState == null) oracleState = afterFrame
                                else assertArrayEquals("renderer $renderer full state", oracleState, afterFrame)
                            }
                            val paused = saveBytes()
                            onUi { assertEquals(0, bridge.setSurface(handle, null, renderer)) }
                            assertArrayEquals("surface detachment must not advance guest time", paused, saveBytes())
                            // Restore while detached. The next window must upload
                            // restored VRAM; the ROM now loops without drawing.
                            assertEquals(0, bridge.loadState(handle, snapshot.absolutePath))
                            // Resume the same state on a fresh queue, never rebind
                            // different producer APIs to an old BufferQueue.
                        } finally {
                            onUi { assertEquals(0, bridge.setSurface(handle, null, renderer)) }
                        }
                    }
                }
                if (renderer != 0) {
                    assertTrue("native renderer $renderer must execute GP0 batches",
                        bridge.getGpuBackendSubmissions(handle) > submittedBefore)
                }
            }
        } finally {
            worker.shutdown()
            assertTrue(worker.awaitTermination(30, TimeUnit.SECONDS))
            bridge.destroySession(handle)
            state.delete()
            snapshot.delete()
        }
    }

    private fun assertPresented(reader: ImageReader, width: Int, height: Int) {
        val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5)
        var image = reader.acquireNextImage()
        while (image == null && System.nanoTime() < deadline) {
            Thread.sleep(1)
            image = reader.acquireNextImage()
        }
        requireNotNull(image) { "No presented image" }.use { captured ->
            assertEquals(width, captured.width)
            assertEquals(height, captured.height)
            val plane = captured.planes.single()
            val data = plane.buffer
            val top = (height - width * 3 / 4) / 2
            val activeHeight = width * 3 / 4
            // RGBA8 bytes, including interior 5-bit levels and letterbox alpha.
            val colors = intArrayOf(0xff1839c6.toInt(), 0xffc61839.toInt(),
                0xff39c618.toInt(), 0xff737373.toInt())
            for (y in 0 until height) for (x in 0 until width) {
                val offset = y * plane.rowStride + x * plane.pixelStride
                val actual = (0..3).fold(0) { word, c ->
                    word or ((data.get(offset + c).toInt() and 255) shl (c * 8))
                }
                val quadrant = (if (x >= width / 2) 1 else 0) +
                    (if (y >= top + activeHeight / 2) 2 else 0)
                val expected = if (y < top || y >= top + activeHeight) 0xff000000.toInt()
                    else colors[quadrant]
                assertEquals("presented pixel ($x,$y)", expected, actual)
            }
        }
    }

    private fun drawingRom(): ByteArray {
        val words = mutableListOf(0x3c08bf80) // lui t0, 0xbf80 (uncached MMIO)
        fun write(value: Int, offset: Int) {
            words += 0x3c090000 or (value ushr 16) // lui t1, upper
            words += 0x35290000 or (value and 0xffff) // ori t1, t1, lower
            words += 0xad090000.toInt() or offset // sw t1, offset(t0)
        }
        write(0x03000000, 0x1814) // GP1 display enabled
        write(0x08000001, 0x1814) // 320x240
        write(0xe3000000.toInt(), 0x1810)
        write(0xe407ffff.toInt(), 0x1810)
        val colors = intArrayOf(0x1838c0, 0xc01838, 0x38c018, 0x707070)
        for (quadrant in 0..3) {
            write(0x60000000 or colors[quadrant], 0x1810)
            write((quadrant % 2 * 160) or ((quadrant / 2 * 120) shl 16), 0x1810)
            write(160 or (120 shl 16), 0x1810)
        }
        words += 0x1000ffff // beq zero, zero, self
        words += 0 // delay slot
        return ByteBuffer.allocate(512 * 1024).order(ByteOrder.LITTLE_ENDIAN).apply {
            words.forEach { putInt(it) }
        }.array()
    }
}
