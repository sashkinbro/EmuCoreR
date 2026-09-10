#pragma once

// GLAD has to come first so that Qt doesn't pull in the system GL headers, which are incompatible with glad.
#include <glad.h>

// Hack to prevent Apple's glext.h headers from getting included via qopengl.h, since we still want to use glad.
#ifdef __APPLE__
#define __glext_h_
#endif

#include "common/gl/program.h"
#include "common/gl/shader_cache.h"
#include "common/gl/stream_buffer.h"
#include "common/gl/texture.h"
#include "core/host_display.h"
#include "glad.h"
#include "gpu_hw.h"
#include "gpu_hw_shadergen.h"
#include "texture_replacements.h"
#include <array>
#include <memory>
#include <tuple>
#include <string>
#include <libretro.h>

class LibretroOpenGLHostDisplay final : public HostDisplay
{
public:
  LibretroOpenGLHostDisplay();
  ~LibretroOpenGLHostDisplay();

  static bool RequestHardwareRendererContext(retro_hw_render_callback* cb, bool prefer_gles);

  RenderAPI GetRenderAPI() const override;
  void* GetRenderDevice() const override;
  void* GetRenderContext() const override;

  bool CreateRenderDevice(const WindowInfo& wi, std::string_view adapter_name, bool debug_device,
                          bool threaded_presentation) override;
  bool InitializeRenderDevice(std::string_view shader_cache_directory, bool debug_device,
                              bool threaded_presentation) override;
  void DestroyRenderDevice() override;

  void ResizeRenderWindow(int32_t new_window_width, int32_t new_window_height) override;

  bool ChangeRenderWindow(const WindowInfo& new_wi) override;

  std::unique_ptr<HostDisplayTexture> CreateTexture(uint32_t width, uint32_t height, uint32_t layers, uint32_t levels, uint32_t samples,
                                                    HostDisplayPixelFormat format, const void* data, uint32_t data_stride,
                                                    bool dynamic = false) override;
  bool SupportsDisplayPixelFormat(HostDisplayPixelFormat format) const override;
  bool BeginSetDisplayPixels(HostDisplayPixelFormat format, uint32_t width, uint32_t height, void** out_buffer,
                             uint32_t* out_pitch) override;
  void EndSetDisplayPixels() override;
  bool SetDisplayPixels(HostDisplayPixelFormat format, uint32_t width, uint32_t height, const void* buffer, uint32_t pitch) override;

  bool Render() override;

protected:
  bool CreateResources() override;
  void DestroyResources() override;
  void RenderSoftwareCursor(int32_t left, int32_t top, int32_t width, int32_t height, HostDisplayTexture* texture_handle);

  void RenderDisplay(int32_t left, int32_t bottom, int32_t width, int32_t height, void* texture_handle, uint32_t texture_width,
                     int32_t texture_height, int32_t texture_view_x, int32_t texture_view_y, int32_t texture_view_width,
                     int32_t texture_view_height);

private:
  const char* GetGLSLVersionString() const;
  std::string GetGLSLVersionHeader() const;

  GL::Program m_display_program;
  GL::Program m_cursor_program;
  GLuint m_display_vao = 0;
  GLuint m_display_nearest_sampler = 0;
  GLuint m_display_linear_sampler = 0;
  GLuint m_uniform_buffer_alignment = 1;

  GLuint m_display_pixels_texture_id = 0;
  std::unique_ptr<GL::StreamBuffer> m_display_pixels_texture_pbo;
  uint32_t m_display_pixels_texture_pbo_map_offset = 0;
  uint32_t m_display_pixels_texture_pbo_map_size = 0;
  std::vector<uint8_t> m_gles_pixels_repack_buffer;

  bool m_is_gles = false;
};

class GPU_HW_OpenGL : public GPU_HW
{
public:
  GPU_HW_OpenGL();
  ~GPU_HW_OpenGL() override;

  bool Initialize(HostDisplay* host_display) override;
  void Reset(bool clear_vram) override;
  bool DoState(StateWrapper& sw, HostDisplayTexture** host_texture, bool update_display) override;

  void ResetGraphicsAPIState() override;
  void RestoreGraphicsAPIState() override;
  void UpdateSettings() override;

protected:
  void ClearDisplay() override;
  void UpdateDisplay() override;
  void ReadVRAM(uint32_t x, uint32_t y, uint32_t width, uint32_t height) override;
  void FillVRAM(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t color) override;
  void UpdateVRAM(uint32_t x, uint32_t y, uint32_t width, uint32_t height, const void* data, bool set_mask, bool check_mask) override;
  void CopyVRAM(uint32_t src_x, uint32_t src_y, uint32_t dst_x, uint32_t dst_y, uint32_t width, uint32_t height) override;
  void UpdateVRAMReadTexture() override;
  void UpdateDepthBufferFromMaskBit() override;
  void ClearDepthBuffer() override;
  void SetScissorFromDrawingArea() override;
  void MapBatchVertexPointer(uint32_t required_vertices) override;
  void UnmapBatchVertexPointer(uint32_t used_vertices) override;
  void UploadUniformBuffer(const void* data, uint32_t data_size) override;
  void DrawBatchVertices(BatchRenderMode render_mode, uint32_t base_vertex, uint32_t num_vertices) override;

private:
  struct GLStats
  {
    uint32_t num_batches;
    uint32_t num_vertices;
    uint32_t num_vram_reads;
    uint32_t num_vram_writes;
    uint32_t num_vram_read_texture_updates;
    uint32_t num_uniform_buffer_updates;
  };

  ALWAYS_INLINE bool IsGLES() const { return (m_render_api == HostDisplay::RenderAPI::OpenGLES); }

  void SetCapabilities();
  bool CreateFramebuffer();
  void ClearFramebuffer();
  void CopyFramebufferForState(GLenum target, GLuint src_texture, uint32_t src_fbo, uint32_t src_x, uint32_t src_y, GLuint dst_texture,
                               uint32_t dst_fbo, uint32_t dst_x, uint32_t dst_y, uint32_t width, uint32_t height);

  bool CreateVertexBuffer();
  bool CreateUniformBuffer();
  bool CreateTextureBuffer();

  bool CompilePrograms();
  bool CompileDownsampleProgram();
  bool RebuildDisplayPrograms();

  // Fast-path companion to CompilePrograms for the dim-cache
  // filter-only flip in UpdateSettings: walks just the current
  // m_texture_filtering sub-cube of m_render_programs, without
  // rebuilding the filter-independent non-batch programs
  // (display / vram_fill / vram_read / vram_write / vram_copy /
  // vram_update_depth / downsample) or the m_use_binding_layout
  // flag. All those stay valid through a filter toggle because
  // none of them bake filter into their GLSL source - filter
  // only affects the batch FS via FilteredSampleFromVRAM. Caller
  // must have already populated m_shader_cache and m_shadergen.
  // Called from CompilePrograms itself for the initial / full-
  // rebuild path, and directly from UpdateSettings's
  // only_dim_changed branch to bypass the non-batch rebuild on
  // warm filter cycling.
  bool PrecompileBatchPrograms(ShaderCompileProgressTracker& progress);

  // Lazy batch-program compile path. The libretro hardware-renderer
  // protocol gives us a single GL context bound to the runloop
  // thread; there's no way to compile shaders on a background
  // worker (no second context, no glShareLists hand-off), so
  // unlike the D3D11/D3D12/Vulkan backends OpenGL's 'Lazy' mode
  // degrades to 'Disabled' - compile each combination on the
  // runloop the first time the game dispatches a draw using it.
  // No threading, no atomics, no mutex; everything runs on one
  // thread.
  //
  // The matrix slots start as default-constructed GL::Programs
  // (program id 0); GetBatchProgram fills a slot on demand,
  // applying the Reserved_*Direct16Bit dedup at the matrix level
  // by re-linking the program from the canonical mode's source
  // string (the shader cache makes the second link cheap).
  //
  // filter is the outermost cache dimension. With the cbuffer-
  // refactor patch (7b575a3) the GLSL is invariant under
  // resolution_scale / true_color / scaled_dithering, but it is
  // STILL dependent on texture filter - the FilteredSampleFromVRAM
  // helper is emitted differently for Nearest / Bilinear / JINC2 /
  // xBR / the BinAlpha variants. Dimensioning the cache over
  // filter lets a filter toggle skip the DestroyShaders-equivalent
  // CompilePrograms round trip in UpdateSettings: the previous
  // filter's sub-cube remains valid and reachable, switching back
  // to it later is just a slot validity check. Mirrors the D3D12
  // / D3D11 dim caches from 10c53b8 / 00cf11f and the Vulkan dim
  // cache from the glslang-elimination series. The slow-path
  // shadergen is rebuilt per-call against the requested filter
  // so the helper stays consistent with its sibling backends even
  // though the single-threaded GL context never reaches it with a
  // non-current filter today.
  const GL::Program* GetBatchProgram(GPUTextureFilter filter, uint8_t render_mode, uint8_t texture_mode, bool dithering, bool interlacing);

  void SetDepthFunc();
  void SetDepthFunc(GLenum func);
  void SetBlendMode();

  bool BlitVRAMReplacementTexture(const TextureReplacementTexture* tex, uint32_t dst_x, uint32_t dst_y, uint32_t width, uint32_t height);
  void DownsampleFramebuffer(GL::Texture& source, uint32_t left, uint32_t top, uint32_t width, uint32_t height);
  void DownsampleFramebufferBoxFilter(GL::Texture& source, uint32_t left, uint32_t top, uint32_t width, uint32_t height);

  // downsample texture - used for readbacks at >1xIR.
  GL::Texture m_vram_texture;
  GL::Texture m_vram_depth_texture;
  GL::Texture m_vram_read_texture;
  GL::Texture m_vram_encoding_texture;
  GL::Texture m_display_texture;
  GL::Texture m_vram_write_replacement_texture;

  std::unique_ptr<GL::StreamBuffer> m_vertex_stream_buffer;
  GLuint m_vram_fbo_id = 0;
  GLuint m_vao_id = 0;
  GLuint m_attributeless_vao_id = 0;
  GLuint m_state_copy_fbo_id = 0;

  std::unique_ptr<GL::StreamBuffer> m_uniform_stream_buffer;

  std::unique_ptr<GL::StreamBuffer> m_texture_stream_buffer;
  GLuint m_texture_buffer_r16ui_texture = 0;

  std::array<std::array<std::array<std::array<std::array<GL::Program, 2>, 2>, 9>, 4>, 7>
    m_render_programs;                                          // [filter][render_mode][texture_mode][dithering][interlacing]
  std::array<std::array<GL::Program, 3>, 2> m_display_programs; // [depth_24][interlaced]
  std::array<std::array<GL::Program, 2>, 2> m_vram_fill_programs;
  GL::Program m_vram_read_program;
  GL::Program m_vram_write_program;
  GL::Program m_vram_copy_program;
  GL::Program m_vram_update_depth_program;

  // Persistent shader cache, shadergen, and the
  // use_binding_layout flag the cold-start CompilePrograms picked
  // up. All three used to be locals in CompilePrograms(); they
  // now outlive that function so GetBatchProgram can call into
  // them at draw time on a lazy miss. On UpdateSettings round-
  // trips through CompilePrograms the cache instance persists
  // (Open is guarded by the new GL::ShaderCache::IsOpen()
  // accessor) so the in-memory index isn't re-read from disk each
  // time. The shadergen is rebuilt each CompilePrograms because
  // its baked-in settings may have changed.
  GL::ShaderCache m_shader_cache;
  std::unique_ptr<GPU_HW_ShaderGen> m_shadergen;
  bool m_use_binding_layout = false;

  uint32_t m_uniform_buffer_alignment = 1;
  uint32_t m_texture_stream_buffer_size = 0;

  bool m_use_texture_buffer_for_vram_writes = false;
  bool m_use_ssbo_for_vram_writes = false;

  GLenum m_current_depth_test = 0;
  GPUTransparencyMode m_current_transparency_mode = GPUTransparencyMode::Disabled;
  BatchRenderMode m_current_render_mode = BatchRenderMode::TransparencyDisabled;

  GL::Texture m_downsample_texture;
  GL::Program m_downsample_program;
};
