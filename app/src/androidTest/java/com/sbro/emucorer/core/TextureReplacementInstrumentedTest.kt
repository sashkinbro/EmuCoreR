// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: GPL-3.0-or-later
package com.sbro.emucorer.core

import android.graphics.Bitmap
import android.graphics.Color
import android.graphics.PixelFormat
import android.media.ImageReader
import android.os.Build
import android.util.Log
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class TextureReplacementInstrumentedTest {
    @Test
    fun replacementsWrapAcrossVramEdges() {
        for (renderer in listOf(RendererDefaults.CORE_VULKAN, RendererDefaults.CORE_OPENGL)) {
            for (samples in listOf("1", "2", "4", "4-ssaa")) for (preload in listOf(false, true)) {
                for (wrap in 1..3) runReloadProbe(renderer, preload, multisamples = samples, wrappedUpload = wrap)
            }
        }
    }

    @Test
    fun runaheadRetainsPendingTextureReplacements() {
        for (renderer in listOf(RendererDefaults.CORE_VULKAN, RendererDefaults.CORE_OPENGL)) {
            runPendingWriteProbe(renderer, 7, runaheadFrames = 2, slowDecode = true)
        }
    }

    @Test
    fun delayedReplacementsRespectNewerVramWrites() {
        for (renderer in listOf(RendererDefaults.CORE_VULKAN, RendererDefaults.CORE_OPENGL)) {
            for (mutation in 0..15) runPendingWriteProbe(renderer, mutation)
        }
    }

    @Test
    fun multisampledTexturesKeepVramReplacements() {
        for (renderer in listOf(RendererDefaults.CORE_OPENGL, RendererDefaults.CORE_VULKAN)) {
            for (samples in listOf("2", "4", "4-ssaa")) {
                for (preload in listOf(false, true)) for (sampleTexture in listOf(false, true)) {
                    runReloadProbe(renderer, preload, sampleTexture, multisamples = samples, splitReplacement = true)
                }
            }
        }
    }

    @Test
    fun reloadingAnUpdatedPackReplacesCachedImages() {
        for (preload in listOf(false, true)) {
            for (renderer in listOf(RendererDefaults.CORE_VULKAN, RendererDefaults.CORE_OPENGL)) {
                runReloadProbe(renderer, preload)
            }
        }
    }

    @Test
    fun texturedDrawsPreserveVramReplacements() {
        for (preload in listOf(false, true)) {
            for (renderer in listOf(RendererDefaults.CORE_VULKAN, RendererDefaults.CORE_OPENGL)) {
                runReloadProbe(renderer, preload, sampleTexture = true)
            }
        }
    }

    private fun runReloadProbe(renderer: Int, preload: Boolean, sampleTexture: Boolean = false, multisamples: String = "1",
                               splitReplacement: Boolean = false, wrappedUpload: Int = 0) {
        assertEquals("CPH2747", Build.MODEL)
        assertEquals("OnePlus", Build.MANUFACTURER)
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val root = File(context.cacheDir, "texture-reload-${System.nanoTime()}").apply { mkdirs() }
        val system = File(root, "system").apply { mkdirs() }
        val save = File(root, "save").apply { mkdirs() }
        val assets = File(root, "assets").apply { mkdirs() }
        val textures = File(root, "textures").apply { mkdirs() }
        // XXH3-128 of a 16x16 native BGR555 red upload (512 bytes).
        val image = File(textures, "vram-write-5bf538dc6e289176a1e739cd1e037dd4.png")
        fun writeImage(color: Int) {
            val size = if (splitReplacement && color == Color.BLUE) 16 else 32
            val imageWidth = if (wrappedUpload != 0) if (color == Color.GREEN) 37 else 25 else size
            val imageHeight = if (wrappedUpload != 0) if (color == Color.GREEN) 29 else 19 else size
            val bitmap = Bitmap.createBitmap(imageWidth, imageHeight, Bitmap.Config.ARGB_8888)
            try {
                bitmap.eraseColor(color)
                if (splitReplacement) for (y in size / 2 until size) for (x in 0 until size) {
                    bitmap.setPixel(x, y, if (color == Color.GREEN) Color.BLUE else Color.GREEN)
                }
                if (wrappedUpload != 0) for (y in 0 until imageHeight) for (x in 0 until imageWidth) {
                    bitmap.setPixel(x, y, when {
                        y >= imageHeight / 2 -> if (x < imageWidth / 2) Color.YELLOW else Color.MAGENTA
                        x >= imageWidth / 2 -> if (color == Color.GREEN) Color.BLUE else Color.GREEN
                        else -> color
                    })
                }
                image.outputStream().use { assertTrue(bitmap.compress(Bitmap.CompressFormat.PNG, 100, it)) }
            } finally { bitmap.recycle() }
        }
        writeImage(Color.GREEN)
        val bios = File(system, "texture.bin").apply { writeBytes(makeBios(sampleTexture, wrappedUpload = wrappedUpload)) }
        val bridge = NativeCoreBridge()
        try {
            bridge.nativeInit(system.absolutePath, save.absolutePath, assets.absolutePath)
            bridge.nativeSetShaderEffect(0)
            bridge.nativeSetShaderPreset("", false)
            bridge.setTextureReplacementsPathOverride(textures.absolutePath)
            mapOf(
                "CPU_ExecutionMode" to "Recompiler", "GPU_Renderer" to RendererDefaults.coreRendererName(renderer),
                "GPU_PGXPEnable" to (wrappedUpload != 0).toString(), "GPU_ResolutionScale" to "2", "GPU_UseThread" to "false",
                "GPU_TextureFilter" to "Nearest", "GPU_MSAA" to multisamples, "GPU_WidescreenHack" to "false",
                "GPU_DitheringMode" to "Unscaled", "Display_AspectRatio" to "4:3", "Display_CropMode" to "Borders",
                "MemoryCards_Card1Type" to "None", "MemoryCards_Card2Type" to "None",
                "Main_RunaheadFrameCount" to "0", "Main_ApplyGameSettings" to "false",
                "BIOS_PatchFastBoot" to "false", "Console_Region" to "NTSC-U",
                "TextureReplacements_EnableVRAMWriteReplacements" to "false",
                "TextureReplacements_PreloadTextures" to preload.toString()
            ).forEach { (key, value) -> bridge.nativeSetOption("swanstation_$key", value) }
            val session = bridge.createSession()
            assertTrue(session != 0L)
            try {
                ImageReader.newInstance(320, 240, PixelFormat.RGBA_8888, 3).use { reader ->
                    assertEquals(0, bridge.setSurface(session, reader.surface, renderer))
                    assertEquals(0, bridge.loadBios(session, bios.absolutePath))
                    assertEquals(0, bridge.loadBiosOnly(session))
                    fun awaitColor(channel: Int, stage: String) {
                        var count = 0
                        val counts = IntArray(5)
                        val rowSums = IntArray(5)
                        val columnSums = IntArray(5)
                        repeat(30) {
                            bridge.runFrame(session)
                            val deadline = System.nanoTime() + 100_000_000
                            while (System.nanoTime() < deadline) {
                                val frame = reader.acquireLatestImage() ?: continue
                                frame.use {
                                    val plane = it.planes[0]
                                    count = 0
                                    counts.fill(0)
                                    rowSums.fill(0)
                                    columnSums.fill(0)
                                    // Wrapped source texels may also be visible at the
                                    // display origin; inspect only the copied destination.
                                    val pixelRows = if (wrappedUpload != 0) 64 until 112 else 0 until it.height
                                    val pixelColumns = if (wrappedUpload != 0) 64 until 112 else 0 until it.width
                                    for (y in pixelRows) for (x in pixelColumns) {
                                        val offset = y * plane.rowStride + x * plane.pixelStride
                                        val r = plane.buffer.get(offset).toInt() and 255
                                        val g = plane.buffer.get(offset + 1).toInt() and 255
                                        val b = plane.buffer.get(offset + 2).toInt() and 255
                                        val color = when {
                                            r > 220 && g < 20 && b < 20 -> 0
                                            g > 220 && r < 20 && b < 20 -> 1
                                            b > 220 && r < 20 && g < 20 -> 2
                                            r > 220 && g > 220 && b < 20 -> 3
                                            r > 220 && b > 220 && g < 20 -> 4
                                            else -> -1
                                        }
                                        if (color >= 0) { counts[color]++; rowSums[color] += y; columnSums[color] += x }
                                    }
                                    count = counts[channel]
                                }
                                break
                            }
                            val other = if (channel == 1) 2 else 1
                            if (wrappedUpload != 0) {
                                fun before(sums: IntArray, a: Int, b: Int) =
                                    sums[a].toLong() * counts[b] < sums[b].toLong() * counts[a]
                                if (channel == 0 && count >= 192) return
                                if (channel != 0 && counts[0] == 0 && (1..4).all { counts[it] >= 36 } &&
                                    before(columnSums, channel, other) && before(columnSums, 3, 4) &&
                                    before(rowSums, channel, 3) && before(rowSums, other, 4)) return
                                return@repeat
                            }
                            val oriented = !splitReplacement || channel == 0 || (counts[other] >= 64 &&
                                rowSums[channel].toLong() * counts[other] < rowSums[other].toLong() * counts[channel])
                            if (count >= 64 && oriented) return
                        }
                        fail("$stage: ${RendererDefaults.coreRendererName(renderer)} preload=$preload MSAA=$multisamples wrap=$wrappedUpload expected channel=$channel, RGB counts=${counts.toList()}")
                    }
                    awaitColor(0, "native texture")
                    bridge.nativeSetOption("swanstation_TextureReplacements_EnableVRAMWriteReplacements", "true")
                    awaitColor(1, "initial replacement")
                    writeImage(Color.BLUE)
                    bridge.nativeSetOption("swanstation_TextureReplacements_PreloadTextures", (!preload).toString())
                    awaitColor(2, "updated replacement")
                    bridge.nativeSetOption("swanstation_TextureReplacements_EnableVRAMWriteReplacements", "false")
                    awaitColor(0, "replacement disabled")
                    Log.i("TextureReplacementProbe", "PASS ${RendererDefaults.coreRendererName(renderer)} preload=$preload sampled=$sampleTexture reload")
                }
            } finally { bridge.destroySession(session) }
        } finally {
            bridge.setTextureReplacementsPathOverride(null)
            root.deleteRecursively()
        }
    }

    private fun runPendingWriteProbe(renderer: Int, mutation: Int, runaheadFrames: Int = 0, slowDecode: Boolean = false) {
        assertEquals("CPH2747", Build.MODEL)
        assertEquals("OnePlus", Build.MANUFACTURER)
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val root = File(context.cacheDir, "texture-pending-${System.nanoTime()}").apply { mkdirs() }
        val system = File(root, "system").apply { mkdirs() }
        val save = File(root, "save").apply { mkdirs() }
        val assets = File(root, "assets").apply { mkdirs() }
        val textures = File(root, "textures").apply { mkdirs() }
        val imageSize = if (slowDecode) 2048 else 32
        val bitmap = Bitmap.createBitmap(imageSize, imageSize, Bitmap.Config.ARGB_8888)
        try {
            bitmap.eraseColor(Color.GREEN)
            if (slowDecode) {
                var seed = 0x12345678
                val pixels = IntArray(imageSize * imageSize) {
                    seed = seed xor (seed shl 13)
                    seed = seed xor (seed ushr 17)
                    seed = seed xor (seed shl 5)
                    Color.rgb(seed and 15, 240 + ((seed ushr 8) and 15), (seed ushr 16) and 15)
                }
                bitmap.setPixels(pixels, 0, imageSize, 0, 0, imageSize, imageSize)
            }
            File(textures, "vram-write-5bf538dc6e289176a1e739cd1e037dd4.png").outputStream().use {
                assertTrue(bitmap.compress(Bitmap.CompressFormat.PNG, 100, it))
            }
        } finally { bitmap.recycle() }
        val bios = File(system, "pending.bin").apply { writeBytes(makeBios(false, mutation)) }
        val bridge = NativeCoreBridge()
        try {
            bridge.nativeInit(system.absolutePath, save.absolutePath, assets.absolutePath)
            bridge.nativeSetShaderEffect(0)
            bridge.nativeSetShaderPreset("", false)
            bridge.setTextureReplacementsPathOverride(textures.absolutePath)
            mapOf(
                "CPU_ExecutionMode" to "Recompiler", "GPU_Renderer" to RendererDefaults.coreRendererName(renderer),
                "GPU_PGXPEnable" to "false", "GPU_ResolutionScale" to "2", "GPU_UseThread" to "false",
                "GPU_TextureFilter" to "Nearest", "GPU_MSAA" to "1", "GPU_WidescreenHack" to "false",
                "GPU_DitheringMode" to "Unscaled", "Display_AspectRatio" to "4:3", "Display_CropMode" to "Borders",
                "MemoryCards_Card1Type" to "None", "MemoryCards_Card2Type" to "None",
                "Main_RunaheadFrameCount" to runaheadFrames.toString(), "Main_ApplyGameSettings" to "false",
                "BIOS_PatchFastBoot" to "false", "Console_Region" to "NTSC-U",
                "TextureReplacements_EnableVRAMWriteReplacements" to "false",
                "TextureReplacements_PreloadTextures" to "false"
            ).forEach { (key, value) -> bridge.nativeSetOption("swanstation_$key", value) }
            val session = bridge.createSession()
            assertTrue(session != 0L)
            try {
                ImageReader.newInstance(320, 240, PixelFormat.RGBA_8888, 3).use { reader ->
                    assertEquals(0, bridge.setSurface(session, reader.surface, renderer))
                    assertEquals(0, bridge.loadBios(session, bios.absolutePath))
                    assertEquals(0, bridge.loadBiosOnly(session))
                    bridge.nativeSetOption("swanstation_TextureReplacements_EnableVRAMWriteReplacements", "true")
                    val counts = IntArray(3)
                    repeat(if (slowDecode) 50 else 20) { index ->
                        if (runaheadFrames > 0) bridge.setPadButtons(session, 0, if (index % 2 == 0) 0xffff else 0xbfff)
                        bridge.runFrame(session)
                        val deadline = System.nanoTime() + 100_000_000
                        while (System.nanoTime() < deadline) {
                            val frame = reader.acquireLatestImage() ?: continue
                            frame.use {
                                counts.fill(0)
                                val plane = it.planes[0]
                                for (y in 0 until it.height) for (x in 0 until it.width) {
                                    val offset = y * plane.rowStride + x * plane.pixelStride
                                    val r = plane.buffer.get(offset).toInt() and 255
                                    val g = plane.buffer.get(offset + 1).toInt() and 255
                                    val b = plane.buffer.get(offset + 2).toInt() and 255
                                    if (r > 220 && g < 20 && b < 20) counts[0]++
                                    if (g > 220 && r < 20 && b < 20) counts[1]++
                                    if (b > 220 && r < 20 && g < 20) counts[2]++
                                }
                            }
                            break
                        }
                    }
                    if (mutation == 7 || mutation == 14) {
                        assertTrue("unrelated write must keep replacement renderer=$renderer RGB=${counts.toList()}", counts[1] >= 64)
                    } else {
                        assertTrue("newer write must survive renderer=$renderer mutation=$mutation RGB=${counts.toList()}",
                            (mutation == 15 || counts[0] >= 64) && counts[1] == 0 && counts[2] >= 64)
                    }
                }
            } finally { bridge.destroySession(session) }
        } finally {
            bridge.setTextureReplacementsPathOverride(null)
            root.deleteRecursively()
        }
    }

    private fun makeBios(sampleTexture: Boolean, mutation: Int = -1, wrappedUpload: Int = 0): ByteArray {
        val out = ByteBuffer.allocate(512 * 1024).order(ByteOrder.LITTLE_ENDIAN)
        fun emit(value: Int) { out.putInt(value) }
        fun imm(op: Int, source: Int, target: Int, value: Int) =
            (op shl 26) or (source shl 21) or (target shl 16) or (value and 65535)
        fun constant(register: Int, value: Int) {
            emit(imm(0xf, 0, register, value ushr 16))
            emit(imm(0xd, register, register, value))
        }
        fun write(offset: Int, value: Int) {
            constant(8, value)
            emit(imm(0x2b, 10, 8, offset))
        }
        constant(10, 0x1f801810)
        for (command in listOf(0, 0x08000001, 0x05000000, 0x06c60260, 0x07042018, 0x03000000)) write(4, command)
        val corner = mutation == 13
        val sampledPending = mutation in 10..13
        for (command in listOf(0xe3000000.toInt(), 0xe407ffff.toInt(), 0xe5000000.toInt(),
            if (corner) 0xe1000100.toInt() else 0xe1000108.toInt(), 0x02000000, 0, (240 shl 16) or 320)) write(0, command)
        val loop = out.position()
        val wrapped = mutation in listOf(6, 8, 9)
        val uploadPosition = when {
            wrappedUpload != 0 -> ((if (wrappedUpload and 2 != 0) 508 else 300) shl 16) or
                if (wrappedUpload and 1 != 0) 1020 else 512
            corner -> 0
            sampleTexture || sampledPending -> 512
            else -> (80 shl 16) or if (wrapped) 0 else 80
        }
        for (command in listOf(0xa0000000.toInt(), uploadPosition, (16 shl 16) or 16)) write(0, command)
        repeat(128) { write(0, 0x001f001f) }
        if (wrappedUpload != 0) {
            for (command in listOf(0x80000000.toInt(), uploadPosition, (80 shl 16) or 80, (16 shl 16) or 16))
                write(0, command)
        }
        if (mutation >= 0) {
            if (mutation == 4 || mutation == 5) write(0, 0xe6000002.toInt())
            when (mutation) {
                0, 4, 6 -> {
                    for (command in listOf(0xa0000000.toInt(), (88 shl 16) or if (wrapped) 1020 else 88,
                        (8 shl 16) or if (wrapped) 12 else 8)) write(0, command)
                    repeat(if (wrapped) 48 else 32) { write(0, 0x7c007c00) }
                }
                1, 8 -> for (command in listOf(0x02ff0000, (88 shl 16) or if (wrapped) 1008 else 80,
                    (8 shl 16) or if (wrapped) 32 else 16)) write(0, command)
                2, 5, 9 -> {
                    for (command in listOf(0x02ff0000, (16 shl 16) or 16, (16 shl 16) or 16,
                        0x80000000.toInt(), (16 shl 16) or 16, (88 shl 16) or if (wrapped) 1020 else 88,
                        (8 shl 16) or if (wrapped) 12 else 8)) write(0, command)
                }
                3 -> for (command in listOf(0x60ff0000, (88 shl 16) or 88, (8 shl 16) or 8)) write(0, command)
                7 -> for (command in listOf(0x02ff0000, (16 shl 16) or 16, (16 shl 16) or 16)) write(0, command)
                10, 13 -> {
                    for (command in listOf(0xa0000000.toInt(), (508 shl 16) or if (corner) 1020 else 520,
                        (12 shl 16) or if (corner) 12 else 8)) write(0, command)
                    repeat(if (corner) 72 else 48) { write(0, 0x7c007c00) }
                }
                11 -> for (command in listOf(0x02ff0000, (508 shl 16) or 512, (12 shl 16) or 16)) write(0, command)
                12 -> for (command in listOf(0x02ff0000, (16 shl 16) or 16, (16 shl 16) or 16,
                    0x80000000.toInt(), (16 shl 16) or 16, (508 shl 16) or 520, (12 shl 16) or 8)) write(0, command)
                14, 15 -> {
                    for (command in listOf(0xa0000000.toInt(), (80 shl 16) or if (mutation == 14) 96 else 80,
                        (16 shl 16) or if (mutation == 14) 8 else 32)) write(0, command)
                    repeat(if (mutation == 14) 64 else 256) { write(0, 0x7c007c00) }
                }
            }
            val idle = out.position()
            if (sampledPending) {
                for (command in listOf(0x01000000, 0x65808080, (80 shl 16) or 80, 0, (16 shl 16) or 16)) write(0, command)
            }
            emit(0x08000000 or (((0xbfc00000.toInt() + idle) ushr 2) and 0x03ffffff))
            emit(0)
            return out.array()
        }
        if (sampleTexture) {
            for (command in listOf(0x01000000, 0x65808080, (80 shl 16) or 80, 0, (16 shl 16) or 16)) write(0, command)
        }
        emit(0x08000000 or (((0xbfc00000.toInt() + loop) ushr 2) and 0x03ffffff))
        emit(0)
        return out.array()
    }
}
