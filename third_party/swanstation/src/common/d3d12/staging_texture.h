#pragma once
#include "../types.h"
#include "../windows_headers.h"
#include <cstring>
#include <d3d12.h>
#include <wrl/client.h>

namespace D3D12 {
class StagingTexture
{
public:
  template<typename T>
  using ComPtr = Microsoft::WRL::ComPtr<T>;

  StagingTexture();
  ~StagingTexture();

  ALWAYS_INLINE ID3D12Resource* GetD3DResource() const { return m_resource.Get(); }

  ALWAYS_INLINE uint32_t GetWidth() const { return m_width; }
  ALWAYS_INLINE uint32_t GetHeight() const { return m_height; }
  ALWAYS_INLINE DXGI_FORMAT GetFormat() const { return m_format; }
  ALWAYS_INLINE bool IsMapped() const { return m_mapped_pointer != nullptr; }
  ALWAYS_INLINE const void* GetMapPointer() const { return m_mapped_pointer; }

  ALWAYS_INLINE operator bool() const { return static_cast<bool>(m_resource); }

  bool Create(uint32_t width, uint32_t height, DXGI_FORMAT format, bool for_uploading);
  void Destroy(bool defer = true);

  bool Map(bool writing);
  void Unmap();
  void Flush();

  void CopyToTexture(uint32_t src_x, uint32_t src_y, ID3D12Resource* dst_texture, uint32_t dst_subresource, uint32_t dst_x, uint32_t dst_y,
                     uint32_t width, uint32_t height);
  void CopyFromTexture(ID3D12Resource* src_texture, uint32_t src_subresource, uint32_t src_x, uint32_t src_y, uint32_t dst_x, uint32_t dst_y,
                       uint32_t width, uint32_t height);


  bool ReadPixels(uint32_t x, uint32_t y, uint32_t width, uint32_t height, void* data, uint32_t row_pitch);

  bool WritePixels(uint32_t x, uint32_t y, uint32_t width, uint32_t height, const void* data, uint32_t row_pitch);

  bool EnsureSize(uint32_t width, uint32_t height, DXGI_FORMAT format, bool for_uploading);

protected:
  ComPtr<ID3D12Resource> m_resource;
  uint32_t m_width;
  uint32_t m_height;
  DXGI_FORMAT m_format;
  uint32_t m_texel_size;
  uint32_t m_row_pitch;
  uint32_t m_buffer_size;

  void* m_mapped_pointer = nullptr;
  uint64_t m_completed_fence = 0;
  bool m_mapped_for_write = false;
  bool m_needs_flush = false;
};

} // namespace D3D12