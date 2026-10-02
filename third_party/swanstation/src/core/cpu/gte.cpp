#include "gte.h"
#include "common/bitutils.h"
#include "common/state_wrapper.h"
#include "cpu_core.h"
#include "cpu_core_private.h"
#include "host_display.h"
#include "host_interface.h"
#include "pgxp.h"
#include "settings.h"
#include "timing_event.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <numeric>
namespace GTE {

static constexpr int64_t MAC0_MIN_VALUE = -(INT64_C(1) << 31);
static constexpr int64_t MAC0_MAX_VALUE = (INT64_C(1) << 31) - 1;
static constexpr int64_t MAC123_MIN_VALUE = -(INT64_C(1) << 43);
static constexpr int64_t MAC123_MAX_VALUE = (INT64_C(1) << 43) - 1;
static constexpr int32_t IR0_MIN_VALUE = 0x0000;
static constexpr int32_t IR0_MAX_VALUE = 0x1000;
static constexpr int32_t IR123_MIN_VALUE = -(INT64_C(1) << 15);
static constexpr int32_t IR123_MAX_VALUE = (INT64_C(1) << 15) - 1;

static DisplayAspectRatio s_aspect_ratio = DisplayAspectRatio::R4_3;
static uint32_t s_custom_aspect_ratio_numerator;
static uint32_t s_custom_aspect_ratio_denominator;
static float s_custom_aspect_ratio_f;

#define REGS CPU::g_state.gte_regs

ALWAYS_INLINE static uint32_t CountLeadingBits(uint32_t value)
{
  // if top-most bit is set, we want to count ones not zeros
  if (value & UINT32_C(0x80000000))
    value ^= UINT32_C(0xFFFFFFFF);

  return (value == 0u) ? 32 : CountLeadingZeros(value);
}

template<uint32_t index>
ALWAYS_INLINE static void CheckMACOverflow(int64_t value)
{
  constexpr int64_t MIN_VALUE = (index == 0) ? MAC0_MIN_VALUE : MAC123_MIN_VALUE;
  constexpr int64_t MAX_VALUE = (index == 0) ? MAC0_MAX_VALUE : MAC123_MAX_VALUE;
  if (value < MIN_VALUE)
  {
    if constexpr (index == 0)
      REGS.FLAG.mac0_underflow = true;
    else if constexpr (index == 1)
      REGS.FLAG.mac1_underflow = true;
    else if constexpr (index == 2)
      REGS.FLAG.mac2_underflow = true;
    else if constexpr (index == 3)
      REGS.FLAG.mac3_underflow = true;
  }
  else if (value > MAX_VALUE)
  {
    if constexpr (index == 0)
      REGS.FLAG.mac0_overflow = true;
    else if constexpr (index == 1)
      REGS.FLAG.mac1_overflow = true;
    else if constexpr (index == 2)
      REGS.FLAG.mac2_overflow = true;
    else if constexpr (index == 3)
      REGS.FLAG.mac3_overflow = true;
  }
}

template<uint32_t index>
ALWAYS_INLINE static int64_t SignExtendMACResult(int64_t value)
{
  CheckMACOverflow<index>(value);
  // Sign-extend low 31 bits (MAC0) or 44 bits (MAC1..3) to int64_t.
  // Shift in unsigned space to avoid UB on negative left shift
  // (UB in C++<20; well-defined in C++20+).
  constexpr int shift = index == 0 ? 33 : 20;
  return static_cast<int64_t>(static_cast<uint64_t>(value) << shift) >> shift;
}

template<uint32_t index>
ALWAYS_INLINE static void TruncateAndSetMAC(int64_t value, uint8_t shift)
{
  CheckMACOverflow<index>(value);

  // shift should be done before storing to avoid losing precision
  value >>= shift;

  REGS.dr32[24 + index] = static_cast<uint32_t>(static_cast<uint64_t>(value));
}

template<uint32_t index>
ALWAYS_INLINE static void TruncateAndSetIR(int32_t value, bool lm)
{
  constexpr int32_t MIN_VALUE = (index == 0) ? IR0_MIN_VALUE : IR123_MIN_VALUE;
  constexpr int32_t MAX_VALUE = (index == 0) ? IR0_MAX_VALUE : IR123_MAX_VALUE;
  const int32_t actual_min_value = lm ? 0 : MIN_VALUE;
  if (value < actual_min_value)
  {
    value = actual_min_value;
    if constexpr (index == 0)
      REGS.FLAG.ir0_saturated = true;
    else if constexpr (index == 1)
      REGS.FLAG.ir1_saturated = true;
    else if constexpr (index == 2)
      REGS.FLAG.ir2_saturated = true;
    else if constexpr (index == 3)
      REGS.FLAG.ir3_saturated = true;
  }
  else if (value > MAX_VALUE)
  {
    value = MAX_VALUE;
    if constexpr (index == 0)
      REGS.FLAG.ir0_saturated = true;
    else if constexpr (index == 1)
      REGS.FLAG.ir1_saturated = true;
    else if constexpr (index == 2)
      REGS.FLAG.ir2_saturated = true;
    else if constexpr (index == 3)
      REGS.FLAG.ir3_saturated = true;
  }

  // store sign-extended 16-bit value as 32-bit
  REGS.dr32[8 + index] = value;
}

template<uint32_t index>
ALWAYS_INLINE static void TruncateAndSetMACAndIR(int64_t value, uint8_t shift, bool lm)
{
  CheckMACOverflow<index>(value);

  // shift should be done before storing to avoid losing precision
  value >>= shift;

  // set MAC
  const int32_t value32 = static_cast<int32_t>(value);
  REGS.dr32[24 + index] = value32;

  // set IR
  TruncateAndSetIR<index>(value32, lm);
}

template<uint32_t index>
ALWAYS_INLINE static uint32_t TruncateRGB(int32_t value)
{
  if (value < 0 || value > 0xFF)
  {
    if constexpr (index == 0)
      REGS.FLAG.color_r_saturated = true;
    else if constexpr (index == 1)
      REGS.FLAG.color_g_saturated = true;
    else
      REGS.FLAG.color_b_saturated = true;

    return (value < 0) ? 0 : 0xFF;
  }

  return static_cast<uint32_t>(value);
}

void Initialize()
{
  UpdateAspectRatio();
  Reset();
}

void Reset()
{
  std::memset(&REGS, 0, sizeof(REGS));
}

bool DoState(StateWrapper& sw)
{
  sw.DoArray(REGS.r32, NUM_DATA_REGS + NUM_CONTROL_REGS);
  return !sw.HasError();
}

void UpdateAspectRatio()
{
  if (!g_settings.gpu_widescreen_hack)
  {
    s_aspect_ratio = DisplayAspectRatio::R4_3;
    return;
  }

  s_aspect_ratio = g_settings.display_aspect_ratio;

  uint32_t num, denom;
  switch (s_aspect_ratio)
  {
    case DisplayAspectRatio::MatchWindow:
    {
      const HostDisplay* display = g_host_interface->GetDisplay();
      if (!display)
      {
        s_aspect_ratio = DisplayAspectRatio::R4_3;
        return;
      }

      num = display->GetWindowWidth();
      denom = display->GetWindowHeight();
    }
    break;

    case DisplayAspectRatio::Custom:
    {
      num = g_settings.display_aspect_ratio_custom_numerator;
      denom = g_settings.display_aspect_ratio_custom_denominator;
    }
    break;

    default:
      return;
  }

  // (4 / 3) / (num / denom) => gcd((4 * denom) / (3 * num))
  const uint32_t x = 4u * denom;
  const uint32_t y = 3u * num;
  const uint32_t gcd = std::gcd(x, y);

  s_custom_aspect_ratio_numerator = x / gcd;
  s_custom_aspect_ratio_denominator = y / gcd;

  s_custom_aspect_ratio_f = static_cast<float>((4.0 / 3.0) / (static_cast<double>(num) / static_cast<double>(denom)));
}

uint32_t ReadRegister(uint32_t index)
{
  switch (index)
  {
    case 15: // SXY3
    {
      // mirror of SXY2
      return REGS.r32[14];
    }

    case 28: // IRGB
    case 29: // ORGB
    {
      // ORGB register, convert 16-bit to 555
      const uint8_t r = static_cast<uint8_t>(std::clamp(REGS.IR1 / 0x80, 0x00, 0x1F));
      const uint8_t g = static_cast<uint8_t>(std::clamp(REGS.IR2 / 0x80, 0x00, 0x1F));
      const uint8_t b = static_cast<uint8_t>(std::clamp(REGS.IR3 / 0x80, 0x00, 0x1F));
      return static_cast<uint32_t>(r) | (static_cast<uint32_t>(g) << 5) | (static_cast<uint32_t>(b) << 10);
    }

    default:
      return REGS.r32[index];
  }
}

void WriteRegister(uint32_t index, uint32_t value)
{
  switch (index)
  {
    case 1:  // V0[z]
    case 3:  // V1[z]
    case 5:  // V2[z]
    case 8:  // IR0
    case 9:  // IR1
    case 10: // IR2
    case 11: // IR3
    case 36: // RT33
    case 44: // L33
    case 52: // LR33
    case 58: // H       - sign-extended on read but zext on use
    case 59: // DQA
    case 61: // ZSF3
    case 62: // ZSF4
    {
      // sign-extend z component of vector registers
      REGS.r32[index] = static_cast<uint32_t>(static_cast<int16_t>(value));
    }
    break;

    case 7:  // OTZ
    case 16: // SZ0
    case 17: // SZ1
    case 18: // SZ2
    case 19: // SZ3
    {
      // zero-extend unsigned values
      REGS.r32[index] = static_cast<uint32_t>(static_cast<uint16_t>(value));
    }
    break;

    case 15: // SXY3
    {
      // writing to SXYP pushes to the FIFO
      REGS.r32[12] = REGS.r32[13]; // SXY0 <- SXY1
      REGS.r32[13] = REGS.r32[14]; // SXY1 <- SXY2
      REGS.r32[14] = value;        // SXY2 <- SXYP
    }
    break;

    case 28: // IRGB
    {
      // IRGB register, convert 555 to 16-bit
      REGS.IRGB = value & UINT32_C(0x7FFF);
      REGS.r32[9] = static_cast<uint32_t>(static_cast<int16_t>(static_cast<uint16_t>((value & UINT32_C(0x1F)) * UINT32_C(0x80))));
      REGS.r32[10] = static_cast<uint32_t>(static_cast<int16_t>(static_cast<uint16_t>(((value >> 5) & UINT32_C(0x1F)) * UINT32_C(0x80))));
      REGS.r32[11] = static_cast<uint32_t>(static_cast<int16_t>(static_cast<uint16_t>(((value >> 10) & UINT32_C(0x1F)) * UINT32_C(0x80))));
    }
    break;

    case 30: // LZCS
    {
      REGS.LZCS = static_cast<int32_t>(value);
      REGS.LZCR = CountLeadingBits(value);
    }
    break;

    case 29: // ORGB
    case 31: // LZCR
    {
      // read-only registers
    }
    break;

    case 63: // FLAG
    {
      REGS.FLAG.bits = value & UINT32_C(0x7FFFF000);
      REGS.FLAG.UpdateError();
    }
    break;

    default:
    {
      // written as-is, 2x16 or 1x32 bits
      REGS.r32[index] = value;
    }
    break;
  }
}

ALWAYS_INLINE static void SetOTZ(int32_t value)
{
  if (value < 0)
  {
    REGS.FLAG.sz1_otz_saturated = true;
    value = 0;
  }
  else if (value > 0xFFFF)
  {
    REGS.FLAG.sz1_otz_saturated = true;
    value = 0xFFFF;
  }

  REGS.dr32[7] = static_cast<uint32_t>(value);
}

ALWAYS_INLINE static void PushSXY(int32_t x, int32_t y)
{
  if (x < -1024)
  {
    REGS.FLAG.sx2_saturated = true;
    x = -1024;
  }
  else if (x > 1023)
  {
    REGS.FLAG.sx2_saturated = true;
    x = 1023;
  }

  if (y < -1024)
  {
    REGS.FLAG.sy2_saturated = true;
    y = -1024;
  }
  else if (y > 1023)
  {
    REGS.FLAG.sy2_saturated = true;
    y = 1023;
  }

  REGS.dr32[12] = REGS.dr32[13]; // SXY0 <- SXY1
  REGS.dr32[13] = REGS.dr32[14]; // SXY1 <- SXY2
  REGS.dr32[14] = (static_cast<uint32_t>(x) & 0xFFFFu) | (static_cast<uint32_t>(y) << 16);
}

ALWAYS_INLINE static void PushSZ(int32_t value)
{
  if (value < 0)
  {
    REGS.FLAG.sz1_otz_saturated = true;
    value = 0;
  }
  else if (value > 0xFFFF)
  {
    REGS.FLAG.sz1_otz_saturated = true;
    value = 0xFFFF;
  }

  REGS.dr32[16] = REGS.dr32[17];           // SZ0 <- SZ1
  REGS.dr32[17] = REGS.dr32[18];           // SZ1 <- SZ2
  REGS.dr32[18] = REGS.dr32[19];           // SZ2 <- SZ3
  REGS.dr32[19] = static_cast<uint32_t>(value); // SZ3 <- value
}

static void PushRGBFromMAC()
{
  // Note: SHR 4 used instead of /16 as the results are different.
  const uint32_t r = TruncateRGB<0>(static_cast<uint32_t>(REGS.MAC1 >> 4));
  const uint32_t g = TruncateRGB<1>(static_cast<uint32_t>(REGS.MAC2 >> 4));
  const uint32_t b = TruncateRGB<2>(static_cast<uint32_t>(REGS.MAC3 >> 4));
  const uint32_t c = static_cast<uint32_t>(REGS.RGBC[3]);

  REGS.dr32[20] = REGS.dr32[21];                        // RGB0 <- RGB1
  REGS.dr32[21] = REGS.dr32[22];                        // RGB1 <- RGB2
  REGS.dr32[22] = r | (g << 8) | (b << 16) | (c << 24); // RGB2 <- Value
}

ALWAYS_INLINE static uint32_t UNRDivide(uint32_t lhs, uint32_t rhs)
{
  if (rhs * 2 <= lhs)
  {
    REGS.FLAG.divide_overflow = true;
    return 0x1FFFF;
  }

  const uint32_t shift = (rhs == 0) ? 16 : CountLeadingZeros(static_cast<uint16_t>(rhs));
  lhs <<= shift;
  rhs <<= shift;

  // constexpr eliminates the C++11 thread-safe-static-init guard.
  // UNRDivide is called from RTPS / RTPT / DPCS / DPCT / DCPL / GPF /
  // GPL / SQR - effectively the entire 3D transform path - so this
  // is one of the hotter functions in the GTE.
  static constexpr uint8_t unr_table[257] = {
    0xFF, 0xFD, 0xFB, 0xF9, 0xF7, 0xF5, 0xF3, 0xF1, 0xEF, 0xEE, 0xEC, 0xEA, 0xE8, 0xE6, 0xE4, 0xE3, //
    0xE1, 0xDF, 0xDD, 0xDC, 0xDA, 0xD8, 0xD6, 0xD5, 0xD3, 0xD1, 0xD0, 0xCE, 0xCD, 0xCB, 0xC9, 0xC8, //  00h..3Fh
    0xC6, 0xC5, 0xC3, 0xC1, 0xC0, 0xBE, 0xBD, 0xBB, 0xBA, 0xB8, 0xB7, 0xB5, 0xB4, 0xB2, 0xB1, 0xB0, //
    0xAE, 0xAD, 0xAB, 0xAA, 0xA9, 0xA7, 0xA6, 0xA4, 0xA3, 0xA2, 0xA0, 0x9F, 0x9E, 0x9C, 0x9B, 0x9A, //
    0x99, 0x97, 0x96, 0x95, 0x94, 0x92, 0x91, 0x90, 0x8F, 0x8D, 0x8C, 0x8B, 0x8A, 0x89, 0x87, 0x86, //
    0x85, 0x84, 0x83, 0x82, 0x81, 0x7F, 0x7E, 0x7D, 0x7C, 0x7B, 0x7A, 0x79, 0x78, 0x77, 0x75, 0x74, //  40h..7Fh
    0x73, 0x72, 0x71, 0x70, 0x6F, 0x6E, 0x6D, 0x6C, 0x6B, 0x6A, 0x69, 0x68, 0x67, 0x66, 0x65, 0x64, //
    0x63, 0x62, 0x61, 0x60, 0x5F, 0x5E, 0x5D, 0x5D, 0x5C, 0x5B, 0x5A, 0x59, 0x58, 0x57, 0x56, 0x55, //
    0x54, 0x53, 0x53, 0x52, 0x51, 0x50, 0x4F, 0x4E, 0x4D, 0x4D, 0x4C, 0x4B, 0x4A, 0x49, 0x48, 0x48, //
    0x47, 0x46, 0x45, 0x44, 0x43, 0x43, 0x42, 0x41, 0x40, 0x3F, 0x3F, 0x3E, 0x3D, 0x3C, 0x3C, 0x3B, //  80h..BFh
    0x3A, 0x39, 0x39, 0x38, 0x37, 0x36, 0x36, 0x35, 0x34, 0x33, 0x33, 0x32, 0x31, 0x31, 0x30, 0x2F, //
    0x2E, 0x2E, 0x2D, 0x2C, 0x2C, 0x2B, 0x2A, 0x2A, 0x29, 0x28, 0x28, 0x27, 0x26, 0x26, 0x25, 0x24, //
    0x24, 0x23, 0x22, 0x22, 0x21, 0x20, 0x20, 0x1F, 0x1E, 0x1E, 0x1D, 0x1D, 0x1C, 0x1B, 0x1B, 0x1A, //
    0x19, 0x19, 0x18, 0x18, 0x17, 0x16, 0x16, 0x15, 0x15, 0x14, 0x14, 0x13, 0x12, 0x12, 0x11, 0x11, //  C0h..FFh
    0x10, 0x0F, 0x0F, 0x0E, 0x0E, 0x0D, 0x0D, 0x0C, 0x0C, 0x0B, 0x0A, 0x0A, 0x09, 0x09, 0x08, 0x08, //
    0x07, 0x07, 0x06, 0x06, 0x05, 0x05, 0x04, 0x04, 0x03, 0x03, 0x02, 0x02, 0x01, 0x01, 0x00, 0x00, //
    0x00 // <-- one extra table entry (for "(d-7FC0h)/80h"=100h)
  };

  const uint32_t divisor = rhs | 0x8000;
  const int32_t x       = static_cast<int32_t>(0x101 + static_cast<uint32_t>(unr_table[((divisor & 0x7FFF) + 0x40) >> 7]));
  const int32_t d       = ((static_cast<int32_t>(divisor) * -x) + 0x80) >> 8;
  const uint32_t recip   = static_cast<uint32_t>(((x * (0x20000 + d)) + 0x80) >> 8);

  const uint32_t result  = static_cast<uint32_t>((static_cast<uint64_t>(lhs) * static_cast<uint64_t>(recip) + uint64_t(0x8000)) >> 16);

  // The min(1FFFFh) limit is needed for cases like FE3Fh/7F20h, F015h/780Bh, etc. (these do produce UNR result 20000h,
  // and are saturated to 1FFFFh, but without setting overflow FLAG bits).
  return std::min<uint32_t>(0x1FFFF, result);
}

static void MulMatVec(const int16_t M[3][3], const int16_t Vx, const int16_t Vy, const int16_t Vz, uint8_t shift, bool lm)
{
#define dot3(i)                                                                                                        \
  TruncateAndSetMACAndIR<i + 1>(SignExtendMACResult<i + 1>((int64_t(M[i][0]) * int64_t(Vx)) + (int64_t(M[i][1]) * int64_t(Vy))) +      \
                                  (int64_t(M[i][2]) * int64_t(Vz)),                                                            \
                                shift, lm)

  dot3(0);
  dot3(1);
  dot3(2);

#undef dot3
}

static void MulMatVec(const int16_t M[3][3], const int32_t T[3], const int16_t Vx, const int16_t Vy, const int16_t Vz, uint8_t shift, bool lm)
{
#define dot3(i)                                                                                                        \
  TruncateAndSetMACAndIR<i + 1>(                                                                                       \
    SignExtendMACResult<i + 1>(SignExtendMACResult<i + 1>((int64_t(T[i]) << 12) + (int64_t(M[i][0]) * int64_t(Vx))) +              \
                               (int64_t(M[i][1]) * int64_t(Vy))) +                                                             \
      (int64_t(M[i][2]) * int64_t(Vz)),                                                                                        \
    shift, lm)

  dot3(0);
  dot3(1);
  dot3(2);

#undef dot3
}

static void MulMatVecBuggy(const int16_t M[3][3], const int32_t T[3], const int16_t Vx, const int16_t Vy, const int16_t Vz, uint8_t shift,
                           bool lm)
{
#define dot3(i)                                                                                                        \
  do                                                                                                                   \
  {                                                                                                                    \
    TruncateAndSetIR<i + 1>(static_cast<int32_t>(SignExtendMACResult<i + 1>(SignExtendMACResult<i + 1>(                    \
                                               (int64_t(T[i]) << 12) + (int64_t(M[i][0]) * int64_t(Vx)))) >>                       \
                                             shift),                                                                   \
                            false);                                                                                    \
    TruncateAndSetMACAndIR<i + 1>(SignExtendMACResult<i + 1>((int64_t(M[i][1]) * int64_t(Vy))) + (int64_t(M[i][2]) * int64_t(Vz)),     \
                                  shift, lm);                                                                          \
  } while (0)

  dot3(0);
  dot3(1);
  dot3(2);

#undef dot3
}

static void Execute_MVMVA(Instruction inst)
{
  REGS.FLAG.Clear();

  // TODO: Remove memcpy..
  int16_t M[3][3];
  switch (inst.mvmva_multiply_matrix)
  {
    case 0:
      std::memcpy(M, REGS.RT, sizeof(int16_t) * 3 * 3);
      break;
    case 1:
      std::memcpy(M, REGS.LLM, sizeof(int16_t) * 3 * 3);
      break;
    case 2:
      std::memcpy(M, REGS.LCM, sizeof(int16_t) * 3 * 3);
      break;
    default:
    {
      // buggy
      M[0][0] = -static_cast<int16_t>(static_cast<uint16_t>(REGS.RGBC[0]) << 4);
      M[0][1] = static_cast<int16_t>(static_cast<uint16_t>(REGS.RGBC[0]) << 4);
      M[0][2] = REGS.IR0;
      M[1][0] = REGS.RT[0][2];
      M[1][1] = REGS.RT[0][2];
      M[1][2] = REGS.RT[0][2];
      M[2][0] = REGS.RT[1][1];
      M[2][1] = REGS.RT[1][1];
      M[2][2] = REGS.RT[1][1];
    }
    break;
  }

  int16_t Vx, Vy, Vz;
  switch (inst.mvmva_multiply_vector)
  {
    case 0:
      Vx = REGS.V0[0];
      Vy = REGS.V0[1];
      Vz = REGS.V0[2];
      break;
    case 1:
      Vx = REGS.V1[0];
      Vy = REGS.V1[1];
      Vz = REGS.V1[2];
      break;
    case 2:
      Vx = REGS.V2[0];
      Vy = REGS.V2[1];
      Vz = REGS.V2[2];
      break;
    default:
      Vx = REGS.IR1;
      Vy = REGS.IR2;
      Vz = REGS.IR3;
      break;
  }

  static const int32_t zero_T[3] = {};
  switch (inst.mvmva_translation_vector)
  {
    case 0:
      MulMatVec(M, REGS.TR, Vx, Vy, Vz, inst.GetShift(), inst.lm);
      break;
    case 1:
      MulMatVec(M, REGS.BK, Vx, Vy, Vz, inst.GetShift(), inst.lm);
      break;
    case 2:
      MulMatVecBuggy(M, REGS.FC, Vx, Vy, Vz, inst.GetShift(), inst.lm);
      break;
    default:
      MulMatVec(M, zero_T, Vx, Vy, Vz, inst.GetShift(), inst.lm);
      break;
  }

  REGS.FLAG.UpdateError();
}

static void Execute_SQR(Instruction inst)
{
  REGS.FLAG.Clear();

  // 32-bit multiply for speed - 16x16 isn't >32bit, and we know it won't overflow/underflow.
  const uint8_t shift = inst.GetShift();
  REGS.MAC1 = (int32_t(REGS.IR1) * int32_t(REGS.IR1)) >> shift;
  REGS.MAC2 = (int32_t(REGS.IR2) * int32_t(REGS.IR2)) >> shift;
  REGS.MAC3 = (int32_t(REGS.IR3) * int32_t(REGS.IR3)) >> shift;

  const bool lm = inst.lm;
  TruncateAndSetIR<1>(REGS.MAC1, lm);
  TruncateAndSetIR<2>(REGS.MAC2, lm);
  TruncateAndSetIR<3>(REGS.MAC3, lm);

  REGS.FLAG.UpdateError();
}

static void Execute_OP(Instruction inst)
{
  REGS.FLAG.Clear();

  // Take copies since we overwrite them in each step.
  const uint8_t shift = inst.GetShift();
  const bool lm = inst.lm;
  const int32_t D1 = int32_t(REGS.RT[0][0]);
  const int32_t D2 = int32_t(REGS.RT[1][1]);
  const int32_t D3 = int32_t(REGS.RT[2][2]);
  const int32_t IR1 = int32_t(REGS.IR1);
  const int32_t IR2 = int32_t(REGS.IR2);
  const int32_t IR3 = int32_t(REGS.IR3);

  // [MAC1,MAC2,MAC3] = [IR3*D2-IR2*D3, IR1*D3-IR3*D1, IR2*D1-IR1*D2] SAR (sf*12)
  // [IR1, IR2, IR3] = [MAC1, MAC2, MAC3]; copy result
  TruncateAndSetMACAndIR<1>(int64_t(IR3 * D2) - int64_t(IR2 * D3), shift, lm);
  TruncateAndSetMACAndIR<2>(int64_t(IR1 * D3) - int64_t(IR3 * D1), shift, lm);
  TruncateAndSetMACAndIR<3>(int64_t(IR2 * D1) - int64_t(IR1 * D2), shift, lm);

  REGS.FLAG.UpdateError();
}

static void RTPS(const int16_t V[3], uint8_t shift, bool lm, bool last)
{
#define dot3(i)                                                                                                        \
  SignExtendMACResult<i + 1>(SignExtendMACResult<i + 1>((int64_t(REGS.TR[i]) << 12) + (int64_t(REGS.RT[i][0]) * int64_t(V[0]))) +  \
                             (int64_t(REGS.RT[i][1]) * int64_t(V[1]))) +                                                       \
    (int64_t(REGS.RT[i][2]) * int64_t(V[2]))

  // IR1 = MAC1 = (TRX*1000h + RT11*VX0 + RT12*VY0 + RT13*VZ0) SAR (sf*12)
  // IR2 = MAC2 = (TRY*1000h + RT21*VX0 + RT22*VY0 + RT23*VZ0) SAR (sf*12)
  // IR3 = MAC3 = (TRZ*1000h + RT31*VX0 + RT32*VY0 + RT33*VZ0) SAR (sf*12)
  const int64_t x = dot3(0);
  const int64_t y = dot3(1);
  const int64_t z = dot3(2);
  TruncateAndSetMAC<1>(x, shift);
  TruncateAndSetMAC<2>(y, shift);
  TruncateAndSetMAC<3>(z, shift);
  TruncateAndSetIR<1>(REGS.MAC1, lm);
  TruncateAndSetIR<2>(REGS.MAC2, lm);

  // The command does saturate IR1,IR2,IR3 to -8000h..+7FFFh (regardless of lm bit). When using RTP with sf=0, then the
  // IR3 saturation flag (FLAG.22) gets set <only> if "MAC3 SAR 12" exceeds -8000h..+7FFFh (although IR3 is saturated
  // when "MAC3" exceeds -8000h..+7FFFh).
  TruncateAndSetIR<3>(int32_t(z >> 12), false);
  REGS.dr32[11] = std::clamp(REGS.MAC3, lm ? 0 : IR123_MIN_VALUE, IR123_MAX_VALUE);
#undef dot3

  // SZ3 = MAC3 SAR ((1-sf)*12)                           ;ScreenZ FIFO 0..+FFFFh
  PushSZ(int32_t(z >> 12));

  // MAC0=(((H*20000h/SZ3)+1)/2)*IR1+OFX, SX2=MAC0/10000h ;ScrX FIFO -400h..+3FFh
  // MAC0=(((H*20000h/SZ3)+1)/2)*IR2+OFY, SY2=MAC0/10000h ;ScrY FIFO -400h..+3FFh
  const int64_t result = static_cast<int64_t>(UNRDivide(REGS.H, REGS.SZ3));

  int64_t Sx;
  switch (s_aspect_ratio)
  {
    case DisplayAspectRatio::R16_9:
      Sx = ((((int64_t(result) * int64_t(REGS.IR1)) * int64_t(3)) / int64_t(4)) + int64_t(REGS.OFX));
      break;

    case DisplayAspectRatio::R19_9:
      Sx = ((((int64_t(result) * int64_t(REGS.IR1)) * int64_t(12)) / int64_t(19)) + int64_t(REGS.OFX));
      break;

    case DisplayAspectRatio::R20_9:
      Sx = ((((int64_t(result) * int64_t(REGS.IR1)) * int64_t(3)) / int64_t(5)) + int64_t(REGS.OFX));
      break;

    case DisplayAspectRatio::Custom:
    case DisplayAspectRatio::MatchWindow:
      Sx = ((((int64_t(result) * int64_t(REGS.IR1)) * int64_t(s_custom_aspect_ratio_numerator)) /
             int64_t(s_custom_aspect_ratio_denominator)) +
            int64_t(REGS.OFX));
      break;

    case DisplayAspectRatio::Auto:
    case DisplayAspectRatio::R4_3:
    case DisplayAspectRatio::PAR1_1:
    default:
      Sx = (int64_t(result) * int64_t(REGS.IR1) + int64_t(REGS.OFX));
      break;
  }

  const int64_t Sy = int64_t(result) * int64_t(REGS.IR2) + int64_t(REGS.OFY);
  CheckMACOverflow<0>(Sx);
  CheckMACOverflow<0>(Sy);
  PushSXY(int32_t(Sx >> 16), int32_t(Sy >> 16));

  if (g_settings.gpu_pgxp_enable)
  {
    float precise_sz3, precise_ir1, precise_ir2;

    if (g_settings.gpu_pgxp_preserve_proj_fp)
    {
      precise_sz3 = float(z) / 4096.0f;
      precise_ir1 = float(x) / (static_cast<float>(1 << shift));
      precise_ir2 = float(y) / (static_cast<float>(1 << shift));
      if (lm)
      {
        precise_ir1 = std::clamp(precise_ir1, float(IR123_MIN_VALUE), float(IR123_MAX_VALUE));
        precise_ir2 = std::clamp(precise_ir2, float(IR123_MIN_VALUE), float(IR123_MAX_VALUE));
      }
      else
      {
        precise_ir1 = std::min(precise_ir1, float(IR123_MAX_VALUE));
        precise_ir2 = std::min(precise_ir2, float(IR123_MAX_VALUE));
      }
    }
    else
    {
      precise_sz3 = float(REGS.SZ3);
      precise_ir1 = float(REGS.IR1);
      precise_ir2 = float(REGS.IR2);
    }

    // this can potentially use increased precision on Z
    const float precise_z = std::max<float>(float(REGS.H) / 2.0f, precise_sz3);
    const float precise_h_div_sz = float(REGS.H) / precise_z;
    const float fofx = float(REGS.OFX) / float(1 << 16);
    const float fofy = float(REGS.OFY) / float(1 << 16);
    float precise_x = precise_ir1 * precise_h_div_sz;

    switch (s_aspect_ratio)
    {
      case DisplayAspectRatio::MatchWindow:
      case DisplayAspectRatio::Custom:
        precise_x = precise_x * s_custom_aspect_ratio_f;
        break;

      case DisplayAspectRatio::R16_9:
        precise_x = (precise_x * 3.0f) / 4.0f;
        break;

      case DisplayAspectRatio::R19_9:
        precise_x = (precise_x * 12.0f) / 19.0f;
        break;

      case DisplayAspectRatio::R20_9:
        precise_x = (precise_x * 3.0f) / 5.0f;
        break;

      case DisplayAspectRatio::Auto:
      case DisplayAspectRatio::R4_3:
      case DisplayAspectRatio::PAR1_1:
      default:
        break;
    }

    precise_x += fofx;

    float precise_y = fofy + (precise_ir2 * precise_h_div_sz);

    precise_x = std::clamp<float>(precise_x, -1024.0f, 1023.0f);
    precise_y = std::clamp<float>(precise_y, -1024.0f, 1023.0f);
    PGXP::GTE_PushSXYZ2f(precise_x, precise_y, precise_z, REGS.dr32[14]);
  }

  if (last)
  {
    // MAC0=(((H*20000h/SZ3)+1)/2)*DQA+DQB, IR0=MAC0/1000h  ;Depth cueing 0..+1000h
    const int64_t Sz = int64_t(result) * int64_t(REGS.DQA) + int64_t(REGS.DQB);
    TruncateAndSetMAC<0>(Sz, 0);
    TruncateAndSetIR<0>(int32_t(Sz >> 12), true);
  }
}

static void Execute_RTPS(Instruction inst)
{
  REGS.FLAG.Clear();
  RTPS(REGS.V0, inst.GetShift(), inst.lm, true);
  REGS.FLAG.UpdateError();
}

static void Execute_RTPT(Instruction inst)
{
  REGS.FLAG.Clear();

  const uint8_t shift = inst.GetShift();
  const bool lm = inst.lm;

  RTPS(REGS.V0, shift, lm, false);
  RTPS(REGS.V1, shift, lm, false);
  RTPS(REGS.V2, shift, lm, true);

  REGS.FLAG.UpdateError();
}

static void Execute_NCLIP(Instruction inst)
{
  // MAC0 =   SX0*SY1 + SX1*SY2 + SX2*SY0 - SX0*SY2 - SX1*SY0 - SX2*SY1
  REGS.FLAG.Clear();

  TruncateAndSetMAC<0>(int64_t(REGS.SXY0[0]) * int64_t(REGS.SXY1[1]) + int64_t(REGS.SXY1[0]) * int64_t(REGS.SXY2[1]) +
                         int64_t(REGS.SXY2[0]) * int64_t(REGS.SXY0[1]) - int64_t(REGS.SXY0[0]) * int64_t(REGS.SXY2[1]) -
                         int64_t(REGS.SXY1[0]) * int64_t(REGS.SXY0[1]) - int64_t(REGS.SXY2[0]) * int64_t(REGS.SXY1[1]),
                       0);

  REGS.FLAG.UpdateError();
}

static void Execute_NCLIP_PGXP(Instruction inst)
{
  if (PGXP::GTE_NCLIP_valid(REGS.dr32[12], REGS.dr32[13], REGS.dr32[14]))
  {
    REGS.FLAG.Clear();
    REGS.MAC0 = static_cast<int32_t>(PGXP::GTE_NCLIP());
  }
  else
  {
    Execute_NCLIP(inst);
  }
}

static void Execute_AVSZ3(Instruction inst)
{
  REGS.FLAG.Clear();

  const int64_t result = int64_t(REGS.ZSF3) * int32_t(uint32_t(REGS.SZ1) + uint32_t(REGS.SZ2) + uint32_t(REGS.SZ3));
  TruncateAndSetMAC<0>(result, 0);
  SetOTZ(int32_t(result >> 12));

  REGS.FLAG.UpdateError();
}

static void Execute_AVSZ4(Instruction inst)
{
  REGS.FLAG.Clear();

  const int64_t result = int64_t(REGS.ZSF4) * int32_t(uint32_t(REGS.SZ0) + uint32_t(REGS.SZ1) + uint32_t(REGS.SZ2) + uint32_t(REGS.SZ3));
  TruncateAndSetMAC<0>(result, 0);
  SetOTZ(int32_t(result >> 12));

  REGS.FLAG.UpdateError();
}

static ALWAYS_INLINE void InterpolateColor(int64_t in_MAC1, int64_t in_MAC2, int64_t in_MAC3, uint8_t shift, bool lm)
{
  // [MAC1,MAC2,MAC3] = MAC+(FC-MAC)*IR0
  //   [IR1,IR2,IR3] = (([RFC,GFC,BFC] SHL 12) - [MAC1,MAC2,MAC3]) SAR (sf*12)
  TruncateAndSetMACAndIR<1>((int64_t(REGS.FC[0]) << 12) - in_MAC1, shift, false);
  TruncateAndSetMACAndIR<2>((int64_t(REGS.FC[1]) << 12) - in_MAC2, shift, false);
  TruncateAndSetMACAndIR<3>((int64_t(REGS.FC[2]) << 12) - in_MAC3, shift, false);

  //   [MAC1,MAC2,MAC3] = (([IR1,IR2,IR3] * IR0) + [MAC1,MAC2,MAC3])
  // [MAC1,MAC2,MAC3] = [MAC1,MAC2,MAC3] SAR (sf*12)
  TruncateAndSetMACAndIR<1>(int64_t(int32_t(REGS.IR1) * int32_t(REGS.IR0)) + in_MAC1, shift, lm);
  TruncateAndSetMACAndIR<2>(int64_t(int32_t(REGS.IR2) * int32_t(REGS.IR0)) + in_MAC2, shift, lm);
  TruncateAndSetMACAndIR<3>(int64_t(int32_t(REGS.IR3) * int32_t(REGS.IR0)) + in_MAC3, shift, lm);
}

static void NCS(const int16_t V[3], uint8_t shift, bool lm)
{
  // [IR1,IR2,IR3] = [MAC1,MAC2,MAC3] = (LLM*V0) SAR (sf*12)
  MulMatVec(REGS.LLM, V[0], V[1], V[2], shift, lm);

  // [IR1,IR2,IR3] = [MAC1,MAC2,MAC3] = (BK*1000h + LCM*IR) SAR (sf*12)
  MulMatVec(REGS.LCM, REGS.BK, REGS.IR1, REGS.IR2, REGS.IR3, shift, lm);

  // Color FIFO = [MAC1/16,MAC2/16,MAC3/16,CODE], [IR1,IR2,IR3] = [MAC1,MAC2,MAC3]
  PushRGBFromMAC();
}

static void Execute_NCS(Instruction inst)
{
  REGS.FLAG.Clear();

  NCS(REGS.V0, inst.GetShift(), inst.lm);

  REGS.FLAG.UpdateError();
}

static void Execute_NCT(Instruction inst)
{
  REGS.FLAG.Clear();

  const uint8_t shift = inst.GetShift();
  const bool lm = inst.lm;

  NCS(REGS.V0, shift, lm);
  NCS(REGS.V1, shift, lm);
  NCS(REGS.V2, shift, lm);

  REGS.FLAG.UpdateError();
}

static void NCCS(const int16_t V[3], uint8_t shift, bool lm)
{
  // [IR1,IR2,IR3] = [MAC1,MAC2,MAC3] = (LLM*V0) SAR (sf*12)
  MulMatVec(REGS.LLM, V[0], V[1], V[2], shift, lm);

  // [IR1,IR2,IR3] = [MAC1,MAC2,MAC3] = (BK*1000h + LCM*IR) SAR (sf*12)
  MulMatVec(REGS.LCM, REGS.BK, REGS.IR1, REGS.IR2, REGS.IR3, shift, lm);

  // [MAC1,MAC2,MAC3] = [R*IR1,G*IR2,B*IR3] SHL 4          ;<--- for NCDx/NCCx
  // [MAC1,MAC2,MAC3] = [MAC1,MAC2,MAC3] SAR (sf*12)       ;<--- for NCDx/NCCx
  TruncateAndSetMACAndIR<1>(int64_t(int32_t(static_cast<uint32_t>(REGS.RGBC[0])) * int32_t(REGS.IR1)) << 4, shift, lm);
  TruncateAndSetMACAndIR<2>(int64_t(int32_t(static_cast<uint32_t>(REGS.RGBC[1])) * int32_t(REGS.IR2)) << 4, shift, lm);
  TruncateAndSetMACAndIR<3>(int64_t(int32_t(static_cast<uint32_t>(REGS.RGBC[2])) * int32_t(REGS.IR3)) << 4, shift, lm);

  // Color FIFO = [MAC1/16,MAC2/16,MAC3/16,CODE], [IR1,IR2,IR3] = [MAC1,MAC2,MAC3]
  PushRGBFromMAC();
}

static void Execute_NCCS(Instruction inst)
{
  REGS.FLAG.Clear();

  NCCS(REGS.V0, inst.GetShift(), inst.lm);

  REGS.FLAG.UpdateError();
}

static void Execute_NCCT(Instruction inst)
{
  REGS.FLAG.Clear();

  const uint8_t shift = inst.GetShift();
  const bool lm = inst.lm;

  NCCS(REGS.V0, shift, lm);
  NCCS(REGS.V1, shift, lm);
  NCCS(REGS.V2, shift, lm);

  REGS.FLAG.UpdateError();
}

static void NCDS(const int16_t V[3], uint8_t shift, bool lm)
{
  // [IR1,IR2,IR3] = [MAC1,MAC2,MAC3] = (LLM*V0) SAR (sf*12)
  MulMatVec(REGS.LLM, V[0], V[1], V[2], shift, lm);

  // [IR1,IR2,IR3] = [MAC1,MAC2,MAC3] = (BK*1000h + LCM*IR) SAR (sf*12)
  MulMatVec(REGS.LCM, REGS.BK, REGS.IR1, REGS.IR2, REGS.IR3, shift, lm);

  // No need to assign these to MAC[1-3], as it'll never overflow.
  // [MAC1,MAC2,MAC3] = [R*IR1,G*IR2,B*IR3] SHL 4          ;<--- for NCDx/NCCx
  const int32_t in_MAC1 = (int32_t(static_cast<uint32_t>(REGS.RGBC[0])) * int32_t(REGS.IR1)) << 4;
  const int32_t in_MAC2 = (int32_t(static_cast<uint32_t>(REGS.RGBC[1])) * int32_t(REGS.IR2)) << 4;
  const int32_t in_MAC3 = (int32_t(static_cast<uint32_t>(REGS.RGBC[2])) * int32_t(REGS.IR3)) << 4;

  // [MAC1,MAC2,MAC3] = MAC+(FC-MAC)*IR0                   ;<--- for NCDx only
  InterpolateColor(in_MAC1, in_MAC2, in_MAC3, shift, lm);

  // Color FIFO = [MAC1/16,MAC2/16,MAC3/16,CODE], [IR1,IR2,IR3] = [MAC1,MAC2,MAC3]
  PushRGBFromMAC();
}

static void Execute_NCDS(Instruction inst)
{
  REGS.FLAG.Clear();

  NCDS(REGS.V0, inst.GetShift(), inst.lm);

  REGS.FLAG.UpdateError();
}

static void Execute_NCDT(Instruction inst)
{
  REGS.FLAG.Clear();

  const uint8_t shift = inst.GetShift();
  const bool lm = inst.lm;

  NCDS(REGS.V0, shift, lm);
  NCDS(REGS.V1, shift, lm);
  NCDS(REGS.V2, shift, lm);

  REGS.FLAG.UpdateError();
}

static void Execute_CC(Instruction inst)
{
  REGS.FLAG.Clear();

  const uint8_t shift = inst.GetShift();
  const bool lm = inst.lm;

  // [IR1,IR2,IR3] = [MAC1,MAC2,MAC3] = (BK*1000h + LCM*IR) SAR (sf*12)
  MulMatVec(REGS.LCM, REGS.BK, REGS.IR1, REGS.IR2, REGS.IR3, shift, lm);

  // [MAC1,MAC2,MAC3] = [R*IR1,G*IR2,B*IR3] SHL 4
  // [MAC1,MAC2,MAC3] = [MAC1,MAC2,MAC3] SAR (sf*12)
  TruncateAndSetMACAndIR<1>(int64_t(int32_t(static_cast<uint32_t>(REGS.RGBC[0])) * int32_t(REGS.IR1)) << 4, shift, lm);
  TruncateAndSetMACAndIR<2>(int64_t(int32_t(static_cast<uint32_t>(REGS.RGBC[1])) * int32_t(REGS.IR2)) << 4, shift, lm);
  TruncateAndSetMACAndIR<3>(int64_t(int32_t(static_cast<uint32_t>(REGS.RGBC[2])) * int32_t(REGS.IR3)) << 4, shift, lm);

  // Color FIFO = [MAC1/16,MAC2/16,MAC3/16,CODE], [IR1,IR2,IR3] = [MAC1,MAC2,MAC3]
  PushRGBFromMAC();

  REGS.FLAG.UpdateError();
}

static void Execute_CDP(Instruction inst)
{
  REGS.FLAG.Clear();

  const uint8_t shift = inst.GetShift();
  const bool lm = inst.lm;

  // [IR1,IR2,IR3] = [MAC1,MAC2,MAC3] = (BK*1000h + LCM*IR) SAR (sf*12)
  MulMatVec(REGS.LCM, REGS.BK, REGS.IR1, REGS.IR2, REGS.IR3, shift, lm);

  // No need to assign these to MAC[1-3], as it'll never overflow.
  // [MAC1,MAC2,MAC3] = [R*IR1,G*IR2,B*IR3] SHL 4
  const int32_t in_MAC1 = (int32_t(static_cast<uint32_t>(REGS.RGBC[0])) * int32_t(REGS.IR1)) << 4;
  const int32_t in_MAC2 = (int32_t(static_cast<uint32_t>(REGS.RGBC[1])) * int32_t(REGS.IR2)) << 4;
  const int32_t in_MAC3 = (int32_t(static_cast<uint32_t>(REGS.RGBC[2])) * int32_t(REGS.IR3)) << 4;

  // [MAC1,MAC2,MAC3] = MAC+(FC-MAC)*IR0                   ;<--- for CDP only
  // [MAC1, MAC2, MAC3] = [MAC1, MAC2, MAC3] SAR(sf * 12)
  InterpolateColor(in_MAC1, in_MAC2, in_MAC3, shift, lm);

  // Color FIFO = [MAC1/16,MAC2/16,MAC3/16,CODE], [IR1,IR2,IR3] = [MAC1,MAC2,MAC3]
  PushRGBFromMAC();

  REGS.FLAG.UpdateError();
}

static void DPCS(const uint8_t color[3], uint8_t shift, bool lm)
{
  // In: [IR1,IR2,IR3]=Vector, FC=Far Color, IR0=Interpolation value, CODE=MSB of RGBC
  // [MAC1,MAC2,MAC3] = [R,G,B] SHL 16                     ;<--- for DPCS/DPCT
  TruncateAndSetMAC<1>((int64_t(static_cast<uint64_t>(color[0])) << 16), 0);
  TruncateAndSetMAC<2>((int64_t(static_cast<uint64_t>(color[1])) << 16), 0);
  TruncateAndSetMAC<3>((int64_t(static_cast<uint64_t>(color[2])) << 16), 0);

  // [MAC1,MAC2,MAC3] = MAC+(FC-MAC)*IR0
  InterpolateColor(REGS.MAC1, REGS.MAC2, REGS.MAC3, shift, lm);

  // Color FIFO = [MAC1/16,MAC2/16,MAC3/16,CODE], [IR1,IR2,IR3] = [MAC1,MAC2,MAC3]
  PushRGBFromMAC();
}

static void Execute_DPCS(Instruction inst)
{
  REGS.FLAG.Clear();

  DPCS(REGS.RGBC, inst.GetShift(), inst.lm);

  REGS.FLAG.UpdateError();
}

static void Execute_DPCT(Instruction inst)
{
  REGS.FLAG.Clear();

  const uint8_t shift = inst.GetShift();
  const bool lm = inst.lm;

  for (uint32_t i = 0; i < 3; i++)
    DPCS(REGS.RGB0, shift, lm);

  REGS.FLAG.UpdateError();
}

static void Execute_DCPL(Instruction inst)
{
  REGS.FLAG.Clear();

  const uint8_t shift = inst.GetShift();
  const bool lm = inst.lm;

  // No need to assign these to MAC[1-3], as it'll never overflow.
  // [MAC1,MAC2,MAC3] = [R*IR1,G*IR2,B*IR3] SHL 4          ;<--- for DCPL only
  const int32_t in_MAC1 = (int32_t(static_cast<uint32_t>(REGS.RGBC[0])) * int32_t(REGS.IR1)) << 4;
  const int32_t in_MAC2 = (int32_t(static_cast<uint32_t>(REGS.RGBC[1])) * int32_t(REGS.IR2)) << 4;
  const int32_t in_MAC3 = (int32_t(static_cast<uint32_t>(REGS.RGBC[2])) * int32_t(REGS.IR3)) << 4;

  // [MAC1,MAC2,MAC3] = MAC+(FC-MAC)*IR0
  InterpolateColor(in_MAC1, in_MAC2, in_MAC3, shift, lm);

  // Color FIFO = [MAC1/16,MAC2/16,MAC3/16,CODE], [IR1,IR2,IR3] = [MAC1,MAC2,MAC3]
  PushRGBFromMAC();

  REGS.FLAG.UpdateError();
}

static void Execute_INTPL(Instruction inst)
{
  REGS.FLAG.Clear();

  const uint8_t shift = inst.GetShift();
  const bool lm = inst.lm;

  // No need to assign these to MAC[1-3], as it'll never overflow.
  // [MAC1,MAC2,MAC3] = [IR1,IR2,IR3] SHL 12               ;<--- for INTPL only
  // [MAC1,MAC2,MAC3] = MAC+(FC-MAC)*IR0
  InterpolateColor(int32_t(REGS.IR1) << 12, int32_t(REGS.IR2) << 12, int32_t(REGS.IR3) << 12, shift, lm);

  // Color FIFO = [MAC1/16,MAC2/16,MAC3/16,CODE], [IR1,IR2,IR3] = [MAC1,MAC2,MAC3]
  PushRGBFromMAC();

  REGS.FLAG.UpdateError();
}

static void Execute_GPL(Instruction inst)
{
  REGS.FLAG.Clear();

  const uint8_t shift = inst.GetShift();
  const bool lm = inst.lm;

  // [MAC1,MAC2,MAC3] = [MAC1,MAC2,MAC3] SHL (sf*12)       ;<--- for GPL only
  // [MAC1,MAC2,MAC3] = (([IR1,IR2,IR3] * IR0) + [MAC1,MAC2,MAC3]) SAR (sf*12)
  TruncateAndSetMACAndIR<1>((int64_t(int32_t(REGS.IR1) * int32_t(REGS.IR0)) + (int64_t(REGS.MAC1) << shift)), shift, lm);
  TruncateAndSetMACAndIR<2>((int64_t(int32_t(REGS.IR2) * int32_t(REGS.IR0)) + (int64_t(REGS.MAC2) << shift)), shift, lm);
  TruncateAndSetMACAndIR<3>((int64_t(int32_t(REGS.IR3) * int32_t(REGS.IR0)) + (int64_t(REGS.MAC3) << shift)), shift, lm);

  // Color FIFO = [MAC1/16,MAC2/16,MAC3/16,CODE], [IR1,IR2,IR3] = [MAC1,MAC2,MAC3]
  PushRGBFromMAC();

  REGS.FLAG.UpdateError();
}

static void Execute_GPF(Instruction inst)
{
  REGS.FLAG.Clear();

  const uint8_t shift = inst.GetShift();
  const bool lm = inst.lm;

  // [MAC1,MAC2,MAC3] = [0,0,0]                            ;<--- for GPF only
  // [MAC1,MAC2,MAC3] = (([IR1,IR2,IR3] * IR0) + [MAC1,MAC2,MAC3]) SAR (sf*12)
  TruncateAndSetMACAndIR<1>(int64_t(int32_t(REGS.IR1) * int32_t(REGS.IR0)), shift, lm);
  TruncateAndSetMACAndIR<2>(int64_t(int32_t(REGS.IR2) * int32_t(REGS.IR0)), shift, lm);
  TruncateAndSetMACAndIR<3>(int64_t(int32_t(REGS.IR3) * int32_t(REGS.IR0)), shift, lm);

  // Color FIFO = [MAC1/16,MAC2/16,MAC3/16,CODE], [IR1,IR2,IR3] = [MAC1,MAC2,MAC3]
  PushRGBFromMAC();

  REGS.FLAG.UpdateError();
}

void ExecuteInstruction(uint32_t inst_bits)
{
  const Instruction inst{inst_bits};
  switch (inst.command)
  {
    case 0x01:
      CPU::AddGTETicks(15);
      Execute_RTPS(inst);
      break;

    case 0x06:
    {
      CPU::AddGTETicks(8);
      if (g_settings.gpu_pgxp_enable && g_settings.gpu_pgxp_culling)
        Execute_NCLIP_PGXP(inst);
      else
        Execute_NCLIP(inst);
    }
    break;

    case 0x0C:
      CPU::AddGTETicks(6);
      Execute_OP(inst);
      break;

    case 0x10:
      CPU::AddGTETicks(8);
      Execute_DPCS(inst);
      break;

    case 0x11:
      CPU::AddGTETicks(8);
      Execute_INTPL(inst);
      break;

    case 0x12:
      CPU::AddGTETicks(8);
      Execute_MVMVA(inst);
      break;

    case 0x13:
      CPU::AddGTETicks(19);
      Execute_NCDS(inst);
      break;

    case 0x14:
      CPU::AddGTETicks(13);
      Execute_CDP(inst);
      break;

    case 0x16:
      CPU::AddGTETicks(44);
      Execute_NCDT(inst);
      break;

    case 0x1B:
      CPU::AddGTETicks(17);
      Execute_NCCS(inst);
      break;

    case 0x1C:
      CPU::AddGTETicks(11);
      Execute_CC(inst);
      break;

    case 0x1E:
      CPU::AddGTETicks(14);
      Execute_NCS(inst);
      break;

    case 0x20:
      CPU::AddGTETicks(30);
      Execute_NCT(inst);
      break;

    case 0x28:
      CPU::AddGTETicks(5);
      Execute_SQR(inst);
      break;

    case 0x29:
      CPU::AddGTETicks(8);
      Execute_DCPL(inst);
      break;

    case 0x2A:
      CPU::AddGTETicks(17);
      Execute_DPCT(inst);
      break;

    case 0x2D:
      CPU::AddGTETicks(5);
      Execute_AVSZ3(inst);
      break;

    case 0x2E:
      CPU::AddGTETicks(6);
      Execute_AVSZ4(inst);
      break;

    case 0x30:
      CPU::AddGTETicks(23);
      Execute_RTPT(inst);
      break;

    case 0x3D:
      CPU::AddGTETicks(5);
      Execute_GPF(inst);
      break;

    case 0x3E:
      CPU::AddGTETicks(5);
      Execute_GPL(inst);
      break;

    case 0x3F:
      CPU::AddGTETicks(39);
      Execute_NCCT(inst);
      break;

    default:
      break;
  }
}

InstructionImpl GetInstructionImpl(uint32_t inst_bits, TickCount* ticks)
{
  const Instruction inst{inst_bits};
  switch (inst.command)
  {
    case 0x01:
      *ticks = 15;
      return &Execute_RTPS;

    case 0x06:
    {
      *ticks = 8;
      if (g_settings.gpu_pgxp_enable && g_settings.gpu_pgxp_culling)
        return &Execute_NCLIP_PGXP;
      else
        return &Execute_NCLIP;
    }

    case 0x0C:
      *ticks = 6;
      return &Execute_OP;

    case 0x10:
      *ticks = 8;
      return &Execute_DPCS;

    case 0x11:
      *ticks = 8;
      return &Execute_INTPL;

    case 0x12:
      *ticks = 8;
      return &Execute_MVMVA;

    case 0x13:
      *ticks = 19;
      return &Execute_NCDS;

    case 0x14:
      *ticks = 13;
      return &Execute_CDP;

    case 0x16:
      *ticks = 44;
      return &Execute_NCDT;

    case 0x1B:
      *ticks = 17;
      return &Execute_NCCS;

    case 0x1C:
      *ticks = 11;
      return &Execute_CC;

    case 0x1E:
      *ticks = 14;
      return &Execute_NCS;

    case 0x20:
      *ticks = 30;
      return &Execute_NCT;

    case 0x28:
      *ticks = 5;
      return &Execute_SQR;

    case 0x29:
      *ticks = 8;
      return &Execute_DCPL;

    case 0x2A:
      *ticks = 17;
      return &Execute_DPCT;

    case 0x2D:
      *ticks = 5;
      return &Execute_AVSZ3;

    case 0x2E:
      *ticks = 6;
      return &Execute_AVSZ4;

    case 0x30:
      *ticks = 23;
      return &Execute_RTPT;

    case 0x3D:
      *ticks = 5;
      return &Execute_GPF;

    case 0x3E:
      *ticks = 5;
      return &Execute_GPL;

    case 0x3F:
      *ticks = 39;
      return &Execute_NCCT;

    default:
      break;
  }
  return nullptr;
}

} // namespace GTE
