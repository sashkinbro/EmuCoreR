// Copyright 2019 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include "../types.h"
#include "../windows_headers.h"
#include "descriptor_heap_manager.h"
#include "stream_buffer.h"
#include <array>
#include <d3d12.h>
#include <memory>
#include <vector>
#include <wrl/client.h>

struct IDXGIFactory;

namespace D3D12 {

class Context
{
public:
  template<typename T>
  using ComPtr = Microsoft::WRL::ComPtr<T>;

  static constexpr uint32_t
    // Number of command lists. One is being built while the other(s) are executed.
    NUM_COMMAND_LISTS = 3,

    // Textures that don't fit into this buffer will be uploaded with a staging buffer.
    TEXTURE_UPLOAD_BUFFER_SIZE = 16 * 1024 * 1024;

  ~Context();

  // Creates new device and context.
  static bool Create(IDXGIFactory* dxgi_factory, uint32_t adapter_index, bool enable_debug_layer);

  // Creates a context that adopts the frontend's ID3D12Device and
  // ID3D12CommandQueue (e.g. via retro_hw_render_interface_d3d12).
  // The frontend retains ownership of both - we just AddRef them
  // and create the rest of the context (descriptor heaps, command
  // lists, fence, texture upload buffer) on top. enable_debug_layer
  // is ignored; the frontend already configured the device.
  static bool CreateForLibretro(ID3D12Device* device, ID3D12CommandQueue* command_queue);

  // Destroys active context.
  static void Destroy();

  ID3D12Device* GetDevice() const { return m_device.Get(); }
  ID3D12CommandQueue* GetCommandQueue() const { return m_command_queue.Get(); }

  // Returns the current command list, commands can be recorded directly.
  ID3D12GraphicsCommandList* GetCommandList() const
  {
    return m_command_lists[m_current_command_list].command_list.Get();
  }

  // Descriptor manager access.
  DescriptorHeapManager& GetDescriptorHeapManager() { return m_descriptor_heap_manager; }
  DescriptorHeapManager& GetRTVHeapManager() { return m_rtv_heap_manager; }
  DescriptorHeapManager& GetDSVHeapManager() { return m_dsv_heap_manager; }
  DescriptorHeapManager& GetSamplerHeapManager() { return m_sampler_heap_manager; }
  ID3D12DescriptorHeap* const* GetGPUDescriptorHeaps() const { return m_gpu_descriptor_heaps.data(); }
  uint32_t GetGPUDescriptorHeapCount() const { return static_cast<uint32_t>(m_gpu_descriptor_heaps.size()); }
  const DescriptorHandle& GetNullSRVDescriptor() const { return m_null_srv_descriptor; }
  StreamBuffer& GetTextureStreamBuffer() { return m_texture_stream_buffer; }

  // Root signature access.
  ComPtr<ID3DBlob> SerializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC* desc);
  ComPtr<ID3D12RootSignature> CreateRootSignature(const D3D12_ROOT_SIGNATURE_DESC* desc);

  // Fence value for current command list.
  uint64_t GetCurrentFenceValue() const { return m_current_fence_value; }

  // Last "completed" fence.
  uint64_t GetCompletedFenceValue() const { return m_completed_fence_value; }

  // Feature level to use when compiling shaders.
  D3D_FEATURE_LEVEL GetFeatureLevel() const { return m_feature_level; }

  // Test for support for the specified texture format.
  bool SupportsTextureFormat(DXGI_FORMAT format);

  // Executes the current command list.
  void ExecuteCommandList(bool wait_for_completion);

  // Waits for a specific fence.
  void WaitForFence(uint64_t fence);

  // Waits for any in-flight command buffers to complete.
  void WaitForGPUIdle();

  // Defers destruction of a D3D resource (associates it with the current list).
  void DeferResourceDestruction(ID3D12Resource* resource);

  // Defers destruction of a descriptor handle (associates it with the current list).
  void DeferDescriptorDestruction(DescriptorHeapManager& manager, uint32_t index);
  void DeferDescriptorDestruction(DescriptorHeapManager& manager, DescriptorHandle* handle);

private:
  struct CommandListResources
  {
    ComPtr<ID3D12CommandAllocator> command_allocator;
    ComPtr<ID3D12GraphicsCommandList> command_list;
    std::vector<ID3D12Resource*> pending_resources;
    std::vector<std::pair<DescriptorHeapManager&, uint32_t>> pending_descriptors;
    uint64_t ready_fence_value = 0;
  };

  Context();

  bool CreateDevice(IDXGIFactory* dxgi_factory, uint32_t adapter_index, bool enable_debug_layer);
  bool CreateCommandQueue();
  bool CreateFence();
  bool CreateDescriptorHeaps();
  bool CreateCommandLists();
  bool CreateTextureStreamBuffer();
  void MoveToNextCommandList();
  void DestroyPendingResources(CommandListResources& cmdlist);
  void DestroyResources();

  ComPtr<ID3D12Debug> m_debug_interface;
  ComPtr<ID3D12Device> m_device;
  ComPtr<ID3D12CommandQueue> m_command_queue;

  ComPtr<ID3D12Fence> m_fence = nullptr;
  HANDLE m_fence_event = {};
  uint32_t m_current_fence_value = 0;
  uint64_t m_completed_fence_value = 0;

  std::array<CommandListResources, NUM_COMMAND_LISTS> m_command_lists;
  uint32_t m_current_command_list = NUM_COMMAND_LISTS - 1;

  DescriptorHeapManager m_descriptor_heap_manager;
  DescriptorHeapManager m_rtv_heap_manager;
  DescriptorHeapManager m_dsv_heap_manager;
  DescriptorHeapManager m_sampler_heap_manager;
  std::array<ID3D12DescriptorHeap*, 2> m_gpu_descriptor_heaps = {};
  DescriptorHandle m_null_srv_descriptor;
  StreamBuffer m_texture_stream_buffer;

  D3D_FEATURE_LEVEL m_feature_level = D3D_FEATURE_LEVEL_11_0;
};

} // namespace D3D12

extern std::unique_ptr<D3D12::Context> g_d3d12_context;
