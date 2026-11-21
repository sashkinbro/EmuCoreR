// Copyright 2016 Dolphin Emulator Project
// Copyright 2020 DuckStation Emulator Project
// Licensed under GPLv2+
// Refer to the LICENSE file included.

#pragma once
#include "staging_buffer.h"
#include "texture.h"

namespace Vulkan {

class StagingTexture final
{
public:
  StagingTexture();
  StagingTexture(StagingTexture&& move);
  StagingTexture(const StagingTexture&) = delete;
  ~StagingTexture();

  StagingTexture& operator=(StagingTexture&& move);
  StagingTexture& operator=(const StagingTexture&) = delete;

  ALWAYS_INLINE bool IsValid() const { return m_staging_buffer.IsValid(); }
  ALWAYS_INLINE const char* GetMappedPointer() const { return m_staging_buffer.GetMapPointer(); }
  ALWAYS_INLINE char* GetMappedPointer() { return m_staging_buffer.GetMapPointer(); }
  ALWAYS_INLINE uint32_t GetMappedStride() const { return m_map_stride; }
  ALWAYS_INLINE uint32_t GetWidth() const { return m_width; }
  ALWAYS_INLINE uint32_t GetHeight() const { return m_height; }

  bool Create(StagingBuffer::Type type, VkFormat format, uint32_t width, uint32_t height);
  void Destroy(bool defer = true);

  // Copies from the GPU texture object to the staging texture, which can be mapped/read by the CPU.
  // Both src_rect and dst_rect must be with within the bounds of the the specified textures.
  void CopyFromTexture(VkCommandBuffer command_buffer, Texture& src_texture, uint32_t src_x, uint32_t src_y, uint32_t src_layer,
                       uint32_t src_level, uint32_t dst_x, uint32_t dst_y, uint32_t width, uint32_t height);
  void CopyFromTexture(Texture& src_texture, uint32_t src_x, uint32_t src_y, uint32_t src_layer, uint32_t src_level, uint32_t dst_x, uint32_t dst_y,
                       uint32_t width, uint32_t height);

  // Wrapper for copying a whole layer of a texture to a readback texture.
  // Assumes that the level of src texture and this texture have the same dimensions.
  void CopyToTexture(VkCommandBuffer command_buffer, uint32_t src_x, uint32_t src_y, Texture& dst_texture, uint32_t dst_x, uint32_t dst_y,
                     uint32_t dst_layer, uint32_t dst_level, uint32_t width, uint32_t height);
  void CopyToTexture(uint32_t src_x, uint32_t src_y, Texture& dst_texture, uint32_t dst_x, uint32_t dst_y, uint32_t dst_layer, uint32_t dst_level,
                     uint32_t width, uint32_t height);

  // Flushes pending writes from the CPU to the GPU, and reads from the GPU to the CPU.
  // This may cause a command buffer flush depending on if one has occurred between the last
  // call to CopyFromTexture()/CopyToTexture() and the Flush() call.
  void Flush();

  // Reads the specified rectangle from the staging texture to out_ptr, with the specified stride
  // (length in bytes of each row). CopyFromTexture must be called first. The contents of any
  // texels outside of the rectangle used for CopyFromTexture is undefined.
  void ReadTexels(uint32_t src_x, uint32_t src_y, uint32_t width, uint32_t height, void* out_ptr, uint32_t out_stride);

  // Copies the texels from in_ptr to the staging texture, which can be read by the GPU, with the
  // specified stride (length in bytes of each row). After updating the staging texture with all
  // changes, call CopyToTexture() to update the GPU copy.
  void WriteTexels(uint32_t dst_x, uint32_t dst_y, uint32_t width, uint32_t height, const void* in_ptr, uint32_t in_stride);

private:
  void PrepareForAccess();

  StagingBuffer m_staging_buffer;
  uint64_t m_flush_fence_counter = 0;
  uint32_t m_width = 0;
  uint32_t m_height = 0;
  uint32_t m_texel_size = 0;
  uint32_t m_map_stride = 0;
  bool m_needs_flush = false;
};

} // namespace Vulkan