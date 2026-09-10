#pragma once
#include "types.h"
#include <cstring>
#include <optional>
#include <string>
#include <vector>
namespace BIOS {

inline constexpr uint32_t BIOS_BASE = 0x1FC00000, BIOS_SIZE = 0x80000, BIOS_SIZE_PS2 = 0x400000, BIOS_SIZE_PS3 = 0x3E66F0;

using Image = std::vector<uint8_t>;

struct Hash
{
  uint8_t bytes[16];

  ALWAYS_INLINE bool operator==(const Hash& bh) const { return (std::memcmp(bytes, bh.bytes, sizeof(bytes)) == 0); }
};

struct ImageInfo
{
  const char* description;
  ConsoleRegion region;
  Hash hash;
  bool patch_compatible;
};

#pragma pack(push, 1)
struct PSEXEHeader
{
  char id[8];            // 0x000-0x007 PS-X EXE
  char pad1[8];          // 0x008-0x00F
  uint32_t initial_pc;        // 0x010
  uint32_t initial_gp;        // 0x014
  uint32_t load_address;      // 0x018
  uint32_t file_size;         // 0x01C excluding 0x800-byte header
  uint32_t unk0;              // 0x020
  uint32_t unk1;              // 0x024
  uint32_t memfill_start;     // 0x028
  uint32_t memfill_size;      // 0x02C
  uint32_t initial_sp_base;   // 0x030
  uint32_t initial_sp_offset; // 0x034
  uint32_t reserved[5];       // 0x038-0x04B
  char marker[0x7B4];    // 0x04C-0x7FF
};
#pragma pack(pop)

Hash GetHash(const uint8_t *image, size_t image_size);
std::optional<Image> LoadImageFromFile(const char* filename);

const ImageInfo* GetImageInfoForHash(const Hash& hash);
const ImageInfo* GetImageInfo(const Image& image);

void PatchBIOS(uint8_t* image, uint32_t image_size, uint32_t address, uint32_t value, uint32_t mask = UINT32_C(0xFFFFFFFF));

bool PatchBIOSFastBoot(uint8_t* image, uint32_t image_size, const Hash& hash);
bool PatchBIOSForEXE(uint8_t* image, uint32_t image_size, uint32_t r_pc, uint32_t r_gp, uint32_t r_sp, uint32_t r_fp);

bool IsValidPSExeHeader(const PSEXEHeader& header, uint32_t file_size);
DiscRegion GetPSExeDiscRegion(const PSEXEHeader& header);
} // namespace BIOS
