
package com.sbro.emucorer.core


/** The native callback never enters Java. Short/zero writes use FrameAudioOutput's
 * existing suffix retention and interruptible wait, not another guest frame.
 * Synchronization owns the handle lifetime; native writes are bounded/nonblocking.
 */
internal class NativeAudioPcmSink : PcmSink {
    private val bridge = NativeCoreBridge()
    private var handle = createHandle()
    private var playing = false

    private fun createHandle(): Long = bridge.createAudioOutput().also {
        check(it != 0L) { "Could not open native AAudio output" }
    }

    /** AAudio reports disconnect asynchronously. Recovery belongs to this
     * owner thread: the callback only latches the error and stops itself.
     * Device-buffer and queued host PCM are obsolete after a route loss.
     */
    private fun recreate(start: Boolean) {
        val old = handle
        handle = 0L
        if (old != 0L) bridge.destroyAudioOutput(old)
        val replacement = createHandle()
        handle = replacement
        if (start && bridge.startAudioOutput(replacement) != 0) {
            handle = 0L
            bridge.destroyAudioOutput(replacement)
            error("AAudio recovery start failed")
        }
        playing = start
    }

    @Synchronized override fun write(samples: ShortArray, offset: Int, count: Int): Int {
        check(handle != 0L) { "AAudio output is closed" }
        if (offset < 0 || count < 0 || count and 1 != 0 || offset > samples.size - count)
            return -1 // EMUCORER_ERR_INVALID_ARGUMENT; do not rebuild a healthy stream.
        val result = bridge.writeAudioOutput(handle, samples, offset, count)
        if (result != -1) return result // EMUCORER_ERR_INTERNAL means backend loss/failure.
        recreate(playing)
        return bridge.writeAudioOutput(handle, samples, offset, count)
    }
    @Synchronized override fun play() {
        check(handle != 0L) { "AAudio output is closed" }
        if (bridge.startAudioOutput(handle) != 0) recreate(start = true)
        playing = true
    }
    @Synchronized override fun pause() {
        check(handle != 0L) { "AAudio output is closed" }
        if (bridge.pauseAudioOutput(handle) != 0) recreate(start = false)
        playing = false
    }
    @Synchronized override fun flush() {
        check(handle != 0L) { "AAudio output is closed" }
        if (bridge.flushAudioOutput(handle) != 0) recreate(start = false)
    }
    @Synchronized override fun release() {
        if (handle != 0L) {
            bridge.destroyAudioOutput(handle)
            handle = 0L
        }
        playing = false
    }
    @Synchronized override fun stats(): LongArray? =
        if (handle == 0L) null else bridge.audioOutputStats(handle)
}
