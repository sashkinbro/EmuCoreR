#pragma once
#include "../types.h"
#include "../windows_headers.h"
#include <cstring>
#include <d3d11.h>
#include <wrl/client.h>

namespace D3D11 {
class StagingTexture
{
public:
  template<typename T>
  using ComPtr = Microsoft::WRL::ComPtr<T>;

  StagingTexture();
  ~StagingTexture();

  ALWAYS_INLINE ID3D11Texture2D* GetD3DTexture() const { return m_texture.Get(); }

  ALWAYS_INLINE uint32_t GetWidth() const { return m_width; }
  ALWAYS_INLINE uint32_t GetHeight() const { return m_height; }
  ALWAYS_INLINE DXGI_FORMAT GetFormat() const { return m_format; }
  ALWAYS_INLINE bool IsMapped() const { return m_map.pData != nullptr; }

  ALWAYS_INLINE operator bool() const { return static_cast<bool>(m_texture); }

  bool Create(ID3D11Device* device, uint32_t width, uint32_t height, DXGI_FORMAT format, bool for_uploading);
  void Destroy();

  bool Map(ID3D11DeviceContext* context, bool writing);
  void Unmap(ID3D11DeviceContext* context);

  void CopyFromTexture(ID3D11DeviceContext* context, ID3D11Resource* src_texture, uint32_t src_subresource, uint32_t src_x,
                       uint32_t src_y, uint32_t dst_x, uint32_t dst_y, uint32_t width, uint32_t height);

  template<typename T>
  void ReadPixels(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t stride, T* data)
  {
    const uint8_t* src_ptr = static_cast<uint8_t*>(m_map.pData) + (y * m_map.RowPitch) + (x * sizeof(T));
    uint8_t* dst_ptr = reinterpret_cast<uint8_t*>(data);
    if (m_map.RowPitch != stride || width != m_width || x != 0)
    {
      for (uint32_t row = 0; row < height; row++)
      {
        std::memcpy(dst_ptr, src_ptr, sizeof(T) * width);
        src_ptr += m_map.RowPitch;
        dst_ptr += stride;
      }
    }
    else
      std::memcpy(dst_ptr, src_ptr, stride * height);
  }

protected:
  ComPtr<ID3D11Texture2D> m_texture;
  uint32_t m_width;
  uint32_t m_height;
  DXGI_FORMAT m_format;

  D3D11_MAPPED_SUBRESOURCE m_map = {};
};

class AutoStagingTexture : public StagingTexture
{
public:
  bool EnsureSize(ID3D11DeviceContext* context, uint32_t width, uint32_t height, DXGI_FORMAT format, bool for_uploading);

  void CopyFromTexture(ID3D11DeviceContext* context, ID3D11Resource* src_texture, uint32_t src_subresource, uint32_t src_x,
                       uint32_t src_y, uint32_t dst_x, uint32_t dst_y, uint32_t width, uint32_t height);
};
} // namespace D3D11
