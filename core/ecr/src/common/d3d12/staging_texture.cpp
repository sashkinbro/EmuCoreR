#include "staging_texture.h"
#include "../align.h"
#include "context.h"
#include "util.h"

#include <cstring>
namespace D3D12 {

StagingTexture::StagingTexture() : m_width(0), m_height(0) {}

StagingTexture::~StagingTexture()
{
  Destroy();
}

bool StagingTexture::Create(uint32_t width, uint32_t height, DXGI_FORMAT format, bool for_uploading)
{
  const uint32_t texel_size  = GetTexelSize(format);
  const uint32_t row_pitch   = Common::AlignUpPow2(width * texel_size, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
  const uint32_t buffer_size = height * row_pitch;

  const D3D12_HEAP_PROPERTIES heap_properties = {for_uploading ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_READBACK};

  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = buffer_size;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = DXGI_FORMAT_UNKNOWN;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  desc.Flags = D3D12_RESOURCE_FLAG_NONE;

  D3D12_RESOURCE_STATES state = for_uploading ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST;

  ComPtr<ID3D12Resource> resource;
  HRESULT hr = g_d3d12_context->GetDevice()->CreateCommittedResource(
    &heap_properties, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(resource.GetAddressOf()));
  if (FAILED(hr))
    return false;

  Destroy(true);

  m_resource = std::move(resource);
  m_width = width;
  m_height = height;
  m_format = format;
  m_buffer_size = buffer_size;
  m_row_pitch = row_pitch;
  m_texel_size = texel_size;
  return true;
}

void StagingTexture::Destroy(bool defer)
{
  if (IsMapped())
    Unmap();

  if (m_resource && defer)
    g_d3d12_context->DeferResourceDestruction(m_resource.Get());
  m_resource.Reset();
  m_width = 0;
  m_height = 0;
  m_format = DXGI_FORMAT_UNKNOWN;
  m_buffer_size = 0;
  m_row_pitch = 0;
  m_texel_size = 0;
}

bool StagingTexture::Map(bool writing)
{
  D3D12_RANGE range{0u, m_buffer_size};

  const HRESULT hr = m_resource->Map(0, writing ? nullptr : &range, &m_mapped_pointer);
  if (FAILED(hr))
    return false;

  m_mapped_for_write = writing;
  return true;
}

void StagingTexture::Unmap()
{
  D3D12_RANGE range{0u, m_buffer_size};
  m_resource->Unmap(0, m_mapped_for_write ? &range : nullptr);
  m_mapped_pointer = nullptr;
  m_mapped_for_write = false;
}

void StagingTexture::Flush()
{
  if (!m_needs_flush)
    return;

  m_needs_flush = false;

  // If the completed fence is the same as the current command buffer fence, we need to execute
  // the current list and wait for it to complete. This is the slowest path. Otherwise, if the
  // command list with the copy has been submitted, we only need to wait for the fence.
  if (m_completed_fence == g_d3d12_context->GetCurrentFenceValue())
    g_d3d12_context->ExecuteCommandList(true);
  else
    g_d3d12_context->WaitForFence(m_completed_fence);
}

void StagingTexture::CopyToTexture(uint32_t src_x, uint32_t src_y, ID3D12Resource* dst_texture, uint32_t dst_subresource, uint32_t dst_x,
                                   uint32_t dst_y, uint32_t width, uint32_t height)
{
  D3D12_TEXTURE_COPY_LOCATION dst;
  dst.pResource = dst_texture;
  dst.SubresourceIndex = 0;
  dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

  D3D12_TEXTURE_COPY_LOCATION src;
  src.pResource = m_resource.Get();
  src.SubresourceIndex = 0;
  src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  src.PlacedFootprint.Offset = 0;
  src.PlacedFootprint.Footprint.Width = m_width;
  src.PlacedFootprint.Footprint.Height = m_height;
  src.PlacedFootprint.Footprint.Depth = 1;
  src.PlacedFootprint.Footprint.Format = m_format;
  src.PlacedFootprint.Footprint.RowPitch = m_row_pitch;

  const D3D12_BOX src_box{src_x, src_y, 0u, src_x + width, src_y + height, 1u};
  g_d3d12_context->GetCommandList()->CopyTextureRegion(&dst, dst_x, dst_y, 0, &src, &src_box);
}

void StagingTexture::CopyFromTexture(ID3D12Resource* src_texture, uint32_t src_subresource, uint32_t src_x, uint32_t src_y, uint32_t dst_x,
                                     uint32_t dst_y, uint32_t width, uint32_t height)
{
  D3D12_TEXTURE_COPY_LOCATION src;
  src.pResource = src_texture;
  src.SubresourceIndex = 0;
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

  D3D12_TEXTURE_COPY_LOCATION dst;
  dst.pResource = m_resource.Get();
  dst.SubresourceIndex = 0;
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint.Offset = 0;
  dst.PlacedFootprint.Footprint.Width = m_width;
  dst.PlacedFootprint.Footprint.Height = m_height;
  dst.PlacedFootprint.Footprint.Depth = 1;
  dst.PlacedFootprint.Footprint.Format = m_format;
  dst.PlacedFootprint.Footprint.RowPitch = m_row_pitch;

  const D3D12_BOX src_box{src_x, src_y, 0u, src_x + width, src_y + height, 1u};
  g_d3d12_context->GetCommandList()->CopyTextureRegion(&dst, dst_x, dst_y, 0, &src, &src_box);
  m_completed_fence = g_d3d12_context->GetCurrentFenceValue();
  m_needs_flush = true;
}

bool StagingTexture::ReadPixels(uint32_t x, uint32_t y, uint32_t width, uint32_t height, void* data, uint32_t row_pitch)
{
  if (m_needs_flush)
    Flush();

  const bool was_mapped = IsMapped();
  if (!was_mapped && !Map(false))
    return false;

  const uint8_t* src_ptr = static_cast<uint8_t*>(m_mapped_pointer) + (y * m_row_pitch) + (x * m_texel_size);
  uint8_t* dst_ptr = reinterpret_cast<uint8_t*>(data);
  if (m_row_pitch != row_pitch || width != m_width || x != 0)
  {
    const uint32_t copy_size = m_texel_size * width;
    for (uint32_t row = 0; row < height; row++)
    {
      std::memcpy(dst_ptr, src_ptr, copy_size);
      src_ptr += m_row_pitch;
      dst_ptr += row_pitch;
    }
  }
  else
  {
    std::memcpy(dst_ptr, src_ptr, row_pitch * height);
  }

  return true;
}

bool StagingTexture::WritePixels(uint32_t x, uint32_t y, uint32_t width, uint32_t height, const void* data, uint32_t row_pitch)
{
  const bool was_mapped = IsMapped();
  if (!was_mapped && !Map(true))
    return false;

  const uint8_t* src_ptr = reinterpret_cast<const uint8_t*>(data);
  uint8_t* dst_ptr = static_cast<uint8_t*>(m_mapped_pointer) + (y * m_row_pitch) + (x * m_texel_size);
  if (m_row_pitch != row_pitch || width != m_width || x != 0)
  {
    const uint32_t copy_size = m_texel_size * width;
    for (uint32_t row = 0; row < height; row++)
    {
      std::memcpy(dst_ptr, src_ptr, copy_size);
      src_ptr += row_pitch;
      dst_ptr += m_row_pitch;
    }
  }
  else
  {
    std::memcpy(dst_ptr, src_ptr, m_row_pitch * height);
  }

  if (!was_mapped)
    Unmap();

  return true;
}

bool StagingTexture::EnsureSize(uint32_t width, uint32_t height, DXGI_FORMAT format, bool for_uploading)
{
  if (m_resource && m_width >= width && m_height >= height && m_format == format)
    return true;

  return Create(width, height, format, for_uploading);
}

} // namespace D3D12
