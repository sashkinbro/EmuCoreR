#include "image.h"
#include "byte_stream.h"
#include "file_system.h"
#include "stb_image.h"
#include "string_util.h"

namespace Common {
bool LoadImageFromFile(Common::RGBA8Image* image, const char* filename)
{
  std::optional<std::vector<uint8_t>> file_data = FileSystem::ReadBinaryFile(filename);
  if (!file_data.has_value() || file_data->empty())
    return false;

  int width, height, file_channels;
  uint8_t* pixel_data = stbi_load_from_memory(file_data->data(), static_cast<int>(file_data->size()),
                                              &width, &height, &file_channels, 4);
  if (!pixel_data)
    return false;

  image->SetPixels(static_cast<uint32_t>(width), static_cast<uint32_t>(height), reinterpret_cast<const uint32_t*>(pixel_data));
  stbi_image_free(pixel_data);
  return true;
}

} // namespace Common
