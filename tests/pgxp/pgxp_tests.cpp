// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/cpu/pgxp.cpp"
#include "core/system.h"
#include "core/gte.h"
#include <limits>
#include <vector>

// A standalone fixture for the production precision tracker. Instruction
// execution and rendering integration are exercised by the Android tests.
Settings::Settings() = default;
Settings g_settings;
namespace CPU { State g_state; }
namespace Bus {
uint32_t g_ram_size = RAM_2MB_SIZE;
uint32_t g_ram_mask = RAM_2MB_MASK;
}

static int failures = 0;
static int checks = 0;
static void Check(bool condition, const char* name)
{
  std::printf("%s %s\n", condition ? "PASS" : "FAIL", name);
  failures += !condition;
  checks++;
}

static uint32_t I(uint32_t opcode, uint32_t source, uint32_t target, uint16_t immediate = 0)
{
  return (opcode << 26) | (source << 21) | (target << 16) | immediate;
}

static uint32_t R(uint32_t source, uint32_t target, uint32_t dest, uint32_t shift = 0)
{
  return (source << 21) | (target << 16) | (dest << 11) | (shift << 6);
}

static uint32_t XY(int16_t x, int16_t y)
{
  return uint16_t(x) | (uint32_t(uint16_t(y)) << 16);
}

static void Seed(uint32_t address, uint32_t raw = XY(10, 20), float x = 10.25f, float y = 20.5f,
                 float z = 1000.f)
{
  PGXP::GTE_PushSXYZ2f(x, y, z, raw);
  PGXP::CPU_SWC2(I(0x3a, 0, 14), raw, address);
}

static bool Vertex(uint32_t address, uint32_t raw, float expected_x, float expected_y, bool depth = true)
{
  float x, y, w;
  const bool valid = PGXP::GetPreciseVertex(address, raw, int16_t(raw), int16_t(raw >> 16), 0, 0, &x, &y, &w);
  return valid == depth && std::abs(x - expected_x) < 0.0001f && std::abs(y - expected_y) < 0.0001f &&
         std::isfinite(w) && w > 0.f;
}

static void CheckShifts()
{
  using namespace PGXP;
  // Independent expectations follow raw carry bits and the component that
  // survives the shift. The discarded half must not supply fractional carry.
  for (uint32_t operation = 0; operation < 6; operation++)
  {
    const bool left = operation % 3 == 0;
    const bool arithmetic = operation % 3 == 2;
    for (uint32_t shift = 0; shift < 32; shift++)
    {
      Reset();
      Seed(0x100, XY(-2, -20), -1.25f, -19.5f);
      CPU_LW(I(0x23, 0, 1), XY(-2, -20), 0x100);
      CPU_ADDI(I(9, 0, 3, shift + 32), 0);
      const uint32_t instruction = R(3, 1, 2, shift);
      const uint32_t raw = XY(-2, -20);
      switch (operation)
      {
        case 0: CPU_SLL(instruction, raw); break;
        case 1: CPU_SRL(instruction, raw); break;
        case 2: CPU_SRA(instruction, raw); break;
        case 3: CPU_SLLV(instruction, raw, shift + 32); break;
        case 4: CPU_SRLV(instruction, raw, shift + 32); break;
        case 5: CPU_SRAV(instruction, raw, shift + 32); break;
      }
      const uint32_t expected_raw = left ? raw << shift : arithmetic ?
        static_cast<uint32_t>(static_cast<int32_t>(raw) >> shift) : raw >> shift;
      auto wrap = [](double value) {
        value = std::fmod(value, 65536.0);
        if (value >= 32768.0) value -= 65536.0;
        if (value < -32768.0) value += 65536.0;
        return static_cast<float>(value);
      };
      float x = -1.25f, y = -19.5f;
      if (shift > 0 && left)
      {
        if (shift < 16)
        {
          x = wrap(65534.75 * double(uint32_t(1) << shift));
          y = wrap(-19.5 * double(uint32_t(1) << shift) + ((raw & 65535u) >> (16 - shift)));
        }
        else
        {
          x = 0.f;
          y = wrap(-1.25 * double(uint32_t(1) << (shift - 16)));
        }
      }
      else if (shift > 0)
      {
        if (shift < 16)
        {
          x = wrap(65534.75 / double(uint32_t(1) << shift) +
            double((raw >> 16) & ((uint32_t(1) << shift) - 1)) * double(uint32_t(1) << (16 - shift)));
          y = wrap((arithmetic ? -19.5 : 65516.5) / double(uint32_t(1) << shift));
        }
        else
        {
          x = shift == 16 ? -19.5f : wrap((arithmetic ? -19.5 : 65516.5) / double(uint32_t(1) << (shift - 16)));
          y = arithmetic ? -1.f : 0.f;
        }
      }
      char name[100];
      std::snprintf(name, sizeof(name), "shift operation %u amount %u preserves components and raw carry", operation, shift);
      Check(CPU_reg[2].value == expected_raw && (CPU_reg[2].flags & VALID_01) == VALID_01 &&
            std::abs(CPU_reg[2].x - x) < 0.0001f && std::abs(CPU_reg[2].y - y) < 0.0001f, name);
    }
  }

  Reset();
  Seed(0x100, 0, 0.25f, 0.5f);
  CPU_LW(I(0x23, 0, 1), 0, 0x100);
  CPU_SRL(R(0, 1, 2, 1), 0);
  Check(CPU_reg[2].x == 0.125f && CPU_reg[2].y == 0.25f,
        "right shift retains zero-valued fractional geometry");
  CPU_SRA(R(0, 1, 2, 16), 0);
  Check(CPU_reg[2].x == 0.5f && CPU_reg[2].y == 0.f,
        "arithmetic half extraction retains a fractional zero");
  CPU_reg[1].flags = VALID_0 | VALID_Z;
  CPU_SLL(R(0, 1, 2, 16), 0);
  Check((CPU_reg[2].flags & VALID_01) == VALID_01 && CPU_reg[2].x == 0.f && CPU_reg[2].y == 0.25f,
        "left half packing transfers low precision validity to the high half");
  CPU_reg[1].flags = VALID_1 | VALID_Z;
  CPU_SRL(R(0, 1, 2, 16), 0);
  Check((CPU_reg[2].flags & VALID_01) == VALID_01 && CPU_reg[2].x == 0.5f && CPU_reg[2].y == 0.f,
        "right half extraction transfers high precision validity to the low half");
  CPU_SLL(R(0, 1, 2, 16), 0);
  Check((CPU_reg[2].flags & VALID_1) == 0 && (CPU_reg[2].flags & VALID_Z) == 0,
        "discarding the only precise half cannot invent geometry or depth");
  Seed(0x100, XY(0, 20), -0.25f, 20.5f);
  CPU_LW(I(0x23, 0, 1), XY(0, 20), 0x100);
  CPU_SRL(R(0, 1, 2, 1), XY(0, 20));
  Check(CPU_reg[2].x == -0.125f && CPU_reg[2].y == 10.25f,
        "fractional sign crossing cannot create a spurious unsigned carry");
}

int main()
{
  using namespace PGXP;
  g_settings.gpu_pgxp_enable = true;
  Initialize();
  CheckShifts();
  Reset();
  Seed(0x100);
  Check(Vertex(0x100, XY(10, 20), 10.25f, 20.5f), "GTE to RAM preserves subpixel coordinates and depth");
  Check(Vertex(0x80000100, XY(10, 20), 10.25f, 20.5f), "cached RAM alias shares precision");
  Check(Vertex(0xa0200100, XY(10, 20), 10.25f, 20.5f), "2 MB RAM mirror shares precision");
  Seed(0x1f800100);
  Check(Vertex(0x1f800100, XY(10, 20), 10.25f, 20.5f), "scratchpad shares the precision pipeline");

  CPU_ADDI(I(9, 0, 1, uint16_t(-1)), 0);
  Check(CPU_reg[1].x == -1.f && CPU_reg[1].y == -1.f && CPU_reg[1].value == 0xffffffffu,
        "negative immediate has a sign extended upper half");
  GTE_PushSXYZ2f(10.25f, 20.5f, 1000.f, XY(10, 20));
  CPU_MFC2(R(0, 0, 14), XY(10, 20), XY(10, 20));
  CPU_ORI(I(0x0d, 0, 2, 7), 0);
  Check(CPU_reg[2].x == 7.f && CPU_reg[2].y == 0.f && (CPU_reg[2].flags & VALID_Z) == 0,
        "writes to r0 cannot leak geometry into constants");

  PGXP_value half{10.25f, 0.f, 1000.f, {VALID_0 | VALID_Z}, XY(10, 20)};
  MakeValid(&half, XY(10, 20));
  Check(half.x == 10.25f && half.y == 20.f, "reconstructing an unknown half preserves the precise half");
  CPU_LW(I(0x23, 0, 1), XY(10, 20), 0x100);
  CPU_SLL(R(0, 1, 2, 0), XY(10, 20));
  Check(CPU_reg[2].flags == CPU_reg[1].flags && CPU_reg[2].x == 10.25f,
        "zero left shift is an exact move");

  CPU_ADDI(I(9, 0, 2, 3), 0);
  CPU_ADD(R(2, 1, 3), 3, XY(10, 20));
  CPU_SW(I(0x2b, 0, 3), XY(13, 20), 0x110);
  Check(Vertex(0x110, XY(13, 20), 13.25f, 20.5f), "adding a constant retains vertex depth from either operand");

  Seed(0x100, XY(-2, 20), -1.25f, 20.5f);
  CPU_LW(I(0x23, 0, 1), XY(-2, 20), 0x100);
  CPU_ADDI(I(9, 1, 2, 1), XY(-2, 20));
  CPU_SW(I(0x2b, 0, 2), XY(-1, 20), 0x110);
  Check(Vertex(0x110, XY(-1, 20), -0.25f, 20.5f), "fractional immediate arithmetic cannot carry into packed Y early");
  CPU_ADDI(I(9, 0, 3, 1), 0);
  CPU_ADD(R(1, 3, 2), XY(-2, 20), 1);
  CPU_SW(I(0x2b, 0, 2), XY(-1, 20), 0x110);
  Check(Vertex(0x110, XY(-1, 20), -0.25f, 20.5f), "fractional register arithmetic cannot carry into packed Y early");

  Seed(0x100, XY(0, 0), 0.25f, 0.5f);
  CPU_LW(I(0x23, 0, 1), 0, 0x100);
  CPU_OR_(R(1, 0, 2), 0, 0);
  CPU_SW(I(0x2b, 0, 2), 0, 0x110);
  Check(Vertex(0x110, 0, 0.25f, 0.5f), "OR with zero preserves fractions of a zero-valued vertex");
  CPU_XOR_(R(1, 0, 2), 0, 0);
  CPU_SW(I(0x2b, 0, 2), 0, 0x110);
  Check(Vertex(0x110, 0, 0.25f, 0.5f), "XOR with zero preserves fractions of a zero-valued vertex");
  CPU_AND_(R(1, 0, 2), 0, 0);
  CPU_SW(I(0x2b, 0, 2), 0, 0x110);
  Check(Vertex(0x110, 0, 0.f, 0.f, false), "AND with zero produces a constant without vertex depth");
  CPU_ANDI(I(0x0c, 1, 2), 0);
  CPU_SW(I(0x2b, 0, 2), 0, 0x110);
  Check(Vertex(0x110, 0, 0.f, 0.f, false), "ANDI with zero does not manufacture a 3D vertex");

  Seed(0x100);
  CPU_LW(I(0x23, 0, 1), XY(10, 20), 0x100);
  CPU_ORI(I(0x0d, 1, 2, 2), XY(10, 20));
  CPU_SW(I(0x2b, 0, 2), XY(10, 20), 0x110);
  Check(Vertex(0x110, XY(10, 20), 10.25f, 20.5f), "redundant ORI bits preserve subpixel geometry");
  CPU_SLTI(I(0x0a, 1, 2, 100), XY(10, 20));
  Check(CPU_reg[2].x == 0.f && CPU_reg[2].value == 0 && (CPU_reg[2].flags & VALID_Z) == 0,
        "SLTI compares the full register and drops vertex depth");
  CPU_SLTIU(I(0x0b, 1, 2, 0xffff), XY(10, 20));
  Check(CPU_reg[2].x == 1.f && CPU_reg[2].value == 1 && (CPU_reg[2].flags & VALID_Z) == 0,
        "SLTIU sign extends the immediate before unsigned comparison");
  Seed(0x120, XY(20, 10), 20.25f, 10.5f);
  CPU_LW(I(0x23, 0, 2), XY(20, 10), 0x120);
  CPU_SLT(R(1, 2, 3), XY(10, 20), XY(20, 10));
  Check(CPU_reg[3].x == 0.f && CPU_reg[3].value == 0 && (CPU_reg[3].flags & VALID_Z) == 0,
        "SLT compares the full word without treating halves as coordinates");
  CPU_SLTU(R(1, 2, 3), XY(10, 20), XY(20, 10));
  Check(CPU_reg[3].x == 0.f && CPU_reg[3].value == 0 && (CPU_reg[3].flags & VALID_Z) == 0,
        "SLTU produces a constant without borrowed vertex depth");

  Seed(0x100, XY(10, 0), 10.25f, 0.f, 1000.f);
  Seed(0x120, XY(0, 20), 0.f, 20.5f, 2000.f);
  CPU_LW(I(0x23, 0, 1), XY(10, 0), 0x100);
  CPU_LW(I(0x23, 0, 2), XY(0, 20), 0x120);
  CPU_OR_(R(1, 2, 3), XY(10, 0), XY(0, 20));
  CPU_SW(I(0x2b, 0, 3), XY(10, 20), 0x110);
  Check(Vertex(0x110, XY(10, 20), 10.25f, 20.5f, false), "packing halves from different depths keeps geometry without inventing perspective");

  // Cover every byte offset and both directions. Raw CPU results are computed
  // from the ISA merge masks, separately from the tracker under test.
  for (uint32_t opcode : {0x22u, 0x26u})
  {
    for (uint32_t offset = 0; offset < 4; offset++)
    {
      Reset();
      const uint32_t raw = XY(10, 20);
      Seed(0x100);
      CPU_ADDI(I(9, 0, 1, 0), 0);
      const uint32_t merged = opcode == 0x22 ? raw << (24 - offset * 8) : raw >> (offset * 8);
      CPU_LW(I(opcode, 0, 1), merged, 0x100 + offset);
      CPU_SW(I(0x2b, 0, 1), merged, 0x200);
      char name[100];
      std::snprintf(name, sizeof(name), "%s byte offset %u preserves only whole source halves", opcode == 0x22 ? "LWL" : "LWR", offset);
      const bool full = opcode == 0x22 ? offset == 3 : offset == 0;
      const bool halfword = opcode == 0x22 ? offset == 1 : offset == 2;
      const auto& v = *GetPtr(0x200);
      Check(full ? Vertex(0x200, merged, 10.25f, 20.5f) :
            (halfword ? (opcode == 0x22 ? v.y == 10.25f : v.x == 20.5f) : (v.flags & VALID_01) != VALID_01), name);
    }
  }

  Reset();
  Seed(0x100);
  // Pretend DMA has replaced the word, bypassing the CPU precision hooks.
  CPU_LW(I(0x26, 0, 1), XY(30, 40), 0x100);
  CPU_SW(I(0x2b, 0, 1), XY(30, 40), 0x200);
  Check(Vertex(0x200, XY(30, 40), 30.f, 40.f, false), "merged load rejects stale DMA precision");
  Reset();
  Seed(0x100);
  CPU_LW(I(0x23, 0, 1), XY(10, 20), 0x100);
  CPU_SW(I(0x2e, 0, 1), XY(30, 40), 0x200);
  Check(Vertex(0x200, XY(30, 40), 30.f, 40.f, false), "merged store rejects stale source register precision");

  for (float z : {0.f, -1.f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
  {
    Reset();
    Seed(0x100, XY(10, 20), 10.25f, 20.5f, z);
    Check(Vertex(0x100, XY(10, 20), 10.25f, 20.5f, false), "invalid depth preserves geometry with safe affine fallback");
  }
  Reset();
  Seed(0x100, XY(10, 20), std::numeric_limits<float>::quiet_NaN());
  Check(Vertex(0x100, XY(10, 20), 10.f, 20.f, false), "nonfinite geometry falls back before integer conversion");
  g_settings.SetPGXPDepthClearThreshold(300.f);
  Check(std::abs(g_settings.gpu_pgxp_depth_clear_threshold - 300.f / 65535.f) < 0.000001f,
        "depth clear threshold matches normalized GTE depth");

  Reset();
  g_settings.gpu_pgxp_vertex_cache = true;
  Initialize();
  Seed(0x100);
  auto stream = ByteStream_CreateGrowableMemoryStream();
  StateWrapper writer(stream.get(), StateWrapper::Mode::Write, 56);
  Check(DoState(writer), "write a persistent precision snapshot");
  Check(Vertex(0x1f000000, XY(10, 20), 10.25f, 20.5f, false), "saving does not invalidate the live vertex cache");
  Reset();
  stream->SeekAbsolute(0);
  StateWrapper reader(stream.get(), StateWrapper::Mode::Read, 56);
  Check(DoState(reader) && Vertex(0x100, XY(10, 20), 10.25f, 20.5f), "load restores RAM precision from version 56");
  Check(Vertex(0x1f000000, XY(10, 20), 10.f, 20.f, false), "load clears derived vertices from the abandoned timeline");
  Check(stream->GetSize() + 5 * 1024 * 1024 < System::MAX_SAVE_STATE_SIZE,
        "2 MB persistent PGXP snapshot fits the libretro state bound");
  Shutdown();
  Bus::g_ram_size = Bus::RAM_8MB_SIZE;
  Bus::g_ram_mask = Bus::RAM_8MB_MASK;
  Initialize();
  Seed(0x700100);
  Seed(0x1f800100, XY(30, 40), 30.25f, 40.5f);
  auto large_stream = ByteStream_CreateGrowableMemoryStream();
  StateWrapper large_writer(large_stream.get(), StateWrapper::Mode::Write, 56);
  Check(DoState(large_writer) && large_stream->GetSize() + 11 * 1024 * 1024 < System::MAX_SAVE_STATE_SIZE,
        "8 MB persistent PGXP snapshot fits the libretro state bound");
  Check(Vertex(0x700100, XY(10, 20), 10.25f, 20.5f) && Vertex(0x1f800100, XY(30, 40), 30.25f, 40.5f),
        "8 MB RAM precision does not overlap the scratchpad");

  // Drive the real GTE with an identity rotation and a deliberately
  // fractional projection. LM saturation must agree with native IR values.
  g_settings.gpu_pgxp_vertex_cache = false;
  g_settings.gpu_pgxp_preserve_proj_fp = true;
  for (bool limit : {false, true})
  {
    GTE::Reset();
    GTE::WriteRegister(32, 4096);
    GTE::WriteRegister(34, 4096);
    GTE::WriteRegister(36, 4096);
    GTE::WriteRegister(58, 100);
    GTE::WriteRegister(0, XY(-100, 50));
    GTE::WriteRegister(1, 301);
    GTE::ExecuteInstruction(0x80001u | (limit ? 0x400u : 0u));
    const uint32_t raw = GTE::ReadRegister(14);
    CPU_SWC2(I(0x3a, 0, 14), raw, 0x100);
    Check(Vertex(0x100, raw, limit ? 0.f : -10000.f / 301.f, 5000.f / 301.f),
          limit ? "projection precision obeys LM zero saturation" : "projection precision keeps fractional negative coordinates");
  }
  GTE::Reset();
  GTE::ExecuteInstruction(0x80001u);
  const uint32_t zero_projection = GTE::ReadRegister(14);
  CPU_SWC2(I(0x3a, 0, 14), zero_projection, 0x100);
  Check(Vertex(0x100, zero_projection, 0.f, 0.f, false), "zero GTE projection distance cannot inject NaNs");

  Reset();
  GTE_PushSXYZ2f(1000.f, 1000.f, 1000.f, XY(1000, 1000));
  GTE_PushSXYZ2f(1000.01f, 1000.f, 1000.f, XY(1000, 1000));
  GTE_PushSXYZ2f(1000.f, 1000.01f, 1000.f, XY(1000, 1000));
  Check(GTE_NCLIP_valid(XY(1000, 1000), XY(1000, 1000), XY(1000, 1000)) && GTE_NCLIP() >= 1.f,
        "subpixel triangle keeps its front facing sign near the screen edge");
  CPU_LUI(I(0x0f, 0, 1, 20));
  CPU_ORI(I(0x0d, 1, 1, 10), XY(0, 20));
  CPU_MTC2(R(0, 1, 14), XY(10, 20), XY(10, 20));
  CPU_SWC2(I(0x3a, 0, 15), XY(10, 20), 0x100);
  Check(Vertex(0x100, XY(10, 20), 10.f, 20.f, false), "direct write to SXY2 also updates its SXYP mirror");
  Shutdown();
  std::printf("PGXP: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
