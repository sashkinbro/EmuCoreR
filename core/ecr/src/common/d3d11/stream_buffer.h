#pragma once
#include "../types.h"
#include "../windows_headers.h"
#include <d3d11.h>
#include <wrl/client.h>

namespace D3D11 {
class StreamBuffer
{
public:
  template<typename T>
  using ComPtr = Microsoft::WRL::ComPtr<T>;

  StreamBuffer();
  StreamBuffer(ComPtr<ID3D11Buffer> buffer);
  ~StreamBuffer();

  ALWAYS_INLINE ID3D11Buffer* GetD3DBuffer() const { return m_buffer.Get(); }
  ALWAYS_INLINE ID3D11Buffer* const* GetD3DBufferArray() const { return m_buffer.GetAddressOf(); }
  ALWAYS_INLINE uint32_t GetSize() const { return m_size; }
  ALWAYS_INLINE uint32_t GetPosition() const { return m_position; }

  bool Create(ID3D11Device* device, D3D11_BIND_FLAG bind_flags, uint32_t size);
  void Release();
  
  struct MappingResult
  {
    void* pointer;
    uint32_t buffer_offset;
    uint32_t index_aligned; // offset / alignment, suitable for base vertex
    uint32_t space_aligned; // remaining space / alignment
  };

  MappingResult Map(ID3D11DeviceContext* context, uint32_t alignment, uint32_t min_size);
  void Unmap(ID3D11DeviceContext* context, uint32_t used_size);

private:
  ComPtr<ID3D11Buffer> m_buffer;
  uint32_t m_size;
  uint32_t m_position;
  bool m_use_map_no_overwrite = false;
};
} // namespace GL
