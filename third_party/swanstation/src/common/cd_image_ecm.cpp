#include "cd_image.h"
#include "cd_subchannel_replacement.h"
#include "error.h"
#include "file_system.h"
#include "log.h"
#include <array>
#include <cerrno>
#include <cstring>
#include <map>
Log_SetChannel(CDImageEcm);

// unecm.c by Neill Corlett (c) 2002, GPL licensed

/* LUTs used for computing ECC/EDC */

static constexpr std::array<uint8_t, 256> ComputeECCFLUT()
{
  std::array<uint8_t, 256> ecc_lut{};
  for (uint32_t i = 0; i < 256; i++)
  {
    uint32_t j = (i << 1) ^ (i & 0x80 ? 0x11D : 0);
    ecc_lut[i] = static_cast<uint8_t>(j);
  }
  return ecc_lut;
}

static constexpr std::array<uint8_t, 256> ComputeECCBLUT()
{
  std::array<uint8_t, 256> ecc_lut{};
  for (uint32_t i = 0; i < 256; i++)
  {
    uint32_t j = (i << 1) ^ (i & 0x80 ? 0x11D : 0);
    ecc_lut[i ^ j] = static_cast<uint8_t>(i);
  }
  return ecc_lut;
}

static constexpr std::array<uint32_t, 256> ComputeEDCLUT()
{
  std::array<uint32_t, 256> edc_lut{};
  for (uint32_t i = 0; i < 256; i++)
  {
    uint32_t edc = i;
    for (uint32_t k = 0; k < 8; k++)
      edc = (edc >> 1) ^ (edc & 1 ? 0xD8018001 : 0);
    edc_lut[i] = edc;
  }
  return edc_lut;
}

static constexpr std::array<uint8_t, 256> ecc_f_lut = ComputeECCFLUT();
static constexpr std::array<uint8_t, 256> ecc_b_lut = ComputeECCBLUT();
static constexpr std::array<uint32_t, 256> edc_lut = ComputeEDCLUT();

/***************************************************************************/
/*
** Compute EDC for a block
*/
static uint32_t edc_partial_computeblock(uint32_t edc, const uint8_t* src, uint16_t size)
{
  while (size--)
    edc = (edc >> 8) ^ edc_lut[(edc ^ (*src++)) & 0xFF];
  return edc;
}

static void edc_computeblock(const uint8_t* src, uint16_t size, uint8_t* dest)
{
  uint32_t edc = edc_partial_computeblock(0, src, size);
  dest[0] = (edc >> 0) & 0xFF;
  dest[1] = (edc >> 8) & 0xFF;
  dest[2] = (edc >> 16) & 0xFF;
  dest[3] = (edc >> 24) & 0xFF;
}

/***************************************************************************/
/*
** Compute ECC for a block (can do either P or Q)
*/
static void ecc_computeblock(uint8_t* src, uint32_t major_count, uint32_t minor_count, uint32_t major_mult, uint32_t minor_inc, uint8_t* dest)
{
  uint32_t size = major_count * minor_count;
  uint32_t major, minor;
  for (major = 0; major < major_count; major++)
  {
    uint32_t index = (major >> 1) * major_mult + (major & 1);
    uint8_t ecc_a = 0;
    uint8_t ecc_b = 0;
    for (minor = 0; minor < minor_count; minor++)
    {
      uint8_t temp = src[index];
      index += minor_inc;
      if (index >= size)
        index -= size;
      ecc_a ^= temp;
      ecc_b ^= temp;
      ecc_a = ecc_f_lut[ecc_a];
    }
    ecc_a = ecc_b_lut[ecc_f_lut[ecc_a] ^ ecc_b];
    dest[major] = ecc_a;
    dest[major + major_count] = ecc_a ^ ecc_b;
  }
}

/*
** Generate ECC P and Q codes for a block
*/
static void ecc_generate(uint8_t* sector, int zeroaddress)
{
  uint8_t address[4], i;
  /* Save the address and zero it out */
  if (zeroaddress)
    for (i = 0; i < 4; i++)
    {
      address[i] = sector[12 + i];
      sector[12 + i] = 0;
    }
  /* Compute ECC P code */
  ecc_computeblock(sector + 0xC, 86, 24, 2, 86, sector + 0x81C);
  /* Compute ECC Q code */
  ecc_computeblock(sector + 0xC, 52, 43, 86, 88, sector + 0x8C8);
  /* Restore the address */
  if (zeroaddress)
    for (i = 0; i < 4; i++)
      sector[12 + i] = address[i];
}

/***************************************************************************/
/*
** Generate ECC/EDC information for a sector (must be 2352 = 0x930 bytes)
** Returns 0 on success
*/
static void eccedc_generate(uint8_t* sector, int type)
{
  switch (type)
  {
    case 1: /* Mode 1 */
      /* Compute EDC */
      edc_computeblock(sector + 0x00, 0x810, sector + 0x810);
      /* Write out zero bytes */
      for (uint32_t i = 0; i < 8; i++)
        sector[0x814 + i] = 0;
      /* Generate ECC P/Q codes */
      ecc_generate(sector, 0);
      break;
    case 2: /* Mode 2 form 1 */
      /* Compute EDC */
      edc_computeblock(sector + 0x10, 0x808, sector + 0x818);
      /* Generate ECC P/Q codes */
      ecc_generate(sector, 1);
      break;
    case 3: /* Mode 2 form 2 */
      /* Compute EDC */
      edc_computeblock(sector + 0x10, 0x91C, sector + 0x92C);
      break;
  }
}

class CDImageEcm : public CDImage
{
public:
  CDImageEcm(OpenFlags open_flags);
  ~CDImageEcm() override;

  bool Open(const char* filename, Common::Error* error);

  bool ReadSubChannelQ(SubChannelQ* subq, const Index& index, LBA lba_in_index) override;
  bool HasNonStandardSubchannel() const override;

protected:
  bool ReadSectorFromIndex(void* buffer, const Index& index, LBA lba_in_index) override;

private:
  bool ReadChunks(uint32_t disc_offset, uint32_t size);

  /// Read @size bytes at absolute file @offset: memcpy out of the
  /// whole-file mapping when the open produced one, else a
  /// position-guarded seek + read (adjacent chunks skip the seek).
  bool ReadFileBytes(int64_t offset, void* dst, uint32_t size);

  RFILE* m_fp = nullptr;
  const uint8_t* m_map = nullptr;
  int64_t m_map_size = 0;
  int64_t m_file_position = -1;

  enum class SectorType : uint32_t
  {
    Raw = 0x00,
    Mode1 = 0x01,
    Mode2Form1 = 0x02,
    Mode2Form2 = 0x03,
    Count,
  };

  static constexpr std::array<uint32_t, static_cast<uint32_t>(SectorType::Count)> s_sector_sizes = {
    0x930, // raw
    0x803, // mode1
    0x804, // mode2form1
    0x918, // mode2form2
  };

  static constexpr std::array<uint32_t, static_cast<uint32_t>(SectorType::Count)> s_chunk_sizes = {
    0,    // raw
    2352, // mode1
    2336, // mode2form1
    2336, // mode2form2
  };

  struct SectorEntry
  {
    uint32_t file_offset;
    uint32_t chunk_size;
    SectorType type;
  };

  using DataMap = std::map<uint32_t, SectorEntry>;

  DataMap m_data_map;
  std::vector<uint8_t> m_chunk_buffer;
  uint32_t m_chunk_start = 0;

  CDSubChannelReplacement m_sbi;
};

CDImageEcm::CDImageEcm(OpenFlags open_flags) : CDImage(open_flags) {}

CDImageEcm::~CDImageEcm()
{
  if (m_fp)
    rfclose(m_fp);
}

namespace {

/// Forward-only reader over the chunk-header bytes of an ECM file.
///
/// ECM interleaves 1-5 byte chunk headers with 2-4 KiB payloads, and
/// the natural parse - getc per header byte, seek past each payload -
/// costs two VFS round trips per chunk.  A typical 330k-chunk disc
/// paid ~700k calls, and with a lookahead-buffered getc it was worse:
/// every seek discards the 16 KiB the previous getc pulled in, ~5 GiB
/// of read amplification against a 700 MiB file.
///
/// This reader works from the whole-file mapping when the open
/// produced one (no I/O calls at all - the header pages fault in via
/// OS readahead), and otherwise scans in 256 KiB blocks: one refill
/// covers ~110 chunks' worth of headers, in-block payload skips are
/// pointer arithmetic, and only a skip past the block's end (large
/// Raw runs) costs a seek.
struct EcmHeaderReader
{
  RFILE* fp;
  const uint8_t* map;
  int64_t file_size;
  std::vector<uint8_t> buf;
  int64_t buf_base = 0;
  uint32_t buf_len = 0;

  static constexpr uint32_t BLOCK_SIZE = 256 * 1024;

  EcmHeaderReader(RFILE* fp_, const uint8_t* map_, int64_t file_size_) : fp(fp_), map(map_), file_size(file_size_) {}

  /// Byte at absolute file offset, or EOF past the end / on error.
  int GetByte(int64_t offset)
  {
    if (offset < 0 || offset >= file_size)
      return EOF;
    if (map)
      return map[offset];

    if (offset < buf_base || offset >= (buf_base + static_cast<int64_t>(buf_len)))
    {
      if (buf.empty())
        buf.resize(BLOCK_SIZE);
      if (rfseek(fp, offset, SEEK_SET) != 0)
        return EOF;

      const int64_t nread = rfread(buf.data(), 1, BLOCK_SIZE, fp);
      if (nread <= 0)
        return EOF;

      buf_base = offset;
      buf_len = static_cast<uint32_t>(nread);
    }

    return buf[static_cast<size_t>(offset - buf_base)];
  }
};

} // namespace

bool CDImageEcm::Open(const char* filename, Common::Error* error)
{
  m_filename = filename;
  m_fp = FileSystem::OpenMappableRFile(filename);
  if (!m_fp)
  {
    Log_ErrorPrintf("Failed to open binfile '%s': errno %d", filename, errno);
    return false;
  }

  int64_t file_size;
  if (FileSystem::FSeek64(m_fp, 0, SEEK_END) != 0 || (file_size = FileSystem::FTell64(m_fp)) <= 0 ||
      FileSystem::FSeek64(m_fp, 0, SEEK_SET) != 0)
  {
    Log_ErrorPrintf("Get file size failed: errno %d", errno);
    return false;
  }

  // Borrowed view of the whole file when the platform mapped it;
  // nullptr is a normal answer (32-bit host, frontend-backed handle,
  // no mapping support) and every consumer below keeps a read path.
  m_map = FileSystem::GetMappedView(m_fp, &m_map_size);
  if (m_map && m_map_size < file_size)
  {
    // A mapping shorter than the file would turn bounds checks
    // against file_size into out-of-bounds reads; use plain I/O.
    m_map = nullptr;
    m_map_size = 0;
  }

  char header[4];
  if (rfread(header, sizeof(header), 1, m_fp) != 1 || header[0] != 'E' || header[1] != 'C' || header[2] != 'M' ||
      header[3] != 0)
  {
    Log_ErrorPrintf("Failed to read/invalid header");
    if (error)
      error->SetMessage("Failed to read/invalid header");

    return false;
  }

  // build sector map
  EcmHeaderReader reader(m_fp, m_map, file_size);
  int64_t file_offset = FileSystem::FTell64(m_fp);
  uint32_t disc_offset = 0;

  for (;;)
  {
    int bits = reader.GetByte(file_offset);
    if (bits == EOF)
    {
      Log_ErrorPrintf("Unexpected EOF after %zu chunks", m_data_map.size());
      if (error)
        error->SetFormattedMessage("Unexpected EOF after %zu chunks", m_data_map.size());

      return false;
    }

    file_offset++;
    const SectorType type = static_cast<SectorType>(static_cast<uint32_t>(bits) & 0x03u);
    uint32_t count = (static_cast<uint32_t>(bits) >> 2) & 0x1F;
    uint32_t shift = 5;
    while (bits & 0x80)
    {
      bits = reader.GetByte(file_offset);
      if (bits == EOF)
      {
        Log_ErrorPrintf("Unexpected EOF after %zu chunks", m_data_map.size());
        if (error)
          error->SetFormattedMessage("Unexpected EOF after %zu chunks", m_data_map.size());

        return false;
      }

      count |= (static_cast<uint32_t>(bits) & 0x7F) << shift;
      shift += 7;
      file_offset++;
    }

    if (count == 0xFFFFFFFFu)
      break;

    // for this sector
    count++;

    if (count >= 0x80000000u)
    {
      Log_ErrorPrintf("Corrupted header after %zu chunks", m_data_map.size());
      if (error)
        error->SetFormattedMessage("Corrupted header after %zu chunks", m_data_map.size());

      return false;
    }

    if (type == SectorType::Raw)
    {
      while (count > 0)
      {
        const uint32_t size = std::min<uint32_t>(count, 2352);
        m_data_map.emplace(disc_offset, SectorEntry{static_cast<uint32_t>(file_offset), size, type});
        disc_offset += size;
        file_offset += size;
        count -= size;

        if (file_offset > file_size)
        {
          Log_ErrorPrintf("Out of file bounds after %zu chunks", m_data_map.size());
          if (error)
            error->SetFormattedMessage("Out of file bounds after %zu chunks", m_data_map.size());
        }
      }
    }
    else
    {
      const uint32_t size = s_sector_sizes[static_cast<uint32_t>(type)];
      const uint32_t chunk_size = s_chunk_sizes[static_cast<uint32_t>(type)];
      for (uint32_t i = 0; i < count; i++)
      {
        m_data_map.emplace(disc_offset, SectorEntry{static_cast<uint32_t>(file_offset), chunk_size, type});
        disc_offset += chunk_size;
        file_offset += size;

        if (file_offset > file_size)
        {
          Log_ErrorPrintf("Out of file bounds after %zu chunks", m_data_map.size());
          if (error)
            error->SetFormattedMessage("Out of file bounds after %zu chunks", m_data_map.size());
        }
      }
    }

    // No per-chunk seek: the reader addresses header bytes by
    // absolute offset and skips payloads arithmetically.
  }

  if (m_data_map.empty())
  {
    Log_ErrorPrintf("No data in image '%s'", filename);
    if (error)
      error->SetFormattedMessage("No data in image '%s'", filename);

    return false;
  }

  m_lba_count = disc_offset / RAW_SECTOR_SIZE;
  if ((disc_offset % RAW_SECTOR_SIZE) != 0)
    Log_WarningPrintf("ECM image is misaligned with offset %u", disc_offset);
  if (m_lba_count == 0)
    return false;

  SubChannelQ::Control control = {};
  TrackMode mode = TrackMode::Mode2Raw;
  control.data = mode != TrackMode::Audio;

  // Two seconds default pregap.
  const uint32_t pregap_frames = 2 * FRAMES_PER_SECOND;
  Index pregap_index = {};
  pregap_index.file_sector_size = RAW_SECTOR_SIZE;
  pregap_index.start_lba_on_disc = 0;
  pregap_index.start_lba_in_track = static_cast<LBA>(-static_cast<int32_t>(pregap_frames));
  pregap_index.length = pregap_frames;
  pregap_index.track_number = 1;
  pregap_index.index_number = 0;
  pregap_index.mode = mode;
  pregap_index.control.bits = control.bits;
  pregap_index.is_pregap = true;
  m_indices.push_back(pregap_index);

  // Data index.
  Index data_index = {};
  data_index.file_index = 0;
  data_index.file_offset = 0;
  data_index.file_sector_size = RAW_SECTOR_SIZE;
  data_index.start_lba_on_disc = pregap_index.length;
  data_index.track_number = 1;
  data_index.index_number = 1;
  data_index.start_lba_in_track = 0;
  data_index.length = m_lba_count;
  data_index.mode = mode;
  data_index.control.bits = control.bits;
  m_indices.push_back(data_index);

  // Assume a single track.
  m_tracks.push_back(
    Track{static_cast<uint32_t>(1), data_index.start_lba_on_disc, static_cast<uint32_t>(0), m_lba_count, mode, control});

  AddLeadOutIndex();

  m_sbi.LoadSBIFromImagePath(filename);

  m_chunk_buffer.reserve(RAW_SECTOR_SIZE * 2);
  return Seek(1, Position{0, 0, 0});
}

bool CDImageEcm::ReadFileBytes(int64_t offset, void* dst, uint32_t size)
{
  if (m_map)
  {
    if (offset < 0 || size > m_map_size || offset > (m_map_size - static_cast<int64_t>(size)))
      return false;
    std::memcpy(dst, m_map + offset, size);
    return true;
  }

  if (m_file_position != offset)
  {
    if (rfseek(m_fp, offset, SEEK_SET) != 0)
      return false;
    m_file_position = offset;
  }

  if (rfread(dst, 1, size, m_fp) != static_cast<int64_t>(size))
  {
    m_file_position = -1;
    return false;
  }

  m_file_position += size;
  return true;
}

bool CDImageEcm::ReadChunks(uint32_t disc_offset, uint32_t size)
{
  DataMap::iterator next =
    m_data_map.lower_bound((disc_offset > RAW_SECTOR_SIZE) ? (disc_offset - RAW_SECTOR_SIZE) : 0);
  DataMap::iterator current = m_data_map.begin();
  while (next != m_data_map.end() && next->first <= disc_offset)
    current = next++;

  // extra bytes if we need to buffer some at the start
  m_chunk_start = current->first;
  m_chunk_buffer.clear();
  if (m_chunk_start < disc_offset)
    size += (disc_offset - current->first);

  uint32_t total_bytes_read = 0;
  while (total_bytes_read < size)
  {
    if (current == m_data_map.end())
      return false;

    const int64_t file_offset = current->second.file_offset;
    const uint32_t chunk_size = current->second.chunk_size;
    const uint32_t chunk_start = static_cast<uint32_t>(m_chunk_buffer.size());
    m_chunk_buffer.resize(chunk_start + chunk_size);

    if (current->second.type == SectorType::Raw)
    {
      if (!ReadFileBytes(file_offset, &m_chunk_buffer[chunk_start], chunk_size))
        return false;

      total_bytes_read += chunk_size;
    }
    else
    {
      uint8_t sector[RAW_SECTOR_SIZE];

      // TODO: needed?
      std::memset(sector, 0, RAW_SECTOR_SIZE);
      std::memset(sector + 1, 0xFF, 10);

      uint32_t skip;
      switch (current->second.type)
      {
        case SectorType::Mode1:
        {
          sector[0x0F] = 0x01;
          if (!ReadFileBytes(file_offset, sector + 0x00C, 0x003) ||
              !ReadFileBytes(file_offset + 0x003, sector + 0x010, 0x800))
            return false;

          eccedc_generate(sector, 1);
          skip = 0;
        }
        break;

        case SectorType::Mode2Form1:
        {
          sector[0x0F] = 0x02;
          if (!ReadFileBytes(file_offset, sector + 0x014, 0x804))
            return false;

          sector[0x10] = sector[0x14];
          sector[0x11] = sector[0x15];
          sector[0x12] = sector[0x16];
          sector[0x13] = sector[0x17];

          eccedc_generate(sector, 2);
          skip = 0x10;
        }
        break;

        case SectorType::Mode2Form2:
        {
          sector[0x0F] = 0x02;
          if (!ReadFileBytes(file_offset, sector + 0x014, 0x918))
            return false;

          sector[0x10] = sector[0x14];
          sector[0x11] = sector[0x15];
          sector[0x12] = sector[0x16];
          sector[0x13] = sector[0x17];

          eccedc_generate(sector, 3);
          skip = 0x10;
        }
        break;

        default:
          return false;
      }

      std::memcpy(&m_chunk_buffer[chunk_start], sector + skip, chunk_size);
      total_bytes_read += chunk_size;
    }

    ++current;
  }

  return true;
}

bool CDImageEcm::ReadSubChannelQ(SubChannelQ* subq, const Index& index, LBA lba_in_index)
{
  if (m_sbi.GetReplacementSubChannelQ(index.start_lba_on_disc + lba_in_index, subq))
    return true;

  return CDImage::ReadSubChannelQ(subq, index, lba_in_index);
}

bool CDImageEcm::HasNonStandardSubchannel() const
{
  return (m_sbi.GetReplacementSectorCount() > 0);
}

bool CDImageEcm::ReadSectorFromIndex(void* buffer, const Index& index, LBA lba_in_index)
{
  const uint32_t file_start = static_cast<uint32_t>(index.file_offset) + (lba_in_index * index.file_sector_size);
  const uint32_t file_end = file_start + RAW_SECTOR_SIZE;

  if (file_start < m_chunk_start || file_end > (m_chunk_start + m_chunk_buffer.size()))
  {
    if (!ReadChunks(file_start, RAW_SECTOR_SIZE))
      return false;
  }

  const size_t chunk_offset = static_cast<size_t>(file_start - m_chunk_start);
  std::memcpy(buffer, &m_chunk_buffer[chunk_offset], RAW_SECTOR_SIZE);
  return true;
}

std::unique_ptr<CDImage> CDImage::OpenEcmImage(const char* filename, OpenFlags open_flags, Common::Error* error)
{
  std::unique_ptr<CDImageEcm> image = std::make_unique<CDImageEcm>(open_flags);
  if (!image->Open(filename, error))
    return {};

  return image;
}
