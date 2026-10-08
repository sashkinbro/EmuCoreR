/***************************************************************************
 *   Original copyright notice from PGXP code from Beetle PSX.             *
 *   Copyright (C) 2016 by iCatButler                                      *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.           *
 ***************************************************************************/

#include "pgxp.h"
#include "bus.h"
#include "cpu_core.h"
#include "settings.h"
#include "common/state_wrapper.h"
#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstring>
#include <limits>
namespace PGXP {

// GPU vertex coordinates are 11-bit signed, so a 2048x2048 window covers the
// entire drawable range on both axes. The previous 4096x4096 cache was four
// times larger than it needed to be.
inline constexpr uint32_t VERTEX_CACHE_WIDTH = 0x400 * 2, VERTEX_CACHE_HEIGHT = 0x400 * 2,
                     VERTEX_CACHE_SIZE = VERTEX_CACHE_WIDTH * VERTEX_CACHE_HEIGHT;

// PGXP memory mirrors the console RAM plus the scratchpad, sized from the
// RAM configuration the bus actually booted with (2MB or 8MB).
static uint32_t s_mem_value_count = 0;
static uint32_t s_mem_scratch_offset = 0;
static uint32_t s_mem_allocated_count = 0;

#define NONE 0
#define ALL 0xFFFFFFFF
#define VALID 1
#define VALID_0 (VALID << 0)
#define VALID_1 (VALID << 8)
#define VALID_2 (VALID << 16)
#define VALID_3 (VALID << 24)
#define VALID_01 (VALID_0 | VALID_1)
#define VALID_012 (VALID_0 | VALID_1 | VALID_2)
#define VALID_ALL (VALID_0 | VALID_1 | VALID_2 | VALID_3)
#define INV_VALID_ALL (ALL ^ VALID_ALL)

// Byte 2 marks a tracked Z, byte 3 marks it as imprecise (came through an
// operation whose effect on the depth value is unknown).
#define VALID_Z VALID_2
#define TAINTED_Z VALID_3

// Depth values are normalized to the GTE's 16-bit Z range before reaching the
// GPU, so the depth buffer and the perspective divide share one scale.
static constexpr float PGXP_MAX_Z = 65535.0f;

typedef struct PGXP_value_Tag
{
  float x;
  float y;
  float z;
  union
  {
    unsigned int flags;
    unsigned char compFlags[4];
    unsigned short halfFlags[2];
  };
  unsigned int value;
} PGXP_value;

typedef union
{
  struct
  {
    uint8_t l, h, h2, h3;
  } b;
  struct
  {
    uint16_t l, h;
  } w;
  struct
  {
    int8_t l, h, h2, h3;
  } sb;
  struct
  {
    int16_t l, h;
  } sw;
  uint32_t d;
  int32_t sd;
} psx_value;

static void PGXP_CacheVertex(int16_t sx, int16_t sy, const PGXP_value& vertex);

static void MakeValid(PGXP_value* pV, uint32_t psxV);
static void Validate(PGXP_value* pV, uint32_t psxV);
static void MaskValidate(PGXP_value* pV, uint32_t psxV, uint32_t mask, uint32_t validMask);

static double f16Sign(double in);
static double f16Unsign(double in);
static double f16Overflow(double in);

static PGXP_value* GetPtr(uint32_t addr);
static PGXP_value* ReadMem(uint32_t addr);

static const PGXP_value PGXP_value_invalid = {0.f, 0.f, 0.f, {0}, 0};
static const PGXP_value PGXP_value_zero = {0.f, 0.f, 0.f, {VALID_01}, 0};

static PGXP_value CPU_reg[34];
static PGXP_value CP0_reg[32];
#define CPU_Hi CPU_reg[32]
#define CPU_Lo CPU_reg[33]

// GTE registers
static PGXP_value GTE_data_reg[32];
static PGXP_value GTE_ctrl_reg[32];

static PGXP_value* Mem = nullptr;
static PGXP_value* vertexCache = nullptr;

ALWAYS_INLINE_RELEASE void MakeValid(PGXP_value* pV, uint32_t psxV)
{
  if (VALID_01 != (pV->flags & VALID_01))
  {
    if ((pV->flags & VALID_01) == 0)
    {
      pV->z = 0.f;
      pV->flags &= ~(VALID_Z | TAINTED_Z);
    }
    if ((pV->flags & VALID_0) == 0)
      pV->x = static_cast<float>(static_cast<int16_t>(static_cast<uint16_t>(psxV)));
    if ((pV->flags & VALID_1) == 0)
      pV->y = static_cast<float>(static_cast<int16_t>(static_cast<uint16_t>(psxV >> 16)));
    pV->flags |= VALID_01;
    pV->value = psxV;
  }
}

ALWAYS_INLINE_RELEASE void Validate(PGXP_value* pV, uint32_t psxV)
{
  // CPU writes to r0 are discarded by hardware, including delayed loads.
  // Restore its precision mirror before any instruction reads it.
  if (pV == &CPU_reg[0])
  {
    *pV = PGXP_value_zero;
    return;
  }
  // assume pV is not NULL
  pV->flags &= (pV->value == psxV) ? ALL : INV_VALID_ALL;
}

ALWAYS_INLINE_RELEASE void MaskValidate(PGXP_value* pV, uint32_t psxV, uint32_t mask, uint32_t validMask)
{
  // assume pV is not NULL
  pV->flags &= ((pV->value & mask) == (psxV & mask)) ? ALL : (ALL ^ (validMask));
}

enum class ZState : uint8_t
{
  Missing,
  Tainted,
  Precise,
};

ALWAYS_INLINE_RELEASE ZState GetZState(const PGXP_value& v)
{
  if ((v.flags & VALID_Z) == 0)
    return ZState::Missing;

  return (v.flags & TAINTED_Z) ? ZState::Tainted : ZState::Precise;
}

ALWAYS_INLINE_RELEASE void ClearZ(PGXP_value& v)
{
  v.flags &= ~(VALID_Z | TAINTED_Z);
}

ALWAYS_INLINE_RELEASE void SetZ(PGXP_value& v, float z, bool tainted)
{
  v.z = z;
  v.flags = (v.flags & ~(VALID_Z | TAINTED_Z)) | VALID_Z | (tainted ? TAINTED_Z : 0u);
}

ALWAYS_INLINE_RELEASE void CopyZState(PGXP_value& dst, const PGXP_value& src)
{
  dst.z = src.z;
  dst.flags = (dst.flags & ~(VALID_Z | TAINTED_Z)) | (src.flags & (VALID_Z | TAINTED_Z));
}

ALWAYS_INLINE_RELEASE void TaintZ(PGXP_value& v)
{
  if (v.flags & VALID_Z)
    v.flags |= TAINTED_Z;
}

// Keep the best available Z: a precise one beats an imprecise one, and any
// tracked Z beats none.
ALWAYS_INLINE_RELEASE void AdoptZ(PGXP_value& dst, const PGXP_value& src)
{
  const ZState src_state = GetZState(src);
  if (src_state == ZState::Missing)
    return;

  const ZState dst_state = GetZState(dst);
  if (dst_state == ZState::Precise || (dst_state == ZState::Tainted && src_state == ZState::Tainted))
    return;

  CopyZState(dst, src);
}

// Z for add/sub: the tracked depth belongs to the vertex the value came from,
// not to the packed components, so it must not be summed. Keep the first
// operand's Z and fall back to the second only when the first has none.
ALWAYS_INLINE_RELEASE void CombineZ(PGXP_value& dst, const PGXP_value& a, const PGXP_value& b)
{
  dst.flags &= ~(VALID_Z | TAINTED_Z);
  AdoptZ(dst, a);
  AdoptZ(dst, b);
}

ALWAYS_INLINE_RELEASE double f16Sign(double in)
{
  // Reduce before converting to fixed point: large shifts can exceed int64
  // after scaling, even though only the low 16 bits of the component survive.
  if (!std::isfinite(in))
    return 0.0;
  const int64_t scaled = static_cast<int64_t>(std::fmod(in, 65536.0) * 65536.0);
  const uint32_t bits = static_cast<uint32_t>(static_cast<uint64_t>(scaled) & UINT32_C(0xFFFFFFFF));
  return static_cast<double>(static_cast<int32_t>(bits)) / 65536.0;
}
ALWAYS_INLINE_RELEASE double f16Unsign(double in)
{
  return (in >= 0) ? in : ((double)in + (double)USHRT_MAX + 1);
}
ALWAYS_INLINE_RELEASE double f16Overflow(double in)
{
  if (!std::isfinite(in))
    return 0.0;
  return std::floor(std::trunc(in) / 65536.0);
}

ALWAYS_INLINE_RELEASE PGXP_value* GetPtr(uint32_t addr)
{
  if (!Mem)
    return nullptr;
  if ((addr & CPU::DCACHE_LOCATION_MASK) == CPU::DCACHE_LOCATION)
    return &Mem[s_mem_scratch_offset + ((addr & CPU::DCACHE_OFFSET_MASK) >> 2)];

  const uint32_t paddr = (addr & CPU::PHYSICAL_MEMORY_ADDRESS_MASK);
  if (paddr < Bus::RAM_MIRROR_END)
    return &Mem[(paddr & Bus::g_ram_mask) >> 2];
  else
    return nullptr;
}

ALWAYS_INLINE_RELEASE PGXP_value* ReadMem(uint32_t addr)
{
  return GetPtr(addr);
}

ALWAYS_INLINE_RELEASE void ValidateAndCopyMem(PGXP_value* dest, uint32_t addr, uint32_t value)
{
  PGXP_value* pMem = GetPtr(addr);
  if (pMem != NULL)
  {
    Validate(pMem, value);
    *dest = *pMem;
    return;
  }

  *dest = PGXP_value_invalid;
}

ALWAYS_INLINE_RELEASE static void ValidateAndCopyMem16(PGXP_value* dest, uint32_t addr, uint32_t value, int sign)
{
  uint32_t validMask = 0;
  psx_value val, mask;
  PGXP_value* pMem = GetPtr(addr);
  if (pMem != NULL)
  {
    mask.d = val.d = 0;
    // determine if high or low word
    if ((addr % 4) == 2)
    {
      val.w.h = static_cast<uint16_t>(value);
      mask.w.h = 0xFFFF;
      validMask = VALID_1;
    }
    else
    {
      val.w.l = static_cast<uint16_t>(value);
      mask.w.l = 0xFFFF;
      validMask = VALID_0;
    }

    // validate and copy whole value
    MaskValidate(pMem, val.d, mask.d, validMask);
    *dest = *pMem;

    // if high word then shift
    if ((addr % 4) == 2)
    {
      dest->x = dest->y;
      dest->compFlags[0] = dest->compFlags[1];
    }

    // truncate value
    dest->y = (dest->x < 0) ? -1.f * sign : 0.f; // 0.f;
    dest->value = value;
    dest->compFlags[1] = VALID; // iCB: High word is valid, just 0
    // The register was only half overwritten, so any attached Z no longer
    // describes the whole value.
    TaintZ(*dest);
    return;
  }

  *dest = PGXP_value_invalid;
}

ALWAYS_INLINE_RELEASE void WriteMem(const PGXP_value* value, uint32_t addr)
{
  PGXP_value* pMem = GetPtr(addr);

  if (pMem)
    *pMem = *value;
}

ALWAYS_INLINE_RELEASE static void WriteMem16(const PGXP_value* src, uint32_t addr)
{
  PGXP_value* dest = GetPtr(addr);

  if (dest)
  {
    /* determine if high or low word. Writing the half through explicit shift/
     * mask on dest->value rather than aliasing it as a psx_value* (a union of
     * a different type) avoids the strict-aliasing violation; the result is
     * identical to the little-endian w.l / w.h union writes on all supported
     * targets and endianness-independent besides. */
    const uint16_t half = static_cast<uint16_t>(src->value);
    if ((addr % 4) == 2)
    {
      dest->y = src->x;
      dest->compFlags[1] = src->compFlags[0];
      dest->value = (dest->value & UINT32_C(0x0000FFFF)) | (static_cast<uint32_t>(half) << 16);
    }
    else
    {
      dest->x = src->x;
      dest->compFlags[0] = src->compFlags[0];
      dest->value = (dest->value & UINT32_C(0xFFFF0000)) | static_cast<uint32_t>(half);
    }

    // overwrite z/w if valid
    if (src->compFlags[2] == VALID)
    {
      dest->z = src->z;
      dest->compFlags[2] = src->compFlags[2];
      dest->compFlags[3] = src->compFlags[3];
    }

    // dest->valid = dest->valid && src->valid;
  }
}

// Size the precision mirror from the RAM the bus actually booted with. The
// buffer is reallocated when the 2MB/8MB configuration changes.
static void EnsureMemory()
{
  const uint32_t ram_size = (Bus::g_ram_size != 0) ? Bus::g_ram_size : Bus::RAM_8MB_SIZE;
  s_mem_scratch_offset = ram_size / 4;
  s_mem_value_count = s_mem_scratch_offset + (CPU::DCACHE_SIZE / 4);

  if (Mem && s_mem_allocated_count != s_mem_value_count)
  {
    std::free(Mem);
    Mem = nullptr;
    s_mem_allocated_count = 0;
  }

  if (!Mem)
  {
    Mem = static_cast<PGXP_value*>(std::calloc(s_mem_value_count, sizeof(PGXP_value)));
    if (!Mem)
    {
      std::fprintf(stderr, "Failed to allocate PGXP memory\n");
      std::abort();
    }
    s_mem_allocated_count = s_mem_value_count;
  }
}

void Initialize()
{
  std::memset(CPU_reg, 0, sizeof(CPU_reg));
  std::memset(CP0_reg, 0, sizeof(CP0_reg));

  std::memset(GTE_data_reg, 0, sizeof(GTE_data_reg));
  std::memset(GTE_ctrl_reg, 0, sizeof(GTE_ctrl_reg));

  EnsureMemory();

  if (g_settings.gpu_pgxp_vertex_cache && !vertexCache)
  {
    vertexCache = static_cast<PGXP_value*>(std::calloc(VERTEX_CACHE_SIZE, sizeof(PGXP_value)));
    if (!vertexCache)
      g_settings.gpu_pgxp_vertex_cache = false;
  }

  if (vertexCache)
    std::memset(vertexCache, 0, sizeof(PGXP_value) * VERTEX_CACHE_SIZE);
}

void Reset()
{
  std::memset(CPU_reg, 0, sizeof(CPU_reg));
  std::memset(CP0_reg, 0, sizeof(CP0_reg));

  std::memset(GTE_data_reg, 0, sizeof(GTE_data_reg));
  std::memset(GTE_ctrl_reg, 0, sizeof(GTE_ctrl_reg));

  if (Mem)
    std::memset(Mem, 0, sizeof(PGXP_value) * s_mem_value_count);

  if (vertexCache)
    std::memset(vertexCache, 0, sizeof(PGXP_value) * VERTEX_CACHE_SIZE);
}

void Shutdown()
{
  if (vertexCache)
  {
    std::free(vertexCache);
    vertexCache = nullptr;
  }
  if (Mem)
  {
    std::free(Mem);
    Mem = nullptr;
    s_mem_allocated_count = 0;
  }

  std::memset(GTE_data_reg, 0, sizeof(GTE_data_reg));
  std::memset(GTE_ctrl_reg, 0, sizeof(GTE_ctrl_reg));

  std::memset(CPU_reg, 0, sizeof(CPU_reg));
  std::memset(CP0_reg, 0, sizeof(CP0_reg));
}

bool DoState(StateWrapper& sw)
{
  bool has_state = g_settings.gpu_pgxp_enable;
  sw.Do(&has_state);

  if (!has_state)
  {
    if (sw.IsReading())
      Reset();

    return !sw.HasError();
  }

  EnsureMemory();

  sw.DoBytes(CPU_reg, sizeof(CPU_reg));
  sw.DoBytes(CP0_reg, sizeof(CP0_reg));
  sw.DoBytes(GTE_data_reg, sizeof(GTE_data_reg));
  sw.DoBytes(GTE_ctrl_reg, sizeof(GTE_ctrl_reg));
  sw.DoBytes(Mem, sizeof(PGXP_value) * s_mem_value_count);

  // The vertex cache is a derived lookup table; let it rebuild instead of
  // serializing another cache-sized buffer into every state.
  if (sw.IsReading() && vertexCache)
    std::memset(vertexCache, 0, sizeof(PGXP_value) * VERTEX_CACHE_SIZE);

  return !sw.HasError();
}

bool DoMemoryState(StateWrapper& sw)
{
  bool has_state = g_settings.gpu_pgxp_enable;
  sw.Do(&has_state);
  if (!has_state)
  {
    if (sw.IsReading())
      Reset();
    return !sw.HasError();
  }

  EnsureMemory();
  if (sw.IsReading())
    Reset();
  sw.DoBytes(CPU_reg, sizeof(CPU_reg));
  sw.DoBytes(CP0_reg, sizeof(CP0_reg));
  sw.DoBytes(GTE_data_reg, sizeof(GTE_data_reg));
  sw.DoBytes(GTE_ctrl_reg, sizeof(GTE_ctrl_reg));

  // Each 64-word page carries a bitmap and only the entries with tracked
  // components. Empty RAM needs eight bytes per page instead of 1280, while
  // fully tracked RAM adds only a bitmap to the existing precision payload.
  constexpr uint32_t page_size = 64;
  std::array<PGXP_value, page_size> values;
  for (uint32_t base = 0; base < s_mem_value_count; base += page_size)
  {
    const uint32_t count = std::min(page_size, s_mem_value_count - base);
    uint64_t mask = 0;
    uint32_t tracked = 0;
    if (sw.IsWriting())
    {
      for (uint32_t offset = 0; offset < count; offset++)
      {
        if ((Mem[base + offset].flags & VALID_ALL) != 0)
        {
          mask |= uint64_t(1) << offset;
          values[tracked++] = Mem[base + offset];
        }
      }
    }
    sw.Do(&mask);
    if (sw.HasError() || (count < page_size && (mask >> count) != 0))
      return false;
    if (sw.IsReading())
    {
      for (uint32_t offset = 0; offset < count; offset++)
        tracked += static_cast<uint32_t>((mask >> offset) & 1u);
    }
    sw.DoBytes(values.data(), sizeof(PGXP_value) * tracked);
    if (sw.HasError())
      return false;
    if (sw.IsReading())
    {
      uint32_t entry = 0;
      for (uint32_t offset = 0; offset < count; offset++)
      {
        if ((mask & (uint64_t(1) << offset)) != 0)
          Mem[base + offset] = values[entry++];
      }
    }
  }
  return !sw.HasError();
}

// Instruction register decoding
#define op(_instr) (_instr >> 26) // The op part of the instruction register
#define func(_instr) ((_instr)&0x3F) // The funct part of the instruction register
#define sa(_instr) ((_instr >> 6) & 0x1F) // The sa part of the instruction register
#define rd(_instr) ((_instr >> 11) & 0x1F) // The rd part of the instruction register
#define rt(_instr) ((_instr >> 16) & 0x1F) // The rt part of the instruction register
#define rs(_instr) ((_instr >> 21) & 0x1F) // The rs part of the instruction register
#define imm(_instr) (_instr & 0xFFFF) // The immediate part of the instruction register

#define SX0 (GTE_data_reg[12].x)
#define SY0 (GTE_data_reg[12].y)
#define SX1 (GTE_data_reg[13].x)
#define SY1 (GTE_data_reg[13].y)
#define SX2 (GTE_data_reg[14].x)
#define SY2 (GTE_data_reg[14].y)

#define SXY0 (GTE_data_reg[12])
#define SXY1 (GTE_data_reg[13])
#define SXY2 (GTE_data_reg[14])
#define SXYP (GTE_data_reg[15])

void GTE_PushSXYZ2f(float x, float y, float z, uint32_t v)
{
  // push values down FIFO
  SXY0 = SXY1;
  SXY1 = SXY2;

  SXY2.x = x;
  SXY2.y = y;
  SXY2.z = z;
  SXY2.value = v;
  SXY2.flags = (std::isfinite(x) && std::isfinite(y)) ? VALID_01 : 0;
  if (std::isfinite(z) && z > 0.f)
    SXY2.flags |= VALID_Z;

  // Register 15 (SXYP) is a mirror of SXY2 on hardware; keep the mirror in
  // sync so MFC2/SWC2 reads of reg 15 see the vertex that was just pushed.
  SXYP = SXY2;

  if (g_settings.gpu_pgxp_vertex_cache)
    PGXP_CacheVertex(static_cast<int16_t>(static_cast<uint16_t>(v)), static_cast<int16_t>(static_cast<uint16_t>(v >> 16)), SXY2);
}

#define VX(n) (psxRegs.CP2D.p[n << 1].sw.l)
#define VY(n) (psxRegs.CP2D.p[n << 1].sw.h)
#define VZ(n) (psxRegs.CP2D.p[(n << 1) + 1].sw.l)

int GTE_NCLIP_valid(uint32_t sxy0, uint32_t sxy1, uint32_t sxy2)
{
  Validate(&SXY0, sxy0);
  Validate(&SXY1, sxy1);
  Validate(&SXY2, sxy2);
  // Only use the precise NCLIP when all three vertices carry a tracked Z.
  // Screen-space coordinates pushed by the game itself have no Z, and
  // feeding them through the precise path makes culling drift from the
  // hardware result.
  if (((SXY0.flags & SXY1.flags & SXY2.flags & VALID_012) == VALID_012))
    return 1;
  return 0;
}

float GTE_NCLIP()
{
  // Translate before taking the cross product to avoid cancellation near
  // the screen edges. Preserve the sign of even a subpixel-sized triangle
  // when the GTE converts the result to an integer for backface culling.
  const double ax = double(SX1) - double(SX0);
  const double ay = double(SY1) - double(SY0);
  const double bx = double(SX2) - double(SX0);
  const double by = double(SY2) - double(SY0);
  const double area = ax * by - ay * bx;
  if (area > 0.0 && area < 1.0)
    return 1.f;
  if (area < 0.0 && area > -1.0)
    return -1.f;
  return static_cast<float>(area);
}

static void PGXP_MTC2_int(PGXP_value value, uint32_t reg)
{
  switch (reg)
  {
    case 15:
      // push FIFO
      SXY0 = SXY1;
      SXY1 = SXY2;
      SXY2 = value;
      SXYP = SXY2;
      break;

    case 31:
      return;
  }

  GTE_data_reg[reg] = value;
  if (reg == 14)
    SXYP = SXY2;
}

////////////////////////////////////
// Data transfer tracking
////////////////////////////////////

void CPU_MFC2(uint32_t instr, uint32_t rtVal, uint32_t rdVal)
{
  // CPU[Rt] = GTE_D[Rd]
  Validate(&GTE_data_reg[rd(instr)], rdVal);
  CPU_reg[rt(instr)] = GTE_data_reg[rd(instr)];
  CPU_reg[rt(instr)].value = rtVal;
}

void CPU_MTC2(uint32_t instr, uint32_t rdVal, uint32_t rtVal)
{
  // GTE_D[Rd] = CPU[Rt]
  Validate(&CPU_reg[rt(instr)], rtVal);
  PGXP_MTC2_int(CPU_reg[rt(instr)], rd(instr));
  GTE_data_reg[rd(instr)].value = rdVal;
}

void CPU_CFC2(uint32_t instr, uint32_t rtVal, uint32_t rdVal)
{
  // CPU[Rt] = GTE_C[Rd]
  Validate(&GTE_ctrl_reg[rd(instr)], rdVal);
  CPU_reg[rt(instr)] = GTE_ctrl_reg[rd(instr)];
  CPU_reg[rt(instr)].value = rtVal;
}

void CPU_CTC2(uint32_t instr, uint32_t rdVal, uint32_t rtVal)
{
  // GTE_C[Rd] = CPU[Rt]
  Validate(&CPU_reg[rt(instr)], rtVal);
  GTE_ctrl_reg[rd(instr)] = CPU_reg[rt(instr)];
  GTE_ctrl_reg[rd(instr)].value = rdVal;
}

////////////////////////////////////
// Memory Access
////////////////////////////////////
void CPU_LWC2(uint32_t instr, uint32_t rtVal, uint32_t addr)
{
  // GTE_D[Rt] = Mem[addr]
  PGXP_value val;
  ValidateAndCopyMem(&val, addr, rtVal);
  PGXP_MTC2_int(val, rt(instr));
}

void CPU_SWC2(uint32_t instr, uint32_t rtVal, uint32_t addr)
{
  //  Mem[addr] = GTE_D[Rt]
  Validate(&GTE_data_reg[rt(instr)], rtVal);
  WriteMem(&GTE_data_reg[rt(instr)], addr);
}

ALWAYS_INLINE_RELEASE void PGXP_CacheVertex(int16_t sx, int16_t sy, const PGXP_value& vertex)
{
  if (sx >= -0x400 && sx <= 0x3ff && sy >= -0x400 && sy <= 0x3ff)
  {
    // Write vertex into cache
    vertexCache[(sy + 0x400) * VERTEX_CACHE_WIDTH + (sx + 0x400)] = vertex;
  }
}

static ALWAYS_INLINE_RELEASE PGXP_value* PGXP_GetCachedVertex(short sx, short sy)
{
  if (sx >= -0x400 && sx <= 0x3ff && sy >= -0x400 && sy <= 0x3ff)
  {
    // Return pointer to cache entry
    return &vertexCache[(sy + 0x400) * VERTEX_CACHE_WIDTH + (sx + 0x400)];
  }

  return nullptr;
}

static ALWAYS_INLINE_RELEASE float TruncateVertexPosition(float p)
{
  const int32_t int_part = static_cast<int32_t>(p);
  const float int_part_f = static_cast<float>(int_part);
  // Sign-extend the low 11 bits via shift idiom. Do the left shift in
  // unsigned space to avoid C++<20 UB on negative int_part (the GPU
  // truncates positions to an 11-bit signed range, so int_part is
  // routinely negative).
  return static_cast<float>(static_cast<int16_t>(static_cast<uint32_t>(int_part) << 5) >> 5) + (p - int_part_f);
}

static ALWAYS_INLINE_RELEASE bool IsWithinTolerance(float precise_x, float precise_y, int int_x, int int_y)
{
  const float tolerance = g_settings.gpu_pgxp_tolerance;
  if (tolerance < 0.0f)
    return true;

  return (std::abs(precise_x - static_cast<float>(int_x)) <= tolerance &&
          std::abs(precise_y - static_cast<float>(int_y)) <= tolerance);
}

bool GetPreciseVertex(uint32_t addr, uint32_t value, int x, int y, int xOffs, int yOffs, float* out_x, float* out_y, float* out_w)
{
  *out_x = static_cast<float>(x);
  *out_y = static_cast<float>(y);
  *out_w = 1.f;

  auto use_vertex = [&](const PGXP_value* v, bool allow_depth) {
    if (!v || (v->flags & VALID_01) != VALID_01 || v->value != value ||
        !std::isfinite(v->x) || !std::isfinite(v->y) ||
        double(v->x) < double(std::numeric_limits<int32_t>::min()) ||
        double(v->x) > double(std::numeric_limits<int32_t>::max()) ||
        double(v->y) < double(std::numeric_limits<int32_t>::min()) ||
        double(v->y) > double(std::numeric_limits<int32_t>::max()))
      return false;

    const float px = TruncateVertexPosition(v->x) + static_cast<float>(xOffs);
    const float py = TruncateVertexPosition(v->y) + static_cast<float>(yOffs);
    if (!IsWithinTolerance(px, py, x, y))
      return false;

    *out_x = px;
    *out_y = py;
    if (allow_depth && (v->flags & VALID_Z) != 0 && std::isfinite(v->z) && v->z > 0.f)
      *out_w = v->z / PGXP_MAX_Z;
    return true;
  };

  const PGXP_value* vert = ReadMem(addr);
  if (use_vertex(vert, true))
    return (vert->flags & VALID_Z) != 0 && std::isfinite(vert->z) && vert->z > 0.f;

  if (g_settings.gpu_pgxp_vertex_cache && vertexCache)
  {
    const short psx_x = (short)(value & 0xFFFFu);
    const short psx_y = (short)(value >> 16);

    // Look in cache for valid vertex
    vert = PGXP_GetCachedVertex(psx_x, psx_y);
    // A coordinate-only cache cannot identify which of several vertices
    // sharing an integer position supplied this polygon's depth.
    if (use_vertex(vert, false))
      return false;
  }

  // no valid value can be found anywhere, use the native PSX data
  *out_x = static_cast<float>(x);
  *out_y = static_cast<float>(y);
  *out_w = 1.0f;
  return false;
}

// Instruction register decoding
#define op(_instr) (_instr >> 26) // The op part of the instruction register
#define func(_instr) ((_instr)&0x3F) // The funct part of the instruction register
#define sa(_instr) ((_instr >> 6) & 0x1F) // The sa part of the instruction register
#define rd(_instr) ((_instr >> 11) & 0x1F) // The rd part of the instruction register
#define rt(_instr) ((_instr >> 16) & 0x1F) // The rt part of the instruction register
#define rs(_instr) ((_instr >> 21) & 0x1F) // The rs part of the instruction register
#define imm(_instr) (_instr & 0xFFFF) // The immediate part of the instruction register
#define imm_sext(_instr)                                                                                               \
  static_cast<int32_t>(static_cast<int16_t>(_instr & 0xFFFF)) // The immediate part of the instruction register

// LWL/LWR merge bytes of an aligned memory word into a register. Keep the
// tracked halves that are fully overwritten by memory so the common
// lwl+lwr unaligned load pattern keeps its precision.
static void MergeLoad(uint32_t instr, uint32_t rtVal, uint32_t addr)
{
  const uint32_t off = addr & UINT32_C(3);
  const bool left = (op(instr) == 0x22u);
  const uint32_t first = left ? (3u - off) : 0u; // first register byte from memory
  const uint32_t last = left ? 3u : (3u - off);  // last register byte from memory

  PGXP_value ret = CPU_reg[rt(instr)];
  const PGXP_value* mem = GetPtr(addr & ~UINT32_C(3));
  PGXP_value loaded = mem ? *mem : PGXP_value_invalid;
  const uint32_t shift = (left ? 3u - off : off) * 8u;
  const uint32_t memory_mask = left ? (UINT32_MAX >> shift) : (UINT32_MAX << shift);
  const uint32_t memory_bits = left ? (rtVal >> shift) : (rtVal << shift);
  // DMA and other non-CPU writers do not update the precision mirror. Check
  // the actual bytes transferred by this load before reusing their metadata.
  if ((loaded.value & memory_mask) != (memory_bits & memory_mask))
    loaded.flags = 0;
  const uint32_t register_mask = left ? (UINT32_MAX << shift) : (UINT32_MAX >> shift);
  if ((ret.value & ~register_mask) != (rtVal & ~register_mask))
    ret.flags = 0;

  for (uint32_t half = 0; half < 2; half++)
  {
    const uint32_t b0 = half * 2u;
    const bool covered = (first <= b0 && last >= b0 + 1u);
    const bool touched = (first <= b0 + 1u && last >= b0);

    if (covered)
    {
      // Register byte b0 maps to this aligned memory byte; the pair forms a
      // trackable memory half only when that byte is the even one.
      const int32_t mem_byte = left ? (static_cast<int32_t>(b0) - static_cast<int32_t>(3u - off))
                                    : (static_cast<int32_t>(b0) + static_cast<int32_t>(off));
      if ((mem_byte & 1) == 0 && loaded.compFlags[mem_byte >> 1] == VALID)
      {
        const float value = (mem_byte >> 1) ? loaded.y : loaded.x;
        if (half == 0)
          ret.x = value;
        else
          ret.y = value;
        ret.compFlags[half] = loaded.compFlags[mem_byte >> 1];
      }
      else
      {
        ret.compFlags[half] = NONE;
      }
    }
    else if (touched)
    {
      ret.compFlags[half] = NONE;
    }
  }

  if (first == 0u && last == 3u)
    CopyZState(ret, loaded);
  else if ((ret.flags & loaded.flags & VALID_Z) != 0 && ret.z != loaded.z)
    ClearZ(ret);
  else
    AdoptZ(ret, loaded);

  ret.value = rtVal;
  CPU_reg[rt(instr)] = ret;
}

// SWL/SWR merge bytes of a register into an aligned memory word.
static void MergeStore(uint32_t instr, uint32_t memVal, uint32_t addr)
{
  PGXP_value* mem = GetPtr(addr & ~UINT32_C(3));
  if (!mem)
    return;

  const uint32_t off = addr & UINT32_C(3);
  const bool left = (op(instr) == 0x2Au);
  const uint32_t first = left ? 0u : off; // first memory byte from the register
  const uint32_t last = left ? off : 3u;  // last memory byte from the register

  PGXP_value src = CPU_reg[rt(instr)];
  if (rt(instr) == 0)
    src = PGXP_value_zero;
  const uint32_t shift = (left ? 3u - off : off) * 8u;
  const uint32_t source_mask = left ? (UINT32_MAX << shift) : (UINT32_MAX >> shift);
  const uint32_t source_bits = left ? (memVal << shift) : (memVal >> shift);
  if ((src.value & source_mask) != (source_bits & source_mask))
    src.flags = 0;
  const uint32_t memory_mask = left ? (UINT32_MAX >> shift) : (UINT32_MAX << shift);
  if ((mem->value & ~memory_mask) != (memVal & ~memory_mask))
    mem->flags = 0;

  for (uint32_t half = 0; half < 2; half++)
  {
    const uint32_t b0 = half * 2u;
    const bool covered = (first <= b0 && last >= b0 + 1u);
    const bool touched = (first <= b0 + 1u && last >= b0);

    if (covered)
    {
      // Memory byte b0 comes from this source register byte.
      const int32_t src_byte = left ? (static_cast<int32_t>(3u - off) + static_cast<int32_t>(b0))
                                    : (static_cast<int32_t>(b0) - static_cast<int32_t>(off));
      if ((src_byte & 1) == 0 && src.compFlags[src_byte >> 1] == VALID)
      {
        const float value = (src_byte >> 1) ? src.y : src.x;
        if (half == 0)
          mem->x = value;
        else
          mem->y = value;
        mem->compFlags[half] = src.compFlags[src_byte >> 1];
      }
      else
      {
        mem->compFlags[half] = NONE;
      }
    }
    else if (touched)
    {
      mem->compFlags[half] = NONE;
    }
  }

  if (first == 0u && last == 3u)
    CopyZState(*mem, src);
  else if ((mem->flags & src.flags & VALID_Z) != 0 && mem->z != src.z)
    ClearZ(*mem);
  else
    AdoptZ(*mem, src);

  mem->value = memVal;
}

void CPU_LW(uint32_t instr, uint32_t rtVal, uint32_t addr)
{
  // Rt = Mem[Rs + Im]
  const uint32_t opcode = op(instr);
  if (opcode == 0x22u || opcode == 0x26u)
  {
    MergeLoad(instr, rtVal, addr);
    return;
  }

  ValidateAndCopyMem(&CPU_reg[rt(instr)], addr, rtVal);
}

void CPU_LBx(uint32_t instr, uint32_t rtVal, uint32_t addr)
{
  CPU_reg[rt(instr)] = PGXP_value_invalid;
}

void CPU_LHx(uint32_t instr, uint32_t rtVal, uint32_t addr)
{
  // Rt = Mem[Rs + Im] (sign/zero extended)
  ValidateAndCopyMem16(&CPU_reg[rt(instr)], addr, rtVal, 1);
}

void CPU_SB(uint32_t instr, uint8_t rtVal, uint32_t addr)
{
  WriteMem(&PGXP_value_invalid, addr);
}

void CPU_SH(uint32_t instr, uint16_t rtVal, uint32_t addr)
{
  PGXP_value* val = &CPU_reg[rt(instr)];

  // validate and copy half value
  MaskValidate(val, rtVal, 0xFFFF, VALID_0);
  WriteMem16(val, addr);
}

void CPU_SW(uint32_t instr, uint32_t rtVal, uint32_t addr)
{
  // Mem[Rs + Im] = Rt
  const uint32_t opcode = op(instr);
  if (opcode == 0x2Au || opcode == 0x2Eu)
  {
    MergeStore(instr, rtVal, addr);
    return;
  }

  PGXP_value* val = &CPU_reg[rt(instr)];
  Validate(val, rtVal);
  WriteMem(val, addr);
}

void CPU_MOVE(uint32_t rd_and_rs, uint32_t rsVal)
{
  const uint32_t Rs = (rd_and_rs & 0xFFu);
  Validate(&CPU_reg[Rs], rsVal);
  CPU_reg[(rd_and_rs >> 8)] = CPU_reg[Rs];
}

void CPU_ADDI(uint32_t instr, uint32_t rsVal)
{
  // Rt = Rs + Imm (signed)
  if (rs(instr) == 0)
  {
    // Adding to the hardwired zero register is a constant load; do not read
    // the tracked value of r0, which the CPU itself never writes.
    CPU_reg[rt(instr)] = PGXP_value_zero;
    CPU_reg[rt(instr)].x = static_cast<float>(imm_sext(instr));
    CPU_reg[rt(instr)].y = (imm_sext(instr) < 0) ? -1.f : 0.f;
    CPU_reg[rt(instr)].value = static_cast<uint32_t>(imm_sext(instr));
    CPU_reg[rt(instr)].flags = VALID_01;
    return;
  }

  psx_value tempImm;
  PGXP_value ret;

  Validate(&CPU_reg[rs(instr)], rsVal);
  ret = CPU_reg[rs(instr)];
  tempImm.d = imm(instr);
  tempImm.sd = (tempImm.sd << 16) >> 16; // sign extend

  if (tempImm.d != 0)
  {
    ret.x = (float)f16Unsign(ret.x);
    ret.x += (float)tempImm.w.l;

    // carry on over/underflow
    const float of = static_cast<float>(((rsVal & 0xffffu) + tempImm.w.l) >> 16);
    ret.x = (float)f16Sign(ret.x);
    // ret.x -= of * (USHRT_MAX + 1);
    ret.y += tempImm.sw.h + of;

    // truncate on overflow/underflow
    ret.y = static_cast<float>(f16Sign(ret.y));
  }

  CPU_reg[rt(instr)] = ret;
  CPU_reg[rt(instr)].value = rsVal + imm_sext(instr);
}

void CPU_ANDI(uint32_t instr, uint32_t rsVal)
{
  // Rt = Rs & Imm
  const uint32_t rtVal = rsVal & imm(instr);
  psx_value vRt;
  PGXP_value ret;

  Validate(&CPU_reg[rs(instr)], rsVal);
  ret = CPU_reg[rs(instr)];

  vRt.d = rtVal;

  ret.y = 0.f; // remove upper 16-bits

  switch (imm(instr))
  {
    case 0:
      // if 0 then x == 0
      ret = PGXP_value_zero;
      break;
    case 0xFFFF:
      // if saturated then x == x
      break;
    default:
      // otherwise x is low precision value
      if ((rtVal & 0xffffu) != (rsVal & 0xffffu) || ret.compFlags[0] != VALID)
        ret.x = vRt.sw.l;
      ret.flags |= VALID_0;
  }

  ret.flags |= VALID_1;

  CPU_reg[rt(instr)] = ret;
  CPU_reg[rt(instr)].value = rtVal;
}

void CPU_ORI(uint32_t instr, uint32_t rsVal)
{
  // Rt = Rs | Imm
  const uint32_t rtVal = rsVal | imm(instr);
  psx_value vRt;
  PGXP_value ret;

  Validate(&CPU_reg[rs(instr)], rsVal);
  ret = CPU_reg[rs(instr)];

  vRt.d = rtVal;

  switch (imm(instr))
  {
    case 0:
      // if 0 then x == x
      break;
    default:
      // otherwise x is low precision value
      if (rtVal != rsVal || ret.compFlags[0] != VALID)
        ret.x = vRt.sw.l;
      ret.flags |= VALID_0;
  }

  ret.value = rtVal;
  CPU_reg[rt(instr)] = ret;
}

void CPU_XORI(uint32_t instr, uint32_t rsVal)
{
  // Rt = Rs ^ Imm
  const uint32_t rtVal = rsVal ^ imm(instr);
  psx_value vRt;
  PGXP_value ret;

  Validate(&CPU_reg[rs(instr)], rsVal);
  ret = CPU_reg[rs(instr)];

  vRt.d = rtVal;

  switch (imm(instr))
  {
    case 0:
      // if 0 then x == x
      break;
    default:
      // otherwise x is low precision value
      ret.x = vRt.sw.l;
      ret.flags |= VALID_0;
  }

  ret.value = rtVal;
  CPU_reg[rt(instr)] = ret;
}

void CPU_SLTI(uint32_t instr, uint32_t rsVal)
{
  Validate(&CPU_reg[rs(instr)], rsVal);
  PGXP_value ret = PGXP_value_zero;
  ret.value = static_cast<uint32_t>(static_cast<int32_t>(rsVal) < imm_sext(instr));
  ret.x = static_cast<float>(ret.value);
  CPU_reg[rt(instr)] = ret;
}

void CPU_SLTIU(uint32_t instr, uint32_t rsVal)
{
  Validate(&CPU_reg[rs(instr)], rsVal);
  PGXP_value ret = PGXP_value_zero;
  ret.value = static_cast<uint32_t>(rsVal < static_cast<uint32_t>(imm_sext(instr)));
  ret.x = static_cast<float>(ret.value);
  CPU_reg[rt(instr)] = ret;
}

////////////////////////////////////
// Load Upper
////////////////////////////////////
void CPU_LUI(uint32_t instr)
{
  // Rt = Imm << 16
  CPU_reg[rt(instr)] = PGXP_value_zero;
  CPU_reg[rt(instr)].y = (float)(int16_t)imm(instr);
  CPU_reg[rt(instr)].value = static_cast<uint32_t>(imm(instr)) << 16;
  CPU_reg[rt(instr)].flags = VALID_01;
}

////////////////////////////////////
// Register Arithmetic
////////////////////////////////////

void CPU_ADD(uint32_t instr, uint32_t rsVal, uint32_t rtVal)
{
  // Rd = Rs + Rt (signed)
  PGXP_value ret;
  Validate(&CPU_reg[rs(instr)], rsVal);
  Validate(&CPU_reg[rt(instr)], rtVal);

  if (rtVal != 0)
  {
    // iCB: Only require one valid input
    if (((CPU_reg[rt(instr)].flags & VALID_01) != VALID_01) != ((CPU_reg[rs(instr)].flags & VALID_01) != VALID_01))
    {
      MakeValid(&CPU_reg[rs(instr)], rsVal);
      MakeValid(&CPU_reg[rt(instr)], rtVal);
    }

    ret = CPU_reg[rs(instr)];

    ret.x = (float)f16Unsign(ret.x);
    ret.x += (float)f16Unsign(CPU_reg[rt(instr)].x);

    // carry on over/underflow
    const float of = static_cast<float>(((rsVal & 0xffffu) + (rtVal & 0xffffu)) >> 16);
    ret.x = (float)f16Sign(ret.x);
    // ret.x -= of * (USHRT_MAX + 1);
    ret.y += CPU_reg[rt(instr)].y + of;

    // truncate on overflow/underflow
    ret.y = static_cast<float>(f16Sign(ret.y));

    // Carry the tracked depth from the operands.
    CombineZ(ret, CPU_reg[rs(instr)], CPU_reg[rt(instr)]);

    ret.halfFlags[0] &= CPU_reg[rt(instr)].halfFlags[0];
  }
  else
  {
    ret = CPU_reg[rs(instr)];
  }

  ret.value = rsVal + rtVal;

  CPU_reg[rd(instr)] = ret;
}

void CPU_SUB(uint32_t instr, uint32_t rsVal, uint32_t rtVal)
{
  // Rd = Rs - Rt (signed)
  PGXP_value ret;
  Validate(&CPU_reg[rs(instr)], rsVal);
  Validate(&CPU_reg[rt(instr)], rtVal);

  if (rtVal == 0)
  {
    // Subtracting zero is a plain move; keep the tracked value untouched.
    ret = CPU_reg[rs(instr)];
    ret.value = rsVal;
    CPU_reg[rd(instr)] = ret;
    return;
  }

  // iCB: Only require one valid input
  if (((CPU_reg[rt(instr)].flags & VALID_01) != VALID_01) != ((CPU_reg[rs(instr)].flags & VALID_01) != VALID_01))
  {
    MakeValid(&CPU_reg[rs(instr)], rsVal);
    MakeValid(&CPU_reg[rt(instr)], rtVal);
  }

  ret = CPU_reg[rs(instr)];

  ret.x = (float)f16Unsign(ret.x);
  ret.x -= (float)f16Unsign(CPU_reg[rt(instr)].x);

  // carry on over/underflow
  const float of = (rsVal & 0xffffu) < (rtVal & 0xffffu) ? -1.f : 0.f;
  ret.x = (float)f16Sign(ret.x);
  // ret.x -= of * (USHRT_MAX + 1);
  ret.y -= CPU_reg[rt(instr)].y - of;

  // truncate on overflow/underflow
  ret.y = static_cast<float>(f16Sign(ret.y));

  // Carry the tracked depth from the operands.
  CombineZ(ret, CPU_reg[rs(instr)], CPU_reg[rt(instr)]);

  ret.halfFlags[0] &= CPU_reg[rt(instr)].halfFlags[0];

  ret.value = rsVal - rtVal;

  CPU_reg[rd(instr)] = ret;
}

enum class BitwiseOperation { And, Or, Xor, Nor };

static void CPU_BITWISE(uint32_t instr, uint32_t rdVal, uint32_t rsVal, uint32_t rtVal, BitwiseOperation operation)
{
  Validate(&CPU_reg[rs(instr)], rsVal);
  Validate(&CPU_reg[rt(instr)], rtVal);
  PGXP_value ret = PGXP_value_zero;
  const PGXP_value* sources[2] = {};
  for (uint32_t half = 0; half < 2; half++)
  {
    const uint16_t a = static_cast<uint16_t>(rsVal >> (half * 16));
    const uint16_t b = static_cast<uint16_t>(rtVal >> (half * 16));
    const uint16_t result = static_cast<uint16_t>(rdVal >> (half * 16));
    const PGXP_value* source = nullptr;
    bool constant = false;
    switch (operation)
    {
      case BitwiseOperation::And:
        if (b == 0xffffu)
          source = &CPU_reg[rs(instr)];
        else if (a == 0xffffu)
          source = &CPU_reg[rt(instr)];
        else
          constant = (a == 0 || b == 0);
        break;
      case BitwiseOperation::Or:
        if (b == 0)
          source = &CPU_reg[rs(instr)];
        else if (a == 0)
          source = &CPU_reg[rt(instr)];
        else
          constant = (a == 0xffffu || b == 0xffffu);
        break;
      case BitwiseOperation::Xor:
        if (rs(instr) == rt(instr))
          constant = true;
        else if (b == 0)
          source = &CPU_reg[rs(instr)];
        else if (a == 0)
          source = &CPU_reg[rt(instr)];
        break;
      case BitwiseOperation::Nor:
        constant = true;
        break;
    }
    if (!source && !constant && result != 0)
    {
      if (result == a)
        source = &CPU_reg[rs(instr)];
      else if (result == b)
        source = &CPU_reg[rt(instr)];
    }
    const bool precise = source && source->compFlags[half] == VALID;
    const float component = precise ? (half == 0 ? source->x : source->y) :
                                     static_cast<float>(static_cast<int16_t>(result));
    if (half == 0)
      ret.x = component;
    else
      ret.y = component;
    if (precise)
      sources[half] = source;
  }

  // Depth follows the components actually retained by the mask. Combining
  // coordinates from different depths does not establish a new perspective.
  if (!(sources[0] && sources[1] && (sources[0]->flags & sources[1]->flags & VALID_Z) != 0 &&
        sources[0]->z != sources[1]->z))
  {
    for (const PGXP_value* source : sources)
    {
      if (source)
        AdoptZ(ret, *source);
    }
  }
  ret.value = rdVal;
  CPU_reg[rd(instr)] = ret;
}

void CPU_AND_(uint32_t instr, uint32_t rsVal, uint32_t rtVal)
{
  // Rd = Rs & Rt
  const uint32_t rdVal = rsVal & rtVal;
  CPU_BITWISE(instr, rdVal, rsVal, rtVal, BitwiseOperation::And);
}

void CPU_OR_(uint32_t instr, uint32_t rsVal, uint32_t rtVal)
{
  // Rd = Rs | Rt
  const uint32_t rdVal = rsVal | rtVal;
  CPU_BITWISE(instr, rdVal, rsVal, rtVal, BitwiseOperation::Or);
}

void CPU_XOR_(uint32_t instr, uint32_t rsVal, uint32_t rtVal)
{
  // Rd = Rs ^ Rt
  const uint32_t rdVal = rsVal ^ rtVal;
  CPU_BITWISE(instr, rdVal, rsVal, rtVal, BitwiseOperation::Xor);
}

void CPU_NOR(uint32_t instr, uint32_t rsVal, uint32_t rtVal)
{
  // Rd = Rs NOR Rt
  const uint32_t rdVal = ~(rsVal | rtVal);
  CPU_BITWISE(instr, rdVal, rsVal, rtVal, BitwiseOperation::Nor);
}

void CPU_SLT(uint32_t instr, uint32_t rsVal, uint32_t rtVal)
{
  Validate(&CPU_reg[rs(instr)], rsVal);
  Validate(&CPU_reg[rt(instr)], rtVal);
  PGXP_value ret = PGXP_value_zero;
  ret.value = static_cast<uint32_t>(static_cast<int32_t>(rsVal) < static_cast<int32_t>(rtVal));
  ret.x = static_cast<float>(ret.value);
  CPU_reg[rd(instr)] = ret;
}

void CPU_SLTU(uint32_t instr, uint32_t rsVal, uint32_t rtVal)
{
  Validate(&CPU_reg[rs(instr)], rsVal);
  Validate(&CPU_reg[rt(instr)], rtVal);
  PGXP_value ret = PGXP_value_zero;
  ret.value = static_cast<uint32_t>(rsVal < rtVal);
  ret.x = static_cast<float>(ret.value);
  CPU_reg[rd(instr)] = ret;
}

////////////////////////////////////
// Register mult/div
////////////////////////////////////

void CPU_MULT(uint32_t instr, uint32_t rsVal, uint32_t rtVal)
{
  // Hi/Lo = Rs * Rt (signed)
  Validate(&CPU_reg[rs(instr)], rsVal);
  Validate(&CPU_reg[rt(instr)], rtVal);

  // iCB: Only require one valid input
  if (((CPU_reg[rt(instr)].flags & VALID_01) != VALID_01) != ((CPU_reg[rs(instr)].flags & VALID_01) != VALID_01))
  {
    MakeValid(&CPU_reg[rs(instr)], rsVal);
    MakeValid(&CPU_reg[rt(instr)], rtVal);
  }

  CPU_Lo = CPU_Hi = CPU_reg[rs(instr)];
  TaintZ(CPU_Lo);
  TaintZ(CPU_Hi);

  CPU_Lo.halfFlags[0] = CPU_Hi.halfFlags[0] = (CPU_reg[rs(instr)].halfFlags[0] & CPU_reg[rt(instr)].halfFlags[0]);

  double xx, xy, yx, yy;
  double lx = 0, ly = 0, hx = 0, hy = 0;

  // Multiply out components
  xx = f16Unsign(CPU_reg[rs(instr)].x) * f16Unsign(CPU_reg[rt(instr)].x);
  xy = f16Unsign(CPU_reg[rs(instr)].x) * (CPU_reg[rt(instr)].y);
  yx = (CPU_reg[rs(instr)].y) * f16Unsign(CPU_reg[rt(instr)].x);
  yy = (CPU_reg[rs(instr)].y) * (CPU_reg[rt(instr)].y);

  // Split values into outputs
  lx = xx;

  ly = f16Overflow(xx);
  ly += xy + yx;

  hx = f16Overflow(ly);
  hx += yy;

  hy = f16Overflow(hx);

  CPU_Lo.x = (float)f16Sign(lx);
  CPU_Lo.y = (float)f16Sign(ly);
  CPU_Hi.x = (float)f16Sign(hx);
  CPU_Hi.y = (float)f16Sign(hy);

  // compute PSX value (signed 32x32 -> 64 multiply matching MIPS MULT)
  const uint64_t result = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(rsVal)) * static_cast<int64_t>(static_cast<int32_t>(rtVal)));
  CPU_Hi.value = static_cast<uint32_t>(result >> 32);
  CPU_Lo.value = static_cast<uint32_t>(result);
}

void CPU_MULTU(uint32_t instr, uint32_t rsVal, uint32_t rtVal)
{
  // Hi/Lo = Rs * Rt (unsigned)
  Validate(&CPU_reg[rs(instr)], rsVal);
  Validate(&CPU_reg[rt(instr)], rtVal);

  // iCB: Only require one valid input
  if (((CPU_reg[rt(instr)].flags & VALID_01) != VALID_01) != ((CPU_reg[rs(instr)].flags & VALID_01) != VALID_01))
  {
    MakeValid(&CPU_reg[rs(instr)], rsVal);
    MakeValid(&CPU_reg[rt(instr)], rtVal);
  }

  CPU_Lo = CPU_Hi = CPU_reg[rs(instr)];
  TaintZ(CPU_Lo);
  TaintZ(CPU_Hi);

  CPU_Lo.halfFlags[0] = CPU_Hi.halfFlags[0] = (CPU_reg[rs(instr)].halfFlags[0] & CPU_reg[rt(instr)].halfFlags[0]);

  double xx, xy, yx, yy;
  double lx = 0, ly = 0, hx = 0, hy = 0;

  // Multiply out components
  xx = f16Unsign(CPU_reg[rs(instr)].x) * f16Unsign(CPU_reg[rt(instr)].x);
  xy = f16Unsign(CPU_reg[rs(instr)].x) * f16Unsign(CPU_reg[rt(instr)].y);
  yx = f16Unsign(CPU_reg[rs(instr)].y) * f16Unsign(CPU_reg[rt(instr)].x);
  yy = f16Unsign(CPU_reg[rs(instr)].y) * f16Unsign(CPU_reg[rt(instr)].y);

  // Split values into outputs
  lx = xx;

  ly = f16Overflow(xx);
  ly += xy + yx;

  hx = f16Overflow(ly);
  hx += yy;

  hy = f16Overflow(hx);

  CPU_Lo.x = (float)f16Sign(lx);
  CPU_Lo.y = (float)f16Sign(ly);
  CPU_Hi.x = (float)f16Sign(hx);
  CPU_Hi.y = (float)f16Sign(hy);

  // compute PSX value
  const uint64_t result = static_cast<uint64_t>(rsVal) * static_cast<uint64_t>(rtVal);
  CPU_Hi.value = static_cast<uint32_t>(result >> 32);
  CPU_Lo.value = static_cast<uint32_t>(result);
}

static PGXP_value IntegerValue(uint32_t raw)
{
  return {static_cast<float>(static_cast<int16_t>(raw)),
          static_cast<float>(static_cast<int16_t>(raw >> 16)), 0.f, {VALID_01}, raw};
}

static bool IsIntegerValue(const PGXP_value& value)
{
  return (value.flags & (VALID_01 | VALID_Z)) == VALID_01 &&
         value.x == static_cast<float>(static_cast<int16_t>(value.value)) &&
         value.y == static_cast<float>(static_cast<int16_t>(value.value >> 16));
}

static void CPU_DIVIDE(uint32_t instr, uint32_t numerator, uint32_t denominator, bool sign)
{
  Validate(&CPU_reg[rs(instr)], numerator);
  Validate(&CPU_reg[rt(instr)], denominator);
  PGXP_value num = CPU_reg[rs(instr)];
  PGXP_value denom = CPU_reg[rt(instr)];
  MakeValid(&num, numerator);
  MakeValid(&denom, denominator);

  const bool overflow = sign && numerator == 0x80000000u && denominator == 0xffffffffu;
  uint32_t quotient, remainder;
  if (denominator == 0)
  {
    quotient = sign && static_cast<int32_t>(numerator) < 0 ? 1u : 0xffffffffu;
    remainder = numerator;
  }
  else if (overflow)
  {
    quotient = numerator;
    remainder = 0;
  }
  else if (sign)
  {
    quotient = static_cast<uint32_t>(static_cast<int32_t>(numerator) / static_cast<int32_t>(denominator));
    remainder = static_cast<uint32_t>(static_cast<int32_t>(numerator) % static_cast<int32_t>(denominator));
  }
  else
  {
    quotient = numerator / denominator;
    remainder = numerator % denominator;
  }
  CPU_Lo = IntegerValue(quotient);
  CPU_Hi = IntegerValue(remainder);

  if (overflow || !std::isfinite(num.x) || !std::isfinite(num.y) ||
      !std::isfinite(denom.x) || !std::isfinite(denom.y) || (IsIntegerValue(num) && IsIntegerValue(denom)))
    return;
  // Exceptional results are defined by the integer hardware. HI retains the
  // unchanged dividend on division by zero; LO does not inherit its depth.
  if (denominator == 0)
  {
    CPU_Hi = num;
    return;
  }
  if (denominator == 1 && IsIntegerValue(denom))
  {
    CPU_Lo = num;
    return;
  }
  if (sign && denominator == 0xffffffffu && IsIntegerValue(denom))
  {
    CPU_Lo.x = static_cast<float>(f16Sign(-double(num.x)));
    CPU_Lo.y = static_cast<float>(f16Sign(-double(num.y) - ((numerator & 0xffffu) != 0 ? 1.0 : 0.0)));
    CopyZState(CPU_Lo, num);
    TaintZ(CPU_Lo);
    return;
  }

  // Unsigned conversion follows the native sign bits, including fractional
  // components that cross zero without changing their associated integer.
  const double n = double(num.x) + ((numerator & 0x8000u) ? 65536.0 : 0.0) +
    (double(num.y) + (!sign && (numerator & 0x80000000u) ? 65536.0 : 0.0)) * 65536.0;
  const double d = double(denom.x) + ((denominator & 0x8000u) ? 65536.0 : 0.0) +
    (double(denom.y) + (!sign && (denominator & 0x80000000u) ? 65536.0 : 0.0)) * 65536.0;
  if (d == 0.0 || !std::isfinite(n) || !std::isfinite(d))
    return;
  const double lo = n / d;
  const double hi = std::fmod(n, d);
  // A precision divisor near zero can produce an unbounded quotient while
  // the real CPU result remains a finite 32-bit value. Keep that real result.
  if (!std::isfinite(lo) || !std::isfinite(hi) || std::abs(lo) >= 4294967296.0)
    return;
  CPU_Lo.x = static_cast<float>(f16Sign(lo));
  CPU_Lo.y = static_cast<float>(f16Sign(f16Overflow(lo)));
  CPU_Hi.x = static_cast<float>(f16Sign(hi));
  CPU_Hi.y = static_cast<float>(f16Sign(f16Overflow(hi)));
  CopyZState(CPU_Lo, num);
  CopyZState(CPU_Hi, num);
  TaintZ(CPU_Lo);
  TaintZ(CPU_Hi);
}

void CPU_DIV(uint32_t instr, uint32_t rsVal, uint32_t rtVal)
{
  CPU_DIVIDE(instr, rsVal, rtVal, true);
}

void CPU_DIVU(uint32_t instr, uint32_t rsVal, uint32_t rtVal)
{
  CPU_DIVIDE(instr, rsVal, rtVal, false);
}

////////////////////////////////////
// Shift operations
////////////////////////////////////
enum class ShiftOperation { Left, LogicalRight, ArithmeticRight };

static void CPU_SHIFT(uint32_t instr, uint32_t rtVal, uint32_t shift, ShiftOperation operation)
{
  Validate(&CPU_reg[rt(instr)], rtVal);
  const PGXP_value source = CPU_reg[rt(instr)];
  if (shift == 0)
  {
    CPU_reg[rd(instr)] = source;
    CPU_reg[rd(instr)].value = rtVal;
    return;
  }

  const bool left = operation == ShiftOperation::Left;
  const bool arithmetic = operation == ShiftOperation::ArithmeticRight;
  PGXP_value result = PGXP_value_invalid;
  result.value = left ? rtVal << shift : arithmetic ?
    static_cast<uint32_t>(static_cast<int32_t>(rtVal) >> shift) : rtVal >> shift;
  result.x = static_cast<float>(static_cast<int16_t>(static_cast<uint16_t>(result.value)));
  result.y = static_cast<float>(static_cast<int16_t>(static_cast<uint16_t>(result.value >> 16)));

  const bool valid_x = (source.flags & VALID_0) != 0 && std::isfinite(source.x);
  const bool valid_y = (source.flags & VALID_1) != 0 && std::isfinite(source.y);
  // Use the native sign bit for unsigned conversion. A subpixel coordinate
  // may cross zero while its associated integer component still equals zero.
  const double unsigned_x = double(source.x) + ((rtVal & 0x8000u) ? 65536.0 : 0.0);
  const double unsigned_y = double(source.y) + ((rtVal & 0x80000000u) ? 65536.0 : 0.0);
  bool retained_precision = false;
  if (shift < 16)
  {
    const double scale = double(uint32_t(1) << shift);
    if (valid_x)
    {
      // Bits crossing the halfword boundary are integer bits. Multiplying
      // fractional Y into X would manufacture a large unrelated coordinate.
      const uint32_t carry = (rtVal >> 16) & ((uint32_t(1) << shift) - 1u);
      result.x = static_cast<float>(f16Sign(left ? unsigned_x * scale :
        unsigned_x / scale + double(carry) * double(uint32_t(1) << (16 - shift))));
      result.flags |= VALID_0;
      retained_precision = true;
    }
    if (valid_y)
    {
      const uint32_t carry = (rtVal & 0xffffu) >> (16 - shift);
      result.y = static_cast<float>(f16Sign(left ? double(source.y) * scale + carry :
        (arithmetic ? double(source.y) : unsigned_y) / scale));
      result.flags |= VALID_1;
      retained_precision = true;
    }
  }
  else if (left)
  {
    result.x = 0.f;
    result.flags |= VALID_0;
    if (valid_x)
    {
      result.y = static_cast<float>(f16Sign(double(source.x) * double(uint32_t(1) << (shift - 16))));
      result.flags |= VALID_1;
      retained_precision = true;
    }
  }
  else
  {
    // The upper result contains only zero or sign bits. The low component
    // now comes entirely from the original high half, including its fraction.
    result.flags |= VALID_1;
    if (valid_y)
    {
      result.x = static_cast<float>(f16Sign((arithmetic ? double(source.y) : unsigned_y) /
        double(uint32_t(1) << (shift - 16))));
      result.flags |= VALID_0;
      retained_precision = true;
    }
  }
  if (retained_precision)
  {
    CopyZState(result, source);
    TaintZ(result);
  }
  CPU_reg[rd(instr)] = result;
}

void CPU_SLL(uint32_t instr, uint32_t rtVal)
{
  CPU_SHIFT(instr, rtVal, sa(instr), ShiftOperation::Left);
}

void CPU_SRL(uint32_t instr, uint32_t rtVal)
{
  CPU_SHIFT(instr, rtVal, sa(instr), ShiftOperation::LogicalRight);
}

void CPU_SRA(uint32_t instr, uint32_t rtVal)
{
  CPU_SHIFT(instr, rtVal, sa(instr), ShiftOperation::ArithmeticRight);
}

void CPU_SLLV(uint32_t instr, uint32_t rtVal, uint32_t rsVal)
{
  Validate(&CPU_reg[rs(instr)], rsVal);
  CPU_SHIFT(instr, rtVal, rsVal & 31u, ShiftOperation::Left);
}

void CPU_SRLV(uint32_t instr, uint32_t rtVal, uint32_t rsVal)
{
  Validate(&CPU_reg[rs(instr)], rsVal);
  CPU_SHIFT(instr, rtVal, rsVal & 31u, ShiftOperation::LogicalRight);
}

void CPU_SRAV(uint32_t instr, uint32_t rtVal, uint32_t rsVal)
{
  Validate(&CPU_reg[rs(instr)], rsVal);
  CPU_SHIFT(instr, rtVal, rsVal & 31u, ShiftOperation::ArithmeticRight);
}

void CPU_MFHI(uint32_t instr, uint32_t hiVal)
{
  // Rd = Hi
  Validate(&CPU_Hi, hiVal);

  CPU_reg[rd(instr)] = CPU_Hi;
}

void CPU_MTHI(uint32_t instr, uint32_t rsVal)
{
  Validate(&CPU_reg[rs(instr)], rsVal);

  CPU_Hi = CPU_reg[rs(instr)];
}

void CPU_MFLO(uint32_t instr, uint32_t loVal)
{
  // Rd = Lo
  Validate(&CPU_Lo, loVal);

  CPU_reg[rd(instr)] = CPU_Lo;
}

void CPU_MTLO(uint32_t instr, uint32_t rsVal)
{
  Validate(&CPU_reg[rs(instr)], rsVal);

  CPU_Lo = CPU_reg[rs(instr)];
}

void CPU_MFC0(uint32_t instr, uint32_t rdVal)
{
  // CPU[Rt] = CP0[Rd]
  Validate(&CP0_reg[rd(instr)], rdVal);
  CPU_reg[rt(instr)] = CP0_reg[rd(instr)];
  CPU_reg[rt(instr)].value = rdVal;
}

void CPU_MTC0(uint32_t instr, uint32_t rdVal, uint32_t rtVal)
{
  // CP0[Rd] = CPU[Rt]
  Validate(&CPU_reg[rt(instr)], rtVal);
  CP0_reg[rd(instr)] = CPU_reg[rt(instr)];
  CP0_reg[rd(instr)].value = rdVal;
}

} // namespace PGXP
