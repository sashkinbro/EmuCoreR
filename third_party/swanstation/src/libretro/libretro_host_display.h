#pragma once
#include "core/host_display.h"
#include <libretro.h>

class LibretroHostDisplay final : public HostDisplay
{
public:
  LibretroHostDisplay();
  ~LibretroHostDisplay();

  RenderAPI GetRenderAPI() const override;
  void* GetRenderDevice() const override;
  void* GetRenderContext() const override;

  bool CreateRenderDevice(const WindowInfo& wi, std::string_view adapter_name, bool debug_device,
                          bool threaded_presentation) override;
  bool InitializeRenderDevice(std::string_view shader_cache_directory, bool debug_device,
                              bool threaded_presentation) override;
  void DestroyRenderDevice() override;

  bool ChangeRenderWindow(const WindowInfo& wi) override;
  void ResizeRenderWindow(int32_t new_window_width, int32_t new_window_height) override;
  bool CreateResources() override;
  void DestroyResources() override;

  std::unique_ptr<HostDisplayTexture> CreateTexture(uint32_t width, uint32_t height, uint32_t layers, uint32_t levels, uint32_t samples,
                                                    HostDisplayPixelFormat format, const void* data, uint32_t data_stride,
                                                    bool dynamic = false) override;
  bool Render() override;

  bool SupportsDisplayPixelFormat(HostDisplayPixelFormat format) const override;

  bool BeginSetDisplayPixels(HostDisplayPixelFormat format, uint32_t width, uint32_t height, void** out_buffer,
                             uint32_t* out_pitch) override;
  void EndSetDisplayPixels() override;

private:
  bool CheckPixelFormat(retro_pixel_format new_format);

  std::vector<uint32_t> m_frame_buffer;
  uint32_t m_frame_buffer_pitch = 0;
  retro_framebuffer m_software_fb = {};
  retro_pixel_format m_current_pixel_format = RETRO_PIXEL_FORMAT_UNKNOWN;
};
