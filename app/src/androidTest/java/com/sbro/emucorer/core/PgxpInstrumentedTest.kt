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
    fun nativeInstructionsAndDepthRenderCorrectly() = runProbe(false)

    @Test
    fun texturedPolygonsAndDepthRenderCorrectly() = runProbe(true)

    @Test
    fun runaheadPreservesPrecisionAcrossInputChanges() = runProbe(false, true)

    private fun runProbe(textured: Boolean, runahead: Boolean = false) {
        val deviceModel = InstrumentationRegistry.getArguments().getString("pgxpDeviceModel", "CPH2747")
        assertTrue("unsupported PGXP test device", deviceModel in listOf("CPH2747", "RG556"))
        assertEquals(deviceModel, Build.MODEL)
        assertEquals(if (deviceModel == "RG556") "Anbernic" else "OnePlus", Build.MANUFACTURER)
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
                            "Main_RunaheadFrameCount" to if (runahead) "2" else "0",
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
                                val label = "$cpu/${RendererDefaults.coreRendererName(renderer)}/depth=$depth/textured=$textured/runahead=$runahead"
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
                                assertEquals("redundant ORI: $label", sx, before.getFloat(0x2010 / 4 * 20), 0.001f)
                                for ((address, value) in listOf(0x2014 to 0f, 0x2018 to 1f, 0x201c to 0f, 0x2020 to 0f)) {
                                    val entry = address / 4 * 20
                                    assertEquals("constant result at $address: $label", value, before.getFloat(entry), 0f)
                                    assertEquals("constant borrowed a depth at $address: $label", 0, before.getInt(entry + 12) and 0x10000)
                                }
                                for (address in listOf(0x2028, 0x202c)) {
                                    val entry = address / 4 * 20
                                    assertEquals("zero-valued vertex X at $address: $label", 0.25f, before.getFloat(entry), 0.001f)
                                    assertEquals("zero-valued vertex Y at $address: $label", 0.5f, before.getFloat(entry + 4), 0.001f)
                                }
                                for ((address, expected) in listOf(
                                    0x2030 to (0.5f to 1f), 0x2034 to (0.125f to 0.25f),
                                    0x2038 to (0.5f to 0f), 0x203c to (0f to 0.25f),
                                    0x2040 to (0.5f to 1f), 0x2044 to (0.125f to 0.25f),
                                    0x2048 to (0.125f to 0.25f)
                                )) {
                                    val entry = address / 4 * 20
                                    assertEquals("shift X at $address: $label", expected.first, before.getFloat(entry), 0.001f)
                                    assertEquals("shift Y at $address: $label", expected.second, before.getFloat(entry + 4), 0.001f)
                                    assertEquals("shift lost component validity at $address: $label", 0x101, before.getInt(entry + 12) and 0x101)
                                }
                                val shiftedX = sx / 2f + ((sy.toInt() and 1) shl 15)
                                val wrappedX = if (shiftedX >= 32768f) shiftedX - 65536f else shiftedX
                                assertEquals("right shift used fractional carry: $label", wrappedX, before.getFloat(0x2060 / 4 * 20), 0.001f)
                                assertEquals("right shift Y: $label", sy / 2f, before.getFloat(0x2060 / 4 * 20 + 4), 0.001f)
                                val original = 0x2064 / 4 * 20
                                for (address in listOf(0x2074, 0x2078)) {
                                    val entry = address / 4 * 20
                                    assertEquals("HI/LO transfer X at $address: $label", sx, before.getFloat(entry), 0.001f)
                                    assertEquals("HI/LO transfer Y at $address: $label", sy, before.getFloat(entry + 4), 0.001f)
                                    assertEquals("HI/LO transfer lost validity at $address: $label", 0x101, before.getInt(entry + 12) and 0x101)
                                }
                                for ((address, y) in listOf(0x2080 to 32758f, 0x2084 to -10f)) {
                                    val entry = address / 4 * 20
                                    assertEquals("division quotient X at $address: $label", 5.125f, before.getFloat(entry), 0.001f)
                                    assertEquals("division quotient Y at $address: $label", y, before.getFloat(entry + 4), 0.001f)
                                    assertEquals("division lost validity at $address: $label", 0x101, before.getInt(entry + 12) and 0x101)
                                }
                                for (address in listOf(0x2088, 0x208c)) {
                                    val entry = address / 4 * 20
                                    assertEquals("division identity X at $address: $label", sx, before.getFloat(entry), 0.001f)
                                    assertEquals("division identity Y at $address: $label", sy, before.getFloat(entry + 4), 0.001f)
                                    assertEquals("division identity lost validity at $address: $label", 0x101, before.getInt(entry + 12) and 0x101)
                                }
                                for ((address, expected) in listOf(
                                    0x2090 to (0f to 0f), 0x2094 to (0f to 0f),
                                    0x2098 to (1f to 0f), 0x209c to (-1f to -1f),
                                    0x20a8 to (0f to -32768f), 0x20ac to (0f to 0f),
                                    0x20b0 to (1f to 0f), 0x20b4 to (1f to 0f)
                                )) {
                                    val entry = address / 4 * 20
                                    assertEquals("integer division X at $address: $label", expected.first, before.getFloat(entry), 0f)
                                    assertEquals("integer division Y at $address: $label", expected.second, before.getFloat(entry + 4), 0f)
                                    assertEquals("integer division borrowed depth at $address: $label", 0, before.getInt(entry + 12) and 0x10000)
                                }
                                for (address in listOf(0x20a0, 0x20a4)) {
                                    val entry = address / 4 * 20
                                    assertEquals("zero divisor remainder X: $label", 10.25f, before.getFloat(entry), 0f)
                                    assertEquals("zero divisor remainder Y: $label", -20f, before.getFloat(entry + 4), 0f)
                                    assertEquals("zero divisor lost the dividend: $label", 0x101, before.getInt(entry + 12) and 0x101)
                                }
                                assertEquals("variable shift invalidated its source: $label", 0x101, before.getInt(original + 12) and 0x101)
                                assertEquals("variable shift changed source X: $label", sx, before.getFloat(original), 0.001f)
                                assertEquals("variable shift changed source Y: $label", sy, before.getFloat(original + 4), 0.001f)
                                val shift = sx.toInt() and 31
                                assertTrue("probe expects a shift beyond one halfword", shift > 16)
                                for ((address, expected) in listOf(
                                    0x2068 to (0f to sx * (1 shl (shift - 16))),
                                    0x206c to (sy / (1 shl (shift - 16)) to 0f),
                                    0x2070 to (sy / (1 shl (shift - 16)) to 0f)
                                )) {
                                    val entry = address / 4 * 20
                                    assertEquals("aliased variable shift X at $address: $label", expected.first, before.getFloat(entry), 0.001f)
                                    assertEquals("aliased variable shift Y at $address: $label", expected.second, before.getFloat(entry + 4), 0.001f)
                                    assertEquals("aliased variable shift lost precision at $address: $label", 0x101, before.getInt(entry + 12) and 0x101)
                                }
                                if (runahead) {
                                    for (buttons in listOf(0xbfff, 0xffff)) {
                                        bridge.setPadButtons(session, 0, buttons)
                                        repeat(4) {
                                            bridge.runFrame(session)
                                            pixel = centerPixel(reader) ?: pixel
                                        }
                                        assertEquals("runahead save failed: $label", 0, bridge.saveState(session, state.absolutePath))
                                        assertArrayEquals("input rollback lost precision: $label buttons=$buttons",
                                            before.array(), precisionMemory(state, ramSize).array())
                                    }
                                }
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
        emit((8 shl 16) or (14 shl 11) or (1 shl 6) or 0x02) // srl
        emit(imm(0x2b, 12, 14, 0x60))
        for ((function, address) in listOf(0x04 to 0x68, 0x06 to 0x6c, 0x07 to 0x70)) {
            emit((8 shl 21) or (8 shl 16) or (14 shl 11) or function)
            emit(imm(0x2b, 12, 14, address))
        }
        emit(imm(0x2b, 12, 8, 0x64))
        emit((8 shl 21) or 0x11) // mthi
        emit((14 shl 11) or 0x10) // mfhi
        emit(imm(0x2b, 12, 14, 0x74))
        emit((8 shl 21) or 0x13) // mtlo
        emit((14 shl 11) or 0x12) // mflo
        emit(imm(0x2b, 12, 14, 0x78))
        constant(13, 1)
        for ((function, quotient, remainder) in listOf(Triple(0x1a, 0x88, 0x90), Triple(0x1b, 0x8c, 0x94))) {
            emit((8 shl 21) or (13 shl 16) or function)
            repeat(16) { emit(0) }
            emit((14 shl 11) or 0x12)
            emit(imm(0x2b, 12, 14, quotient))
            emit((14 shl 11) or 0x10)
            emit(imm(0x2b, 12, 14, remainder))
        }
        emit(imm(0x2a, 12, 8, 9)) // SWL: Y into low half
        emit(imm(0x2e, 12, 8, 14)) // SWR: X into high half
        emit(imm(0x0d, 8, 14, 8)) // X already contains this bit.
        emit(imm(0x2b, 12, 14, 0x10))
        emit(imm(0x0a, 8, 14, 100))
        emit(imm(0x2b, 12, 14, 0x14))
        emit(imm(0x0b, 8, 14, 0xffff))
        emit(imm(0x2b, 12, 14, 0x18))
        constant(13, 65535)
        emit((8 shl 21) or (13 shl 16) or (14 shl 11) or 0x2a) // slt
        emit(imm(0x2b, 12, 14, 0x1c))
        emit(imm(0x0c, 8, 14, 0))
        emit(imm(0x2b, 12, 14, 0x20))
        gte(24, 16384, true)
        gte(25, 32768, true)
        gte(0, 0, false)
        gte(1, 1000, false)
        emit(0x4a080001)
        repeat(16) { emit(0) }
        emit(0x48000000 or (8 shl 16) or (14 shl 11)) // mfc2
        emit(0)
        emit(imm(0x2b, 12, 0, 0x50))
        emit(imm(0x23, 12, 13, 0x50)) // runtime zero operand
        emit(0)
        emit((8 shl 21) or (13 shl 16) or (14 shl 11) or 0x25) // or
        emit(imm(0x2b, 12, 14, 0x28))
        emit((8 shl 21) or (13 shl 16) or (15 shl 11) or 0x26) // xor
        emit(imm(0x2b, 12, 15, 0x2c))
        for ((function, shift, address) in listOf(
            Triple(0x00, 1, 0x30), Triple(0x02, 1, 0x34),
            Triple(0x03, 16, 0x38), Triple(0x00, 16, 0x3c)
        )) {
            emit((8 shl 16) or (14 shl 11) or (shift shl 6) or function)
            emit(imm(0x2b, 12, 14, address))
        }
        constant(13, 33)
        emit(imm(0x2b, 12, 13, 0x54))
        emit(imm(0x23, 12, 13, 0x54))
        emit(0)
        for ((function, address) in listOf(0x04 to 0x40, 0x06 to 0x44, 0x07 to 0x48)) {
            emit((13 shl 21) or (8 shl 16) or (14 shl 11) or function)
            emit(imm(0x2b, 12, 14, address))
        }
        gte(24, (10 shl 16) + 16384, true)
        gte(25, -20 shl 16, true)
        gte(0, 0, false)
        gte(1, 1000, false)
        emit(0x4a080001)
        repeat(16) { emit(0) }
        emit(0x48000000 or (8 shl 16) or (14 shl 11))
        emit(0)
        constant(13, 2)
        for ((function, address) in listOf(0x1b to 0x80, 0x1a to 0x84)) {
            emit((8 shl 21) or (13 shl 16) or function)
            repeat(16) { emit(0) }
            emit((14 shl 11) or 0x12)
            emit(imm(0x2b, 12, 14, address))
        }
        constant(13, 0)
        for ((function, quotient, remainder) in listOf(Triple(0x1a, 0x98, 0xa0), Triple(0x1b, 0x9c, 0xa4))) {
            emit((8 shl 21) or (13 shl 16) or function)
            repeat(16) { emit(0) }
            emit((14 shl 11) or 0x12)
            emit(imm(0x2b, 12, 14, quotient))
            emit((14 shl 11) or 0x10)
            emit(imm(0x2b, 12, 14, remainder))
        }
        constant(8, 0x80000000.toInt())
        constant(13, -1)
        emit((8 shl 21) or (13 shl 16) or 0x1a)
        repeat(16) { emit(0) }
        emit((14 shl 11) or 0x12)
        emit(imm(0x2b, 12, 14, 0xa8))
        emit((14 shl 11) or 0x10)
        emit(imm(0x2b, 12, 14, 0xac))
        constant(8, 3)
        constant(13, 2)
        emit((8 shl 21) or (13 shl 16) or 0x1a)
        repeat(16) { emit(0) }
        emit((14 shl 11) or 0x12)
        emit(imm(0x2b, 12, 14, 0xb0))
        emit((14 shl 11) or 0x10)
        emit(imm(0x2b, 12, 14, 0xb4))
        write(11, 0x50, 0x800) // enable DMA channel 2
        write(11, 0, 0x1000)
        write(11, 4, if (textured) 14 else 8)
        write(11, 8, 0x11000001)
        emit(0x1000ffff) // stationary branch after the probe
        emit(0)
        return out.array()
    }
}
