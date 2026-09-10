#pragma once
#include "core/types.h"
#include <bitset>
#include <optional>
#include <string>
#include <memory>

class ByteStream;

namespace GameSettings {
enum class Trait : uint32_t
{
  ForceInterpreter,
  ForceSoftwareRenderer,
  ForceSoftwareRendererForReadbacks,
  ForceInterlacing,
  DisableTrueColor,
  DisableUpscaling,
  DisableScaledDithering,
  DisableForceNTSCTimings,
  DisableWidescreen,
  DisablePGXP,
  DisablePGXPCulling,
  DisablePGXPTextureCorrection,
  DisablePGXPColorCorrection,
  DisablePGXPDepthBuffer,
  ForcePGXPVertexCache,
  ForcePGXPCPUMode,
  ForceRecompilerMemoryExceptions,
  ForceRecompilerICache,
  ForceRecompilerLUTFastmem,
  ForceOldAudioHook,

  Count
};

struct Entry
{
  std::bitset<static_cast<int>(Trait::Count)> traits{};
  std::optional<int16_t> display_active_start_offset;
  std::optional<int16_t> display_active_end_offset;
  std::optional<int8_t> display_line_start_offset;
  std::optional<int8_t> display_line_end_offset;
  std::optional<uint32_t> dma_max_slice_ticks;
  std::optional<uint32_t> dma_halt_ticks;
  std::optional<uint32_t> gpu_fifo_size;
  std::optional<uint32_t> gpu_max_run_ahead;
  std::optional<float> gpu_pgxp_tolerance;
  std::optional<float> gpu_pgxp_depth_threshold;

  // user settings
  std::optional<uint32_t> runahead_frames;
  std::optional<uint32_t> cpu_overclock_numerator;
  std::optional<uint32_t> cpu_overclock_denominator;
  std::optional<bool> cpu_overclock_enable;
  std::optional<bool> enable_8mb_ram;
  std::optional<uint32_t> cdrom_read_speedup;
  std::optional<uint32_t> cdrom_seek_speedup;
  std::optional<DisplayCropMode> display_crop_mode;
  std::optional<DisplayAspectRatio> display_aspect_ratio;
  std::optional<GPURenderer> gpu_renderer;
  std::optional<bool> gpu_use_software_renderer_for_readbacks;
  std::optional<GPUDownsampleMode> gpu_downsample_mode;
  std::optional<bool> display_force_4_3_for_24bit;
  std::optional<uint16_t> display_aspect_ratio_custom_numerator;
  std::optional<uint16_t> display_aspect_ratio_custom_denominator;
  std::optional<uint32_t> gpu_resolution_scale;
  std::optional<uint32_t> gpu_multisamples;
  std::optional<bool> gpu_per_sample_shading;
  std::optional<bool> gpu_true_color;
  std::optional<bool> gpu_scaled_dithering;
  std::optional<bool> gpu_force_ntsc_timings;
  std::optional<GPUTextureFilter> gpu_texture_filter;
  std::optional<bool> gpu_widescreen_hack;
  std::optional<bool> gpu_pgxp;
  std::optional<bool> gpu_pgxp_projection_precision;
  std::optional<bool> gpu_pgxp_depth_buffer;
  std::optional<MultitapMode> multitap_mode;
  std::optional<ControllerType> controller_1_type;
  std::optional<ControllerType> controller_2_type;
  std::optional<MemoryCardType> memory_card_1_type;
  std::optional<MemoryCardType> memory_card_2_type;

  ALWAYS_INLINE bool HasTrait(Trait trait) const { return traits[static_cast<int>(trait)]; }
  ALWAYS_INLINE void AddTrait(Trait trait) { traits[static_cast<int>(trait)] = true; }

  void ApplySettings(bool display_osd_messages) const;
};

}; // namespace GameSettings

std::unique_ptr<GameSettings::Entry> GetSettingsForGame(const std::string& game_code);
