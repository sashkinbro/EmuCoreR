// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: GPL-3.0-or-later
package com.sbro.emucorer.core

import android.graphics.PixelFormat
import android.media.ImageReader
import android.os.Build
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.math.roundToInt
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class ShaderChainInstrumentedTest {
    @Test
    fun shadersReceiveSourceTimingAndAspect() {
        for (renderer in listOf(RendererDefaults.CORE_VULKAN, RendererDefaults.CORE_OPENGL)) {
            withScene(renderer, uniformsShader) { bridge, session, reader, _ ->
                val fps = bridge.getFrameRate(session)
                val expected = intArrayOf((fps * 2.55).roundToInt(), (255.0 * 8.0 / 9.0).roundToInt(),
                    ((1000.0 / fps).roundToInt() * 2.55).roundToInt())
                awaitPixel(bridge, session, reader, expected, "timing/aspect renderer=$renderer")
            }
        }
    }

    private fun withScene(renderer: Int, shader: String,
                          block: (NativeCoreBridge, Long, ImageReader, File) -> Unit) {
        assertEquals("CPH2747", Build.MODEL)
        assertEquals("OnePlus", Build.MANUFACTURER)
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val root = File(context.cacheDir, "shader-probe-${System.nanoTime()}").apply { mkdirs() }
        val system = File(root, "system").apply { mkdirs() }
        val save = File(root, "save").apply { mkdirs() }
        val assets = File(root, "assets").apply { mkdirs() }
        File(root, "probe.slang").writeText(shader)
        val preset = File(root, "probe.slangp").apply {
            writeText("shaders = 1\nshader0 = probe.slang\nfilter_linear0 = false\n")
        }
        val bios = File(system, "shader.bin").apply { writeBytes(makeBios()) }
        val bridge = NativeCoreBridge()
        try {
            bridge.nativeInit(system.absolutePath, save.absolutePath, assets.absolutePath)
            bridge.nativeSetShaderEffect(0)
            bridge.setDisplayCrop(0, 0, 0, 0)
            bridge.nativeSetShaderPreset(preset.absolutePath, true)
            mapOf(
                "CPU_ExecutionMode" to "Recompiler", "GPU_Renderer" to RendererDefaults.coreRendererName(renderer),
                "GPU_PGXPEnable" to "false", "GPU_ResolutionScale" to "2", "GPU_UseThread" to "false",
                "GPU_TextureFilter" to "Nearest", "GPU_MSAA" to "1", "GPU_WidescreenHack" to "false",
                "Display_AspectRatio" to "16:9", "Display_CropMode" to "Borders",
                "MemoryCards_Card1Type" to "None", "MemoryCards_Card2Type" to "None",
                "Main_RunaheadFrameCount" to "0", "Main_ApplyGameSettings" to "false",
                "BIOS_PatchFastBoot" to "false", "Console_Region" to "NTSC-U",
                "TextureReplacements_EnableVRAMWriteReplacements" to "false"
            ).forEach { (key, value) -> bridge.nativeSetOption("swanstation_$key", value) }
            val session = bridge.createSession()
            assertTrue(session != 0L)
            try {
                ImageReader.newInstance(320, 240, PixelFormat.RGBA_8888, 3).use { reader ->
                    assertEquals(0, bridge.setSurface(session, reader.surface, renderer))
                    assertEquals(0, bridge.loadBios(session, bios.absolutePath))
                    assertEquals(0, bridge.loadBiosOnly(session))
                    block(bridge, session, reader, root)
                }
            } finally { bridge.destroySession(session) }
        } finally {
            bridge.nativeSetShaderPreset("", false)
            bridge.setDisplayCrop(0, 0, 0, 0)
            root.deleteRecursively()
        }
    }

    private fun awaitPixel(bridge: NativeCoreBridge, session: Long, reader: ImageReader,
                           expected: IntArray, stage: String, frames: Int = 20) {
        var actual = intArrayOf(-1, -1, -1)
        repeat(frames) {
            bridge.runFrame(session)
            val deadline = System.nanoTime() + 100_000_000
            while (System.nanoTime() < deadline) {
                val frame = reader.acquireLatestImage() ?: continue
                frame.use {
                    val plane = it.planes[0]
                    val offset = (it.height / 2) * plane.rowStride + (it.width / 2) * plane.pixelStride
                    actual = IntArray(3) { c -> plane.buffer.get(offset + c).toInt() and 255 }
                }
                break
            }
            if (expected.indices.all { kotlin.math.abs(actual[it] - expected[it]) <= 3 }) return
        }
        fail("$stage expected=${expected.toList()} actual=${actual.toList()}")
    }

    private fun makeBios(): ByteArray {
        val out = ByteBuffer.allocate(512 * 1024).order(ByteOrder.LITTLE_ENDIAN)
        fun emit(value: Int) { out.putInt(value) }
        fun constant(register: Int, value: Int) {
            emit((0xf shl 26) or (register shl 16) or (value ushr 16))
            emit((0xd shl 26) or (register shl 21) or (register shl 16) or (value and 65535))
        }
        fun write(offset: Int, value: Int) {
            constant(8, value)
            emit((0x2b shl 26) or (10 shl 21) or (8 shl 16) or offset)
        }
        constant(10, 0x1f801810)
        for (command in listOf(0, 0x08000001, 0x05000000, 0x06c60260, 0x07042018, 0x03000000)) write(4, command)
        for (command in listOf(0xe3000000.toInt(), 0xe407ffff.toInt(), 0xe5000000.toInt())) write(0, command)
        for (command in listOf(0x02ffffff, 0, (240 shl 16) or 320)) write(0, command)
        val loop = out.position()
        emit(0x08000000 or (((0xbfc00000.toInt() + loop) ushr 2) and 0x03ffffff))
        emit(0)
        return out.array()
    }

    private val uniformsShader = """
        #version 450
        layout(set = 0, binding = 0, std140) uniform UBO {
            mat4 MVP;
            float OriginalFPS;
            float OriginalAspect;
            uint FrameTimeDelta;
        } params;
        #pragma stage vertex
        layout(location = 0) in vec4 Position;
        layout(location = 1) in vec2 TexCoord;
        layout(location = 0) out vec2 vUV;
        void main() { gl_Position = params.MVP * Position; vUV = TexCoord; }
        #pragma stage fragment
        layout(location = 0) in vec2 vUV;
        layout(location = 0) out vec4 FragColor;
        void main() {
            FragColor = vec4(params.OriginalFPS / 100.0, params.OriginalAspect / 2.0,
                             float(params.FrameTimeDelta) / 100.0, 1.0);
            FragColor.rgb += vec3((vUV.x + vUV.y) * 0.001);
            #if !defined(_HAS_ORIGINALASPECT_UNIFORMS) || !defined(_HAS_FRAMETIME_UNIFORMS)
            FragColor = vec4(1.0, 0.0, 1.0, 1.0);
            #endif
        }
    """.trimIndent()
}
