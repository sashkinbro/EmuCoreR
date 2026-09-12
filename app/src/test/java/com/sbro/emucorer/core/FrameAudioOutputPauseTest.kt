package com.sbro.emucorer.core

import java.util.concurrent.Callable
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class FrameAudioOutputPauseTest {
    private open class BlockingSink : PcmSink {
        val queued = mutableListOf<Short>()
        val entered = CountDownLatch(1)
        private val released = CountDownLatch(1)
        private var firstWrite = true
        var releaseCount = 0
            private set

        override fun write(samples: ShortArray, offset: Int, count: Int): Int {
            if (firstWrite) {
                firstWrite = false
                entered.countDown()
                check(released.await(5, TimeUnit.SECONDS))
                queued.addAll(samples.slice(offset until offset + 4))
                return 4
            }
            queued.addAll(samples.slice(offset until offset + count))
            return count
        }

        override fun play() = Unit
        override fun pause() {
            released.countDown()
        }

        override fun flush() {
            queued.clear()
        }

        override fun release() {
            releaseCount++
        }
    }

    @Test
    fun pausedProducerRunsOwnerWorkUntilResume() {
        val sink = BlockingSink()
        val serviced = AtomicInteger()
        val output = FrameAudioOutput(sink, onPaused = { serviced.incrementAndGet() })
        val worker = Executors.newSingleThreadExecutor()
        try {
            output.resume()
            val timeline = output.timeline
            val result = worker.submit(Callable {
                output.writeFrame(ShortArray(12) { it.toShort() }, timeline)
            })
            assertTrue(sink.entered.await(5, TimeUnit.SECONDS))
            output.pause()
            val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5)
            while (serviced.get() == 0 && System.nanoTime() < deadline) Thread.sleep(1)
            assertTrue(serviced.get() > 0)
            assertFalse(result.isDone)
            output.resume()
            assertTrue(result.get(5, TimeUnit.SECONDS))
            assertEquals((0..11).map(Int::toShort), sink.queued)
        } finally {
            output.close()
            worker.shutdown()
            assertTrue(worker.awaitTermination(5, TimeUnit.SECONDS))
        }
        assertEquals(1, sink.releaseCount)
    }
}
