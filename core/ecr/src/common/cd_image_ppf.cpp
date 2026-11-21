#include "cd_image.h"
#include "cd_subchannel_replacement.h"
#include "file_system.h"
#include "log.h"
#include <formats/data_transfer.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <map>
#include <unordered_map>
Log_SetChannel(CDImagePPF);

static constexpr uint32_t DESC_SIZE = 50, BLOCKCHECK_SIZE = 1024;

class CDImagePPF : public CDImage
{
public:
  CDImagePPF(OpenFlags open_flags);
  ~CDImagePPF() override;

  bool Open(const char* filename, std::unique_ptr<CDImage> parent_image);

  bool ReadSubChannelQ(SubChannelQ* subq, const Index& index, LBA lba_in_index) override;
  bool HasNonStandardSubchannel() const override;

  std::string GetMetadata(const std::string_view& type) const override;
  std::string GetSubImageMetadata(uint32_t index, const std::string_view& type) const override;

protected:
  bool ReadSectorFromIndex(void* buffer, const Index& index, LBA lba_in_index) override;

private:
  bool ReadV1Patch(const uint8_t* data, uint32_t len);
  bool ReadV2Patch(const uint8_t* data, uint32_t len);
  bool ReadV3Patch(const uint8_t* data, uint32_t len);
  uint32_t ReadFileIDDiz(const uint8_t* data, uint32_t len, uint32_t version);

  bool AddPatch(uint64_t offset, const uint8_t* patch, uint32_t patch_size);

  std::unique_ptr<CDImage> m_parent_image;
  std::vector<uint8_t> m_replacement_data;
  std::unordered_map<uint32_t, uint32_t> m_replacement_map;
  uint32_t m_replacement_offset = 0;
};

CDImagePPF::CDImagePPF(OpenFlags open_flags) : CDImage(open_flags) {}

CDImagePPF::~CDImagePPF() = default;

bool CDImagePPF::Open(const char* filename, std::unique_ptr<CDImage> parent_image)
{
  // A PPF is thousands of 5-9 byte entry headers followed by short
  // payloads; parsed through the stream API that was three unbuffered
  // VFS reads per entry.  Pull the whole patch through a
  // data_transfer prefix instead - one open, a few large reads - and
  // parse from the base pointer with explicit bounds.  The transfer
  // reports a short file as failure with an honest byte count rather
  // than as completion, and on reserve-capable platforms the
  // uncommitted tail past avail() is hardware-protected, so a parser
  // overrun faults instead of consuming garbage.
  data_transfer_t* dt = data_transfer_open_prefix(filename, 0);
  if (!dt)
  {
    Log_ErrorPrintf("Failed to open '%s'", filename);
    return false;
  }

  data_transfer_iterate(dt, 0);
  if (!data_transfer_complete(dt))
  {
    Log_ErrorPrintf("Failed to read '%s'", filename);
    data_transfer_free(dt);
    return false;
  }

  size_t data_len_sz = 0;
  const uint8_t* data = data_transfer_ptr(dt, &data_len_sz);
  if (!data || data_len_sz < sizeof(uint32_t) || data_len_sz > UINT32_MAX)
  {
    Log_ErrorPrintf("Invalid ppf file '%s'", filename);
    data_transfer_free(dt);
    return false;
  }

  const uint32_t data_len = static_cast<uint32_t>(data_len_sz);

  uint32_t magic;
  std::memcpy(&magic, data, sizeof(magic));

  // work out the offset from the start of the parent image which we need to patch
  // i.e. the two second implicit pregap on data sectors
  if (parent_image->GetTrack(1).mode != TrackMode::Audio)
    m_replacement_offset = parent_image->GetIndex(1).start_lba_on_disc;

  // copy all the stuff from the parent image
  m_filename = parent_image->GetFileName();
  m_tracks = parent_image->GetTracks();
  m_indices = parent_image->GetIndices();
  m_parent_image = std::move(parent_image);

  bool result = false;
  if (magic == 0x33465050) // PPF3
    result = ReadV3Patch(data, data_len);
  else if (magic == 0x32465050) // PPF2
    result = ReadV2Patch(data, data_len);
  else if (magic == 0x31465050) // PPF1
    result = ReadV1Patch(data, data_len);
  else
    Log_ErrorPrintf("Unknown PPF magic %08X", magic);

  data_transfer_free(dt);
  return result;
}

uint32_t CDImagePPF::ReadFileIDDiz(const uint8_t* data, uint32_t len, uint32_t version)
{
  const uint32_t lenidx = (version == 2) ? 4 : 2;

  uint32_t magic = 0;
  if (len < (lenidx + 4))
  {
    Log_WarningPrintf("Failed to read diz magic");
    return 0;
  }
  std::memcpy(&magic, &data[len - (lenidx + 4)], sizeof(magic));

  if (magic != 0x5A49442E) // .DIZ
    return 0;

  uint32_t dlen = 0;
  std::memcpy(&dlen, &data[len - lenidx], lenidx);

  if (dlen > len)
  {
    Log_WarningPrintf("diz length out of range");
    return 0;
  }

  const uint32_t diz_tail = lenidx + 16 + dlen;
  if (diz_tail > len)
  {
    Log_WarningPrintf("Failed to read fdiz");
    return 0;
  }

  std::string fdiz(reinterpret_cast<const char*>(&data[len - diz_tail]), dlen);
  Log_InfoPrintf("File_Id.diz: %s", fdiz.c_str());
  return dlen;
}

bool CDImagePPF::ReadV1Patch(const uint8_t* data, uint32_t len)
{
  char desc[DESC_SIZE + 1] = {};
  if (len < 56)
  {
    Log_ErrorPrintf("Invalid ppf file");
    return false;
  }
  std::memcpy(desc, &data[6], DESC_SIZE);

  uint32_t count = len - 56;
  if (count == 0)
    return false;

  uint32_t pos = 56;
  while (count > 0)
  {
    uint32_t offset;
    uint8_t chunk_size;
    if (count < (sizeof(offset) + sizeof(chunk_size)))
    {
      Log_ErrorPrintf("Incomplete ppf");
      return false;
    }

    std::memcpy(&offset, &data[pos], sizeof(offset));
    chunk_size = data[pos + sizeof(offset)];
    pos += sizeof(offset) + sizeof(chunk_size);
    count -= sizeof(offset) + sizeof(chunk_size);

    if (count < chunk_size)
    {
      Log_ErrorPrintf("Failed to read patch data");
      return false;
    }

    if (!AddPatch(offset, &data[pos], chunk_size))
      return false;

    pos += chunk_size;
    count -= chunk_size;
  }

  Log_InfoPrintf("Loaded %zu replacement sectors from version 1 PPF", m_replacement_map.size());
  return true;
}

bool CDImagePPF::ReadV2Patch(const uint8_t* data, uint32_t len)
{
  uint32_t origlen;
  char desc[DESC_SIZE + 1] = {};
  if (len < 1084)
  {
    Log_ErrorPrintf("Invalid ppf file");
    return false;
  }
  std::memcpy(desc, &data[6], DESC_SIZE);

  Log_InfoPrintf("Patch description: %s", desc);

  const uint32_t idlen = ReadFileIDDiz(data, len, 2);

  std::memcpy(&origlen, &data[56], sizeof(origlen));

  // do blockcheck
  {
    uint32_t blockcheck_src_sector = 16 + m_replacement_offset;
    uint32_t blockcheck_src_offset = 32;

    std::vector<uint8_t> src_sector(RAW_SECTOR_SIZE);
    if (m_parent_image->Seek(blockcheck_src_sector) && m_parent_image->ReadRawSector(src_sector.data(), nullptr))
    {
      if (std::memcmp(&src_sector[blockcheck_src_offset], &data[60], BLOCKCHECK_SIZE) != 0)
        Log_WarningPrintf("Blockcheck failed. The patch may not apply correctly.");
    }
    else
    {
      Log_WarningPrintf("Failed to read blockcheck sector %u", blockcheck_src_sector);
    }
  }

  uint32_t count = len - 1084;
  if (idlen > 0)
  {
    if ((idlen + 38) > count)
    {
      Log_ErrorPrintf("File is too short (diz)");
      return false;
    }
    count -= (idlen + 38);
  }

  if (count == 0)
    return false;

  uint32_t pos = 1084;
  while (count > 0)
  {
    uint32_t offset;
    uint8_t chunk_size;
    if (count < (sizeof(offset) + sizeof(chunk_size)))
    {
      Log_ErrorPrintf("Incomplete ppf");
      return false;
    }

    std::memcpy(&offset, &data[pos], sizeof(offset));
    chunk_size = data[pos + sizeof(offset)];
    pos += sizeof(offset) + sizeof(chunk_size);
    count -= sizeof(offset) + sizeof(chunk_size);

    if (count < chunk_size)
    {
      Log_ErrorPrintf("Failed to read patch data");
      return false;
    }

    if (!AddPatch(offset, &data[pos], chunk_size))
      return false;

    pos += chunk_size;
    count -= chunk_size;
  }

  Log_InfoPrintf("Loaded %zu replacement sectors from version 2 PPF", m_replacement_map.size());
  return true;
}

bool CDImagePPF::ReadV3Patch(const uint8_t* data, uint32_t len)
{
  char desc[DESC_SIZE + 1] = {};
  if (len < 59)
  {
    Log_ErrorPrintf("Failed to read headers");
    return false;
  }
  std::memcpy(desc, &data[6], DESC_SIZE);

  Log_InfoPrintf("Patch description: %s", desc);

  uint32_t idlen = ReadFileIDDiz(data, len, 3);

  const uint8_t block_check = data[57];

  // TODO: Blockcheck

  uint32_t count = len;

  uint32_t seekpos = (block_check) ? 1084 : 60;
  if (seekpos >= count)
  {
    Log_ErrorPrintf("File is too short");
    return false;
  }

  count -= seekpos;
  if (idlen > 0)
  {
    const uint32_t extralen = idlen + 18 + 16 + 2;
    if (count < extralen)
    {
      Log_ErrorPrintf("File is too short (diz)");
      return false;
    }

    count -= extralen;
  }

  uint32_t pos = seekpos;
  while (count > 0)
  {
    uint64_t offset;
    uint8_t chunk_size;
    if (count < (sizeof(offset) + sizeof(chunk_size)))
    {
      Log_ErrorPrintf("Incomplete ppf");
      return false;
    }

    std::memcpy(&offset, &data[pos], sizeof(offset));
    chunk_size = data[pos + sizeof(offset)];
    pos += sizeof(offset) + sizeof(chunk_size);
    count -= sizeof(offset) + sizeof(chunk_size);

    if (count < chunk_size)
    {
      Log_ErrorPrintf("Failed to read patch data");
      return false;
    }

    if (!AddPatch(offset, &data[pos], chunk_size))
      return false;

    pos += chunk_size;
    count -= chunk_size;
  }

  Log_InfoPrintf("Loaded %zu replacement sectors from version 3 PPF", m_replacement_map.size());
  return true;
}

bool CDImagePPF::AddPatch(uint64_t offset, const uint8_t* patch, uint32_t patch_size)
{
  while (patch_size > 0)
  {
    const uint32_t sector_index = static_cast<uint32_t>(offset / RAW_SECTOR_SIZE) + m_replacement_offset;
    const uint32_t sector_offset = static_cast<uint32_t>(offset % RAW_SECTOR_SIZE);
    if (sector_index >= m_parent_image->GetLBACount())
    {
      Log_ErrorPrintf("Sector %u in patch is out of range", sector_index);
      return false;
    }

    const uint32_t bytes_to_patch = std::min(patch_size, RAW_SECTOR_SIZE - sector_offset);

    auto iter = m_replacement_map.find(sector_index);
    if (iter == m_replacement_map.end())
    {
      const uint32_t replacement_buffer_start = static_cast<uint32_t>(m_replacement_data.size());
      m_replacement_data.resize(m_replacement_data.size() + RAW_SECTOR_SIZE);
      if (!m_parent_image->Seek(sector_index) ||
          !m_parent_image->ReadRawSector(&m_replacement_data[replacement_buffer_start], nullptr))
      {
        Log_ErrorPrintf("Failed to read sector %u from parent image", sector_index);
        return false;
      }

      iter = m_replacement_map.emplace(sector_index, replacement_buffer_start).first;
    }

    // patch it!
    std::memcpy(&m_replacement_data[iter->second + sector_offset], patch, bytes_to_patch);
    offset += bytes_to_patch;
    patch += bytes_to_patch;
    patch_size -= bytes_to_patch;
  }

  return true;
}

bool CDImagePPF::ReadSubChannelQ(SubChannelQ* subq, const Index& index, LBA lba_in_index)
{
  return m_parent_image->ReadSubChannelQ(subq, index, lba_in_index);
}

bool CDImagePPF::HasNonStandardSubchannel() const
{
  return m_parent_image->HasNonStandardSubchannel();
}

std::string CDImagePPF::GetMetadata(const std::string_view& type) const
{
  return m_parent_image->GetMetadata(type);
}

std::string CDImagePPF::GetSubImageMetadata(uint32_t index, const std::string_view& type) const
{
  // We only support a single sub-image for patched games.
  std::string ret;
  if (index == 0)
    ret = m_parent_image->GetSubImageMetadata(index, type);

  return ret;
}

bool CDImagePPF::ReadSectorFromIndex(void* buffer, const Index& index, LBA lba_in_index)
{
  const uint32_t sector_number = index.start_lba_on_disc + lba_in_index;
  const auto it = m_replacement_map.find(sector_number);
  if (it == m_replacement_map.end())
    return m_parent_image->ReadSectorFromIndex(buffer, index, lba_in_index);

  std::memcpy(buffer, &m_replacement_data[it->second], RAW_SECTOR_SIZE);
  return true;
}

std::unique_ptr<CDImage>
CDImage::OverlayPPFPatch(const char* filename, OpenFlags open_flags, std::unique_ptr<CDImage> parent_image,
                         ProgressCallback* progress /* = ProgressCallback::NullProgressCallback */)
{
  std::unique_ptr<CDImagePPF> ppf_image = std::make_unique<CDImagePPF>(open_flags);
  if (!ppf_image->Open(filename, std::move(parent_image)))
    return {};

  return ppf_image;
}
