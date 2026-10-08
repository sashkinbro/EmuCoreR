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

    private fun runReloadProbe(renderer: Int, preload: Boolean, sampleTexture: Boolean = false) {
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
            val bitmap = Bitmap.createBitmap(32, 32, Bitmap.Config.ARGB_8888)
            try {
                bitmap.eraseColor(color)
                image.outputStream().use { assertTrue(bitmap.compress(Bitmap.CompressFormat.PNG, 100, it)) }
            } finally { bitmap.recycle() }
        }
        writeImage(Color.GREEN)
        val bios = File(system, "texture.bin").apply { writeBytes(makeBios(sampleTexture)) }
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
                        val counts = IntArray(3)
                        repeat(30) {
                            bridge.runFrame(session)
                            val deadline = System.nanoTime() + 100_000_000
                            while (System.nanoTime() < deadline) {
                                val frame = reader.acquireLatestImage() ?: continue
                                frame.use {
                                    val plane = it.planes[0]
                                    count = 0
                                    counts.fill(0)
                                    for (y in 0 until it.height) for (x in 0 until it.width) {
                                        val offset = y * plane.rowStride + x * plane.pixelStride
                                        val r = plane.buffer.get(offset).toInt() and 255
                                        val g = plane.buffer.get(offset + 1).toInt() and 255
                                        val b = plane.buffer.get(offset + 2).toInt() and 255
                                        if (r > 220 && g < 20 && b < 20) counts[0]++
                                        if (g > 220 && r < 20 && b < 20) counts[1]++
                                        if (b > 220 && r < 20 && g < 20) counts[2]++
                                    }
                                    count = counts[channel]
                                }
                                break
                            }
                            if (count >= 64) return
                        }
                        fail("$stage: ${RendererDefaults.coreRendererName(renderer)} preload=$preload expected channel=$channel, RGB counts=${counts.toList()}")
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

    private fun makeBios(sampleTexture: Boolean): ByteArray {
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
        for (command in listOf(0xe3000000.toInt(), 0xe407ffff.toInt(), 0xe5000000.toInt(),
            0xe1000108.toInt(), 0x02000000, 0, (240 shl 16) or 320)) write(0, command)
        val loop = out.position()
        val uploadPosition = if (sampleTexture) 512 else (80 shl 16) or 80
        for (command in listOf(0xa0000000.toInt(), uploadPosition, (16 shl 16) or 16)) write(0, command)
        repeat(128) { write(0, 0x001f001f) }
        if (sampleTexture) {
            for (command in listOf(0x01000000, 0x65808080, (80 shl 16) or 80, 0, (16 shl 16) or 16)) write(0, command)
        }
        emit(0x08000000 or (((0xbfc00000.toInt() + loop) ushr 2) and 0x03ffffff))
        emit(0)
        return out.array()
    }
}
