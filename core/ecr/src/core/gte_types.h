#pragma once
#include "common/bitfield.h"
#include "types.h"

namespace GTE {

inline constexpr uint32_t NUM_DATA_REGS = 32, NUM_CONTROL_REGS = 32, NUM_REGS = NUM_DATA_REGS + NUM_CONTROL_REGS;

union FLAGS
{
  uint32_t bits;

  BitField<uint32_t, bool, 31, 1> error;
  BitField<uint32_t, bool, 30, 1> mac1_overflow;
  BitField<uint32_t, bool, 29, 1> mac2_overflow;
  BitField<uint32_t, bool, 28, 1> mac3_overflow;
  BitField<uint32_t, bool, 27, 1> mac1_underflow;
  BitField<uint32_t, bool, 26, 1> mac2_underflow;
  BitField<uint32_t, bool, 25, 1> mac3_underflow;
  BitField<uint32_t, bool, 24, 1> ir1_saturated;
  BitField<uint32_t, bool, 23, 1> ir2_saturated;
  BitField<uint32_t, bool, 22, 1> ir3_saturated;
  BitField<uint32_t, bool, 21, 1> color_r_saturated;
  BitField<uint32_t, bool, 20, 1> color_g_saturated;
  BitField<uint32_t, bool, 19, 1> color_b_saturated;
  BitField<uint32_t, bool, 18, 1> sz1_otz_saturated;
  BitField<uint32_t, bool, 17, 1> divide_overflow;
  BitField<uint32_t, bool, 16, 1> mac0_overflow;
  BitField<uint32_t, bool, 15, 1> mac0_underflow;
  BitField<uint32_t, bool, 14, 1> sx2_saturated;
  BitField<uint32_t, bool, 13, 1> sy2_saturated;
  BitField<uint32_t, bool, 12, 1> ir0_saturated;

  static constexpr uint32_t WRITE_MASK = UINT32_C(0xFFFFF000);

  ALWAYS_INLINE void Clear() { bits = 0; }

  // Bits 30..23, 18..13 OR'ed
  ALWAYS_INLINE void UpdateError() { error = (bits & UINT32_C(0x7F87E000)) != UINT32_C(0); }
};

union Regs
{
  struct
  {
    uint32_t dr32[NUM_DATA_REGS];
    uint32_t cr32[NUM_CONTROL_REGS];
  };

  uint32_t r32[NUM_DATA_REGS + NUM_CONTROL_REGS];

#pragma pack(push, 1)
  struct
  {
    int16_t V0[3];     // 0-1
    uint16_t pad1;      // 1
    int16_t V1[3];     // 2-3
    uint16_t pad2;      // 3
    int16_t V2[3];     // 4-5
    uint16_t pad3;      // 5
    uint8_t RGBC[4];    // 6
    uint16_t OTZ;       // 7
    uint16_t pad4;      // 7
    int16_t IR0;       // 8
    uint16_t pad5;      // 8
    int16_t IR1;       // 9
    uint16_t pad6;      // 9
    int16_t IR2;       // 10
    uint16_t pad7;      // 10
    int16_t IR3;       // 11
    uint16_t pad8;      // 11
    int16_t SXY0[2];   // 12
    int16_t SXY1[2];   // 13
    int16_t SXY2[2];   // 14
    int16_t SXYP[2];   // 15
    uint16_t SZ0;       // 16
    uint16_t pad13;     // 16
    uint16_t SZ1;       // 17
    uint16_t pad14;     // 17
    uint16_t SZ2;       // 18
    uint16_t pad15;     // 18
    uint16_t SZ3;       // 19
    uint16_t pad16;     // 19
    uint8_t RGB0[4];    // 20
    uint8_t RGB1[4];    // 21
    uint8_t RGB2[4];    // 22
    uint32_t RES1;      // 23
    int32_t MAC0;      // 24
    int32_t MAC1;      // 25
    int32_t MAC2;      // 26
    int32_t MAC3;      // 27
    uint32_t IRGB;      // 28
    uint32_t ORGB;      // 29
    int32_t LZCS;      // 30
    uint32_t LZCR;      // 31
    int16_t RT[3][3];  // 32-36
    uint16_t pad17;     // 36
    int32_t TR[3];     // 37-39
    int16_t LLM[3][3]; // 40-44
    uint16_t pad18;     // 44
    int32_t BK[3];     // 45-47
    int16_t LCM[3][3]; // 48-52
    uint16_t pad19;     // 52
    int32_t FC[3];     // 53-55
    int32_t OFX;       // 56
    int32_t OFY;       // 57
    uint16_t H;         // 58
    uint16_t pad20;     // 58
    int16_t DQA;       // 59
    uint16_t pad21;     // 59
    int32_t DQB;       // 60
    int16_t ZSF3;      // 61
    uint16_t pad22;     // 61
    int16_t ZSF4;      // 62
    uint16_t pad23;     // 62
    FLAGS FLAG;    // 63
  };
#pragma pack(pop)
};

union Instruction
{
  uint32_t bits;

  BitField<uint32_t, uint8_t, 20, 5> fake_command;
  BitField<uint32_t, uint8_t, 19, 1> sf; // shift fraction in IR registers, 0=no fraction, 1=12bit fraction
  BitField<uint32_t, uint8_t, 17, 2> mvmva_multiply_matrix;
  BitField<uint32_t, uint8_t, 15, 2> mvmva_multiply_vector;
  BitField<uint32_t, uint8_t, 13, 2> mvmva_translation_vector;
  BitField<uint32_t, bool, 10, 1> lm; // saturate IR1, IR2, IR3 result
  BitField<uint32_t, uint8_t, 0, 6> command;

  ALWAYS_INLINE uint8_t GetShift() const { return sf ? 12 : 0; }

  // only the first 20 bits are needed to execute
  static constexpr uint32_t REQUIRED_BITS_MASK = ((1 << 20) - 1);
};

} // namespace GTE
