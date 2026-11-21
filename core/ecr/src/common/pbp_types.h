#pragma once
#include "types.h"
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace PBP {

inline constexpr uint32_t PBP_HEADER_OFFSET_COUNT = 8u, TOC_NUM_ENTRIES = 102u, BLOCK_TABLE_NUM_ENTRIES = 32256u,
                     DISC_TABLE_NUM_ENTRIES = 5u,
                     DECOMPRESSED_BLOCK_SIZE = 37632u; // 2352 bytes per sector * 16 sectors per block

#pragma pack(push, 1)

struct PBPHeader
{
  uint8_t magic[4]; // "\0PBP"
  uint32_t version;

  union
  {
    uint32_t offsets[PBP_HEADER_OFFSET_COUNT];

    struct
    {
      uint32_t param_sfo_offset; // 0x00000028
      uint32_t icon0_png_offset;
      uint32_t icon1_png_offset;
      uint32_t pic0_png_offset;
      uint32_t pic1_png_offset;
      uint32_t snd0_at3_offset;
      uint32_t data_psp_offset;
      uint32_t data_psar_offset;
    };
  };
};

struct SFOHeader
{
  uint8_t magic[4]; // "\0PSF"
  uint32_t version;
  uint32_t key_table_offset;  // Relative to start of SFOHeader, 0x000000A4 expected
  uint32_t data_table_offset; // Relative to start of SFOHeader, 0x00000100 expected
  uint32_t num_table_entries; // 0x00000009
};

struct SFOIndexTableEntry
{
  uint16_t key_offset; // Relative to key_table_offset
  uint16_t data_type;
  uint32_t data_size;       // Size of actual data in bytes
  uint32_t data_total_size; // Size of data field in bytes, data_total_size >= data_size
  uint32_t data_offset;     // Relative to data_table_offset
};

using SFOIndexTable = std::vector<SFOIndexTableEntry>;
using SFOTableDataValue = std::variant<std::string, uint32_t>;
using SFOTable = std::map<std::string, SFOTableDataValue>;

struct BlockTableEntry
{
  uint32_t offset;
  uint16_t size;
  uint16_t marker;
  uint8_t checksum[0x10];
  uint64_t padding;
};

struct TOCEntry
{
  struct Timecode
  {
    uint8_t m;
    uint8_t s;
    uint8_t f;
  };

  uint8_t type;
  uint8_t unknown;
  uint8_t point;
  Timecode pregap_start;
  uint8_t zero;
  Timecode userdata_start;
};

#pragma pack(pop)

} // namespace PBP
