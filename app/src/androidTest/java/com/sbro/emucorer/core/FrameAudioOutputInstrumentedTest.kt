// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: LicenseRef-EmuCoreR-Proprietary
package com.sbro.emucorer.core

import androidx.test.ext.junit.runners.AndroidJUnit4
import java.util.concurrent.Callable
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class FrameAudioOutputInstrumentedTest {
    private open class Sink : PcmSink {
        val queued = mutableListOf<Short>()
        val offsets = mutableListOf<Int>()
        var released = 0
        open fun transferCount(count: Int) = count
        override fun write(samples: ShortArray, offset: Int, count: Int): Int {
            offsets += offset
            val written = transferCount(count)
            if (written > 0 && written <= count)
                queued.addAll(samples.slice(offset until offset + written))
            return written
        }
        override fun play() = Unit
        override fun pause() = Unit
        override fun flush() { queued.clear() }
        override fun release() { released++ }
    }

    private class PausedWriteSink : Sink() {
        val entered = CountDownLatch(1)
        val paused = CountDownLatch(1)
        private var first = true
        override fun transferCount(count: Int): Int {
            if (!first) return count
            first = false
            entered.countDown()
            check(paused.await(5, TimeUnit.SECONDS))
            return 4 // First two stereo frames reached the sink before pause.
        }
        override fun pause() { paused.countDown() }
    }

    @Test
    fun partialAndZeroWritesPreserveEverySampleInOrder() {
        val counts = ArrayDeque(listOf(0, 4, 2, 6))
        val sink = object : Sink() {
            override fun transferCount(count: Int) = counts.removeFirst()
        }
        FrameAudioOutput(sink).use { output ->
            val pcm = ShortArray(12) { it.toShort() }
            output.resume()
            assertTrue(output.writeFrame(pcm, output.timeline))
            assertEquals(pcm.toList(), sink.queued)
            assertEquals(listOf(0, 0, 4, 6), sink.offsets)
        }
        assertEquals(1, sink.released)
    }

    @Test
    fun pauseResumeContinuesTheUnwrittenTail() = withPausedWrite { output, sink, result ->
        assertFalse(result.isDone)
        output.resume()
        assertTrue(result.get(5, TimeUnit.SECONDS))
        assertEquals((0..11).map(Int::toShort), sink.queued)
        assertEquals(listOf(0, 4), sink.offsets)
    }

    @Test
    fun restoredTimelineDiscardsQueuedAndPendingOldAudio() = withPausedWrite { output, sink, result ->
        val previous = output.timeline
        output.discardTimeline()
        assertNotEquals(previous, output.timeline)
        assertFalse(result.get(5, TimeUnit.SECONDS))
        assertTrue(sink.queued.isEmpty())
        output.resume()
        val fresh = shortArrayOf(100, 101, 102, 103)
        assertTrue(output.writeFrame(fresh, output.timeline))
        assertEquals(fresh.toList(), sink.queued)
        assertFalse(output.writeFrame(shortArrayOf(8, 9), previous))
        assertEquals(fresh.toList(), sink.queued)
    }

    @Test
    fun closeWakesAPausedProducerWithoutWritingItsTail() = withPausedWrite { output, sink, result ->
        output.close()
        assertFalse(result.get(5, TimeUnit.SECONDS))
        assertEquals(listOf<Short>(0, 1, 2, 3), sink.queued)
        assertEquals(1, sink.released)
    }

    @Test
    fun failedAndImpossibleSinkCountsAreNotReportedAsDelivered() {
        for (count in listOf(-6, 100)) {
            FrameAudioOutput(object : Sink() {
                override fun transferCount(requested: Int) = count
            }).use { output ->
                output.resume()
                assertThrows(IllegalStateException::class.java) {
                    output.writeFrame(shortArrayOf(1, 2), output.timeline)
                }
            }
        }
    }

    private fun withPausedWrite(
        check: (FrameAudioOutput, PausedWriteSink, java.util.concurrent.Future<Boolean>) -> Unit
    ) {
        val sink = PausedWriteSink()
        val output = FrameAudioOutput(sink)
        val worker = Executors.newSingleThreadExecutor()
        try {
            output.resume()
            val timeline = output.timeline
            val result = worker.submit(Callable {
                output.writeFrame(ShortArray(12) { it.toShort() }, timeline)
            })
            assertTrue(sink.entered.await(5, TimeUnit.SECONDS))
            output.pause()
            assertEquals(listOf<Short>(0, 1, 2, 3), sink.queued)
            check(output, sink, result)
        } finally {
            output.close()
            worker.shutdown()
            assertTrue(worker.awaitTermination(5, TimeUnit.SECONDS))
        }
        assertEquals(1, sink.released)
    }
}
