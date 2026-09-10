// Copyright 2019 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include "../types.h"
#include "../windows_headers.h"
#include <d3d12.h>
#include <deque>
#include <utility>
#include <wrl/client.h>

namespace D3D12 {
class StreamBuffer
{
public:
  StreamBuffer();
  ~StreamBuffer();

  bool Create(uint32_t size);

  ALWAYS_INLINE bool IsValid() const { return static_cast<bool>(m_buffer); }
  ALWAYS_INLINE ID3D12Resource* GetBuffer() const { return m_buffer.Get(); }
  ALWAYS_INLINE D3D12_GPU_VIRTUAL_ADDRESS GetGPUPointer() const { return m_gpu_pointer; }
  ALWAYS_INLINE void* GetCurrentHostPointer() const { return m_host_pointer + m_current_offset; }
  ALWAYS_INLINE D3D12_GPU_VIRTUAL_ADDRESS GetCurrentGPUPointer() const { return m_gpu_pointer + m_current_offset; }
  ALWAYS_INLINE uint32_t GetSize() const { return m_size; }
  ALWAYS_INLINE uint32_t GetCurrentOffset() const { return m_current_offset; }
  ALWAYS_INLINE uint32_t GetCurrentSpace() const { return m_current_space; }

  bool ReserveMemory(uint32_t num_bytes, uint32_t alignment);
  void CommitMemory(uint32_t final_num_bytes);

  void Destroy(bool defer = true);

private:
  void UpdateCurrentFencePosition();
  void UpdateGPUPosition();

  // Waits for as many fences as needed to allocate num_bytes bytes from the buffer.
  bool WaitForClearSpace(uint32_t num_bytes);

  uint32_t m_size = 0;
  uint32_t m_current_offset = 0;
  uint32_t m_current_space = 0;
  uint32_t m_current_gpu_position = 0;

  Microsoft::WRL::ComPtr<ID3D12Resource> m_buffer;
  D3D12_GPU_VIRTUAL_ADDRESS m_gpu_pointer = {};
  uint8_t* m_host_pointer = nullptr;

  // List of fences and the corresponding positions in the buffer
  std::deque<std::pair<uint64_t, uint32_t>> m_tracked_fences;
};

} // namespace D3D12
