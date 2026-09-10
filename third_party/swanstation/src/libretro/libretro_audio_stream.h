#pragma once
#include "common/fifo_queue.h"
#include "core/types.h"

// 16-bit signed audio for the libretro core.
//
// Threading: the libretro core is single-threaded by design - the SPU
// runs on the same thread as retro_run() driven by TimingEvents that
// eventually calls UploadToFrontend() at end-of-frame. No
// synchronisation primitive is needed around the FIFO. The previously
// present mutex was always uncontended and only added per-write
// atomic-CAS overhead (~50ns x ~300 SPU updates per frame).
//
// Inheritance: this class used to derive from a generic AudioStream
// base. There has only ever been one runtime audio stream in the
// libretro core - this one - and the second subclass (NullAudioStream)
// was only used to discard audio during internal-runahead replay. The
// "discard" behaviour is folded in here behind SetSilentMode() instead,
// dropping the vtable dispatch from FramesAvailable / EndWrite hot
// paths.
//
// Configuration: the libretro frontend does its own resampling, the
// PSX is structurally 44.1 kHz stereo, and the FIFO drain is paced by
// retro_run() rather than by buffer fill, so the historical Reconfigure
// (input rate / output rate / channels / buffer size) signature carried
// no information the core would ever vary. The constants below are
// therefore baked in at compile time.
class LibretroAudioStream
{
public:
  using SampleType = int16_t;

  // PSX audio is 44.1 kHz stereo. These values are referenced by the
  // libretro retro_get_system_av_info path and by the FIFO sizing
  // below; they are not configurable.
  static constexpr uint32_t SAMPLE_RATE = 44100;
  static constexpr uint32_t CHANNELS = 2;

  // Per-frame draining batch size (in stereo frames). The SPU's
  // UpdateEventInterval() schedules tick events to produce no more
  // than this many frames per slice, which both bounds the worst-case
  // SPU work per timing event and ensures BeginWrite never has to clip
  // a request that would have fit.
  static constexpr uint32_t BUFFER_SIZE = 2048;

  // FIFO backing storage size in samples (= stereo frames * channels).
  // Sized generously above BUFFER_SIZE so the wrap-tolerant
  // GetContiguousSpace() query in BeginWrite is never the limiting
  // factor on a normally-drained frame, and so transient bursts (e.g.
  // skip-audio frames during fast-forward where UploadToFrontend
  // doesn't actually feed the frontend) have headroom to absorb a few
  // slices' worth of samples before pressure shows up.
  static constexpr uint32_t FIFO_CAPACITY = 32768;

  LibretroAudioStream() = default;
  ~LibretroAudioStream() = default;

  // Per-frame slice budget (in stereo frames). Used by the SPU to size
  // its tick-event interval. Constant by construction.
  static constexpr uint32_t GetBufferSize() { return BUFFER_SIZE; }

  void BeginWrite(SampleType** buffer_ptr, uint32_t* num_frames);
  void EndWrite(uint32_t num_frames);

  void UploadToFrontend();

  // Switch the FIFO into a "drop everything" mode used by the internal
  // runahead replay path: the SPU keeps producing samples for state
  // correctness, but nothing reaches the libretro frontend and the
  // FIFO is drained inside EndWrite so backpressure does not build up
  // across the runahead window.
  void SetSilentMode(bool silent) { m_silent_mode = silent; }

private:
  bool m_silent_mode = false;
  HeapFIFOQueue<SampleType, FIFO_CAPACITY> m_buffer;
};
