#include "cd_image.h"
#include "cd_subchannel_replacement.h"
#include "file_system.h"
#include "log.h"
#include <formats/data_transfer.h>
#include <cerrno>
#include <cstring>
#include <limits>
Log_SetChannel(CDImageMemory);

class CDImageMemory : public CDImage
{
public:
  CDImageMemory(OpenFlags open_flags);
  ~CDImageMemory() override;

  bool CopyImage(CDImage* image, ProgressCallback* progress);

  bool ReadSubChannelQ(SubChannelQ* subq, const Index& index, LBA lba_in_index) override;
  bool HasNonStandardSubchannel() const override;

protected:
  bool ReadSectorFromIndex(void* buffer, const Index& index, LBA lba_in_index) override;

private:
  uint8_t* m_memory = nullptr;
  uint32_t m_memory_sectors = 0;
  CDSubChannelReplacement m_sbi;
};

CDImageMemory::CDImageMemory(OpenFlags open_flags) : CDImage(open_flags) {}

CDImageMemory::~CDImageMemory()
{
  if (m_memory)
    std::free(m_memory);
}

namespace {

/// Producer state for the data_transfer source below: pulls decoded
/// sectors out of the parent CDImage in index order.
struct SectorProducer
{
  CDImage* image;
  uint32_t index_index;
  uint32_t lba_in_index;
  bool failed;
};

/// data_transfer source callback: fill dst with as many whole decoded
/// sectors as the pacing hint asks for (rounded up to one - producers
/// with chunked granularity may overshoot; dst always has room to the
/// declared end).  Returns bytes produced, 0 at end, negative on a
/// read failure.
int64_t ReadImageSectors(void* ud, uint8_t* dst, size_t n)
{
  SectorProducer* prod = static_cast<SectorProducer*>(ud);
  int64_t produced = 0;

  do
  {
    // advance past exhausted and zero-sized (blank pregap) indices
    while (prod->index_index < prod->image->GetIndexCount())
    {
      const CDImage::Index& idx = prod->image->GetIndex(prod->index_index);
      if (idx.file_sector_size == 0 || prod->lba_in_index >= idx.length)
      {
        prod->index_index++;
        prod->lba_in_index = 0;
        continue;
      }
      break;
    }

    if (prod->index_index >= prod->image->GetIndexCount())
      return produced; // end of stream (0 when nothing was produced)

    const CDImage::Index& index = prod->image->GetIndex(prod->index_index);
    if (!prod->image->ReadSectorFromIndex(dst, index, prod->lba_in_index))
    {
      Log_ErrorPrintf("Failed to read LBA %u in index %u", prod->lba_in_index, prod->index_index);
      prod->failed = true;
      return -1;
    }

    prod->lba_in_index++;
    dst += CDImage::RAW_SECTOR_SIZE;
    produced += CDImage::RAW_SECTOR_SIZE;
  } while (static_cast<size_t>(produced) < n);

  return produced;
}

struct ProgressHook
{
  ProgressCallback* progress;
};

/// Consulted between the fill's internal pulls: report progress from
/// inside the fill rather than wrapping iterate() in a timing loop.
bool ReportProgress(void* ud, size_t avail, size_t len)
{
  ProgressHook* hook = static_cast<ProgressHook*>(ud);
  (void)len;
  hook->progress->SetProgressValue(static_cast<uint32_t>(avail / CDImage::RAW_SECTOR_SIZE));
  return true;
}

} // namespace

bool CDImageMemory::CopyImage(CDImage* image, ProgressCallback* progress)
{
  // figure out the total number of sectors (not including blank pregaps)
  m_memory_sectors = 0;
  for (uint32_t i = 0; i < image->GetIndexCount(); i++)
  {
    const Index& index = image->GetIndex(i);
    if (index.file_sector_size > 0)
      m_memory_sectors += image->GetIndex(i).length;
  }

  const uint64_t memory_size =
    static_cast<uint64_t>(RAW_SECTOR_SIZE) * static_cast<uint64_t>(m_memory_sectors);
  if (memory_size >= static_cast<uint64_t>(std::numeric_limits<size_t>::max()))
  {
    progress->DisplayFormattedModalError("Insufficient address space");
    return false;
  }

  progress->SetFormattedStatusText("Allocating memory for %u sectors...", m_memory_sectors);

  // Decode the parent image through a producer-backed data_transfer:
  // the total length is known up front, so the buffer is one exact
  // malloc the image adopts on detach.  The producer decodes a full
  // pacing chunk of sectors per pull instead of one 2352-byte call
  // per sector, and the
  // continue-hook reports progress from between those pulls - no
  // per-sector callback, no partial fill mistaken for completion (a
  // short read settles the transfer failed, never complete).
  SectorProducer producer = {image, 0, 0, false};
  data_transfer_t* dt = data_transfer_open_source(static_cast<size_t>(memory_size), ReadImageSectors, &producer);
  if (!dt)
  {
    progress->DisplayFormattedModalError("Failed to allocate memory for %u sectors", m_memory_sectors);
    return false;
  }

  progress->SetStatusText("Preloading CD image to RAM...");
  progress->SetProgressRange(m_memory_sectors);
  progress->SetProgressValue(0);

  ProgressHook hook = {progress};
  data_transfer_iterate_while(dt, 0, ReportProgress, &hook);

  if (!data_transfer_complete(dt))
  {
    data_transfer_free(dt);
    return false;
  }

  size_t detached_len = 0;
  m_memory = data_transfer_source_detach(dt, &detached_len);
  if (!m_memory || detached_len != static_cast<size_t>(memory_size))
  {
    // detach frees the transfer on success; only a NULL return leaves
    // it alive
    if (!m_memory)
      data_transfer_free(dt);
    return false;
  }

  progress->SetProgressValue(m_memory_sectors);

  for (uint32_t i = 1; i <= image->GetTrackCount(); i++)
    m_tracks.push_back(image->GetTrack(i));

  uint32_t current_offset = 0;
  for (uint32_t i = 0; i < image->GetIndexCount(); i++)
  {
    Index new_index = image->GetIndex(i);
    new_index.file_index = 0;
    if (new_index.file_sector_size > 0)
    {
      new_index.file_offset = current_offset;
      current_offset += new_index.length;
    }
    m_indices.push_back(new_index);
  }

  m_filename = image->GetFileName();
  m_lba_count = image->GetLBACount();

  m_sbi.LoadSBI(FileSystem::ReplaceExtension(m_filename, "sbi").c_str());

  return Seek(1, Position{0, 0, 0});
}

bool CDImageMemory::ReadSubChannelQ(SubChannelQ* subq, const Index& index, LBA lba_in_index)
{
  if (m_sbi.GetReplacementSubChannelQ(index.start_lba_on_disc + lba_in_index, subq))
    return true;

  return CDImage::ReadSubChannelQ(subq, index, lba_in_index);
}

bool CDImageMemory::HasNonStandardSubchannel() const
{
  return (m_sbi.GetReplacementSectorCount() > 0);
}

bool CDImageMemory::ReadSectorFromIndex(void* buffer, const Index& index, LBA lba_in_index)
{
  const uint64_t sector_number = index.file_offset + lba_in_index;
  if (sector_number >= m_memory_sectors)
    return false;

  const size_t file_offset = static_cast<size_t>(sector_number) * static_cast<size_t>(RAW_SECTOR_SIZE);
  std::memcpy(buffer, &m_memory[file_offset], RAW_SECTOR_SIZE);
  return true;
}

std::unique_ptr<CDImage>
CDImage::CreateMemoryImage(CDImage* image, ProgressCallback* progress /* = ProgressCallback::NullProgressCallback */)
{
  std::unique_ptr<CDImageMemory> memory_image = std::make_unique<CDImageMemory>(image->GetOpenFlags());
  if (!memory_image->CopyImage(image, progress))
    return {};

  return memory_image;
}
