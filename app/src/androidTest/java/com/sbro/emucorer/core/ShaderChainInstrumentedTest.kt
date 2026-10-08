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
import java.util.zip.Inflater
import kotlin.math.roundToInt
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class ShaderChainInstrumentedTest {
    @Test
    fun loadingGpuStateDoesNotDispatchDmaBeforeRestoration() {
        withScene(RendererDefaults.CORE_VULKAN, sourceShader, dmaMode = 0) { bridge, session, reader, root ->
            bridge.runFrame(session)
            reader.acquireLatestImage()?.close()
            val state = File(root, "writable-dma.sav")
            assertEquals(0, bridge.saveState(session, state.absolutePath))
            val bytes = state.readBytes()
            val header = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN)
            val payload = statePayload(state)
            val data = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN)
            val gpu = stateMarker(payload, "GPU")
            val vram = stateMarker(payload, "GPU-VRAM")
            val fifo = (gpu until vram - 12).firstOrNull {
                data.getInt(it) in 1..4096 && data.getInt(it + 4) == 0x80000000.toInt() &&
                    data.getInt(it + 8) in 0x10000..0x20000
            } ?: error("missing pending DMA FIFO")
            val fifoSize = data.getInt(fifo)
            val shortened = payload.copyOfRange(0, fifo + 4) + payload.copyOfRange(fifo + 4 + fifoSize * 8, payload.size)
            val shortenedData = ByteBuffer.wrap(shortened).order(ByteOrder.LITTLE_ENDIAN)
            shortenedData.putInt(fifo, 0)
            val dma = stateMarker(shortened, "DMA")
            shortenedData.putInt(dma, 100) // A pending unhalt event with a writable GPU FIFO.
            shortened[dma + 4 + 2 * 13 + 12] = 1
            val expected = dmaProgress(shortened)
            val offset = header.getInt(212)
            header.putInt(200, 0)
            header.putInt(204, shortened.size)
            header.putInt(208, shortened.size)
            state.writeBytes(bytes.copyOfRange(0, offset) + shortened)
            assertEquals(0, bridge.loadState(session, state.absolutePath))
            val restored = File(root, "restored-dma.sav")
            assertEquals(0, bridge.saveState(session, restored.absolutePath))
            assertEquals("loading state must preserve DMA progress until emulation resumes", expected,
                dmaProgress(statePayload(restored)))
        }
    }

    private fun stateMarker(payload: ByteArray, name: String): Int {
        val marker = ByteBuffer.allocate(4 + name.length).order(ByteOrder.LITTLE_ENDIAN)
            .putInt(name.length).put(name.toByteArray()).array()
        return ((0..payload.size - marker.size).firstOrNull { i ->
            marker.indices.all { payload[i + it] == marker[it] }
        } ?: error("missing $name state marker")) + marker.size
    }

    private fun statePayload(file: File): ByteArray {
        val bytes = file.readBytes()
        val header = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN)
        assertEquals(59, header.getInt(4))
        val offset = header.getInt(212)
        val size = header.getInt(208)
        if (header.getInt(200) == 0) return bytes.copyOfRange(offset, offset + size)
        val inflater = Inflater()
        return try {
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

    private fun dmaProgress(payload: ByteArray): Pair<Int, Int> {
        val dma = stateMarker(payload, "DMA")
        val data = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN)
        return data.getInt(dma + 4 + 2 * 13) to data.getInt(dma + 4 + 7 * 13 + 8)
    }

    @Test
    fun largeDmaUploadsResumeAcrossStateLoads() {
        for (renderer in listOf(RendererDefaults.CORE_VULKAN, RendererDefaults.CORE_OPENGL,
                                RendererDefaults.CORE_SOFTWARE)) {
            for (mode in 0..2) withScene(renderer, sourceShader, dmaMode = mode) { bridge, session, reader, root ->
                bridge.runFrame(session)
                reader.acquireLatestImage()?.close()
                val state = File(root, "partial-dma.sav")
                assertEquals(0, bridge.saveState(session, state.absolutePath))
                val payload = statePayload(state)
                val control = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN)
                    .getInt(stateMarker(payload, "DMA") + 4 + 2 * 13 + 8)
                assertTrue("DMA must still be pending at the save boundary renderer=$renderer mode=$mode",
                    control and (1 shl 24) != 0)
                assertEquals(0, bridge.loadState(session, state.absolutePath))
                val full = if (renderer == RendererDefaults.CORE_SOFTWARE) 248 else 255
                val expected = if (mode == 1) intArrayOf(0, full, 0) else intArrayOf(0, 0, full)
                awaitPixel(bridge, session, reader, expected, "DMA tail renderer=$renderer mode=$mode", frames = 40)
            }
        }
    }

    @Test
    fun busyGpuWritesKeepStateAndRendererTeardownSafe() {
        val shader = counterShader.replace("OriginalHistory1", "Source")
        for (renderer in listOf(RendererDefaults.CORE_VULKAN, RendererDefaults.CORE_OPENGL,
                                RendererDefaults.CORE_SOFTWARE)) {
            withScene(renderer, shader, repeatFill = true) { bridge, session, reader, root ->
                repeat(8) {
                    bridge.runFrame(session)
                    reader.acquireLatestImage()?.close()
                }
                val state = File(root, "busy-gpu.sav")
                assertEquals(0, bridge.saveState(session, state.absolutePath))
                assertEquals(0, bridge.loadState(session, state.absolutePath))
                repeat(3) {
                    bridge.runFrame(session)
                    reader.acquireLatestImage()?.close()
                }
                assertEquals(0, bridge.reset(session))
            }
        }
    }

    @Test
    fun shadersSampleCurrentSource() {
        val shader = counterShader.replace("OriginalHistory1", "Source")
            .replace("float(params.FrameCount) / 255.0", "texture(Source, vUV).g")
        for (renderer in listOf(RendererDefaults.CORE_VULKAN, RendererDefaults.CORE_OPENGL)) {
            withScene(renderer, shader) { bridge, session, reader, _ ->
                awaitPixel(bridge, session, reader, intArrayOf(0, 255, 255), "source renderer=$renderer")
            }
        }
    }

    @Test
    fun resetAndStateLoadsRestartTemporalShaders() {
        for (renderer in listOf(RendererDefaults.CORE_VULKAN, RendererDefaults.CORE_OPENGL)) {
            withScene(renderer, counterShader) { bridge, session, reader, root ->
                fun frame(count: Int, stage: String, history: Int = 255) = awaitPixel(bridge, session, reader,
                    intArrayOf(0, count, history), "$stage renderer=$renderer", frames = 1)
                for (i in 0..6) frame(i, "initial timeline", if (i >= 6) 255 else -1)
                val state = File(root, "temporal.sav")
                assertEquals(0, bridge.saveState(session, state.absolutePath))
                for (i in 7..12) frame(i, "future timeline")
                assertEquals(0, bridge.loadState(session, state.absolutePath))
                frame(0, "restored timeline", 0)
                for (i in 1..4) frame(i, "after state load")
                assertEquals(0, bridge.reset(session))
                frame(0, "reset timeline", 0)
                frame(1, "after reset")
            }
        }
    }

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

    private fun withScene(renderer: Int, shader: String, repeatFill: Boolean = false, dmaMode: Int = -1,
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
        val bios = File(system, "shader.bin").apply { writeBytes(makeBios(repeatFill, dmaMode)) }
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
            if (expected.indices.all { expected[it] < 0 || kotlin.math.abs(actual[it] - expected[it]) <= 3 }) return
        }
        fail("$stage expected=${expected.toList()} actual=${actual.toList()}")
    }

    private fun makeBios(repeatFill: Boolean = false, dmaMode: Int = -1): ByteArray {
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
        val fillStart = out.position()
        for (command in listOf(0x02ffffff, 0, (240 shl 16) or 320)) write(0, command)
        if (dmaMode >= 0) {
            constant(12, 0x80010000.toInt())
            fun streamWord(value: Int) {
                constant(8, value)
                emit((0x2b shl 26) or (12 shl 21) or (8 shl 16))
                emit((9 shl 26) or (12 shl 21) or (12 shl 16) or 4)
            }
            fun uploadHeader() {
                // Queue enough GPU work to span several frames before the upload.
                repeat(8) {
                    streamWord(0x80000000.toInt())
                    repeat(3) { streamWord(0) }
                }
                streamWord(0xa0000000.toInt())
                streamWord((56 shl 16) or 96)
                streamWord((128 shl 16) or 128)
            }
            fun fillWords(count: Int, color: Int = 0x7c007c00) {
                constant(8, color)
                constant(11, count)
                emit((0x2b shl 26) or (12 shl 21) or (8 shl 16))
                emit((9 shl 26) or (12 shl 21) or (12 shl 16) or 4)
                emit((9 shl 26) or (11 shl 21) or (11 shl 16) or 65535)
                emit((5 shl 26) or (11 shl 21) or 65532)
                emit(0)
            }
            if (dmaMode == 2) {
                for (node in 0..32) {
                    streamWord(if (node == 32) 0x43800000 else 0xff000000.toInt() or (0x10000 + (node + 1) * 1024))
                    if (node == 0) uploadHeader()
                    fillWords(when (node) { 0 -> 220; 32 -> 67; else -> 255 })
                }
            } else repeat(if (dmaMode == 1) 2 else 1) { block ->
                uploadHeader()
                fillWords(8192, if (block == 1) 0x03e003e0 else 0x7c007c00)
            }
            write(4, 0x04000002)
            constant(10, 0x1f8010f0)
            write(0, 0x800)
            constant(10, 0x1f8010a0)
            write(0, 0x10000)
            write(4, if (dmaMode == 1) 0x00022023 else 0x2023)
            write(8, when (dmaMode) { 0 -> 0x11000001; 1 -> 0x01000201; else -> 0x01000401 })
        }
        val loop = if (repeatFill) fillStart else out.position()
        emit(0x08000000 or (((0xbfc00000.toInt() + loop) ushr 2) and 0x03ffffff))
        emit(0)
        return out.array()
    }

    private val sourceShader = """
        #version 450
        layout(set = 0, binding = 0, std140) uniform UBO { mat4 MVP; } params;
        #pragma stage vertex
        layout(location = 0) in vec4 Position;
        layout(location = 1) in vec2 TexCoord;
        layout(location = 0) out vec2 vUV;
        void main() { gl_Position = params.MVP * Position; vUV = TexCoord; }
        #pragma stage fragment
        layout(location = 0) in vec2 vUV;
        layout(location = 0) out vec4 FragColor;
        layout(set = 0, binding = 1) uniform sampler2D Source;
        void main() { FragColor = texture(Source, vUV); }
    """.trimIndent()

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

    private val counterShader = """
        #version 450
        layout(set = 0, binding = 0, std140) uniform UBO { mat4 MVP; uint FrameCount; } params;
        #pragma stage vertex
        layout(location = 0) in vec4 Position;
        layout(location = 1) in vec2 TexCoord;
        layout(location = 0) out vec2 vUV;
        void main() { gl_Position = params.MVP * Position; vUV = TexCoord; }
        #pragma stage fragment
        layout(location = 0) in vec2 vUV;
        layout(location = 0) out vec4 FragColor;
        layout(set = 0, binding = 1) uniform sampler2D OriginalHistory1;
        void main() {
            FragColor = vec4((vUV.x + vUV.y) * 0.001, float(params.FrameCount) / 255.0,
                             texture(OriginalHistory1, vUV).r, 1.0);
        }
    """.trimIndent()
}
