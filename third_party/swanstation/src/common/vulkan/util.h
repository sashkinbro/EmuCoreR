// Copyright 2016 Dolphin Emulator Project
// Copyright 2020 DuckStation Emulator Project
// Licensed under GPLv2+
// Refer to the LICENSE file included.

#pragma once

#include "../string.h"
#include "../types.h"
#include "vulkan_loader.h"
#include <algorithm>
#include <array>
#include <cstdarg>
namespace Vulkan {
namespace Util {

bool IsDepthFormat(VkFormat format);
uint32_t GetTexelSize(VkFormat format);
uint32_t GetBlockSize(VkFormat format);

// Safe destroy helpers
void SafeDestroyFramebuffer(VkFramebuffer& fb);
void SafeDestroyPipeline(VkPipeline& p);
void SafeDestroyPipelineLayout(VkPipelineLayout& pl);
void SafeDestroyDescriptorSetLayout(VkDescriptorSetLayout& dsl);
void SafeDestroyBufferView(VkBufferView& bv);
void SafeDestroyImageView(VkImageView& iv);
void SafeDestroySampler(VkSampler& samp);
void SafeFreeGlobalDescriptorSet(VkDescriptorSet& ds);

void SetViewport(VkCommandBuffer command_buffer, int x, int y, int width, int height, float min_depth = 0.0f,
                 float max_depth = 1.0f);
void SetScissor(VkCommandBuffer command_buffer, int x, int y, int width, int height);

// Combines viewport and scissor updates
void SetViewportAndScissor(VkCommandBuffer command_buffer, int x, int y, int width, int height, float min_depth = 0.0f,
                           float max_depth = 1.0f);

void LogVulkanResult(int level, const char* func_name, VkResult res, const char* msg, ...) printflike(4, 5);

#define LOG_VULKAN_ERROR(res, ...) ::Vulkan::Util::LogVulkanResult(1, __func__, res, __VA_ARGS__)

} // namespace Util

} // namespace Vulkan
