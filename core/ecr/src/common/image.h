#pragma once
#include "types.h"
#include <algorithm>
#include <cstring>
#include <optional>
#include <vector>

namespace Common {
template<typename PixelType>
class Image
{
public:
  Image() = default;
  Image(uint32_t width, uint32_t height, const PixelType* pixels) { SetPixels(width, height, pixels); }
  Image(const Image& copy)
  {
    m_width = copy.m_width;
    m_height = copy.m_height;
    m_pixels = copy.m_pixels;
  }
  Image(Image&& move)
  {
    m_width = move.m_width;
    m_height = move.m_height;
    m_pixels = std::move(move.m_pixels);
    move.m_width = 0;
    move.m_height = 0;
  }

  Image& operator=(const Image& copy)
  {
    m_width = copy.m_width;
    m_height = copy.m_height;
    m_pixels = copy.m_pixels;
    return *this;
  }
  Image& operator=(Image&& move)
  {
    m_width = move.m_width;
    m_height = move.m_height;
    m_pixels = std::move(move.m_pixels);
    move.m_width = 0;
    move.m_height = 0;
    return *this;
  }

  ALWAYS_INLINE bool IsValid() const { return (m_width > 0 && m_height > 0); }
  ALWAYS_INLINE uint32_t GetWidth() const { return m_width; }
  ALWAYS_INLINE uint32_t GetHeight() const { return m_height; }
  ALWAYS_INLINE uint32_t GetByteStride() const { return (sizeof(PixelType) * m_width); }
  ALWAYS_INLINE const PixelType* GetPixels() const { return m_pixels.data(); }
  ALWAYS_INLINE PixelType* GetPixels() { return m_pixels.data(); }

  void Clear(PixelType fill_value = static_cast<PixelType>(0))
  {
    std::fill(m_pixels.begin(), m_pixels.end(), fill_value);
  }

  void SetSize(uint32_t new_width, uint32_t new_height, PixelType fill_value = static_cast<PixelType>(0))
  {
    m_width = new_width;
    m_height = new_height;
    m_pixels.resize(new_width * new_height);
    Clear(fill_value);
  }

  void SetPixels(uint32_t width, uint32_t height, const PixelType* pixels)
  {
    m_width = width;
    m_height = height;
    m_pixels.resize(width * height);
    std::memcpy(m_pixels.data(), pixels, width * height * sizeof(PixelType));
  }

private:
  uint32_t m_width = 0;
  uint32_t m_height = 0;
  std::vector<PixelType> m_pixels;
};

using RGBA8Image = Image<uint32_t>;

bool LoadImageFromFile(Common::RGBA8Image* image, const char* filename);

} // namespace Common
