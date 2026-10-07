// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: GPL-3.0-or-later
package com.sbro.emucorer.core

import android.graphics.PixelFormat
import android.media.ImageReader
import android.os.Build
import android.util.Log
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.zip.Inflater
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class PgxpInstrumentedTest {
    @Test
    fun nativeInstructionsAndDepthRenderCorrectlyOnOnePlus() = runProbe(false)

    @Test
    fun texturedPolygonsAndDepthRenderCorrectlyOnOnePlus() = runProbe(true)

    private fun runProbe(textured: Boolean) {
        assertEquals("OnePlus", Build.MANUFACTURER)
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val root = File(context.cacheDir, "pgxp-probe-${System.nanoTime()}").apply { mkdirs() }
        val system = File(root, "system").apply { mkdirs() }
        val save = File(root, "save").apply { mkdirs() }
        val assets = File(root, "assets").apply { mkdirs() }
        val bios = File(system, "pgxp-probe.bin").apply { writeBytes(makeBios(textured)) }
        val bridge = NativeCoreBridge()
        try {
            for (cpu in listOf("Interpreter", "Recompiler")) {
                for (renderer in listOf(RendererDefaults.CORE_VULKAN, RendererDefaults.CORE_OPENGL)) {
                    for (depth in listOf(false, true)) {
                        bridge.nativeInit(system.absolutePath, save.absolutePath, assets.absolutePath)
                        val options = mapOf(
                            "GPU_Renderer" to if (renderer == RendererDefaults.CORE_VULKAN) "Vulkan" else "OpenGL",
                            "CPU_ExecutionMode" to cpu,
                            "GPU_PGXPEnable" to "true", "GPU_PGXPCPU" to "true",
                            "GPU_PGXPPreserveProjFP" to "true", "GPU_PGXPVertexCache" to "false",
                            "GPU_PGXPCulling" to "true", "GPU_PGXPTextureCorrection" to textured.toString(),
                            "GPU_PGXPColorCorrection" to "false", "GPU_PGXPDepthBuffer" to depth.toString(),
                            "GPU_PGXPDisableOn2DPolygons" to "true", "GPU_PGXPTransparentDepthTest" to "false",
                            "GPU_PGXPDepthClearThreshold" to "4096", "GPU_ResolutionScale" to "2",
                            "GPU_TextureFilter" to "Nearest", "GPU_UseThread" to "false",
                            "GPU_Multisamples" to "1", "GPU_WidescreenHack" to "false",
                            "Display_AspectRatio" to "4:3", "Display_CropMode" to "Borders",
                            "MemoryCards_Card1Type" to "None", "MemoryCards_Card2Type" to "None",
                            "BIOS_PatchFastBoot" to "false", "Console_Region" to "NTSC-U",
                            "Main_ApplyGameSettings" to "false", "Main_SaveStateCompression" to "DeflateLow",
                            "Console_Enable8MBRAM" to (cpu == "Recompiler" && depth).toString()
                        )
                        options.forEach { (key, value) -> bridge.nativeSetOption("swanstation_$key", value) }
                        val session = bridge.createSession()
                        assertTrue(session != 0L)
                        ImageReader.newInstance(320, 240, PixelFormat.RGBA_8888, 3).use { reader ->
                            try {
                                assertEquals(0, bridge.setSurface(session, reader.surface, renderer))
                                assertEquals(0, bridge.loadBios(session, bios.absolutePath))
                                assertEquals(0, bridge.loadBiosOnly(session))
                                var pixel: IntArray? = null
                                repeat(8) {
                                    bridge.runFrame(session)
                                    pixel = centerPixel(reader) ?: pixel
                                }
                                val label = "$cpu/${RendererDefaults.coreRendererName(renderer)}/depth=$depth/textured=$textured"
                                Log.i("PGXPProbe", "$label display=${bridge.getDisplayRect(session)?.toList()} diagnostics=${bridge.getDiagnostics()}")
                                assertNotNull("no rendered image: $label", pixel)
                                val rgb = pixel!!
                                // Near red is submitted first, then an overlapping far green.
                                // Painter ordering should show green; depth should retain red,
                                // even with perspective-correct textures disabled.
                                assertTrue("incorrect occlusion $label rgb=${rgb.toList()}",
                                    if (depth) rgb[0] > 180 && rgb[1] < 30 else rgb[1] > 180 && rgb[0] < 30)
                                val state = File(root, "probe.rstate")
                                assertEquals("save failed: $label", 0, bridge.saveState(session, state.absolutePath))
                                val ramSize = if (cpu == "Recompiler" && depth) 8 * 1024 * 1024 else 2 * 1024 * 1024
                                val before = precisionMemory(state, ramSize)
                                val sx = 160.25f - 12000f / 301f
                                val sy = 120.25f - 10000f / 301f
                                assertEquals("LWL full word X: $label", sx, before.getFloat(0x2000 / 4 * 20), 0.001f)
                                assertEquals("LWL full word Y: $label", sy, before.getFloat(0x2000 / 4 * 20 + 4), 0.001f)
                                assertEquals("LWR byte offset 2: $label", sy, before.getFloat(0x2004 / 4 * 20), 0.001f)
                                assertEquals("SWL byte offset 1: $label", sy, before.getFloat(0x2008 / 4 * 20), 0.001f)
                                assertEquals("SWR byte offset 2: $label", sx, before.getFloat(0x200c / 4 * 20 + 4), 0.001f)
                                assertEquals("reload failed: $label", 0, bridge.loadState(session, state.absolutePath))
                                assertEquals(0, bridge.saveState(session, state.absolutePath))
                                assertArrayEquals("load lost precision: $label", before.array(), precisionMemory(state, ramSize).array())
                                repeat(3) {
                                    bridge.runFrame(session)
                                    pixel = centerPixel(reader) ?: pixel
                                }
                                val restored = pixel!!
                                assertTrue("reload changed occlusion: $label",
                                    if (depth) restored[0] > 180 && restored[1] < 30 else restored[1] > 180 && restored[0] < 30)
                                Log.i("PGXPProbe", "PASS $label RGB=${rgb.toList()} RAM=$ramSize state=${state.length()}")
                            } finally {
                                bridge.destroySession(session)
                            }
                        }
                    }
                }
            }
        } finally {
            root.deleteRecursively()
        }
    }

    private fun centerPixel(reader: ImageReader): IntArray? {
        val deadline = System.nanoTime() + 100_000_000
        while (System.nanoTime() < deadline) {
            reader.acquireLatestImage()?.use { image ->
                val plane = image.planes[0]
                val index = image.height / 2 * plane.rowStride + image.width / 2 * plane.pixelStride
                return IntArray(3) { plane.buffer.get(index + it).toInt() and 255 }
            }
            Thread.sleep(2)
        }
        return null
    }

    private fun precisionMemory(file: File, ramSize: Int): ByteBuffer {
        val bytes = file.readBytes()
        val header = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN)
        assertEquals(0x43435544, header.getInt(0))
        assertEquals(56, header.getInt(4))
        val size = header.getInt(208)
        assertTrue("persistent precision exceeds the old state bound", size > 11 * 1024 * 1024)
        val offset = header.getInt(212)
        val payload = if (header.getInt(200) == 0) bytes.copyOfRange(offset, offset + size) else {
            val inflater = Inflater()
            try {
                inflater.setInput(bytes, offset, header.getInt(204))
                val result = ByteArray(size)
                var count = 0
                while (count < size) {
                    val read = inflater.inflate(result, count, size - count)
                    assertTrue("compressed state made no progress", read > 0)
                    count += read
                }
                assertTrue(inflater.finished())
                result
            } finally { inflater.end() }
        }
        val marker = byteArrayOf(3, 0, 0, 0, 68, 77, 65) // StateWrapper's DMA marker.
        val dma = (0..payload.size - marker.size).firstOrNull { index ->
            marker.indices.all { payload[index + it] == marker[it] }
        } ?: error("missing DMA marker")
        val metadataSize = (ramSize / 4 + 1024 / 4) * 20
        val start = dma - metadataSize - 130 * 20 - 1
        assertTrue(start >= 0 && payload[start] == 1.toByte())
        return ByteBuffer.wrap(payload.copyOfRange(start + 1 + 130 * 20, dma)).order(ByteOrder.LITTLE_ENDIAN)
    }

    private fun makeBios(textured: Boolean): ByteArray {
        val out = ByteBuffer.allocate(512 * 1024).order(ByteOrder.LITTLE_ENDIAN)
        fun emit(value: Int) { out.putInt(value) }
        fun imm(op: Int, source: Int, target: Int, value: Int) =
            (op shl 26) or (source shl 21) or (target shl 16) or (value and 65535)
        fun constant(register: Int, value: Int) {
            emit(imm(0x0f, 0, register, value ushr 16))
            emit(imm(0x0d, register, register, value))
        }
        fun write(base: Int, offset: Int, value: Int) {
            constant(8, value)
            emit(imm(0x2b, base, 8, offset))
        }
        fun gte(register: Int, value: Int, control: Boolean) {
            constant(8, value)
            emit((if (control) 0x48c00000 else 0x48800000) or (8 shl 16) or (register shl 11))
            emit(0)
            emit(0)
        }

        // This owned ROM enables COP2, draws two projected triangles via GPU
        // DMA, then waits. No retail BIOS, game data or user's card is needed.
        constant(8, 0x40000000)
        emit(0x40886000) // mtc0 t0, Status
        constant(9, 0xa0001000.toInt())
        constant(10, 0x1f801810)
        constant(11, 0x1f8010a0)
        constant(12, 0xa0002000.toInt())
        for (command in listOf(0, 0x08000001, 0x05000000, 0x06c60260, 0x07042018, 0x03000000, 0x04000002))
            write(10, 4, command)
        for (command in listOf(0xe3000000.toInt(), 0xe407ffff.toInt(), 0xe5000000.toInt(),
            0xe1000000.toInt(), 0x02000000, 0, (240 shl 16) or 320)) write(10, 0, command)
        if (textured) {
            for (command in listOf(0xa0000000.toInt(), 512, 0x00100010)) write(10, 0, command)
            repeat(128) { write(10, 0, 0x7fff7fff) }
        }
        gte(0, 4096, true)
        gte(2, 4096, true)
        gte(4, 4096, true)
        gte(24, (160 shl 16) + 16384, true)
        gte(25, (120 shl 16) + 16384, true)
        gte(26, 100, true)
        val vertices = listOf(-120 to -100, 120 to -100, 0 to 120)
        val stride = if (textured) 28 else 16
        for (triangle in 0..1) {
            val color = if (triangle == 0) 0xff else 0xff00
            write(9, triangle * stride, (if (textured) 0x24000000 else 0x20000000) or color)
            for ((index, point) in vertices.withIndex()) {
                val scale = triangle + 1
                gte(0, ((point.second * scale and 65535) shl 16) or (point.first * scale and 65535), false)
                gte(1, 301 * scale, false)
                emit(0x4a080001) // rtps, sf=1
                repeat(16) { emit(0) }
                val position = triangle * stride + 4 + index * (if (textured) 8 else 4)
                emit(imm(0x3a, 9, 14, position))
                if (textured) {
                    val uv = listOf(0, 15 or (0x108 shl 16), 8 or (15 shl 8))[index]
                    write(9, position + 4, uv)
                }
            }
        }
        for (offset in 0..12 step 4) emit(imm(0x2b, 12, 0, offset))
        constant(8, 0)
        emit(imm(0x22, 9, 8, 7)) // full LWL of first projected vertex
        emit(0)
        emit(imm(0x2b, 12, 8, 0))
        constant(8, 0)
        emit(imm(0x26, 9, 8, 6)) // LWR of its upper half
        emit(0)
        emit(imm(0x2b, 12, 8, 4))
        emit(imm(0x23, 9, 8, 4))
        emit(0)
        emit(imm(0x2a, 12, 8, 9)) // SWL: Y into low half
        emit(imm(0x2e, 12, 8, 14)) // SWR: X into high half
        write(11, 0x50, 0x800) // enable DMA channel 2
        write(11, 0, 0x1000)
        write(11, 4, if (textured) 14 else 8)
        write(11, 8, 0x11000001)
        emit(0x1000ffff) // stationary branch after the probe
        emit(0)
        return out.array()
    }
}
