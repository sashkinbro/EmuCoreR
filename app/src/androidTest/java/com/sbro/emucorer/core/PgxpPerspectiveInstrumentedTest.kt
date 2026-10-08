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
import kotlin.math.abs
import kotlin.math.roundToInt
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class PgxpPerspectiveInstrumentedTest {
    @Test
    fun varyingDepthInterpolatesTextureCoordinatesCorrectly() {
        assertEquals("CPH2747", Build.MODEL)
        assertEquals("OnePlus", Build.MANUFACTURER)
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        val root = File(instrumentation.targetContext.cacheDir, "pgxp-perspective-${System.nanoTime()}").apply { mkdirs() }
        val system = File(root, "system").apply { mkdirs() }
        val save = File(root, "save").apply { mkdirs() }
        val assets = File(root, "assets").apply { mkdirs() }
        val bios = File(system, "perspective.bin").apply { writeBytes(makeBios()) }
        val bridge = NativeCoreBridge()
        // Test-only mutation proves the perspective assertions detect an
        // affine renderer instead of merely accepting a textured frame.
        val forceAffine = InstrumentationRegistry.getArguments().getString("pgxpPerspectiveForceAffine") == "true"
        try {
            for (cpu in listOf("Interpreter", "Recompiler")) {
                for (renderer in listOf(RendererDefaults.CORE_VULKAN, RendererDefaults.CORE_OPENGL)) {
                    for (scale in listOf(1, 3)) {
                        for (perspective in listOf(false, true)) {
                            bridge.nativeInit(system.absolutePath, save.absolutePath, assets.absolutePath)
                            mapOf(
                                "CPU_ExecutionMode" to cpu,
                                "GPU_Renderer" to if (renderer == RendererDefaults.CORE_VULKAN) "Vulkan" else "OpenGL",
                                "GPU_PGXPEnable" to "true", "GPU_PGXPCPU" to "false",
                                "GPU_PGXPPreserveProjFP" to "false", "GPU_PGXPVertexCache" to "false",
                                "GPU_PGXPTextureCorrection" to (perspective && !forceAffine).toString(),
                                "GPU_PGXPColorCorrection" to "false", "GPU_PGXPDepthBuffer" to "false",
                                "GPU_PGXPCulling" to "true", "GPU_PGXPDisableOn2DPolygons" to "true",
                                "GPU_TextureFilter" to "Nearest", "GPU_ResolutionScale" to scale.toString(),
                                "GPU_UseThread" to "false", "GPU_Multisamples" to "1", "GPU_WidescreenHack" to "false",
                                "Display_AspectRatio" to "4:3", "Display_CropMode" to "Borders",
                                "MemoryCards_Card1Type" to "None", "MemoryCards_Card2Type" to "None",
                                "BIOS_PatchFastBoot" to "false", "Console_Region" to "NTSC-U",
                                "Main_ApplyGameSettings" to "false", "Main_RunaheadFrameCount" to "0",
                                "Console_Enable8MBRAM" to "false"
                            ).forEach { (key, value) -> bridge.nativeSetOption("swanstation_$key", value) }
                            val session = bridge.createSession()
                            assertTrue(session != 0L)
                            ImageReader.newInstance(320, 240, PixelFormat.RGBA_8888, 3).use { reader ->
                                try {
                                    assertEquals(0, bridge.setSurface(session, reader.surface, renderer))
                                    assertEquals(0, bridge.loadBios(session, bios.absolutePath))
                                    assertEquals(0, bridge.loadBiosOnly(session))
                                    var frame: Frame? = null
                                    repeat(6) {
                                        bridge.runFrame(session)
                                        frame = capture(reader) ?: frame
                                    }
                                    val label = "$cpu/${RendererDefaults.coreRendererName(renderer)}/scale=$scale/perspective=$perspective"
                                    assertNotNull("missing frame: $label", frame)
                                    checkSamples(frame!!, perspective, label)
                                    Log.i("PGXPPerspective", "PASS $label")
                                } finally { bridge.destroySession(session) }
                            }
                        }
                    }
                }
            }
        } finally { root.deleteRecursively() }
    }

    private data class Frame(val width: Int, val height: Int, val rgb: IntArray)

    private fun capture(reader: ImageReader): Frame? {
        val deadline = System.nanoTime() + 100_000_000
        while (System.nanoTime() < deadline) {
            reader.acquireLatestImage()?.use { image ->
                val plane = image.planes[0]
                val pixels = IntArray(image.width * image.height)
                for (y in 0 until image.height) for (x in 0 until image.width) {
                    val offset = y * plane.rowStride + x * plane.pixelStride
                    pixels[y * image.width + x] = ((plane.buffer.get(offset).toInt() and 255) shl 16) or
                        ((plane.buffer.get(offset + 1).toInt() and 255) shl 8) or (plane.buffer.get(offset + 2).toInt() and 255)
                }
                return Frame(image.width, image.height, pixels)
            }
            Thread.sleep(2)
        }
        return null
    }

    private fun checkSamples(frame: Frame, perspective: Boolean, label: String) {
        fun marker(channel: Int): Pair<Double, Double> {
            var xSum = 0.0
            var ySum = 0.0
            var count = 0
            for (y in 0 until frame.height) for (x in 0 until frame.width) {
                val pixel = frame.rgb[y * frame.width + x]
                val channels = intArrayOf(pixel shr 16 and 255, pixel shr 8 and 255, pixel and 255)
                if (channels[channel] > 220 && channels.withIndex().all { it.index == channel || it.value < 20 }) {
                    xSum += x
                    ySum += y
                    count++
                }
            }
            assertTrue("missing calibration marker $channel: $label", count >= 4)
            return xSum / count to ySum / count
        }
        // Markers calibrate display crop, letterboxing and output scaling.
        val red = marker(0)
        val green = marker(1)
        val blue = marker(2)
        val scaleX = (green.first - red.first) / 290.0
        val scaleY = (blue.second - red.second) / 210.0
        assertTrue("invalid display calibration: $label", scaleX > 0.5 && scaleY > 0.5)
        for ((targetX, targetY) in listOf(80 to 70, 110 to 80, 170 to 60, 70 to 150)) {
            val px = (red.first + (targetX - 14.5) * scaleX).roundToInt()
            val py = (red.second + (targetY - 14.5) * scaleY).roundToInt()
            assertTrue(px in 0 until frame.width && py in 0 until frame.height)
            val x = 14.5 + (px - red.first) / scaleX
            val y = 14.5 + (py - red.second) / scaleY
            val b1 = (x - 40.0) / 240.0
            val b2 = (y - 40.0) / 160.0
            val b0 = 1.0 - b1 - b2
            assertTrue(b0 > 0 && b1 > 0 && b2 > 0)
            val denominator = if (perspective) b0 / 200 + b1 / 400 + b2 / 800 else 1.0
            val u = 63 * (if (perspective) b1 / 400 else b1) / denominator
            val v = 63 * (if (perspective) b2 / 800 else b2) / denominator
            val expected = intArrayOf((5 + u.toInt() / 3) * 255 / 31, (5 + v.toInt() / 3) * 255 / 31, 8 * 255 / 31)
            val pixel = frame.rgb[py * frame.width + px]
            val actual = intArrayOf(pixel shr 16 and 255, pixel shr 8 and 255, pixel and 255)
            Log.i("PGXPPerspective", "$label sample=($x,$y) uv=($u,$v) rgb=${actual.toList()} expected=${expected.toList()}")
            for (channel in 0..2) {
                assertTrue("wrong perspective sample $label at ($targetX,$targetY), channel=$channel: ${actual.toList()} expected=${expected.toList()}",
                    abs(actual[channel] - expected[channel]) <= 10)
            }
        }
    }

    private fun makeBios(): ByteArray {
        val out = ByteBuffer.allocate(512 * 1024).order(ByteOrder.LITTLE_ENDIAN)
        fun emit(value: Int) { out.putInt(value) }
        fun imm(op: Int, source: Int, target: Int, value: Int) =
            (op shl 26) or (source shl 21) or (target shl 16) or (value and 65535)
        fun constant(register: Int, value: Int) {
            emit(imm(15, 0, register, value ushr 16))
            emit(imm(13, register, register, value))
        }
        fun write(base: Int, offset: Int, value: Int) {
            constant(8, value)
            emit(imm(0x2b, base, 8, offset))
        }
        fun gte(register: Int, value: Int, control: Boolean) {
            constant(8, value)
            emit((if (control) 0x48c00000 else 0x48800000) or (8 shl 16) or (register shl 11))
            repeat(2) { emit(0) }
        }
        constant(8, 0x40000000)
        emit(0x40886000)
        constant(9, 0xa0001000.toInt())
        constant(10, 0x1f801810)
        constant(11, 0x1f8010a0)
        for (command in listOf(0, 0x08000001, 0x05000000, 0x06c60260, 0x07042018, 0x03000000, 0x04000002))
            write(10, 4, command)
        for (command in listOf(0xe3000000.toInt(), 0xe407ffff.toInt(), 0xe5000000.toInt(), 0xe1000000.toInt(),
            0x02000000, 0, (240 shl 16) or 320)) write(10, 0, command)
        for (command in listOf(0xa0000000.toInt(), 512, (64 shl 16) or 64)) write(10, 0, command)
        fun texel(u: Int, v: Int) = (5 + u / 3) or ((5 + v / 3) shl 5) or (8 shl 10)
        for (v in 0 until 64) for (u in 0 until 64 step 2)
            write(10, 0, texel(u, v) or (texel(u + 1, v) shl 16))
        gte(0, 4096, true)
        gte(2, 4096, true)
        gte(4, 4096, true)
        gte(24, 160 shl 16, true)
        gte(25, 120 shl 16, true)
        gte(26, 100, true)
        write(9, 0, 0x25808080)
        // Projected points (40,40), (280,40), (40,200), with depths 1:2:4.
        for ((index, xyz) in listOf(Triple(-240, -160, 200), Triple(480, -320, 400), Triple(-960, 640, 800)).withIndex()) {
            gte(0, ((xyz.second and 65535) shl 16) or (xyz.first and 65535), false)
            gte(1, xyz.third, false)
            emit(0x4a080001)
            repeat(16) { emit(0) }
            emit(imm(0x3a, 9, 14, 4 + index * 8))
            write(9, 8 + index * 8, listOf(0, 63 or (0x108 shl 16), 63 shl 8)[index])
        }
        write(11, 0x50, 0x800)
        write(11, 0, 0x1000)
        write(11, 4, 7)
        write(11, 8, 0x11000001)
        // Distinct flat rectangles are independent of texture interpolation.
        for ((color, position) in listOf(0x0000ff to (12 to 12), 0x00ff00 to (302 to 12), 0xff0000 to (12 to 222))) {
            for (command in listOf(0x60000000 or color, position.first or (position.second shl 16), (5 shl 16) or 5))
                write(10, 0, command)
        }
        emit(0x1000ffff)
        emit(0)
        return out.array()
    }
}
