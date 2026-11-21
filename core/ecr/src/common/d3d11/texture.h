#pragma once
#include "../types.h"
#include "../windows_headers.h"
#include <d3d11.h>
#include <wrl/client.h>

namespace D3D11 {
class Texture
{
public:
  template<typename T>
  using ComPtr = Microsoft::WRL::ComPtr<T>;

  Texture();
  Texture(ComPtr<ID3D11Texture2D> texture, ComPtr<ID3D11ShaderResourceView> srv, ComPtr<ID3D11RenderTargetView> rtv);
  ~Texture();

  ALWAYS_INLINE ID3D11Texture2D* GetD3DTexture() const { return m_texture.Get(); }
  ALWAYS_INLINE ID3D11ShaderResourceView* GetD3DSRV() const { return m_srv.Get(); }
  ALWAYS_INLINE ID3D11RenderTargetView* GetD3DRTV() const { return m_rtv.Get(); }
  ALWAYS_INLINE ID3D11ShaderResourceView* const* GetD3DSRVArray() const { return m_srv.GetAddressOf(); }
  ALWAYS_INLINE ID3D11RenderTargetView* const* GetD3DRTVArray() const { return m_rtv.GetAddressOf(); }

  ALWAYS_INLINE uint32_t GetWidth() const { return m_width; }
  ALWAYS_INLINE uint32_t GetHeight() const { return m_height; }
  ALWAYS_INLINE uint16_t GetLevels() const { return m_levels; }
  ALWAYS_INLINE uint16_t GetSamples() const { return m_samples; }
  ALWAYS_INLINE bool IsMultisampled() const { return m_samples > 1; }
  ALWAYS_INLINE DXGI_FORMAT GetFormat() const { return GetDesc().Format; }
  D3D11_TEXTURE2D_DESC GetDesc() const;

  ALWAYS_INLINE operator ID3D11Texture2D*() const { return m_texture.Get(); }
  ALWAYS_INLINE operator ID3D11ShaderResourceView*() const { return m_srv.Get(); }
  ALWAYS_INLINE operator ID3D11RenderTargetView*() const { return m_rtv.Get(); }
  ALWAYS_INLINE operator bool() const { return static_cast<bool>(m_texture); }

  bool Create(ID3D11Device* device, uint32_t width, uint32_t height, uint32_t levels, uint32_t samples, DXGI_FORMAT format, uint32_t bind_flags,
              const void* initial_data = nullptr, uint32_t initial_data_stride = 0, bool dynamic = false);

  void Destroy();

private:
  ComPtr<ID3D11Texture2D> m_texture;
  ComPtr<ID3D11ShaderResourceView> m_srv;
  ComPtr<ID3D11RenderTargetView> m_rtv;
  uint32_t m_width;
  uint32_t m_height;
  uint16_t m_levels;
  uint16_t m_samples;
};
} // namespace D3D11
