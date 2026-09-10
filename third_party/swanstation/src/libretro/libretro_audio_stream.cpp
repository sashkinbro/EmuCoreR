#include "libretro_audio_stream.h"
#include "core/host_interface.h"
#include <algorithm>

void LibretroAudioStream::BeginWrite(SampleType** buffer_ptr, uint32_t* num_frames)
{
  // Single-threaded by design (see header); no synchronisation needed.
  //
  // Non-blocking: tell the SPU what contiguous space is currently
  // available, capped at one slice (BUFFER_SIZE). EndWrite records the
  // count the caller actually filled. The SPU's loop in spu.cpp
  // Execute() handles short returns - which only occur when the FIFO
  // wrapped past the end of its backing array since the last drain -
  // by re-issuing BeginWrite/EndWrite until all generated frames are
  // consumed.
  *buffer_ptr = m_buffer.GetWritePointer();
  *num_frames = std::min(BUFFER_SIZE, m_buffer.GetContiguousSpace() / CHANNELS);
}

void LibretroAudioStream::EndWrite(uint32_t num_frames)
{
  m_buffer.AdvanceTail(num_frames * CHANNELS);

  if (m_silent_mode)
  {
    // Internal-runahead replay: drop everything immediately so the
    // FIFO doesn't fill up across the replay window. We're still
    // ticking the SPU for state correctness, just discarding the
    // resulting audio.
    m_buffer.Remove(m_buffer.GetSize());
    return;
  }

  // No-op in fast-hook mode (the default). The frontend is fed once
  // per retro_run_frame() via UploadToFrontend(), which keeps audio
  // batched and predictable. Calling the frontend per SPU update (the
  // slow-hook path below) is preserved for parity but should be
  // considered a legacy / debugging mode: it produces dozens to
  // hundreds of tiny batch calls per frame, increases callback
  // overhead, and the resulting audio chunking is sensitive to host
  // scheduling jitter on the libretro side.
  if (g_settings.audio_fast_hook)
    return;

  // Slow-hook drain. Same skip-but-still-drain rule as
  // UploadToFrontend; see comment there.
  while (true)
  {
    const uint32_t num_samples = m_buffer.GetContiguousSize();
    if (num_samples == 0)
      break;

    if (!g_retro_skip_audio_this_frame)
      g_retro_audio_sample_batch_callback(m_buffer.GetReadPointer(),
                                          num_samples / CHANNELS);
    m_buffer.Remove(num_samples);
  }
}

void LibretroAudioStream::UploadToFrontend()
{
  // Drain the FIFO straight into the libretro batch callback. The FIFO
  // is a circular buffer, so up to two contiguous regions may be
  // present when the producer wrapped around since the last drain.
  // Most frames are one region (one callback); wraparound costs one
  // extra callback.
  //
  // Previously this routine bounce-copied every sample through a
  // 32 KiB (65 KiB after the int16_t promotion) std::array on the stack
  // and made exactly one callback. The intermediate copy is
  // unnecessary - the libretro frontend reads samples synchronously
  // from the buffer we pass it, and the FIFO storage stays valid until
  // Remove() is called below.
  //
  // When the frontend has cleared RETRO_AV_ENABLE_AUDIO for this frame
  // (single-instance runahead skip frame, fast-forward, etc.), we
  // still need to drain the FIFO so the SPU side doesn't keep
  // accumulating backpressure across the runahead window - we just
  // don't forward anything to the frontend.
  while (true)
  {
    const uint32_t num_samples = m_buffer.GetContiguousSize();
    if (num_samples == 0)
      break;

    if (!g_retro_skip_audio_this_frame)
      g_retro_audio_sample_batch_callback(m_buffer.GetReadPointer(),
                                          num_samples / CHANNELS);
    m_buffer.Remove(num_samples);
  }
}
