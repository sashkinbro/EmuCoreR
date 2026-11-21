// Copyright 2016 Dolphin Emulator Project
// Copyright 2020 DuckStation Emulator Project
// Licensed under GPLv2+
// Refer to the LICENSE file included.

#pragma once
#include "../types.h"
#include "vulkan_loader.h"
#include <algorithm>
#include <memory>

namespace Vulkan {
class Texture
{
public:
  Texture();
  Texture(Texture&& move);
  Texture(const Texture&) = delete;
  ~Texture();

  Texture& operator=(Texture&& move);
  Texture& operator=(const Texture&) = delete;

  ALWAYS_INLINE bool IsValid() const { return (m_image != VK_NULL_HANDLE); }

  ALWAYS_INLINE uint32_t GetWidth() const { return m_width; }
  ALWAYS_INLINE uint32_t GetHeight() const { return m_height; }
  ALWAYS_INLINE uint32_t GetLevels() const { return m_levels; }
  ALWAYS_INLINE uint32_t GetLayers() const { return m_layers; }
  ALWAYS_INLINE uint32_t GetMipWidth(uint32_t level) const { return std::max<uint32_t>(m_width >> level, 1u); }
  ALWAYS_INLINE uint32_t GetMipHeight(uint32_t level) const { return std::max<uint32_t>(m_height >> level, 1u); }
  ALWAYS_INLINE VkFormat GetFormat() const { return m_format; }
  ALWAYS_INLINE VkSampleCountFlagBits GetSamples() const { return m_samples; }
  ALWAYS_INLINE VkImageLayout GetLayout() const { return m_layout; }
  ALWAYS_INLINE VkImage GetImage() const { return m_image; }
  ALWAYS_INLINE VkImageView GetView() const { return m_view; }

  bool Create(uint32_t width, uint32_t height, uint32_t levels, uint32_t layers, VkFormat format, VkSampleCountFlagBits samples,
              VkImageViewType view_type, VkImageTiling tiling, VkImageUsageFlags usage);

  void Destroy(bool defer = true);

  // Used when the render pass is changing the image layout, or to force it to
  // VK_IMAGE_LAYOUT_UNDEFINED, if the existing contents of the image is
  // irrelevant and will not be loaded.
  void OverrideImageLayout(VkImageLayout new_layout);

  void TransitionToLayout(VkCommandBuffer command_buffer, VkImageLayout new_layout);
  void TransitionSubresourcesToLayout(VkCommandBuffer command_buffer, uint32_t start_level, uint32_t num_levels, uint32_t start_layer,
                                      uint32_t num_layers, VkImageLayout old_layout, VkImageLayout new_layout);

  VkFramebuffer CreateFramebuffer(VkRenderPass render_pass);

  void UpdateFromBuffer(VkCommandBuffer cmdbuf, uint32_t level, uint32_t layer, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                        VkBuffer buffer, uint32_t buffer_offset, uint32_t row_length);

private:
  uint32_t m_width = 0;
  uint32_t m_height = 0;
  uint32_t m_levels = 0;
  uint32_t m_layers = 0;
  VkFormat m_format = VK_FORMAT_UNDEFINED;
  VkSampleCountFlagBits m_samples = VK_SAMPLE_COUNT_1_BIT;
  VkImageViewType m_view_type = VK_IMAGE_VIEW_TYPE_2D;
  VkImageLayout m_layout = VK_IMAGE_LAYOUT_UNDEFINED;

  VkImage m_image = VK_NULL_HANDLE;
  VkDeviceMemory m_device_memory = VK_NULL_HANDLE;
  VkImageView m_view = VK_NULL_HANDLE;
};

} // namespace Vulkan
