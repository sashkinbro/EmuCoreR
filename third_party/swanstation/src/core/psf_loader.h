#pragma once
#include "types.h"
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace PSFLoader {

#pragma pack(push, 1)
struct PSFHeader
{
  uint8_t id[3];
  uint8_t version;
  uint32_t reserved_area_size;
  uint32_t compressed_program_size;
  uint32_t program_crc32;
};
#pragma pack(pop)

class File
{
public:
  using TagMap = std::map<std::string, std::string>;
  using ProgramData = std::vector<uint8_t>;

  ALWAYS_INLINE const ProgramData& GetProgramData() const { return m_program_data; }
  ALWAYS_INLINE DiscRegion GetRegion() const { return m_region; }

  std::optional<std::string> GetTagString(const char* tag_name) const;
  std::optional<int> GetTagInt(const char* tag_name) const;

  std::string GetTagString(const char* tag_name, const char* default_value) const;
  int GetTagInt(const char* tag_name, int default_value) const;

  bool Load(const char* path);

private:
  static constexpr uint32_t MAX_PROGRAM_SIZE = 2 * 1024 * 1024;

  ProgramData m_program_data;
  TagMap m_tags;
  DiscRegion m_region = DiscRegion::Other;
};

bool Load(const char* path);

} // namespace PSFLoader
