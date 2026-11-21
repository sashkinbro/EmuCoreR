#include "cd_subchannel_replacement.h"
#include "file_system.h"
#include "log.h"
#include <algorithm>
#include <cstring>
#include <memory>
Log_SetChannel(CDSubChannelReplacement);

#pragma pack(push, 1)
struct SBIFileEntry
{
  uint8_t minute_bcd;
  uint8_t second_bcd;
  uint8_t frame_bcd;
  uint8_t type;
  uint8_t data[10];
};
#pragma pack(pop)

CDSubChannelReplacement::CDSubChannelReplacement() = default;

CDSubChannelReplacement::~CDSubChannelReplacement() = default;

static constexpr uint32_t MSFToLBA(uint8_t minute_bcd, uint8_t second_bcd, uint8_t frame_bcd)
{
  const uint8_t minute = PackedBCDToBinary(minute_bcd);
  const uint8_t second = PackedBCDToBinary(second_bcd);
  const uint8_t frame = PackedBCDToBinary(frame_bcd);

  return (static_cast<uint32_t>(minute) * 60 * 75) + (static_cast<uint32_t>(second) * 75) + static_cast<uint32_t>(frame);
}

bool CDSubChannelReplacement::LoadSBI(const char* path)
{
  RFILE *fp = FileSystem::OpenRFile(path, "rb");
  if (!fp)
    return false;

  char header[4];
  if (rfread(header, sizeof(header), 1, fp) != 1)
  {
    Log_ErrorPrintf("Failed to read header for '%s'", path);
    rfclose(fp);
    return true;
  }

  static constexpr char expected_header[] = {'S', 'B', 'I', '\0'};
  if (std::memcmp(header, expected_header, sizeof(header)) != 0)
  {
    Log_ErrorPrintf("Invalid header in '%s'", path);
    rfclose(fp);
    return true;
  }

  SBIFileEntry entry;
  while (rfread(&entry, sizeof(entry), 1, fp) == 1)
  {
    if (!IsValidPackedBCD(entry.minute_bcd) || !IsValidPackedBCD(entry.second_bcd) ||
        !IsValidPackedBCD(entry.frame_bcd))
    {
      Log_ErrorPrintf("Invalid position [%02x:%02x:%02x] in '%s'", entry.minute_bcd, entry.second_bcd, entry.frame_bcd,
                      path);
      rfclose(fp);
      return false;
    }

    if (entry.type != 1)
    {
      Log_ErrorPrintf("Invalid type 0x%02X in '%s'", entry.type, path);
      rfclose(fp);
      return false;
    }

    const uint32_t lba = MSFToLBA(entry.minute_bcd, entry.second_bcd, entry.frame_bcd);

    CDImage::SubChannelQ subq;
    std::copy_n(entry.data, countof(entry.data), subq.data.data());

    // generate an invalid crc by flipping all bits from the valid crc (will never collide)
    const uint16_t crc = subq.ComputeCRC(subq.data) ^ 0xFFFF;
    subq.data[10] = static_cast<uint8_t>(crc);
    subq.data[11] = static_cast<uint8_t>(crc >> 8);

    m_replacement_subq.emplace(lba, subq);
  }

  Log_InfoPrintf("Loaded %zu replacement sectors from '%s'", m_replacement_subq.size(), path);
  rfclose(fp);
  return true;
}

bool CDSubChannelReplacement::LoadSBIFromImagePath(const char* image_path)
{
  return LoadSBI(FileSystem::ReplaceExtension(image_path, "sbi").c_str());
}

bool CDSubChannelReplacement::GetReplacementSubChannelQ(uint32_t lba, CDImage::SubChannelQ* subq) const
{
  const auto iter = m_replacement_subq.find(lba);
  if (iter == m_replacement_subq.cend())
    return false;

  *subq = iter->second;
  return true;
}
