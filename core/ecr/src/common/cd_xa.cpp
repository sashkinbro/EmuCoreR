#include "cd_xa.h"
#include "cd_image.h"
#include <algorithm>
#include <array>

namespace CDXA {
static constexpr std::array<int32_t, 4> s_xa_adpcm_filter_table_pos = {{0, 60, 115, 98}};
static constexpr std::array<int32_t, 4> s_xa_adpcm_filter_table_neg = {{0, 0, -52, -55}};

template<bool IS_STEREO, bool IS_8BIT>
static void DecodeXA_ADPCMChunk(const uint8_t* chunk_ptr, int16_t* samples, int32_t* last_samples)
{
  // The data layout is annoying here. Each word of data is interleaved with the other blocks, requiring multiple
  // passes to decode the whole chunk.
  constexpr uint32_t NUM_BLOCKS = IS_8BIT ? 4 : 8;
  constexpr uint32_t WORDS_PER_BLOCK = 28;

  const uint8_t* headers_ptr = chunk_ptr + 4;
  const uint8_t* words_ptr = chunk_ptr + 16;

  for (uint32_t block = 0; block < NUM_BLOCKS; block++)
  {
    const XA_ADPCMBlockHeader block_header{headers_ptr[block]};
    const uint8_t shift = block_header.GetShift();
    const uint8_t filter = block_header.GetFilter();
    const int32_t filter_pos = s_xa_adpcm_filter_table_pos[filter];
    const int32_t filter_neg = s_xa_adpcm_filter_table_neg[filter];

    int16_t* out_samples_ptr =
      IS_STEREO ? &samples[(block / 2) * (WORDS_PER_BLOCK * 2) + (block % 2)] : &samples[block * WORDS_PER_BLOCK];
    constexpr uint32_t out_samples_increment = IS_STEREO ? 2 : 1;

    for (uint32_t word = 0; word < 28; word++)
    {
      // The 32-bit XA-ADPCM word as stored on disc is little-endian.
      // The original code memcpy'd four host-order bytes into a uint32_t
      // and then shifted: that works on a little-endian host (where
      // the LE-on-disc layout matches the host layout) but reverses
      // which nibble the (block * N) shift selects on a big-endian
      // host. Build the uint32_t explicitly from individual bytes so the
      // subsequent shifts pick the same nibble regardless of host
      // byte order.
      const uint8_t* const word_bytes = &words_ptr[word * sizeof(uint32_t)];
      const uint32_t word_data = static_cast<uint32_t>(word_bytes[0])
                          | (static_cast<uint32_t>(word_bytes[1]) << 8)
                          | (static_cast<uint32_t>(word_bytes[2]) << 16)
                          | (static_cast<uint32_t>(word_bytes[3]) << 24);

      // extract nibble from block
      const uint32_t nibble = IS_8BIT ? ((word_data >> (block * 8)) & 0xFF) : ((word_data >> (block * 4)) & 0x0F);
      const int16_t sample = static_cast<int16_t>(static_cast<uint16_t>(nibble << 12)) >> shift;

      // mix in previous values
      int32_t* prev = IS_STEREO ? &last_samples[(block & 1) * 2] : last_samples;
      const int32_t interp_sample = int32_t(sample) + ((prev[0] * filter_pos) + (prev[1] * filter_neg) + 32) / 64;

      // update previous values
      prev[1] = prev[0];
      prev[0] = interp_sample;

      *out_samples_ptr = static_cast<int16_t>(std::clamp<int32_t>(interp_sample, -0x8000, 0x7FFF));
      out_samples_ptr += out_samples_increment;
    }
  }
}

template<bool IS_STEREO, bool IS_8BIT>
static void DecodeXA_ADPCMChunks(const uint8_t* chunk_ptr, int16_t* samples, int32_t* last_samples)
{
  constexpr uint32_t NUM_CHUNKS = 18;
  constexpr uint32_t CHUNK_SIZE_IN_BYTES = 128;
  constexpr uint32_t WORDS_PER_CHUNK = 28;
  constexpr uint32_t SAMPLES_PER_CHUNK = WORDS_PER_CHUNK * (IS_8BIT ? 4 : 8);

  for (uint32_t i = 0; i < NUM_CHUNKS; i++)
  {
    DecodeXA_ADPCMChunk<IS_STEREO, IS_8BIT>(chunk_ptr, samples, last_samples);
    samples += SAMPLES_PER_CHUNK;
    chunk_ptr += CHUNK_SIZE_IN_BYTES;
  }
}

void DecodeADPCMSector(const void* data, int16_t* samples, int32_t* last_samples)
{
  const XASubHeader* subheader = reinterpret_cast<const XASubHeader*>(
    reinterpret_cast<const uint8_t*>(data) + CDImage::SECTOR_SYNC_SIZE + sizeof(CDImage::SectorHeader));

  // The XA subheader is repeated?
  const uint8_t* chunk_ptr = reinterpret_cast<const uint8_t*>(data) + CDImage::SECTOR_SYNC_SIZE + sizeof(CDImage::SectorHeader) +
                        sizeof(XASubHeader) + 4;

  if (subheader->codinginfo.bits_per_sample != 1)
  {
    if (subheader->codinginfo.mono_stereo != 1)
      DecodeXA_ADPCMChunks<false, false>(chunk_ptr, samples, last_samples);
    else
      DecodeXA_ADPCMChunks<true, false>(chunk_ptr, samples, last_samples);
  }
  else
  {
    if (subheader->codinginfo.mono_stereo != 1)
      DecodeXA_ADPCMChunks<false, true>(chunk_ptr, samples, last_samples);
    else
      DecodeXA_ADPCMChunks<true, true>(chunk_ptr, samples, last_samples);
  }
}

} // namespace CDXA
