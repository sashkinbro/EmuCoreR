
package com.sbro.emucorer.core

import java.util.concurrent.TimeUnit
import java.util.concurrent.locks.ReentrantLock
import kotlin.concurrent.withLock

/** Streaming sink. Counts are shorts, not bytes or stereo frames.
 * [pause] must unblock or quiesce an in-flight [write], which may be partial.
 */
internal interface PcmSink {
    fun write(samples: ShortArray, offset: Int, count: Int): Int
    fun play()
    fun pause()
    fun flush()
    fun release()
    fun stats(): LongArray? = null
}

/** One frame producer, serialized lifecycle callers. Host waits never advance guest time. */
internal class FrameAudioOutput(
    private val sink: PcmSink,
    private val gainProvider: () -> Float = { 1f }
) : AutoCloseable {
    private val writeLock = ReentrantLock()
    private val changed = writeLock.newCondition()
    @Volatile private var paused = true
    @Volatile private var closed = false
    @Volatile var timeline: Long = 0L
        private set

    fun resume() = writeLock.withLock {
        check(!closed) { "Audio output is closed" }
        sink.play()
        paused = false
        changed.signalAll()
    }

    fun pause() {
        // Do not acquire writeLock first: the sink consumer must be quiesced
        // before waiting for its producer.
        paused = true
        sink.pause()
        writeLock.withLock { } // Barrier: no write admitted before pause is still active.
    }

    fun discardTimeline() = writeLock.withLock {
        check(paused && !closed) { "Pause before replacing the audio timeline" }
        sink.flush()
        timeline++
        changed.signalAll()
    }

    fun stats(): LongArray? = sink.stats()

    /** False means this frame belongs to a replaced/closed timeline, not a sink error. */
    fun writeFrame(samples: ShortArray, expectedTimeline: Long): Boolean = writeLock.withLock {
        val gain = gainProvider().coerceIn(0f, 1f)
        val source = if (gain >= 1f) samples else applyGain(samples, gain)
        var offset = 0
        while (offset < source.size) {
            if (closed || timeline != expectedTimeline) return false
            if (paused) {
                changed.await()
                continue
            }
            val remaining = source.size - offset
            val written = sink.write(source, offset, remaining)
            check(written in 0..remaining) { "PCM sink write failed: $written of $remaining shorts" }
            offset += written
            if (written == 0) {
                // A short/zero transfer is not permission to drop a frame or
                // execute another guest frame. Release the lock for lifecycle
                // changes; this bounded host wait also prevents a busy spin.
                changed.awaitNanos(TimeUnit.MILLISECONDS.toNanos(1))
            }
        }
        !closed && timeline == expectedTimeline
    }

    private fun applyGain(samples: ShortArray, gain: Float): ShortArray {
        val scaled = ShortArray(samples.size)
        for (index in samples.indices) {
            scaled[index] = (samples[index] * gain).toInt().coerceIn(-32768, 32767).toShort()
        }
        return scaled
    }

    override fun close() {
        if (closed) return
        paused = true
        try {
            sink.pause()
        } finally {
            writeLock.withLock {
                if (!closed) {
                    closed = true
                    changed.signalAll()
                    sink.release()
                }
            }
        }
    }
}
