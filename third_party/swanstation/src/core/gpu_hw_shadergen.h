#pragma once
#include "gpu_hw.h"
#include "shadergen.h"

class GPU_HW_ShaderGen : public ShaderGen
{
public:
  GPU_HW_ShaderGen(HostDisplay::RenderAPI render_api, uint32_t resolution_scale, uint32_t multisamples, bool per_sample_shading,
                   bool true_color, bool scaled_dithering, GPUTextureFilter texture_filtering, bool uv_limits,
                   bool pgxp_depth, bool disable_color_perspective, bool supports_dual_source_blend);
  ~GPU_HW_ShaderGen();

  std::string GenerateBatchVertexShader(bool textured);
  std::string GenerateBatchFragmentShader(GPU_HW::BatchRenderMode transparency, GPUTextureMode texture_mode,
                                          bool dithering, bool interlacing);
  std::string GenerateDisplayFragmentShader(bool depth_24bit, GPU_HW::InterlacedRenderMode interlace_mode,
                                            bool smooth_chroma);
  std::string GenerateVRAMReadFragmentShader();
  std::string GenerateVRAMWriteFragmentShader(bool use_ssbo);
  std::string GenerateVRAMCopyFragmentShader();
  std::string GenerateVRAMFillFragmentShader(bool wrapped, bool interlaced);
  std::string GenerateVRAMUpdateDepthFragmentShader();

  std::string GenerateAdaptiveDownsampleMipFragmentShader(bool first_pass);
  std::string GenerateAdaptiveDownsampleBlurFragmentShader();
  std::string GenerateAdaptiveDownsampleCompositeFragmentShader();
  std::string GenerateBoxSampleDownsampleFragmentShader();

private:
  ALWAYS_INLINE bool UsingMSAA() const { return m_multisamples > 1; }
  ALWAYS_INLINE bool UsingPerSampleShading() const { return m_multisamples > 1 && m_per_sample_shading; }

  void WriteCommonFunctions(std::stringstream& ss, bool batch_uniform_buffer = false);
  void WriteBatchUniformBuffer(std::stringstream& ss);

  // Emit the #define aliases that route RESOLUTION_SCALE / VRAM_SIZE /
  // RCP_VRAM_SIZE through u_resolution_scale in the shader's currently-
  // in-scope cbuffer. Used by both WriteBatchUniformBuffer (where the
  // cbuffer is the batch UBO) and by non-batch shaders that have added
  // u_resolution_scale to their own per-shader UBO. Must be called
  // AFTER the cbuffer declaration so u_resolution_scale is in scope.
  void WriteCBufferResolutionScaleAliases(std::stringstream& ss);

  void WriteBatchTextureFilter(std::stringstream& ss, GPUTextureFilter texture_filter);

  uint32_t m_resolution_scale;
  uint32_t m_multisamples;
  bool m_per_sample_shading;
  bool m_true_color;
  bool m_scaled_dithering;
  GPUTextureFilter m_texture_filter;
  bool m_uv_limits;
  bool m_pgxp_depth;
  bool m_disable_color_perspective;
};
