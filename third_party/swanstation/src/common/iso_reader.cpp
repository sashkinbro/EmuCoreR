#include "iso_reader.h"
#include "cd_image.h"
#include "log.h"
#include <cctype>
#include <cstring>
Log_SetChannel(ISOReader);

static bool FilenamesEqual(const char* a, const char* b, uint32_t length)
{
  uint32_t pos = 0;
  for (; pos < length && *a != '\0' && *b != '\0'; pos++)
  {
    if (std::tolower(*(a++)) != std::tolower(*(b++)))
      return false;
  }

  return true;
}

ISOReader::ISOReader() = default;

ISOReader::~ISOReader() = default;

bool ISOReader::Open(CDImage* image, uint32_t track_number)
{
  m_image = image;
  m_track_number = track_number;
  if (!ReadPVD())
    return false;

  return true;
}

bool ISOReader::ReadPVD()
{
  // volume descriptor start at sector 16
  if (!m_image->Seek(m_track_number, 16))
    return false;

  // try only a maximum of 256 volume descriptors
  for (uint32_t i = 0; i < 256; i++)
  {
    uint8_t buffer[SECTOR_SIZE];
    if (m_image->Read(CDImage::ReadMode::DataOnly, 1, buffer) != 1)
      return false;

    const ISOVolumeDescriptorHeader* header = reinterpret_cast<ISOVolumeDescriptorHeader*>(buffer);
    if (header->type_code != 1)
      continue;
    else if (header->type_code == 255)
      break;

    std::memcpy(&m_pvd, buffer, sizeof(ISOPrimaryVolumeDescriptor));
    return true;
  }

  Log_ErrorPrint("PVD not found");
  return false;
}

std::optional<ISOReader::ISODirectoryEntry> ISOReader::LocateFile(const char* path)
{
  uint8_t sector_buffer[SECTOR_SIZE];

  const ISODirectoryEntry* root_de = reinterpret_cast<const ISODirectoryEntry*>(m_pvd.root_directory_entry);
  if (*path == '\0' || std::strcmp(path, "/") == 0)
  {
    // locating the root directory
    return *root_de;
  }

  // start at the root directory
  return LocateFile(path, sector_buffer, root_de->location_le, root_de->length_le);
}

std::optional<ISOReader::ISODirectoryEntry> ISOReader::LocateFile(const char* path, uint8_t* sector_buffer,
                                                                  uint32_t directory_record_lba, uint32_t directory_record_size)
{
  if (directory_record_size == 0)
  {
    Log_ErrorPrintf("Directory entry record size 0 while looking for '%s'", path);
    return std::nullopt;
  }

  // strip any leading slashes
  const char* path_component_start = path;
  while (*path_component_start == '/' || *path_component_start == '\\')
    path_component_start++;

  uint32_t path_component_length = 0;
  const char* path_component_end = path_component_start;
  while (*path_component_end != '\0' && *path_component_end != '/' && *path_component_end != '\\')
  {
    path_component_length++;
    path_component_end++;
  }

  // start reading directory entries
  const uint32_t num_sectors = (directory_record_size + (SECTOR_SIZE - 1)) / SECTOR_SIZE;
  if (!m_image->Seek(m_track_number, directory_record_lba))
  {
    Log_ErrorPrintf("Seek to LBA %u failed", directory_record_lba);
    return std::nullopt;
  }

  for (uint32_t i = 0; i < num_sectors; i++)
  {
    if (m_image->Read(CDImage::ReadMode::DataOnly, 1, sector_buffer) != 1)
    {
      Log_ErrorPrintf("Failed to read LBA %u", directory_record_lba + i);
      return std::nullopt;
    }

    uint32_t sector_offset = 0;
    while ((sector_offset + sizeof(ISODirectoryEntry)) < SECTOR_SIZE)
    {
      const ISODirectoryEntry* de = reinterpret_cast<const ISODirectoryEntry*>(&sector_buffer[sector_offset]);
      const char* de_filename =
        reinterpret_cast<const char*>(&sector_buffer[sector_offset + sizeof(ISODirectoryEntry)]);
      if ((sector_offset + de->entry_length) > SECTOR_SIZE || de->filename_length > de->entry_length ||
          de->entry_length < sizeof(ISODirectoryEntry))
      {
        break;
      }

      sector_offset += de->entry_length;

      // skip current/parent directory
      if (de->filename_length == 1 && (*de_filename == '\x0' || *de_filename == '\x1'))
        continue;

      // check filename length
      if (de->filename_length < path_component_length)
        continue;

      if (de->flags & ISODirectoryEntryFlag_Directory)
      {
        // directories don't have the version? so check the length instead
        if (de->filename_length != path_component_length ||
            !FilenamesEqual(de_filename, path_component_start, path_component_length))
        {
          continue;
        }
      }
      else
      {
        // compare filename
        if (!FilenamesEqual(de_filename, path_component_start, path_component_length) ||
            de_filename[path_component_length] != ';')
        {
          continue;
        }
      }

      // found it. is this the file we're looking for?
      if (*path_component_end == '\0')
        return *de;

      // if it is a directory, recurse into it
      if (de->flags & ISODirectoryEntryFlag_Directory)
        return LocateFile(path_component_end, sector_buffer, de->location_le, de->length_le);

      // we're looking for a directory but got a file
      Log_ErrorPrintf("Looking for directory but got file");
      return std::nullopt;
    }
  }

  std::string temp(path_component_start, path_component_length);
  Log_ErrorPrintf("Path component '%s' not found", temp.c_str());
  return std::nullopt;
}

bool ISOReader::ReadFile(const char* path, std::vector<uint8_t>* data)
{
  auto de = LocateFile(path);
  if (!de)
  {
    Log_ErrorPrintf("File not found: '%s'", path);
    return false;
  }
  if (de->flags & ISODirectoryEntryFlag_Directory)
  {
    Log_ErrorPrintf("File is a directory: '%s'", path);
    return false;
  }

  if (!m_image->Seek(m_track_number, de->location_le))
    return false;

  if (de->length_le == 0)
  {
    data->clear();
    return true;
  }

  const uint32_t num_sectors = (de->length_le + (SECTOR_SIZE - 1)) / SECTOR_SIZE;
  data->resize(num_sectors * uint64_t(SECTOR_SIZE));
  if (m_image->Read(CDImage::ReadMode::DataOnly, num_sectors, data->data()) != num_sectors)
    return false;

  data->resize(de->length_le);
  return true;
}
