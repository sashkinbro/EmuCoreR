#pragma once
#include "types.h"
#include <array>

namespace MemoryCardImage {

inline constexpr uint32_t DATA_SIZE = 128 * 1024, // 1mbit
                          BLOCK_SIZE = 8192,
                          FRAME_SIZE = 128;

using DataArray = std::array<uint8_t, DATA_SIZE>;

bool LoadFromFile(DataArray* data, const char* filename);
bool SaveToFile(const DataArray& data, const char* filename);

void Format(DataArray* data);

} // namespace MemoryCardImage
