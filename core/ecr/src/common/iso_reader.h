#pragma once
#include "types.h"
#include <optional>
#include <string>
#include <vector>

class CDImage;

class ISOReader
{
public:
  static constexpr uint32_t SECTOR_SIZE = 2048;

#pragma pack(push, 1)

  struct ISOVolumeDescriptorHeader
  {
    uint8_t type_code;
    char standard_identifier[5];
    uint8_t version;
  };

  struct ISOBootRecord
  {
    ISOVolumeDescriptorHeader header;
    char boot_system_identifier[32];
    char boot_identifier[32];
    uint8_t data[1977];
  };

  struct ISOPVDDateTime
  {
    char year[4];
    char month[2];
    char day[2];
    char hour[2];
    char minute[2];
    char second[2];
    char milliseconds[2];
    int8_t gmt_offset;
  };

  struct ISOPrimaryVolumeDescriptor
  {
    ISOVolumeDescriptorHeader header;
    uint8_t unused;
    char system_identifier[32];
    char volume_identifier[32];
    char unused2[8];
    uint32_t total_sectors_le;
    uint32_t total_sectors_be;
    char unused3[32];
    uint16_t volume_set_size_le;
    uint16_t volume_set_size_be;
    uint16_t volume_sequence_number_le;
    uint16_t volume_sequence_number_be;
    uint16_t block_size_le;
    uint16_t block_size_be;
    uint32_t path_table_size_le;
    uint32_t path_table_size_be;
    uint32_t path_table_location_le;
    uint32_t optional_path_table_location_le;
    uint32_t path_table_location_be;
    uint32_t optional_path_table_location_be;
    uint8_t root_directory_entry[34];
    char volume_set_identifier[128];
    char publisher_identifier[128];
    char data_preparer_identifier[128];
    char application_identifier[128];
    char copyright_file_identifier[38];
    char abstract_file_identifier[36];
    char bibliographic_file_identifier[37];
    ISOPVDDateTime volume_creation_time;
    ISOPVDDateTime volume_modification_time;
    ISOPVDDateTime volume_expiration_time;
    ISOPVDDateTime volume_effective_time;
    uint8_t structure_version;
    uint8_t unused4;
    uint8_t application_used[512];
    uint8_t reserved[653];
  };

  struct ISODirectoryEntryDateTime
  {
    uint8_t years_since_1900;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    int8_t gmt_offset;
  };

  enum ISODirectoryEntryFlags : uint8_t
  {
    ISODirectoryEntryFlag_Hidden = (1 << 0),
    ISODirectoryEntryFlag_Directory = (1 << 1),
    ISODirectoryEntryFlag_AssociatedFile = (1 << 2),
    ISODirectoryEntryFlag_ExtendedAttributePresent = (1 << 3),
    ISODirectoryEntryFlag_OwnerGroupPermissions = (1 << 4),
    ISODirectoryEntryFlag_MoreExtents = (1 << 7),
  };

  struct ISODirectoryEntry
  {
    uint8_t entry_length;
    uint8_t extended_attribute_length;
    uint32_t location_le;
    uint32_t location_be;
    uint32_t length_le;
    uint32_t length_be;
    ISODirectoryEntryDateTime recoding_time;
    ISODirectoryEntryFlags flags;
    uint8_t interleaved_unit_size;
    uint8_t interleaved_gap_size;
    uint16_t sequence_le;
    uint16_t sequence_be;
    uint8_t filename_length;
  };

#pragma pack(pop)

  ISOReader();
  ~ISOReader();

  ALWAYS_INLINE const ISOPrimaryVolumeDescriptor& GetPVD() const { return m_pvd; }

  bool Open(CDImage* image, uint32_t track_number);

  bool ReadFile(const char* path, std::vector<uint8_t>* data);

private:
  bool ReadPVD();

  std::optional<ISODirectoryEntry> LocateFile(const char* path);
  std::optional<ISODirectoryEntry> LocateFile(const char* path, uint8_t* sector_buffer, uint32_t directory_record_lba,
                                              uint32_t directory_record_size);

  CDImage* m_image;
  uint32_t m_track_number;

  ISOPrimaryVolumeDescriptor m_pvd = {};
};
