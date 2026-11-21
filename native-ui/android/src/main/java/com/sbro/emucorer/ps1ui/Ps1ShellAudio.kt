package com.sbro.emucorer.ps1ui

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.media.SoundPool
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.FileOutputStream
import kotlin.concurrent.thread
import kotlin.math.PI
import kotlin.math.exp
import kotlin.math.roundToInt
import kotlin.math.sin
import kotlin.math.tanh

/**
 * Procedural 44.1 kHz PCM sound synthesizer and ambient pad generator for the
 * PS1 BIOS shell. Synthesizes all samples deterministically at initialization
 * without bundling any external or copyrighted audio assets.
 */
object Ps1ShellAudio {
    const val SFX_MOVE = 0
    const val SFX_CONFIRM = 1
    const val SFX_CANCEL = 2
    const val SFX_ERROR = 3
    const val SFX_BOOT_INTRO = 4
    const val SFX_DISC_LAUNCH = 5

    private const val SAMPLE_RATE = 44100
    private val lock = Any()

    @Volatile
    var keyToneEnabled: Boolean = true

    @Volatile
    var ambientEnabled: Boolean = false
        set(value) {
            field = value
            syncAmbientPlayback()
        }

    @Volatile
    var hostVisible: Boolean = true
        set(value) {
            field = value
            syncAmbientPlayback()
        }

    private var initialized = false
    private var pool: SoundPool? = null
    private val soundIds = IntArray(6)
    private val readyIds = HashSet<Int>()

    private var ambientThread: Thread? = null
    @Volatile
    private var ambientStopRequested = false

    fun initialize(context: Context) {
        synchronized(lock) {
            if (initialized) return
            val sfxDir = File(context.cacheDir, "ps1ui_sfx_v1").apply { mkdirs() }
            val attrs = AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_GAME)
                .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
                .build()
            val sp = SoundPool.Builder()
                .setMaxStreams(6)
                .setAudioAttributes(attrs)
                .build()
            sp.setOnLoadCompleteListener { _, sampleId, status ->
                if (status == 0) {
                    synchronized(lock) {
                        readyIds.add(sampleId)
                    }
                }
            }
            pool = sp

            val generators: List<Pair<Int, () -> ShortArray>> = listOf(
                SFX_MOVE to ::synthMoveSfx,
                SFX_CONFIRM to ::synthConfirmSfx,
                SFX_CANCEL to ::synthCancelSfx,
                SFX_ERROR to ::synthErrorSfx,
                SFX_BOOT_INTRO to ::synthBootIntroSfx,
                SFX_DISC_LAUNCH to ::synthDiscLaunchSfx,
            )

            for ((index, gen) in generators) {
                val wavFile = File(sfxDir, "sfx_$index.wav")
                if (!wavFile.isFile || wavFile.length() < 128L) {
                    runCatching { writeWav(wavFile, gen()) }
                }
                if (wavFile.isFile) {
                    soundIds[index] = sp.load(wavFile.absolutePath, 1)
                }
            }
            initialized = true
        }
        syncAmbientPlayback()
    }

    @JvmStatic
    fun playSfx(kind: Int) {
        if (!keyToneEnabled && kind != SFX_BOOT_INTRO && kind != SFX_DISC_LAUNCH) return
        if (!keyToneEnabled && (kind == SFX_BOOT_INTRO || kind == SFX_DISC_LAUNCH)) return
        if (kind !in soundIds.indices) return
        val sp = pool ?: return
        val id = soundIds[kind]
        if (id == 0) return
        val isReady = synchronized(lock) { readyIds.contains(id) }
        if (!isReady) return
        val vol = when (kind) {
            SFX_MOVE -> 0.42f
            SFX_CONFIRM -> 0.54f
            SFX_CANCEL -> 0.48f
            SFX_ERROR -> 0.48f
            SFX_BOOT_INTRO -> 0.58f
            SFX_DISC_LAUNCH -> 0.56f
            else -> 0.45f
        }
        sp.play(id, vol, vol, 1, 0, 1.0f)
    }

    fun pauseAudio() {
        hostVisible = false
    }

    fun resumeAudio() {
        hostVisible = true
    }

    fun shutdown() {
        stopAmbientThread()
        synchronized(lock) {
            pool?.release()
            pool = null
            readyIds.clear()
            soundIds.fill(0)
            initialized = false
        }
    }

    private fun syncAmbientPlayback() {
        val shouldPlay = initialized && ambientEnabled && hostVisible
        if (shouldPlay) {
            startAmbientThread()
        } else {
            stopAmbientThread()
        }
    }

    private fun startAmbientThread() {
        synchronized(lock) {
            if (ambientThread?.isAlive == true) return
            ambientStopRequested = false
            ambientThread = thread(start = true, isDaemon = true, name = "Ps1AmbientPad") {
                runAmbientLoop()
            }
        }
    }

    private fun stopAmbientThread() {
        ambientStopRequested = true
        val t = synchronized(lock) { ambientThread }
        if (t != null && t !== Thread.currentThread()) {
            runCatching { t.join(250) }
        }
        synchronized(lock) {
            if (ambientThread === t) {
                ambientThread = null
            }
        }
    }

    private fun runAmbientLoop() {
        val minBuf = AudioTrack.getMinBufferSize(
            SAMPLE_RATE,
            AudioFormat.CHANNEL_OUT_MONO,
            AudioFormat.ENCODING_PCM_16BIT
        ).coerceAtLeast(4096)
        val track = runCatching {
            AudioTrack.Builder()
                .setAudioAttributes(
                    AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_GAME)
                        .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                        .build()
                )
                .setAudioFormat(
                    AudioFormat.Builder()
                        .setSampleRate(SAMPLE_RATE)
                        .setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
                        .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                        .build()
                )
                .setBufferSizeInBytes(minBuf * 2)
                .setTransferMode(AudioTrack.MODE_STREAM)
                .build()
        }.getOrNull() ?: return

        val chunk = ShortArray(2048)
        var sampleIdx = 0L
        val freqs = doubleArrayOf(110.0, 164.81, 220.0, 246.94, 329.63)
        val amps = doubleArrayOf(0.28, 0.22, 0.16, 0.12, 0.09)
        var fadeGain = 0.0

        try {
            track.play()
            while (!ambientStopRequested) {
                for (i in chunk.indices) {
                    val t = sampleIdx.toDouble() / SAMPLE_RATE.toDouble()
                    val lfo1 = 0.72 + 0.28 * sin(2.0 * PI * 0.11 * t)
                    val lfo2 = 0.75 + 0.25 * sin(2.0 * PI * 0.07 * t + 1.4)
                    var sum = 0.0
                    for (fIdx in freqs.indices) {
                        val detune = 1.0 + 0.0014 * sin(2.0 * PI * (0.05 + fIdx * 0.017) * t)
                        val mod = if (fIdx % 2 == 0) lfo1 else lfo2
                        sum += amps[fIdx] * mod * sin(2.0 * PI * freqs[fIdx] * detune * t)
                    }
                    fadeGain = (fadeGain + 1.0 / (SAMPLE_RATE * 1.2)).coerceAtMost(1.0)
                    val s = tanh(sum * 0.22 * fadeGain)
                    chunk[i] = (s * 32767.0).roundToInt().coerceIn(-32767, 32767).toShort()
                    sampleIdx++
                }
                track.write(chunk, 0, chunk.size)
            }
        } catch (_: Throwable) {
        } finally {
            runCatching {
                track.stop()
                track.release()
            }
        }
    }

    private fun synthMoveSfx(): ShortArray {
        val len = (SAMPLE_RATE * 0.065).toInt()
        return ShortArray(len) { i ->
            val t = i.toDouble() / SAMPLE_RATE
            val env = (1.0 - exp(-t * 900.0)) * exp(-t * 62.0)
            val f = 1320.0 - 110.0 * (t / 0.065)
            val tone = 0.72 * sin(2.0 * PI * f * t) + 0.28 * sin(2.0 * PI * (f * 2.0) * t)
            (tone * env * 0.55 * 32767.0).roundToInt().coerceIn(-32767, 32767).toShort()
        }
    }

    private fun synthConfirmSfx(): ShortArray {
        val len = (SAMPLE_RATE * 0.22).toInt()
        return ShortArray(len) { i ->
            val t = i.toDouble() / SAMPLE_RATE
            val env1 = (1.0 - exp(-t * 600.0)) * exp(-t * 24.0)
            val s1 = 0.6 * sin(2.0 * PI * 880.0 * t) + 0.3 * sin(2.0 * PI * 1318.5 * t)
            val t2 = (t - 0.045).coerceAtLeast(0.0)
            val env2 = if (t >= 0.045) (1.0 - exp(-t2 * 500.0)) * exp(-t2 * 16.0) else 0.0
            val s2 = 0.65 * sin(2.0 * PI * 1318.5 * t) + 0.35 * sin(2.0 * PI * 1760.0 * t)
            val mixed = tanh((s1 * env1 + s2 * env2) * 0.48)
            (mixed * 32767.0).roundToInt().coerceIn(-32767, 32767).toShort()
        }
    }

    private fun synthCancelSfx(): ShortArray {
        val len = (SAMPLE_RATE * 0.16).toInt()
        return ShortArray(len) { i ->
            val t = i.toDouble() / SAMPLE_RATE
            val env = (1.0 - exp(-t * 500.0)) * exp(-t * 26.0)
            val f = 780.0 * exp(-t * 2.4)
            val tone = 0.75 * sin(2.0 * PI * f * t) + 0.25 * sin(2.0 * PI * (f * 1.5) * t)
            (tone * env * 0.55 * 32767.0).roundToInt().coerceIn(-32767, 32767).toShort()
        }
    }

    private fun synthErrorSfx(): ShortArray {
        val len = (SAMPLE_RATE * 0.18).toInt()
        return ShortArray(len) { i ->
            val t = i.toDouble() / SAMPLE_RATE
            val p1 = (1.0 - exp(-t * 600.0)) * exp(-t * 40.0)
            val t2 = (t - 0.075).coerceAtLeast(0.0)
            val p2 = if (t >= 0.075) (1.0 - exp(-t2 * 600.0)) * exp(-t2 * 34.0) else 0.0
            val tone = sin(2.0 * PI * 210.0 * t) + 0.4 * sin(2.0 * PI * 315.0 * t)
            (tanh(tone * (p1 + p2) * 0.45) * 32767.0).roundToInt().coerceIn(-32767, 32767).toShort()
        }
    }

    private fun synthBootIntroSfx(): ShortArray {
        val dur = 1.65
        val len = (SAMPLE_RATE * dur).toInt()
        val chord = doubleArrayOf(110.0, 164.81, 220.0, 329.63, 440.0, 523.25, 659.25)
        val weights = doubleArrayOf(0.45, 0.34, 0.30, 0.24, 0.18, 0.14, 0.12)
        return ShortArray(len) { i ->
            val t = i.toDouble() / SAMPLE_RATE
            val swell = (1.0 - exp(-t * 3.8)) * exp(-((t - 0.45).coerceAtLeast(0.0)) * 2.1)
            val chimeGate = if (t > 0.38) exp(-(t - 0.38) * 3.2) * (1.0 - exp(-(t - 0.38) * 28.0)) else 0.0
            var pad = 0.0
            for (k in chord.indices) {
                val detune = 1.0 + 0.002 * sin(2.0 * PI * 0.4 * t + k)
                pad += weights[k] * sin(2.0 * PI * chord[k] * detune * t)
            }
            val shimmer = 0.22 * sin(2.0 * PI * 880.0 * t) + 0.16 * sin(2.0 * PI * 1318.5 * t)
            val mixed = tanh((pad * swell + shimmer * chimeGate) * 0.55)
            (mixed * 32767.0).roundToInt().coerceIn(-32767, 32767).toShort()
        }
    }

    private fun synthDiscLaunchSfx(): ShortArray {
        val dur = 0.95
        val len = (SAMPLE_RATE * dur).toInt()
        return ShortArray(len) { i ->
            val t = i.toDouble() / SAMPLE_RATE
            val env = (1.0 - exp(-t * 8.0)) * exp(-((t - 0.18).coerceAtLeast(0.0)) * 3.4)
            val sweepFreq = 220.0 + 440.0 * (1.0 - exp(-t * 3.5))
            val tone = 0.5 * sin(2.0 * PI * sweepFreq * t) +
                0.35 * sin(2.0 * PI * 659.25 * t) +
                0.28 * sin(2.0 * PI * 987.77 * t)
            val mixed = tanh(tone * env * 0.55)
            (mixed * 32767.0).roundToInt().coerceIn(-32767, 32767).toShort()
        }
    }

    private fun writeWav(file: File, pcm: ShortArray) {
        val dataBytes = pcm.size * 2
        val header = ByteArrayOutputStream(44)
        fun writeAscii(s: String) = s.forEach { header.write(it.code) }
        fun writeIntLE(v: Int) {
            header.write(v and 0xFF)
            header.write((v ushr 8) and 0xFF)
            header.write((v ushr 16) and 0xFF)
            header.write((v ushr 24) and 0xFF)
        }
        fun writeShortLE(v: Int) {
            header.write(v and 0xFF)
            header.write((v ushr 8) and 0xFF)
        }
        writeAscii("RIFF")
        writeIntLE(36 + dataBytes)
        writeAscii("WAVE")
        writeAscii("fmt ")
        writeIntLE(16)
        writeShortLE(1)
        writeShortLE(1)
        writeIntLE(SAMPLE_RATE)
        writeIntLE(SAMPLE_RATE * 2)
        writeShortLE(2)
        writeShortLE(16)
        writeAscii("data")
        writeIntLE(dataBytes)

        val pcmBytes = ByteArray(dataBytes)
        var p = 0
        for (sample in pcm) {
            val s = sample.toInt()
            pcmBytes[p] = (s and 0xFF).toByte()
            pcmBytes[p + 1] = ((s ushr 8) and 0xFF).toByte()
            p += 2
        }
        FileOutputStream(file).use { out ->
            out.write(header.toByteArray())
            out.write(pcmBytes)
        }
    }
}
