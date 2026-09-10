#pragma once
#include "vulkan_loader.h"
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace Vulkan {

// VkPipelineCache wrapper. Historically this class also managed an on-
// disk SPIR-V blob cache populated by runtime glslang invocations on
// shadergen output (vulkan_shaders.bin / vulkan_shaders.idx). That
// SPIR-V cache is gone now that every shader in the Vulkan backend is
// pre-baked into the build at .inc-include time and instantiated via
// Vulkan::EmbeddedShaders::CreateShaderModule. What remains is the
// driver's own pipeline-binary cache (vulkan_pipelines.bin) which is
// still essential - VkPipelineCache feeds the driver's SPIR-V -> GPU
// ISA compiler, and saving it across runs avoids recompiling every
// pipeline on every cold boot.
//
// Open() will also opportunistically delete any leftover vulkan_-
// shaders.bin / vulkan_shaders.idx on disk so users with stale caches
// from earlier builds get them tidied up automatically.
class ShaderCache
{
public:
  ~ShaderCache();

  static void Create(std::string_view base_path, bool debug);
  static void Destroy();

  /// Returns a handle to the pipeline cache. Set set_dirty to true if you are planning on writing to it externally.
  VkPipelineCache GetPipelineCache(bool set_dirty = true);

  /// Writes pipeline cache to file, saving all newly compiled pipelines.
  bool FlushPipelineCache();

private:
  ShaderCache();

  static std::string GetPipelineCacheBaseFileName(const std::string_view& base_path, bool debug);
  static std::string GetLegacyShaderCacheBaseFileName(const std::string_view& base_path, bool debug);

  void Open(std::string_view base_path, bool debug);

  bool CreateNewPipelineCache();
  bool ReadExistingPipelineCache();
  void ClosePipelineCache();

  std::string m_pipeline_cache_filename;

  // Serialises external access to m_pipeline_cache. Per the Vulkan
  // spec, the pipelineCache parameter to vkCreateGraphicsPipelines /
  // vkCreateComputePipelines / vkMergePipelineCaches is in the host-
  // synchronisation parameter list - the application must guarantee
  // no concurrent use of the same VkPipelineCache. (The
  // VK_PIPELINE_CACHE_CREATE_EXTERNALLY_SYNCHRONIZED_BIT flag from
  // Vulkan 1.3 / VK_EXT_pipeline_creation_cache_control would
  // confirm the contract to the driver; without it the driver is
  // permitted to assume serial access.)
  //
  // Lazy-fault PSO compile helpers in GPU_HW_Vulkan acquire this via
  // PipelineCacheMutex() around their gpbuilder.Create(...) call.
  std::mutex m_pipeline_cache_mutex;

public:
  // Exposed so lazy-fault PSO compile helpers can synchronise their
  // vkCreateGraphicsPipelines call against any other thread also
  // creating pipelines with the same VkPipelineCache. Returned
  // by-reference; lifetime tied to the ShaderCache singleton.
  std::mutex& PipelineCacheMutex() { return m_pipeline_cache_mutex; }

private:
  VkPipelineCache m_pipeline_cache = VK_NULL_HANDLE;
  bool m_pipeline_cache_dirty = false;
};

} // namespace Vulkan

extern std::unique_ptr<Vulkan::ShaderCache> g_vulkan_shader_cache;
