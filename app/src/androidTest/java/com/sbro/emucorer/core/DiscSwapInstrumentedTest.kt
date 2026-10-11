// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: GPL-3.0-or-later
package com.sbro.emucorer.core

import android.graphics.PixelFormat
import android.content.Intent
import android.media.ImageReader
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.provider.DocumentsContract
import androidx.core.content.FileProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.ConcurrentLinkedQueue
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class DiscSwapInstrumentedTest {
    @Test
    fun swappingDiscsPreservesRunningRamAndRejectsBrokenImages() {
        assertEquals("OnePlus", Build.MANUFACTURER)
        assertEquals("CPH2747", Build.MODEL)
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        val context = instrumentation.targetContext
        val previous = CoreRuntime.settings.toMap()
        val root = File(context.cacheDir, "shared/disc-swap-${System.nanoTime()}").apply { mkdirs() }
        val bios = File(root, "probe.bin").apply { writeBytes(makeBios()) }
        val first = File(root, "first.iso").apply { writeBytes(ByteArray(64 * 2048) { 0x11 }) }
        val second = File(root, "second.iso").apply { writeBytes(ByteArray(64 * 2048) { 0x22 }) }
        val broken = File(root, "broken.iso").apply { writeBytes(byteArrayOf(1, 2, 3)) }
        val state = File(root, "probe.rstate")
        val consumer = HandlerThread("DiscSwap-ImageConsumer").apply { start() }
        val handler = Handler(consumer.looper)
        val authority = "com.sbro.emucorer.test.documents"
        val tree = DocumentsContract.buildTreeDocumentUri(authority, "root")
        val cueUri = DocumentsContract.buildDocumentUriUsingTree(tree, "root/cue")
        val document = DocumentsContract.buildDocumentUri(authority, "root")
        val copiedCue = File(context.cacheDir, "swanstation-cue/${cueUri.toString().hashCode()}")
        copiedCue.deleteRecursively()
        fun save(): ByteArray {
            assertTrue(CoreRuntime.saveState(state.absolutePath))
            return state.readBytes()
        }
        fun ram(bytes: ByteArray): ByteArray {
            val marker = ByteBuffer.allocate(16).order(ByteOrder.LITTLE_ENDIAN)
                .putInt(0x41626344).putInt(0x55667718).putInt(0x71928364).putInt(0x25364758).array()
            val offset = (0..bytes.size - 20).firstOrNull { index ->
                marker.indices.all { bytes[index + it] == marker[it] }
            }
            assertNotNull("owned BIOS RAM marker disappeared: the session was rebooted", offset)
            return bytes.copyOfRange(offset!!, offset + 20)
        }
        try {
            CoreRuntime.settings.clear()
            CoreRuntime.initialize(context)
            context.contentResolver.call(document, "grantFixture", context.packageName, null)
            context.contentResolver.takePersistableUriPermission(tree, Intent.FLAG_GRANT_READ_URI_PERMISSION)
            CoreRuntime.updateSetting("Folders", "Bios", root.absolutePath)
            CoreRuntime.updateSetting("Filenames", "BIOS", bios.name)
            CoreRuntime.updateSetting("EmuCore", "EnableFastBoot", "false")
            CoreRuntime.setMemoryCardPath(0, null)
            CoreRuntime.setMemoryCardPath(1, null)
            for (renderer in listOf(RendererDefaults.SOFTWARE, RendererDefaults.VULKAN, RendererDefaults.OPENGL)) {
                CoreRuntime.updateSetting("EmuCoreR", "Renderer", renderer.toString())
                ImageReader.newInstance(320, 240, PixelFormat.RGBA_8888, 3).use { reader ->
                    reader.setOnImageAvailableListener({ it.acquireLatestImage()?.close() }, handler)
                    instrumentation.runOnMainSync { CoreRuntime.attachSurface(reader.surface, 320, 240) }
                    try {
                        assertTrue(CoreRuntime.start(first.absolutePath, biosOnly = false))
                        CoreRuntime.applyCoreOption("swanstation_Main_SaveStateCompression", "Uncompressed")
                        // Shader/context startup can delay the first guest frame.
                        // Wait for the owned BIOS and the compression option to run.
                        var initial: ByteArray? = null
                        val deadline = System.nanoTime() + 10_000_000_000L
                        while (initial == null && System.nanoTime() < deadline) {
                            Thread.sleep(100)
                            CoreRuntime.pause()
                            val snapshot = save()
                            initial = runCatching { ram(snapshot) }.getOrNull()
                            if (initial == null) CoreRuntime.resume()
                        }
                        assertNotNull("renderer $renderer did not boot the test BIOS", initial)
                        val before = initial!!
                        if (renderer == RendererDefaults.SOFTWARE) {
                            val paused = save()
                            checkCancelledSwap(second.absolutePath)
                            assertArrayEquals("cancelled queued swap still ran", paused, save())
                        }
                        assertTrue(CoreRuntime.changeDisc(second.absolutePath))
                        assertTrue(CoreRuntime.hasDiscMedia())
                        val swapped = save()
                        assertArrayEquals("swap reset RAM or advanced the paused CPU", before, ram(swapped))
                        assertTrue("new image is absent from the state", swapped.toString(Charsets.ISO_8859_1).contains(second.absolutePath))
                        assertFalse("broken image accepted", CoreRuntime.changeDisc(broken.absolutePath))
                        assertTrue(CoreRuntime.hasDiscMedia())
                        assertArrayEquals("failed swap changed the old media or guest state", swapped, save())
                        val uri = FileProvider.getUriForFile(context, "${context.packageName}.fileprovider", first)
                        assertTrue("SAF swap failed", CoreRuntime.changeDisc(uri.toString()))
                        assertArrayEquals("SAF swap reset RAM", before, ram(save()))
                        assertTrue("opaque SAF CUE swap failed", CoreRuntime.changeDisc(cueUri.toString()))
                        assertArrayEquals("CUE swap reset RAM", before, ram(save()))
                        assertTrue("track was not cached", File(copiedCue, "track_1.bin").delete())
                        // Renderer recreation must boot the successfully swapped image.
                        assertTrue(CoreRuntime.restartWithRenderer(renderer))
                        assertArrayEquals("renderer restart lost the swapped session", before, ram(save()))
                        CoreRuntime.resume()
                        Thread.sleep(100)
                        CoreRuntime.pause()
                        assertFalse("guest did not continue after swapping", before.contentEquals(ram(save())))
                    } finally {
                        CoreRuntime.shutdown()
                        instrumentation.runOnMainSync { CoreRuntime.detachSurface() }
                        reader.setOnImageAvailableListener(null, null)
                    }
                }
            }
        } finally {
            CoreRuntime.shutdown()
            CoreRuntime.detachSurface()
            DocumentPathResolver.releasePreparedLaunchHandles()
            copiedCue.deleteRecursively()
            runCatching { context.contentResolver.releasePersistableUriPermission(tree, Intent.FLAG_GRANT_READ_URI_PERMISSION) }
            CoreRuntime.settings.clear()
            CoreRuntime.settings.putAll(previous)
            CoreRuntime.updateSetting("EmuCoreR", "Renderer", previous["EmuCoreR:Renderer"]
                ?: previous["EmuCore/GS:Renderer"] ?: RendererDefaults.defaultForHardware().toString())
            CoreRuntime.initialize(context)
            consumer.quitSafely()
            consumer.join(5_000)
            root.deleteRecursively()
        }
    }

    private fun checkCancelledSwap(path: String) {
        val entered = CountDownLatch(1)
        val release = CountDownLatch(1)
        val dispatch = CoreRuntime::class.java.getDeclaredMethod("runOnFrameThread", kotlin.jvm.functions.Function0::class.java)
            .apply { isAccessible = true }
        val queue = CoreRuntime::class.java.getDeclaredField("frameTasks").apply { isAccessible = true }
            .get(CoreRuntime) as ConcurrentLinkedQueue<*>
        val blocker = Thread {
            dispatch.invoke(CoreRuntime, {
                entered.countDown()
                release.await(5, TimeUnit.SECONDS)
                true
            })
        }.apply { start() }
        var result: Boolean? = null
        var preservedInterrupt = false
        val caller = Thread {
            result = CoreRuntime.changeDisc(path)
            preservedInterrupt = Thread.currentThread().isInterrupted
        }
        try {
            assertTrue(entered.await(5, TimeUnit.SECONDS))
            caller.start()
            val deadline = System.nanoTime() + 5_000_000_000L
            while (queue.isEmpty() && System.nanoTime() < deadline) Thread.sleep(1)
            assertFalse("swap was not queued", queue.isEmpty())
            caller.interrupt()
            caller.join(2_000)
            assertFalse("queued cancellation waited for native execution", caller.isAlive)
            assertEquals(false, result)
            assertTrue("caller interruption was consumed", preservedInterrupt)
        } finally {
            release.countDown()
            blocker.join(5_000)
            caller.join(5_000)
        }
    }

    private fun makeBios(): ByteArray {
        val bytes = ByteBuffer.allocate(512 * 1024).order(ByteOrder.LITTLE_ENDIAN)
        fun constant(register: Int, value: Int) {
            bytes.putInt((0xf shl 26) or (register shl 16) or (value ushr 16))
            bytes.putInt((0xd shl 26) or (register shl 21) or (register shl 16) or (value and 65535))
        }
        constant(10, 0x1f801810)
        fun gpu(offset: Int, value: Int) {
            constant(8, value)
            bytes.putInt((0x2b shl 26) or (10 shl 21) or (8 shl 16) or offset)
        }
        for (command in listOf(0, 0x08000001, 0x05000000, 0x06c60260, 0x07042018, 0x03000000)) gpu(4, command)
        for (command in listOf(0x02ffffff, 0, (240 shl 16) or 320)) gpu(0, command)
        constant(10, 0xa0001000.toInt())
        for ((index, value) in listOf(0x41626344, 0x55667718, 0x71928364, 0x25364758).withIndex()) {
            constant(8, value)
            bytes.putInt((0x2b shl 26) or (10 shl 21) or (8 shl 16) or (index * 4))
        }
        listOf(0x24090000, 0x25290001, 0xad490010.toInt(), 0x1000fffd, 0).forEach { bytes.putInt(it) }
        return bytes.array()
    }
}
