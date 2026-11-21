#pragma once
#include "bitfield.h"
#include "types.h"

namespace CDXA {

inline constexpr uint32_t XA_SUBHEADER_SIZE = 4,
                     XA_ADPCM_SAMPLES_PER_SECTOR_4BIT = 4032, // 28 words * 8 nibbles per word * 18 chunks
  XA_ADPCM_SAMPLES_PER_SECTOR_8BIT = 2016;                    // 28 words * 4 bytes per word * 18 chunks

struct XASubHeader
{
  uint8_t file_number;
  uint8_t channel_number;
  union Submode
  {
    uint8_t bits;
    BitField<uint8_t, bool, 0, 1> eor;
    BitField<uint8_t, bool, 1, 1> video;
    BitField<uint8_t, bool, 2, 1> audio;
    BitField<uint8_t, bool, 3, 1> data;
    BitField<uint8_t, bool, 4, 1> trigger;
    BitField<uint8_t, bool, 5, 1> form2;
    BitField<uint8_t, bool, 6, 1> realtime;
    BitField<uint8_t, bool, 7, 1> eof;
  } submode;
  union Codinginfo
  {
    uint8_t bits;

    BitField<uint8_t, uint8_t, 0, 2> mono_stereo;
    BitField<uint8_t, uint8_t, 2, 2> sample_rate;
    BitField<uint8_t, uint8_t, 4, 2> bits_per_sample;
    BitField<uint8_t, bool, 6, 1> emphasis;

    bool IsStereo() const { return mono_stereo == 1; }
    bool IsHalfSampleRate() const { return sample_rate == 1; }
    uint32_t GetSampleRate() const { return sample_rate == 1 ? 18900 : 37800; }
    uint32_t GetBitsPerSample() const { return bits_per_sample == 1 ? 8 : 4; }
    uint32_t GetSamplesPerSector() const
    {
      return bits_per_sample == 1 ? XA_ADPCM_SAMPLES_PER_SECTOR_8BIT : XA_ADPCM_SAMPLES_PER_SECTOR_4BIT;
    }
  } codinginfo;
};

union XA_ADPCMBlockHeader
{
  uint8_t bits;

  BitField<uint8_t, uint8_t, 0, 4> shift;
  BitField<uint8_t, uint8_t, 4, 2> filter;

  // For both 4bit and 8bit ADPCM, reserved shift values 13..15 will act same as shift=9).
  uint8_t GetShift() const
  {
    const uint8_t shift_value = shift;
    return (shift_value > 12) ? 9 : shift_value;
  }

  uint8_t GetFilter() const { return filter; }
};

// Decodes XA-ADPCM samples in an audio sector. Stereo samples are interleaved with left first.
void DecodeADPCMSector(const void* data, int16_t* samples, int32_t* last_samples);

} // namespace CDXA
