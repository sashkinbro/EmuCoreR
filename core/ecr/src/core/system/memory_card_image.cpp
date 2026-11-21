#include "memory_card_image.h"
#include "common/byte_stream.h"
#include "common/file_system.h"
#include "common/state_wrapper.h"
#include "common/string_util.h"
#include "host_interface.h"
#include "system.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <optional>

#include <file/file_path.h>

namespace MemoryCardImage {

static uint8_t GetChecksum(const uint8_t* frame)
{
  uint8_t checksum = frame[0];
  for (uint32_t i = 1; i < FRAME_SIZE - 1; i++)
    checksum ^= frame[i];
  return checksum;
}

template<typename T>
T* GetFramePtr(DataArray* data, uint32_t block, uint32_t frame)
{
  return reinterpret_cast<T*>(data->data() + (block * BLOCK_SIZE) + (frame * FRAME_SIZE));
}

template<typename T>
const T* GetFramePtr(const DataArray& data, uint32_t block, uint32_t frame)
{
  return reinterpret_cast<const T*>(&data[(block * BLOCK_SIZE) + (frame * FRAME_SIZE)]);
}

bool LoadFromFile(DataArray* data, const char* filename)
{
  int32_t sd_size = path_get_size(filename);
  if (sd_size == -1 || sd_size != DATA_SIZE)
    return false;

  std::unique_ptr<ByteStream> stream = FileSystem::OpenFile(filename, BYTESTREAM_OPEN_READ | BYTESTREAM_OPEN_STREAMED);
  if (!stream || stream->GetSize() != DATA_SIZE)
    return false;
  const size_t num_read = stream->Read(data->data(), DATA_SIZE);
  if (num_read != DATA_SIZE)
    return false;
  return true;
}

bool SaveToFile(const DataArray& data, const char* filename)
{
  std::unique_ptr<ByteStream> stream =
    FileSystem::OpenFile(filename, BYTESTREAM_OPEN_CREATE | BYTESTREAM_OPEN_TRUNCATE | BYTESTREAM_OPEN_WRITE |
                                     BYTESTREAM_OPEN_ATOMIC_UPDATE | BYTESTREAM_OPEN_STREAMED);
  if (!stream)
    return false;

  if (!stream->Write2(data.data(), DATA_SIZE) || !stream->Commit())
  {
    stream->Discard();
    return false;
  }

  return true;
}

void Format(DataArray* data)
{
  // fill everything with FF
  data->fill(uint8_t(0xFF));

  // header
  {
    uint8_t* fptr = GetFramePtr<uint8_t>(data, 0, 0);
    std::fill_n(fptr, FRAME_SIZE, uint8_t(0));
    fptr[0] = 'M';
    fptr[1] = 'C';
    fptr[0x7F] = GetChecksum(fptr);
  }

  // directory
  for (uint32_t frame = 1; frame < 16; frame++)
  {
    uint8_t* fptr = GetFramePtr<uint8_t>(data, 0, frame);
    std::fill_n(fptr, FRAME_SIZE, uint8_t(0));
    fptr[0] = 0xA0;                 // free
    fptr[8] = 0xFF;                 // pointer to next file
    fptr[9] = 0xFF;                 // pointer to next file
    fptr[0x7F] = GetChecksum(fptr); // checksum
  }

  // broken sector list
  for (uint32_t frame = 16; frame < 36; frame++)
  {
    uint8_t* fptr = GetFramePtr<uint8_t>(data, 0, frame);
    std::fill_n(fptr, FRAME_SIZE, uint8_t(0));
    fptr[0] = 0xFF;
    fptr[1] = 0xFF;
    fptr[2] = 0xFF;
    fptr[3] = 0xFF;
    fptr[8] = 0xFF;                 // pointer to next file
    fptr[9] = 0xFF;                 // pointer to next file
    fptr[0x7F] = GetChecksum(fptr); // checksum
  }

  // broken sector replacement data
  for (uint32_t frame = 36; frame < 56; frame++)
  {
    uint8_t* fptr = GetFramePtr<uint8_t>(data, 0, frame);
    std::fill_n(fptr, FRAME_SIZE, uint8_t(0x00));
  }

  // unused frames
  for (uint32_t frame = 56; frame < 63; frame++)
  {
    uint8_t* fptr = GetFramePtr<uint8_t>(data, 0, frame);
    std::fill_n(fptr, FRAME_SIZE, uint8_t(0x00));
  }

  // write test frame
  std::memcpy(GetFramePtr<uint8_t>(data, 0, 63), GetFramePtr<uint8_t>(data, 0, 0), FRAME_SIZE);
}

} // namespace MemoryCardImage
