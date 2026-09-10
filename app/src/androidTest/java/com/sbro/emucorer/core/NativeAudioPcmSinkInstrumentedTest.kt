// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: LicenseRef-EmuCoreR-Proprietary
package com.sbro.emucorer.core

import androidx.test.ext.junit.runners.AndroidJUnit4
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class NativeAudioPcmSinkInstrumentedTest {
    private fun awaitCondition(condition: () -> Boolean) {
        val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5)
        while (!condition() && System.nanoTime() < deadline) Thread.sleep(1)
        assertTrue("Timed out waiting for native audio", condition())
    }

    @Test fun nativeCallbackDrainsBoundedPcmAndFlushRemovesPausedTimeline() {
        val sink = CoreRuntime.createPcmSink() as NativeAudioPcmSink
        try {
            val pcm = ShortArray(4096)
            assertTrue(sink.write(pcm, -1, 2) < 0)
            assertTrue(sink.write(pcm, 0, 3) < 0)
            assertTrue(sink.write(pcm, 4095, 2) < 0)
            repeat(8) { assertEquals(2048, sink.write(pcm, 0, pcm.size)) }
            assertEquals(0, sink.write(pcm, 0, pcm.size))
            assertEquals(8192L, sink.stats()!![2])
            sink.play()
            awaitCondition { sink.stats()!![4] >= 2048L }
            sink.pause()
            val paused = sink.stats()!!
            Thread.sleep(30)
            assertArrayEquals(paused, sink.stats()!!)
            sink.flush()
            assertEquals(0L, sink.stats()!![2])
            assertEquals(2048, sink.write(pcm, 0, pcm.size))
            sink.play()
            awaitCondition { sink.stats()!![4] >= paused[4] + 1024 }
        } finally {
            sink.release()
            sink.release()
        }
    }

    @Test fun frameOutputRetainsPartialPcmAndRejectsOldEpochOnRealNativeSink() {
        val sink = CoreRuntime.createPcmSink() as NativeAudioPcmSink
        val output = FrameAudioOutput(sink)
        val executor = Executors.newSingleThreadExecutor()
        try {
            output.resume()
            val oldEpoch = output.timeline
            val producer = executor.submit<Boolean> {
                output.writeFrame(ShortArray(400_000), oldEpoch)
            }
            awaitCondition { sink.stats()!![3] >= 8192 }
            output.pause()
            assertFalse(producer.isDone)
            val delivered = sink.stats()!![4]
            output.discardTimeline()
            assertEquals(0L, sink.stats()!![2])
            output.resume()
            assertFalse(producer.get(5, TimeUnit.SECONDS))
            assertTrue(output.writeFrame(ShortArray(512), output.timeline))
            awaitCondition { sink.stats()!![4] >= delivered + 256 }
        } finally {
            output.close()
            executor.shutdownNow()
            assertTrue(executor.awaitTermination(5, TimeUnit.SECONDS))
        }
    }
}
