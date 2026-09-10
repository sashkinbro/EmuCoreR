#pragma once
#include "../types.h"
#include "vulkan_loader.h"
#include <array>

namespace Vulkan {

class DescriptorSetLayoutBuilder
{
public:
  static constexpr uint32_t MAX_BINDINGS = 16;

  DescriptorSetLayoutBuilder();

  void Clear();

  VkDescriptorSetLayout Create(VkDevice device);

  void AddBinding(uint32_t binding, VkDescriptorType dtype, uint32_t dcount, VkShaderStageFlags stages);

private:
  VkDescriptorSetLayoutCreateInfo m_ci{};
  std::array<VkDescriptorSetLayoutBinding, MAX_BINDINGS> m_bindings{};
};

class PipelineLayoutBuilder
{
public:
  static constexpr uint32_t MAX_SETS = 8, MAX_PUSH_CONSTANTS = 1;

  PipelineLayoutBuilder();

  void Clear();

  VkPipelineLayout Create(VkDevice device);

  void AddDescriptorSet(VkDescriptorSetLayout layout);

  void AddPushConstants(VkShaderStageFlags stages, uint32_t offset, uint32_t size);

private:
  VkPipelineLayoutCreateInfo m_ci{};
  std::array<VkDescriptorSetLayout, MAX_SETS> m_sets{};
  std::array<VkPushConstantRange, MAX_PUSH_CONSTANTS> m_push_constants{};
};

class GraphicsPipelineBuilder
{
public:
  static constexpr uint32_t MAX_SHADER_STAGES = 3, MAX_VERTEX_ATTRIBUTES = 16, MAX_VERTEX_BUFFERS = 8, MAX_ATTACHMENTS = 2,
                       MAX_DYNAMIC_STATE = 8;

  GraphicsPipelineBuilder();

  void Clear();

  VkPipeline Create(VkDevice device, VkPipelineCache pipeline_cache = VK_NULL_HANDLE, bool clear = true);

  void SetShaderStage(VkShaderStageFlagBits stage, VkShaderModule module, const char* entry_point,
                      const VkSpecializationInfo* spec_info = nullptr);
  void SetVertexShader(VkShaderModule module, const VkSpecializationInfo* spec_info = nullptr)
  {
    SetShaderStage(VK_SHADER_STAGE_VERTEX_BIT, module, "main", spec_info);
  }
  void SetFragmentShader(VkShaderModule module, const VkSpecializationInfo* spec_info = nullptr)
  {
    SetShaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, module, "main", spec_info);
  }

  void AddVertexBuffer(uint32_t binding, uint32_t stride, VkVertexInputRate input_rate = VK_VERTEX_INPUT_RATE_VERTEX);
  void AddVertexAttribute(uint32_t location, uint32_t binding, VkFormat format, uint32_t offset);

  void SetPrimitiveTopology(VkPrimitiveTopology topology, bool enable_primitive_restart = false);

  void SetRasterizationState(VkPolygonMode polygon_mode, VkCullModeFlags cull_mode, VkFrontFace front_face);
  void SetMultisamples(uint32_t multisamples, bool per_sample_shading);
  void SetNoCullRasterizationState();

  void SetDepthState(bool depth_test, bool depth_write, VkCompareOp compare_op);
  void SetNoDepthTestState();

  void SetBlendAttachment(uint32_t attachment, bool blend_enable, VkBlendFactor src_factor, VkBlendFactor dst_factor,
                          VkBlendOp op, VkBlendFactor alpha_src_factor, VkBlendFactor alpha_dst_factor,
                          VkBlendOp alpha_op,
                          VkColorComponentFlags write_mask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT);
  void ClearBlendAttachments();

  void SetBlendConstants(float r, float g, float b, float a);
  void SetNoBlendingState();

  void AddDynamicState(VkDynamicState state);

  void SetDynamicViewportAndScissorState();
  void SetViewport(float x, float y, float width, float height, float min_depth, float max_depth);
  void SetScissorRect(int32_t x, int32_t y, uint32_t width, uint32_t height);

  void SetMultisamples(VkSampleCountFlagBits samples);

  void SetPipelineLayout(VkPipelineLayout layout);
  void SetRenderPass(VkRenderPass render_pass, uint32_t subpass);

private:
  VkGraphicsPipelineCreateInfo m_ci;
  std::array<VkPipelineShaderStageCreateInfo, MAX_SHADER_STAGES> m_shader_stages;

  VkPipelineVertexInputStateCreateInfo m_vertex_input_state;
  std::array<VkVertexInputBindingDescription, MAX_VERTEX_BUFFERS> m_vertex_buffers;
  std::array<VkVertexInputAttributeDescription, MAX_VERTEX_ATTRIBUTES> m_vertex_attributes;

  VkPipelineInputAssemblyStateCreateInfo m_input_assembly;

  VkPipelineRasterizationStateCreateInfo m_rasterization_state;
  VkPipelineDepthStencilStateCreateInfo m_depth_state;

  VkPipelineColorBlendStateCreateInfo m_blend_state;
  std::array<VkPipelineColorBlendAttachmentState, MAX_ATTACHMENTS> m_blend_attachments;

  VkPipelineViewportStateCreateInfo m_viewport_state;
  VkViewport m_viewport;
  VkRect2D m_scissor;

  VkPipelineDynamicStateCreateInfo m_dynamic_state;
  std::array<VkDynamicState, MAX_DYNAMIC_STATE> m_dynamic_state_values;

  VkPipelineMultisampleStateCreateInfo m_multisample_state;
};

class SamplerBuilder
{
public:
  SamplerBuilder();

  void Clear();

  VkSampler Create(VkDevice device, bool clear = true);

  void SetFilter(VkFilter mag_filter, VkFilter min_filter, VkSamplerMipmapMode mip_filter);
  void SetAddressMode(VkSamplerAddressMode u, VkSamplerAddressMode v, VkSamplerAddressMode w);

  void SetPointSampler(VkSamplerAddressMode address_mode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER);
  void SetLinearSampler(bool mipmaps, VkSamplerAddressMode address_mode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER);

private:
  VkSamplerCreateInfo m_ci;
};

class DescriptorSetUpdateBuilder
{
  static constexpr uint32_t MAX_WRITES = 16, MAX_INFOS = 16;

public:
  DescriptorSetUpdateBuilder();

  void Clear();

  void Update(VkDevice device, bool clear = true);

  void AddCombinedImageSamplerDescriptorWrite(VkDescriptorSet set, uint32_t binding, VkImageView view, VkSampler sampler,
                                              VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  void AddBufferDescriptorWrite(VkDescriptorSet set, uint32_t binding, VkDescriptorType dtype, VkBuffer buffer, uint32_t offset,
                                uint32_t size);
  void AddBufferViewDescriptorWrite(VkDescriptorSet set, uint32_t binding, VkDescriptorType dtype, VkBufferView view);

private:
  union InfoUnion
  {
    VkDescriptorBufferInfo buffer;
    VkDescriptorImageInfo image;
    VkBufferView buffer_view;
  };

  std::array<VkWriteDescriptorSet, MAX_WRITES> m_writes;
  uint32_t m_num_writes = 0;

  std::array<InfoUnion, MAX_INFOS> m_infos;
  uint32_t m_num_infos = 0;
};

class FramebufferBuilder
{
  static constexpr uint32_t MAX_ATTACHMENTS = 2;

public:
  FramebufferBuilder();

  void Clear();

  VkFramebuffer Create(VkDevice device, bool clear = true);

  void AddAttachment(VkImageView image);

  void SetSize(uint32_t width, uint32_t height, uint32_t layers);

  void SetRenderPass(VkRenderPass render_pass);

private:
  VkFramebufferCreateInfo m_ci;
  std::array<VkImageView, MAX_ATTACHMENTS> m_images;
};

class BufferViewBuilder
{
public:
  BufferViewBuilder();

  void Clear();

  VkBufferView Create(VkDevice device, bool clear = true);

  void Set(VkBuffer buffer, VkFormat format, uint32_t offset, uint32_t size);

private:
  VkBufferViewCreateInfo m_ci;
};

// Builder for VkSpecializationInfo passed to a pipeline shader stage.
//
// Specialization constants let a single SPIR-V blob serve multiple runtime
// configurations by baking integer / bool / float values at pipeline-
// creation time. This is what lets us pre-bake shaders whose source used
// to vary by emulator settings (RESOLUTION_SCALE, PGXP_DEPTH, FIRST_PASS,
// etc.) without exploding the on-disk blob count.
//
// Lifetime: GetInfo() returns a pointer into this object's storage, so the
// SpecConstants instance must outlive the vkCreateGraphicsPipelines call
// that consumes the VkSpecializationInfo. The typical pattern is a local
// SpecConstants variable on the same stack frame as the GraphicsPipelineBuilder.
//
// Each entry occupies exactly 4 bytes of payload; supports up to MAX_ENTRIES
// constants per stage. uint, int, float, and bool fit; double / vector
// constants do not and would need a wider payload type.
//
// Constant-id allocation convention (used across the Vulkan backend):
//   0-99   reserved for common knobs shared across shaders (RESOLUTION_SCALE,
//          MULTISAMPLES, PER_SAMPLE_SHADING, PGXP_DEPTH).
//   100+   shader-specific (e.g. FIRST_PASS for the adaptive-downsample
//          mip FS).
class SpecConstants
{
public:
  static constexpr uint32_t MAX_ENTRIES = 16;
  static constexpr uint32_t SLOT_SIZE = 4u;

  SpecConstants() = default;

  void Clear();

  void AddBool(uint32_t constant_id, bool value);
  void AddUInt(uint32_t constant_id, uint32_t value);
  void AddInt(uint32_t constant_id, int32_t value);
  void AddFloat(uint32_t constant_id, float value);

  // Returns nullptr if no entries have been added. Otherwise returns a
  // pointer to an internal VkSpecializationInfo whose lifetime is tied to
  // this object.
  const VkSpecializationInfo* GetInfo();

private:
  void Add(uint32_t constant_id, uint32_t bits);

  std::array<VkSpecializationMapEntry, MAX_ENTRIES> m_entries{};
  std::array<uint32_t, MAX_ENTRIES> m_data{};
  uint32_t m_count = 0;
  VkSpecializationInfo m_info{};
};

} // namespace Vulkan
