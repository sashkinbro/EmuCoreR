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

int main()
{
  using namespace PGXP;
  g_settings.gpu_pgxp_enable = true;
  Initialize();
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
