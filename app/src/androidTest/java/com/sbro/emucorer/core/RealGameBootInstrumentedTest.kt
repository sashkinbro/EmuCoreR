// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: GPL-3.0-or-later
package com.sbro.emucorer.core

import android.graphics.PixelFormat
import android.content.Intent
import android.media.ImageReader
import android.os.ParcelFileDescriptor
import android.provider.DocumentsContract
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File

/** Opt-in probe using a user-provided disc and BIOS staged in app cache. */
@RunWith(AndroidJUnit4::class)
class RealGameBootInstrumentedTest {
    @Test
    fun realDiscPresentsFramesWithVulkan() {
        val args = InstrumentationRegistry.getArguments()
        assumeTrue("Real-game probe must be explicitly enabled", args.getString("realGameProbe") == "true")
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val root = File(context.cacheDir, "real-game-probe")
        val archiveName = args.getString("archiveName")
        val disc = File(root, archiveName ?: "disc.cue")
        val bios = File(root, "bios.bin")
        assertTrue(disc.isFile && bios.isFile)
        val bridge = NativeCoreBridge()
        val system = File(root, "system").apply { mkdirs() }
        val save = File(root, "save").apply { mkdirs() }
        val assets = File(root, "assets").apply { mkdirs() }
        bridge.nativeInit(system.absolutePath, save.absolutePath, assets.absolutePath)
        bridge.nativeSetCustomGpuDriver(args.getString("driverPath", ""), context.applicationInfo.nativeLibraryDir)
        mapOf("GPU_Renderer" to "Vulkan", "GPU_ResolutionScale" to "1", "GPU_UseThread" to "false",
            "MemoryCards_Card1Type" to "None", "MemoryCards_Card2Type" to "None",
            "BIOS_PatchFastBoot" to "true", "Main_ApplyGameSettings" to "false")
            .forEach { (key, value) -> bridge.nativeSetOption("swanstation_$key", value) }
        val mountedPath = if (archiveName != null) {
            ParcelFileDescriptor.open(disc, ParcelFileDescriptor.MODE_READ_ONLY).use { bridge.nativeMountArchive(it.fd) }
                ?: error("Could not mount $archiveName")
        } else null
        val session = bridge.createSession()
        assertTrue(session != 0L)
        try {
            ImageReader.newInstance(640, 480, PixelFormat.RGBA_8888, 3).use { reader ->
                assertEquals(0, bridge.setSurface(session, reader.surface, RendererDefaults.CORE_VULKAN))
                if (mountedPath != null) {
                    val actual = bridge.getDiscMetadata(mountedPath)
                    val expected = if (archiveName?.startsWith("doom.") == true) "SLUS-00077" else
                        bridge.getDiscMetadata(File(root, "disc.cue").absolutePath)?.lineSequence()?.firstOrNull()
                    assertTrue("Missing expected disc serial", !expected.isNullOrBlank())
                    assertTrue("Archive serial mismatch: expected=$expected actual=$actual", actual?.contains(expected!!) == true)
                }
                assertEquals(0, bridge.loadBios(session, bios.absolutePath))
                assertEquals(0, bridge.loadDisc(session, mountedPath ?: disc.absolutePath))
                var frames = 0
                repeat(500) {
                    bridge.runFrame(session)
                    reader.acquireLatestImage()?.use { frames++ }
                }
                assertTrue("Real disc did not present frames: $frames", frames > 10)
            }
        } finally {
            bridge.destroySession(session)
            mountedPath?.let(bridge::nativeUnmountArchive)
            bridge.nativeSetCustomGpuDriver("", context.applicationInfo.nativeLibraryDir)
        }
    }
    @Test
    fun runtimeStartsArchivedGame() {
        val args = InstrumentationRegistry.getArguments()
        val name = args.getString("archiveName")
        assumeTrue("An archive fixture must be supplied", args.getString("realGameProbe") == "true" && name != null)
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        val context = instrumentation.targetContext
        val root = File(context.cacheDir, "real-game-probe")
        val previous = CoreRuntime.settings.toMap()
        val image = ImageReader.newInstance(640, 480, PixelFormat.RGBA_8888, 3)
        val useSaf = args.getString("archiveSaf") == "true"
        val document = DocumentsContract.buildDocumentUri("com.sbro.emucorer.test.documents", "root/archive")
        try {
            val gamePath = if (useSaf) {
                context.contentResolver.call(document, "grantArchiveFixture", context.packageName, null)
                val flags = Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION
                context.contentResolver.takePersistableUriPermission(document, flags)
                File(root, name!!).inputStream().use { input ->
                    context.contentResolver.openOutputStream(document)!!.use { output -> input.copyTo(output) }
                }
                document.toString()
            } else File(root, name!!).absolutePath
            CoreRuntime.settings.clear()
            CoreRuntime.initialize(context)
            assertTrue(CoreRuntime.updateSetting("Folders", "Bios", root.absolutePath))
            assertTrue(CoreRuntime.updateSetting("Filenames", "BIOS", "bios.bin"))
            assertTrue(CoreRuntime.updateSetting("EmuCoreR", "Renderer", RendererDefaults.VULKAN.toString()))
            instrumentation.runOnMainSync { CoreRuntime.attachSurface(image.surface, 640, 480) }
            assertTrue("Runtime failed to start $name", CoreRuntime.start(gamePath, false))
            var frames = 0
            val deadline = System.nanoTime() + 15_000_000_000L
            while (frames < 20 && System.nanoTime() < deadline) {
                image.acquireLatestImage()?.use { frames++ }
                Thread.sleep(10)
            }
            assertTrue("No frames from archive $name: $frames", frames >= 20)
        } finally {
            CoreRuntime.shutdown()
            instrumentation.runOnMainSync { CoreRuntime.detachSurface() }
            image.close()
            if (useSaf) {
                runCatching { context.contentResolver.call(document, "clearArchiveFixture", context.packageName, null) }
                runCatching { context.contentResolver.releasePersistableUriPermission(document, Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION) }
            }
            CoreRuntime.settings.clear()
            CoreRuntime.settings.putAll(previous)
        }
    }

}
