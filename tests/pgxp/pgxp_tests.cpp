// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/cpu/pgxp.cpp"
#include "core/system.h"
#include "core/gte.h"
#include <limits>
#include <chrono>
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

static void CheckMemorySnapshots()
{
  using namespace PGXP;
  Reset();
  const std::vector<uint32_t> addresses = {0u, 0xfcu, 0x100u, Bus::g_ram_size - 4u, 0x1f800000u, 0x1f8003fcu};
  for (uint32_t address : addresses)
    Seed(address);
  // Fill a complete bitmap page, including its highest bit. Also exercise
  // partial component validity and a tracked depth without known XY.
  for (uint32_t entry = 128; entry < 192; entry++)
    Mem[entry] = {float(entry) + 0.25f, 20.5f, 1000.f, {VALID_012}, XY(int16_t(entry), 20)};
  Mem[128].flags = VALID_0 | VALID_Z | TAINTED_Z;
  Mem[129].flags = VALID_1;
  Mem[130].flags = VALID_Z;
  CPU_LW(I(0x23, 0, 1), XY(10, 20), 0x100);
  CPU_Hi = CPU_Lo = CPU_reg[1];
  CPU_MTC0(R(0, 1, 12), XY(10, 20), XY(10, 20));
  std::array<PGXP_value, 34> expected_cpu;
  std::array<PGXP_value, 32> expected_cp0, expected_gte, expected_control;
  std::array<PGXP_value, 64> expected_page;
  std::memcpy(expected_cpu.data(), CPU_reg, sizeof(CPU_reg));
  std::memcpy(expected_cp0.data(), CP0_reg, sizeof(CP0_reg));
  std::memcpy(expected_gte.data(), GTE_data_reg, sizeof(GTE_data_reg));
  std::memcpy(expected_control.data(), GTE_ctrl_reg, sizeof(GTE_ctrl_reg));
  std::memcpy(expected_page.data(), Mem + 128, sizeof(expected_page));

  auto snapshot = ByteStream_CreateGrowableMemoryStream();
  StateWrapper writer(snapshot.get(), StateWrapper::Mode::Write, 56);
  const auto start = std::chrono::steady_clock::now();
  Check(DoMemoryState(writer), "write a compact runahead precision snapshot");
  const double elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  Check(snapshot->GetSize() < (Bus::g_ram_size == Bus::RAM_8MB_SIZE ? 512u : 128u) * 1024u,
        "sparse runahead snapshots avoid a full RAM precision copy");
  std::printf("PGXP snapshot: RAM=%u bytes=%llu write_ms=%.3f\n", Bus::g_ram_size,
              static_cast<unsigned long long>(snapshot->GetSize()), elapsed_ms);

  Reset();
  Seed(0x4000, XY(30, 40), 30.25f, 40.5f);
  snapshot->SeekAbsolute(0);
  StateWrapper reader(snapshot.get(), StateWrapper::Mode::Read, 56);
  Check(DoMemoryState(reader), "load a compact runahead precision snapshot");
  Check(std::memcmp(CPU_reg, expected_cpu.data(), sizeof(CPU_reg)) == 0 &&
        std::memcmp(CP0_reg, expected_cp0.data(), sizeof(CP0_reg)) == 0 &&
        std::memcmp(GTE_data_reg, expected_gte.data(), sizeof(GTE_data_reg)) == 0 &&
        std::memcmp(GTE_ctrl_reg, expected_control.data(), sizeof(GTE_ctrl_reg)) == 0,
        "runahead restores CPU, HI/LO, CP0 and GTE precision registers exactly");
  bool restored = true;
  for (uint32_t address : addresses)
    restored &= Vertex(address, XY(10, 20), 10.25f, 20.5f);
  Check(restored, "runahead restores RAM and scratchpad boundary entries");
  Check(std::memcmp(Mem + 128, expected_page.data(), sizeof(expected_page)) == 0,
        "runahead restores full bitmap pages and partial component flags");
  Check(GetPtr(0x4000)->flags == 0, "runahead removes precision written by the abandoned timeline");
  Check(!vertexCache || Vertex(0x1f000000, XY(30, 40), 30.f, 40.f, false),
        "runahead clears derived vertices from the abandoned timeline");
  snapshot->Resize(static_cast<uint32_t>(snapshot->GetSize() - 1));
  snapshot->SeekAbsolute(0);
  StateWrapper truncated(snapshot.get(), StateWrapper::Mode::Read, 56);
  Check(!DoMemoryState(truncated), "truncated runahead precision snapshots fail cleanly");

  auto disabled = ByteStream_CreateGrowableMemoryStream();
  g_settings.gpu_pgxp_enable = false;
  StateWrapper disabled_writer(disabled.get(), StateWrapper::Mode::Write, 56);
  Check(DoMemoryState(disabled_writer) && disabled->GetSize() == 1,
        "disabled PGXP adds only its presence byte to runahead");
  g_settings.gpu_pgxp_enable = true;
  Seed(0x100);
  disabled->SeekAbsolute(0);
  StateWrapper disabled_reader(disabled.get(), StateWrapper::Mode::Read, 56);
  Check(DoMemoryState(disabled_reader) && GetPtr(0x100)->flags == 0,
        "loading a snapshot without PGXP cannot retain stale precision");
}

static void CheckHiLoTransfers()
{
  using namespace PGXP;
  for (uint32_t source : {1u, 7u, 31u})
  {
    Reset();
    Seed(0x100);
    CPU_LW(I(0x23, 0, source), XY(10, 20), 0x100);
    CPU_MTHI(R(source, 0, 0), XY(10, 20));
    CPU_MFHI(R(0, 0, 2), XY(10, 20));
    CPU_SW(I(0x2b, 0, 2), XY(10, 20), 0x200);
    Check(Vertex(0x200, XY(10, 20), 10.25f, 20.5f), "MTHI/MFHI preserve geometry from the encoded source register");
    CPU_MTLO(R(source, 0, 0), XY(10, 20));
    CPU_MFLO(R(0, 0, 2), XY(10, 20));
    CPU_SW(I(0x2b, 0, 2), XY(10, 20), 0x200);
    Check(Vertex(0x200, XY(10, 20), 10.25f, 20.5f), "MTLO/MFLO preserve geometry from the encoded source register");
  }
  Reset();
  CPU_MTHI(R(0, 0, 0), 0);
  CPU_MTLO(R(0, 0, 0), 0);
  Check(CPU_Hi.value == 0 && CPU_Lo.value == 0 && CPU_Hi.flags == VALID_01 && CPU_Lo.flags == VALID_01,
        "writing r0 to HI/LO produces exact constants without depth");
}

static void CheckDivisions()
{
  using namespace PGXP;
  auto constant = [](uint32_t value) {
    return PGXP_value{float(int16_t(value)), float(int16_t(value >> 16)), 0.f, {VALID_01}, value};
  };
  auto exact = [](const PGXP_value& value, uint32_t raw) {
    return value.value == raw && value.x == float(int16_t(raw)) && value.y == float(int16_t(raw >> 16)) &&
           value.flags == VALID_01;
  };
  const uint32_t inputs[] = {0u, 1u, 2u, 3u, 0xffffu, 0x10000u, 0x12345678u,
                             0x7fffffffu, 0x80000000u, 0xfffffffdu, 0xffffffffu};
  for (bool sign : {false, true})
  {
    bool correct = true;
    for (uint32_t numerator : inputs)
    {
      for (uint32_t denominator : inputs)
      {
        CPU_reg[1] = constant(numerator);
        CPU_reg[2] = constant(denominator);
        if (sign) CPU_DIV(R(1, 2, 0), numerator, denominator);
        else CPU_DIVU(R(1, 2, 0), numerator, denominator);
        uint32_t quotient, remainder;
        if (denominator == 0)
        {
          quotient = sign && int32_t(numerator) < 0 ? 1u : 0xffffffffu;
          remainder = numerator;
        }
        else if (sign && numerator == 0x80000000u && denominator == 0xffffffffu)
        {
          quotient = numerator;
          remainder = 0;
        }
        else if (sign)
        {
          quotient = uint32_t(int32_t(numerator) / int32_t(denominator));
          remainder = uint32_t(int32_t(numerator) % int32_t(denominator));
        }
        else
        {
          quotient = numerator / denominator;
          remainder = numerator % denominator;
        }
        correct &= exact(CPU_Lo, quotient) && exact(CPU_Hi, remainder);
      }
    }
    Check(correct, sign ? "signed constant division obeys all integer and exceptional results" :
                          "unsigned constant division obeys all integer and exceptional results");
    Reset();
    Seed(0x100);
    CPU_LW(I(0x23, 0, 1), XY(10, 20), 0x100);
    const PGXP_value original = CPU_reg[1];
    CPU_reg[2] = constant(1);
    if (sign) CPU_DIV(R(1, 2, 0), XY(10, 20), 1);
    else CPU_DIVU(R(1, 2, 0), XY(10, 20), 1);
    Check(std::memcmp(&CPU_Lo, &original, sizeof(original)) == 0 && exact(CPU_Hi, 0),
          "division by one preserves geometry exactly and produces a constant remainder");
    CPU_reg[2] = constant(0);
    if (sign) CPU_DIV(R(1, 2, 0), XY(10, 20), 0);
    else CPU_DIVU(R(1, 2, 0), XY(10, 20), 0);
    Check(exact(CPU_Lo, 0xffffffffu) && std::memcmp(&CPU_Hi, &original, sizeof(original)) == 0,
          "division by zero preserves the dividend in HI and has a finite constant quotient");
    CPU_reg[2] = constant(1);
    CPU_reg[2].x = 1e-30f;
    if (sign) CPU_DIV(R(1, 2, 0), XY(10, 20), 1);
    else CPU_DIVU(R(1, 2, 0), XY(10, 20), 1);
    Check(exact(CPU_Lo, XY(10, 20)) && exact(CPU_Hi, 0),
          "unbounded precision quotient falls back without floating point narrowing");
    CPU_reg[2] = constant(2);
    CPU_reg[2].x = 0.f;
    if (sign) CPU_DIV(R(1, 2, 0), XY(10, 20), 2);
    else CPU_DIVU(R(1, 2, 0), XY(10, 20), 2);
    Check(exact(CPU_Lo, XY(5, 10)) && exact(CPU_Hi, 0),
          "zero precision denominator falls back to the actual CPU result");
    CPU_reg[1] = original;
    CPU_reg[1].x = std::numeric_limits<float>::quiet_NaN();
    CPU_reg[2] = constant(2);
    if (sign) CPU_DIV(R(1, 2, 0), XY(10, 20), 2);
    else CPU_DIVU(R(1, 2, 0), XY(10, 20), 2);
    Check(exact(CPU_Lo, XY(5, 10)) && exact(CPU_Hi, 0),
          "nonfinite precision cannot enter division or retain borrowed depth");
    if (sign)
    {
      CPU_reg[1] = original;
      CPU_reg[2] = constant(0xffffffffu);
      CPU_DIV(R(1, 2, 0), XY(10, 20), 0xffffffffu);
      Check(CPU_Lo.value == uint32_t(-int32_t(XY(10, 20))) && CPU_Lo.x == -10.25f && CPU_Lo.y == -21.5f &&
            (CPU_Lo.flags & VALID_Z) != 0 && exact(CPU_Hi, 0),
            "division by minus one negates packed geometry using native borrow");
      CPU_reg[1] = constant(0x80000000u);
      CPU_reg[1].flags |= VALID_Z;
      CPU_reg[1].z = 1000.f;
      CPU_DIV(R(1, 2, 0), 0x80000000u, 0xffffffffu);
      Check(exact(CPU_Lo, 0x80000000u) && exact(CPU_Hi, 0),
            "signed division overflow produces defined constants without borrowed geometry");
    }
  }
}

static void CheckMultiplication()
{
  using namespace PGXP;
  auto constant = [](uint32_t raw) {
    return PGXP_value{float(int16_t(raw)), float(int16_t(raw >> 16)), 0.f, {VALID_01}, raw};
  };
  auto exact = [](const PGXP_value& value, uint32_t raw) {
    return value.value == raw && value.x == float(int16_t(raw)) && value.y == float(int16_t(raw >> 16)) &&
           value.flags == VALID_01;
  };
  for (bool sign : {false, true})
  {
    bool correct = true;
    for (uint32_t a : {0u, 1u, 2u, 3u, 65535u, 65536u, 0x7fffffffu, 0x80000000u, 0xffffffffu})
    {
      for (uint32_t b : {0u, 1u, 2u, 3u, 65535u, 65536u, 0x7fffffffu, 0x80000000u, 0xffffffffu})
      {
        CPU_reg[1] = constant(a);
        CPU_reg[2] = constant(b);
        const uint64_t product = sign ? uint64_t(int64_t(int32_t(a)) * int64_t(int32_t(b))) : uint64_t(a) * b;
        if (sign) CPU_MULT(R(1, 2, 0), a, b);
        else CPU_MULTU(R(1, 2, 0), a, b);
        correct &= exact(CPU_Lo, uint32_t(product)) && exact(CPU_Hi, uint32_t(product >> 32));
      }
    }
    Check(correct, "constant multiplication agrees with the exact 64-bit integer result");
    for (bool reverse : {false, true})
    {
      Reset();
      Seed(0x100);
      CPU_LW(I(0x23, 0, 1), XY(10, 20), 0x100);
      CPU_reg[2] = constant(2);
      const uint32_t instruction = reverse ? R(2, 1, 0) : R(1, 2, 0);
      const uint32_t a = reverse ? 2 : XY(10, 20), b = reverse ? XY(10, 20) : 2;
      if (sign) CPU_MULT(instruction, a, b);
      else CPU_MULTU(instruction, a, b);
      Check(CPU_Lo.value == XY(20, 40) && CPU_Lo.x == 20.5f && CPU_Lo.y == 41.f &&
            (CPU_Lo.flags & VALID_Z) != 0 && CPU_Lo.z == 1000.f,
            "vertex multiplication retains coordinates and depth in either operand order");
    }
    CPU_reg[2] = constant(1);
    const auto original = CPU_reg[1];
    if (sign) CPU_MULT(R(1, 2, 0), XY(10, 20), 1);
    else CPU_MULTU(R(1, 2, 0), XY(10, 20), 1);
    Check(std::memcmp(&CPU_Lo, &original, sizeof(original)) == 0 && exact(CPU_Hi, 0),
          "multiplication by one is an exact geometry copy with constant HI");
    CPU_reg[2] = constant(0);
    if (sign) CPU_MULT(R(1, 2, 0), XY(10, 20), 0);
    else CPU_MULTU(R(1, 2, 0), XY(10, 20), 0);
    Check(exact(CPU_Lo, 0) && exact(CPU_Hi, 0), "multiplication by zero cannot retain vertex depth");
    Seed(0x100, XY(0, 20), -0.25f, 20.5f);
    CPU_LW(I(0x23, 0, 1), XY(0, 20), 0x100);
    CPU_reg[2] = constant(2);
    if (sign) CPU_MULT(R(1, 2, 0), XY(0, 20), 2);
    else CPU_MULTU(R(1, 2, 0), XY(0, 20), 2);
    Check(CPU_Lo.value == XY(0, 40) && CPU_Lo.x == -0.5f && CPU_Lo.y == 41.f,
          "fractional sign crossing cannot carry into Y during multiplication");
    Seed(0x120, XY(5, 10), 5.25f, 10.5f, 2000.f);
    CPU_LW(I(0x23, 0, 2), XY(5, 10), 0x120);
    if (sign) CPU_MULT(R(1, 2, 0), XY(0, 20), XY(5, 10));
    else CPU_MULTU(R(1, 2, 0), XY(0, 20), XY(5, 10));
    Check((CPU_Lo.flags & VALID_Z) == 0 && (CPU_Hi.flags & VALID_Z) == 0,
          "multiplying different vertices cannot invent a shared perspective depth");
    CPU_reg[1].x = std::numeric_limits<float>::quiet_NaN();
    CPU_reg[2] = constant(2);
    if (sign) CPU_MULT(R(1, 2, 0), XY(0, 20), 2);
    else CPU_MULTU(R(1, 2, 0), XY(0, 20), 2);
    Check(exact(CPU_Lo, XY(0, 40)) && exact(CPU_Hi, 0),
          "nonfinite multiplication metadata falls back to the actual integer result");
  }
}

static void CheckPartialTransfers()
{
  using namespace PGXP;
  Reset();
  Seed(0x100, XY(-10, -20), -9.75f, -19.5f);
  CPU_LHx(I(0x25, 0, 1), uint16_t(-20), 0x102);
  Check(CPU_reg[1].x == -19.5f && CPU_reg[1].y == 0.f && CPU_reg[1].value == uint16_t(-20),
        "LHU uses the real zero extended upper half for a negative coordinate");
  CPU_LHx(I(0x21, 0, 1), uint32_t(-20), 0x102);
  Check(CPU_reg[1].x == -19.5f && CPU_reg[1].y == -1.f && CPU_reg[1].value == uint32_t(-20),
        "LH keeps signed extension while preserving the selected coordinate");
  Seed(0x100, XY(0, 20), -0.25f, 20.5f);
  CPU_LHx(I(0x21, 0, 1), 0, 0x100);
  Check(CPU_reg[1].x == -0.25f && CPU_reg[1].y == 0.f,
        "fractional sign crossing cannot alter the native halfword sign extension");
  Seed(0x100);
  CPU_LHx(I(0x21, 0, 1), 30, 0x100);
  Check(CPU_reg[1].x == 30.f && CPU_reg[1].y == 0.f && (CPU_reg[1].flags & VALID_Z) == 0,
        "halfword loads reject stale DMA precision without borrowing the other half depth");
  Seed(0x100);
  CPU_SB(I(0x28, 0, 1), 10, 0x100);
  Check(Vertex(0x100, XY(10, 20), 10.25f, 20.5f), "redundant byte stores preserve the complete vertex");
  CPU_SB(I(0x28, 0, 1), 30, 0x100);
  CPU_LHx(I(0x25, 0, 1), 20, 0x102);
  Check(CPU_reg[1].x == 20.5f && CPU_reg[1].y == 0.f && (CPU_reg[1].flags & VALID_Z) != 0,
        "byte stores preserve the untouched coordinate for subsequent halfword loads");
  CPU_SB(I(0x28, 0, 1), 40, 0x102);
  Check((GetPtr(0x100)->flags & VALID_ALL) == 0,
        "overwriting both coordinate halves clears unrelated vertex depth");
  Seed(0x100);
  CPU_LW(I(0x23, 0, 0), XY(10, 20), 0x100);
  Seed(0x200, XY(30, 40), 30.25f, 40.5f);
  CPU_SH(I(0x29, 0, 0), 0, 0x200);
  Check(Vertex(0x200, XY(0, 40), 0.f, 40.5f), "SH from r0 stores an exact zero despite discarded precision writes");
  Seed(0x100);
  CPU_LW(I(0x23, 0, 1), XY(10, 20), 0x100);
  CPU_SH(I(0x29, 0, 1), 30, 0x200);
  Check(GetPtr(0x200)->value == XY(30, 40) && GetPtr(0x200)->x == 30.f,
        "SH fingerprints the transferred half rather than a stale register value");
  Seed(0x100);
  CPU_LW(I(0x23, 0, 1), XY(10, 20), 0x100);
  Seed(0x200, XY(30, 40), 30.25f, 40.5f, 2000.f);
  CPU_SH(I(0x29, 0, 1), 10, 0x200);
  Check(Vertex(0x200, XY(10, 40), 10.25f, 40.5f, false),
        "halfword packing from different depths retains geometry with affine fallback");
  for (uint32_t offset = 0; offset < 4; offset++)
  {
    Seed(0x100);
    const uint8_t byte = static_cast<uint8_t>(XY(10, 20) >> (offset * 8));
    CPU_SB(I(0x28, 0, 1), byte, 0x100 + offset);
    Check(Vertex(0x100, XY(10, 20), 10.25f, 20.5f), "all byte offsets preserve precision for redundant writes");
    CPU_SB(I(0x28, 0, 1), byte ^ 1u, 0x100 + offset);
    const bool low = offset < 2;
    CPU_LHx(I(0x25, 0, 1), low ? 20 : 10, low ? 0x102 : 0x100);
    Check(CPU_reg[1].x == (low ? 20.5f : 10.25f) && CPU_reg[1].y == 0.f,
          "all byte offsets preserve the untouched halfword precision");
  }
  for (uint32_t offset : {0u, 2u})
  {
    Seed(0x100);
    CPU_LW(I(0x23, 0, 1), XY(10, 20), 0x100);
    Seed(0x200, XY(30, 40), 30.25f, 40.5f);
    CPU_SH(I(0x29, 0, 1), 10, 0x200 + offset);
    Check(Vertex(0x200, offset ? XY(30, 10) : XY(10, 40), offset ? 30.25f : 10.25f,
                 offset ? 10.25f : 40.5f), "SH preserves the unaffected coordinate when packing equal-depth halves");
  }
}

static void CheckGTERegisterTransfers()
{
  using namespace PGXP;
  for (uint32_t reg : {1u, 3u, 5u, 8u, 9u, 10u, 11u, 7u, 16u, 17u, 18u, 19u,
                      36u, 44u, 52u, 58u, 59u, 61u, 62u})
  {
    for (bool memory_load : {false, true})
    {
      if (memory_load && reg >= 32) continue;
      Reset();
      GTE::Reset();
      const uint32_t input = XY(-10, 20);
      Seed(0x100, input, -9.75f, 20.5f);
      CPU_LW(I(0x23, 0, 1), input, 0x100);
      GTE::WriteRegister(reg, input);
      if (reg >= 32) CPU_CTC2(R(0, 1, reg - 32), input, input);
      else if (memory_load) CPU_LWC2(I(0x32, 0, reg), input, 0x100);
      else CPU_MTC2(R(0, 1, reg), input, input);
      const uint32_t actual = GTE::ReadRegister(reg);
      if (reg >= 32) CPU_CFC2(R(0, 2, reg - 32), actual, actual);
      else CPU_MFC2(R(0, 2, reg), actual, actual);
      Check(CPU_reg[2].value == actual && CPU_reg[2].x == -9.75f &&
            CPU_reg[2].y == float(int16_t(actual >> 16)) && (CPU_reg[2].flags & VALID_012) == VALID_012,
            "typed GTE transfers preserve the low coordinate and the actual native extension");
    }
  }
  Reset();
  GTE::Reset();
  GTE::WriteRegister(9, 10 * 128);
  Seed(0x100, 10, 10.25f, 0.5f);
  CPU_LW(I(0x23, 0, 1), 10, 0x100);
  GTE::WriteRegister(29, 10);
  CPU_MTC2(R(0, 1, 29), 10, 10);
  CPU_MFC2(R(0, 2, 29), 10, 10);
  Check(CPU_reg[2].x == 10.f && CPU_reg[2].y == 0.f && CPU_reg[2].flags == VALID_01,
        "read-only packed color registers cannot receive geometry from ignored writes");
  CPU_SWC2(I(0x3a, 0, 29), 10, 0x200);
  Check(Vertex(0x200, 10, 10.f, 0.f, false), "SWC2 reads a derived color as a constant without vertex depth");
  Reset();
  GTE::Reset();
  Seed(0x100, 1280, 1280.25f, 0.5f);
  CPU_LW(I(0x23, 0, 1), 1280, 0x100);
  GTE::WriteRegister(9, 1280);
  CPU_MTC2(R(0, 1, 9), 1280, 1280);
  GTE::WriteRegister(28, 10);
  CPU_reg[1] = {10.f, 0.f, 0.f, {VALID_01}, 10};
  CPU_MTC2(R(0, 1, 28), 10, 10);
  CPU_MFC2(R(0, 2, 9), 1280, 1280);
  Check(CPU_reg[2].x == 1280.f && CPU_reg[2].y == 0.f && CPU_reg[2].flags == VALID_01,
        "IRGB conversion replaces stale IR precision even when the integer IR value is unchanged");
  Seed(0x100, 0, 0.25f, 0.5f);
  CPU_LW(I(0x23, 0, 1), 0, 0x100);
  GTE::WriteRegister(63, 0);
  CPU_CTC2(R(0, 1, 31), 0, 0);
  CPU_CFC2(R(0, 2, 31), 0, 0);
  Check(CPU_reg[2].x == 0.f && CPU_reg[2].y == 0.f && CPU_reg[2].flags == VALID_01,
        "GTE status flags cannot inherit fractional coordinates or depth");
}

static void CheckGTECommandWrites()
{
  using namespace PGXP;
  constexpr uint32_t arithmetic = (7u << 9) | (7u << 25);
  constexpr uint32_t color = arithmetic | (1u << 22);
  constexpr uint32_t triple_color = color | (3u << 20);
  constexpr uint32_t project = arithmetic | (1u << 8) | (1u << 24) | (1u << 19) | (1u << 14);
  struct Command { uint32_t opcode, outputs; };
  const Command commands[] = {
    {0x01, project}, {0x06, 1u << 24}, {0x0c, arithmetic}, {0x10, color},
    {0x11, color}, {0x12, arithmetic}, {0x13, color}, {0x14, color},
    {0x16, triple_color}, {0x1b, color}, {0x1c, color}, {0x1e, color},
    {0x20, triple_color}, {0x28, arithmetic}, {0x29, color}, {0x2a, triple_color},
    {0x2d, (1u << 7) | (1u << 24)}, {0x2e, (1u << 7) | (1u << 24)},
    {0x30, project | (3u << 12) | (3u << 17)},
    {0x3d, color}, {0x3e, color}, {0x3f, triple_color}
  };
  for (bool direct_impl : {false, true})
  {
    for (bool culling : {false, true})
    {
      g_settings.gpu_pgxp_culling = culling;
      for (const auto& command : commands)
      {
        Reset();
        GTE::Reset();
        // Zero operands make every native result zero. Seed matching raw
        // bits with fractional geometry so value validation alone cannot
        // hide a missing register-write notification.
        Seed(0x100, 0, 0.25f, 0.5f);
        CPU_LW(I(0x23, 0, 1), 0, 0x100);
        for (uint32_t reg = 0; reg < 32; reg++)
        {
          if (reg == 15 || reg == 28 || reg == 29 || reg == 31) continue;
          CPU_MTC2(R(0, 1, reg), 0, 0);
        }
        std::array<PGXP_value, 32> before;
        std::copy_n(GTE_data_reg, 32, before.begin());
        const uint32_t instruction = 0x80000u | command.opcode;
        if (direct_impl)
        {
          TickCount ticks = 0;
          const auto impl = GTE::GetInstructionImpl(instruction, &ticks);
          impl(GTE::Instruction{instruction});
        }
        else GTE::ExecuteInstruction(instruction);
        for (uint32_t reg = 0; reg < 32; reg++)
        {
          if (reg == 15 || reg == 28 || reg == 29 || reg == 31) continue;
          CPU_MFC2(R(0, 2, reg), GTE::ReadRegister(reg), GTE::ReadRegister(reg));
          const bool overwritten = (command.outputs & (1u << reg)) != 0;
          const auto& expected = before[reg];
          char name[120];
          std::snprintf(name, sizeof(name), "GTE command %02x register %u %s via %s culling %u",
                        command.opcode, reg, overwritten ? "replaces stale geometry" : "preserves live geometry",
                        direct_impl ? "compiled dispatch" : "interpreter dispatch", unsigned(culling));
          Check(overwritten ?
            (CPU_reg[2].value == 0 && CPU_reg[2].x == 0.f && CPU_reg[2].y == 0.f && CPU_reg[2].flags == VALID_01) :
            (CPU_reg[2].value == expected.value && CPU_reg[2].x == expected.x && CPU_reg[2].y == expected.y &&
             CPU_reg[2].z == expected.z && CPU_reg[2].flags == expected.flags), name);
        }
      }
    }
  }
  g_settings.gpu_pgxp_culling = false;

  for (bool depth_fifo : {false, true})
  {
    Reset();
    GTE::Reset();
    const uint32_t first = depth_fifo ? 16 : 20;
    const uint32_t count = depth_fifo ? 4 : 3;
    for (uint32_t i = 0; i < count; i++)
    {
      Seed(0x100, 0, 0.25f * float(i + 1), 0.5f);
      CPU_LW(I(0x23, 0, 1), 0, 0x100);
      CPU_MTC2(R(0, 1, first + i), 0, 0);
    }
    GTE::ExecuteInstruction(0x80000u | (depth_fifo ? 0x01u : 0x3du));
    for (uint32_t i = 0; i < count; i++)
    {
      CPU_SWC2(I(0x3a, 0, first + i), 0, 0x200 + i * 4);
      Check(Vertex(0x200 + i * 4, 0, i + 1 == count ? 0.f : 0.25f * float(i + 2),
                   depth_fifo || i + 1 == count ? 0.f : 0.5f, i + 1 < count),
            depth_fifo ? "SZ FIFO moves the surviving precision and replaces its tail" :
                         "RGB FIFO moves the surviving precision and replaces its tail");
    }
    GTE::WriteRegister(first + 1, 1);
    GTE::ExecuteInstruction(0x80000u | (depth_fifo ? 0x01u : 0x3du));
    CPU_SWC2(I(0x3a, 0, first), 1, 0x200);
    Check(Vertex(0x200, 1, 1.f, 0.f, false),
          "native FIFO movement rejects precision whose source bits changed");
  }
}

static void CheckPreciseCullingBounds()
{
  using namespace PGXP;
  g_settings.gpu_pgxp_culling = true;
  struct Triangle {
    float xy[6];
    uint32_t result, flags;
  };
  const Triangle triangles[] = {
    {{-32768.f, -32768.f, 32767.f, -32768.f, -32768.f, 0.5f}, 0x7fffffffu, 0},
    {{-32768.f, -32768.f, -32768.f, 0.5f, 32767.f, -32768.f}, 0x80000001u, 0},
    {{-32768.f, -32768.f, 32767.f, -32768.f, -32768.f, 32767.f}, 0xfffe0001u, 0x80010000u},
    {{-32768.f, -32768.f, -32768.f, 32767.f, 32767.f, -32768.f}, 0x0001ffffu, 0x80008000u},
    {{1000.f, 1000.f, 1000.25f, 1000.f, 1000.f, 1000.25f}, 1, 0},
    {{1000.f, 1000.f, 1000.f, 1000.25f, 1000.25f, 1000.f}, 0xffffffffu, 0},
    {{1000.f, 1000.f, 1000.25f, 1000.25f, 1000.5f, 1000.5f}, 0, 0}
  };
  for (bool direct_impl : {false, true})
  {
    for (const auto& triangle : triangles)
    {
      Reset();
      GTE::Reset();
      for (uint32_t i = 0; i < 3; i++)
      {
        const float x = triangle.xy[i * 2], y = triangle.xy[i * 2 + 1];
        const uint32_t raw = XY(static_cast<int16_t>(x), static_cast<int16_t>(y));
        GTE::WriteRegister(12 + i, raw);
        GTE_PushSXYZ2f(x, y, 1000.f, raw);
      }
      if (direct_impl)
      {
        TickCount ticks;
        GTE::GetInstructionImpl(6, &ticks)(GTE::Instruction{6});
      }
      else GTE::ExecuteInstruction(6);
      Check(GTE::ReadRegister(24) == triangle.result && GTE::ReadRegister(63) == triangle.flags,
            "precise culling retains integer range, overflow flags and subpixel orientation");
      CPU_SWC2(I(0x3a, 0, 24), triangle.result, 0x200);
      const auto& stored = *GetPtr(0x200);
      Check(stored.value == triangle.result && stored.x == float(int16_t(triangle.result)) &&
            stored.y == float(int16_t(triangle.result >> 16)) && stored.flags == VALID_01,
            "precise culling stores its new MAC0 without borrowed geometry");
    }
  }
  for (bool negative : {false, true})
  {
    Reset();
    GTE::Reset();
    const float large = std::numeric_limits<float>::max();
    GTE_PushSXYZ2f(0.f, 0.f, 1000.f, 0);
    GTE_PushSXYZ2f(large, 0.f, 1000.f, 0);
    GTE_PushSXYZ2f(0.f, negative ? -large : large, 1000.f, 0);
    GTE::ExecuteInstruction(6);
    Check(GTE::ReadRegister(24) == 0 && GTE::ReadRegister(63) == (negative ? 0x80008000u : 0x80010000u),
          "finite culling areas larger than int64 wrap safely and retain overflow flags");
  }
  for (uint32_t reg : {12u, 13u, 14u})
  {
    for (float invalid : {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
    {
      for (uint32_t component : {0u, 1u, 2u})
      {
        Reset();
        GTE::Reset();
        for (uint32_t i = 0; i < 3; i++) GTE_PushSXYZ2f(0.f, 0.f, 1000.f, 0);
        auto& vertex = GTE_data_reg[reg];
        if (component == 0) vertex.x = invalid;
        else if (component == 1) vertex.y = invalid;
        else vertex.z = invalid;
        // A loaded snapshot or arithmetic result may retain validity bits;
        // culling must inspect its numbers before converting the area.
        Check(!GTE_NCLIP_valid(0, 0, 0), "culling rejects nonfinite metadata despite matching raw bits");
      }
    }
    for (float z : {0.f, -1.f})
    {
      Reset();
      for (uint32_t i = 0; i < 3; i++) GTE_PushSXYZ2f(0.f, 0.f, 1000.f, 0);
      GTE_data_reg[reg].z = z;
      Check(!GTE_NCLIP_valid(0, 0, 0), "culling rejects nonpositive depth despite retained validity bits");
    }
  }
  g_settings.gpu_pgxp_culling = false;
}

static void CheckMemoryALU()
{
  using namespace PGXP;
  const uint32_t raw = XY(10, 20);
  auto seed_registers = [&]() {
    Reset();
    Seed(0x100);
    CPU_MFC2(R(0, 1, 14), raw, raw);
    CPU_reg[2] = IntegerValue(0);
    CPU_reg[3] = CPU_reg[1];
    CPU_reg[3].z = 9000.f;
  };
  auto copied = [&]() {
    return CPU_reg[3].value == raw && CPU_reg[3].x == 10.25f && CPU_reg[3].y == 20.5f &&
           CPU_reg[3].z == 1000.f && CPU_reg[3].flags == CPU_reg[1].flags;
  };
  for (uint32_t funct : {0x20u, 0x21u, 0x22u, 0x23u, 0x25u, 0x26u})
  {
    seed_registers();
    CPU_MemoryALU(R(1, 2, 3) | funct, raw, 0);
    Check(copied(), "memory tracking preserves exact copies through zero register operands");
  }
  for (uint32_t funct : {0x20u, 0x21u, 0x25u, 0x26u})
  {
    seed_registers();
    CPU_MemoryALU(R(2, 1, 3) | funct, 0, raw);
    Check(copied(), "memory tracking preserves copies with zero on the left");
  }
  for (uint32_t funct : {0x24u, 0x25u})
  {
    seed_registers();
    CPU_MemoryALU(R(1, 1, 3) | funct, raw, raw);
    Check(copied(), "idempotent bitwise copies preserve the original geometry");
  }
  for (bool left : {false, true})
  {
    seed_registers();
    CPU_MemoryALU((left ? R(2, 1, 3) : R(1, 2, 3)) | 0x24, left ? 0xffffffffu : raw,
                  left ? raw : 0xffffffffu);
    Check(copied(), "AND with all bits set preserves a complete vertex");
  }
  for (uint32_t funct : {0u, 2u, 3u, 4u, 6u, 7u})
  {
    seed_registers();
    CPU_MemoryALU(R(2, 1, 3) | funct, 32, raw);
    Check(copied(), "zero effective shifts preserve complete precision in memory mode");
  }
  for (uint32_t opcode : {8u, 9u, 0xdu, 0xeu})
  {
    seed_registers();
    CPU_MemoryALU(I(opcode, 1, 3, 0), raw, 0);
    Check(copied(), "zero immediate copies retain their depth and fractional coordinates");
  }
  for (uint32_t funct : {0u, 2u, 3u, 4u, 6u, 7u, 0x10u, 0x12u, 0x20u, 0x21u, 0x22u,
                         0x23u, 0x24u, 0x25u, 0x26u, 0x27u, 0x2au, 0x2bu})
  {
    seed_registers();
    CPU_MemoryALU(R(1, 2, 3, 1) | funct, raw, raw + 1);
    Check(CPU_reg[3].flags == 0 && CPU_reg[1].z == 1000.f,
          "untracked register results discard old matching-bit geometry without touching the source");
  }
  for (uint32_t opcode : {8u, 9u, 0xau, 0xbu, 0xcu, 0xdu, 0xeu, 0xfu})
  {
    seed_registers();
    CPU_MemoryALU(I(opcode, 1, 3, 1), raw, 0);
    Check(CPU_reg[3].flags == 0, "untracked immediate results cannot reuse the destination's former depth");
  }
  seed_registers();
  CPU_MemoryALU(R(1, 0, 1) | 0x25u, raw, 0);
  Check(CPU_reg[1].z == 1000.f && CPU_reg[1].x == 10.25f, "in-place copies retain source precision");
  CPU_reg[0] = CPU_reg[1];
  CPU_reg[0].value = 0;
  CPU_MemoryALU(R(0, 2, 3) | 0x21u, 0, 0);
  Check(CPU_reg[3].value == 0 && CPU_reg[3].x == 0 && (CPU_reg[3].flags & VALID_Z) == 0,
        "memory mode copies use the hardwired zero value instead of discarded geometry");
  for (uint32_t funct : {0x20u, 0x21u, 0x25u, 0x26u})
  {
    for (bool left : {false, true})
    {
      seed_registers();
      CPU_reg[1] = PGXP_value{0.25f, 0.5f, 1000.f, {VALID_012}, 0};
      CPU_MemoryALU((left ? R(0, 1, 3) : R(1, 0, 3)) | funct, 0, 0);
      Check(CPU_reg[3].x == 0.25f && CPU_reg[3].y == 0.5f && CPU_reg[3].z == 1000.f &&
            (CPU_reg[3].flags & VALID_Z), "copies of native-zero vertices retain their fractional geometry and depth");
    }
  }
  for (uint32_t funct : {0x20u, 0x21u, 0x22u, 0x23u, 0x26u})
  {
    seed_registers();
    CPU_reg[1] = PGXP_value{0.25f, 0.5f, 1000.f, {VALID_012}, 0};
    CPU_MemoryALU(R(1, 1, 3) | funct, 0, 0);
    Check(CPU_reg[3].flags == 0, "a tracked zero word is not an integer zero operand in untracked arithmetic");
  }
  seed_registers();
  CPU_MemoryALU(I(0x23, 1, 3), raw, 0);
  CPU_MemoryALU(I(0x2b, 1, 3), raw, 0);
  Check(CPU_reg[3].z == 9000.f, "memory ALU tracking leaves separately tracked transfers alone");
  CPU_InvalidateRegister(3);
  Check(CPU_reg[3].flags == 0 && CPU_reg[1].flags != 0, "explicit precision invalidation only clears the written register");
  *CPU_GetRegisterFlags(1) = 0;
  Check(CPU_reg[1].flags == 0, "compiled invalidation points at the stable register precision flags");
}

static void CheckGPUVertexSnapshots()
{
  using namespace PGXP;
  g_settings.gpu_pgxp_vertex_cache = false;
  auto precise = [](uint32_t token, uint32_t raw, float px, float py, float z) {
    float x, y, w;
    return GetPreciseVertex(token, raw, int16_t(raw), int16_t(raw >> 16), 0, 0, &x, &y, &w) &&
           std::abs(x - px) < 0.0001f && std::abs(y - py) < 0.0001f && std::abs(w - z / 65535.f) < 0.000001f;
  };
  const uint32_t raw = XY(10, 20);
  Reset();
  Seed(0x100);
  const uint32_t queued = CaptureGPUVertex(0x100, raw);
  Seed(0x100, raw, 10.75f, 20.875f, 4000.f);
  Check(precise(queued, raw, 10.25f, 20.5f, 1000.f),
        "queued DMA vertex retains its geometry and depth after matching RAM bits are overwritten");
  Check(CaptureGPUVertex(0x100, XY(30, 40)) == INVALID_GPU_VERTEX_TOKEN,
        "GPU submission rejects stale memory precision before creating a handle");
  Seed(0, raw, 10.875f, 20.75f, 8000.f);
  Check(Vertex(INVALID_GPU_VERTEX_TOKEN, raw, 10.f, 20.f, false),
        "untracked direct GPU writes cannot accidentally consume precision from RAM zero");

  for (uint32_t address : {0x1f801810u, 0x9f801810u, 0xbf801810u})
  {
    Reset();
    Seed(0x100);
    CPU_SWC2(I(0x3a, 0, 14), raw, address);
    const uint32_t direct_gte = ConsumeGPUWrite(raw);
    Check(precise(direct_gte, raw, 10.25f, 20.5f, 1000.f), "direct SWC2 GPU writes retain precision through every bus alias");
    Check(ConsumeGPUWrite(raw) == INVALID_GPU_VERTEX_TOKEN, "direct GPU metadata is consumed only once");
    CPU_MFC2(R(0, 1, 14), raw, raw);
    CPU_SW(I(0x2b, 0, 1), raw, address);
    Check(precise(ConsumeGPUWrite(raw), raw, 10.25f, 20.5f, 1000.f), "direct CPU word stores retain GTE vertex precision");
    CPU_SWC2(I(0x3a, 0, 14), raw, address);
    CPU_SW(I(0x2b, 0, 1), raw, 0x200);
    Check(ConsumeGPUWrite(raw) == INVALID_GPU_VERTEX_TOKEN, "unrelated word stores discard an unconsumed GPU latch");
    CPU_SWC2(I(0x3a, 0, 14), raw, address);
    CPU_SB(I(0x28, 0, 1), 0, address);
    Check(ConsumeGPUWrite(raw) == INVALID_GPU_VERTEX_TOKEN, "byte stores cannot replay a full vertex GPU latch");
    CPU_SWC2(I(0x3a, 0, 14), raw, address);
    CPU_SH(I(0x29, 0, 1), 0, address);
    Check(ConsumeGPUWrite(raw) == INVALID_GPU_VERTEX_TOKEN, "halfword stores cannot replay a full vertex GPU latch");
    for (uint32_t opcode : {0x2au, 0x2eu})
    {
      CPU_SWC2(I(0x3a, 0, 14), raw, address);
      CPU_SW(I(opcode, 0, 1), raw, address);
      Check(ConsumeGPUWrite(raw) == INVALID_GPU_VERTEX_TOKEN, "merged stores discard an unconsumed GPU latch");
    }
    CPU::g_state.cop0_regs.sr.Isc = true;
    CPU_SWC2(I(0x3a, 0, 14), raw, address);
    const uint32_t isolated = ConsumeGPUWrite(raw);
    Check(address == 0xbf801810u ? precise(isolated, raw, 10.25f, 20.5f, 1000.f) :
          isolated == INVALID_GPU_VERTEX_TOKEN, "only uncached GPU stores reach the bus during cache isolation");
    CPU::g_state.cop0_regs.sr.Isc = false;
    CPU_SW(I(0x2b, 0, 1), XY(30, 40), address);
    Check(ConsumeGPUWrite(XY(30, 40)) == INVALID_GPU_VERTEX_TOKEN, "direct GPU writes reject mismatched source register bits");
  }

  Reset();
  Seed(0x100);
  std::vector<uint32_t> tokens;
  for (uint32_t i = 0; i < GPU_VERTEX_SNAPSHOT_COUNT; i++) tokens.push_back(CaptureGPUVertex(0x100, raw));
  bool all_valid = true;
  for (uint32_t token : tokens) all_valid &= precise(token, raw, 10.25f, 20.5f, 1000.f);
  Check(all_valid, "every slot in a full GPU FIFO retains its submitted vertex");
  for (uint32_t i = 0; i < GPU_VERTEX_SNAPSHOT_COUNT; i++)
  {
    CPU_SWC2(I(0x3a, 0, 14), raw, 0x1f801810);
    DiscardGPUWrite();
  }
  all_valid = true;
  for (uint32_t token : tokens) all_valid &= precise(token, raw, 10.25f, 20.5f, 1000.f);
  Check(all_valid, "discarded GPU writes preserve every queued precision snapshot");
  Check(ConsumeGPUWrite(raw) == INVALID_GPU_VERTEX_TOKEN, "discarded GPU writes cannot replay their pending precision");
  Seed(0x100, raw, 10.75f, 20.875f, 4000.f);
  const uint32_t wrapped = CaptureGPUVertex(0x100, raw);
  Check(Vertex(tokens.front(), raw, 10.f, 20.f, false) && precise(wrapped, raw, 10.75f, 20.875f, 4000.f),
        "reused GPU slots reject old generations even when the packed coordinates match");
  Check(precise(tokens.back(), raw, 10.25f, 20.5f, 1000.f), "ring wrap preserves the other queued vertices");
  Reset();
  Seed(0x100);
  const uint32_t after_reset = CaptureGPUVertex(0x100, raw);
  Check(Vertex(wrapped, raw, 10.f, 20.f, false) && precise(after_reset, raw, 10.25f, 20.5f, 1000.f),
        "reset cannot attach a new vertex to an old GPU token");

  for (bool memory_state : {false, true})
  {
    Reset();
    Seed(0x100);
    const uint32_t saved = CaptureGPUVertex(0x100, raw);
    Seed(0x100, raw, 10.75f, 20.875f, 4000.f);
    auto stream = ByteStream_CreateGrowableMemoryStream();
    StateWrapper writer(stream.get(), StateWrapper::Mode::Write, 57);
    Check(memory_state ? DoMemoryState(writer) : DoState(writer),
          memory_state ? "runahead saves GPU vertex handles" : "persistent states save GPU vertex handles");
    const uint32_t abandoned = CaptureGPUVertex(0x100, raw);
    stream->SeekAbsolute(0);
    StateWrapper reader(stream.get(), StateWrapper::Mode::Read, 57);
    Check((memory_state ? DoMemoryState(reader) : DoState(reader)) &&
          precise(saved, raw, 10.25f, 20.5f, 1000.f) && Vertex(0x100, raw, 10.75f, 20.875f) &&
          Vertex(abandoned, raw, 10.f, 20.f, false),
          "state reload restores queued precision independently of RAM and removes abandoned tokens");
    stream->Resize(static_cast<uint32_t>(stream->GetSize() - 1));
    stream->SeekAbsolute(0);
    StateWrapper truncated(stream.get(), StateWrapper::Mode::Read, 57);
    Check(!(memory_state ? DoMemoryState(truncated) : DoState(truncated)), "truncated GPU snapshot data fails cleanly");
  }

  Reset();
  auto empty = ByteStream_CreateGrowableMemoryStream();
  StateWrapper empty_writer(empty.get(), StateWrapper::Mode::Write, 57);
  Check(DoGPUVertices(empty_writer) && empty->GetSize() == 516,
        "empty GPU precision snapshots add only a sequence and sparse slot bitmaps");
  Seed(0x100);
  CaptureGPUVertex(0x100, raw);
  auto malformed = ByteStream_CreateGrowableMemoryStream();
  StateWrapper malformed_writer(malformed.get(), StateWrapper::Mode::Write, 57);
  Check(DoGPUVertices(malformed_writer), "write GPU token validation fixture");
  const uint32_t slot = (s_gpu_vertex_sequence - 1u) & (GPU_VERTEX_SNAPSHOT_COUNT - 1u);
  // Sequence, preceding page bitmaps, this page's bitmap, then value and token.
  malformed->SeekAbsolute(4 + (slot / 64) * 8 + 8 + sizeof(PGXP_value));
  const uint32_t invalid_token = GPU_VERTEX_TOKEN_BASE | ((slot + 1u) & (GPU_VERTEX_SNAPSHOT_COUNT - 1u));
  malformed->Write2(&invalid_token, sizeof(invalid_token), nullptr);
  malformed->SeekAbsolute(0);
  StateWrapper malformed_reader(malformed.get(), StateWrapper::Mode::Read, 57);
  Check(!DoGPUVertices(malformed_reader), "GPU state rejects a token stored under the wrong ring slot");
  Reset();
}

static void CheckLoadDelays()
{
  using namespace PGXP;
  const uint32_t raw = XY(10, 20);
  auto setup = [&]() {
    Reset();
    Seed(0x100, raw, 10.25f, 20.5f, 1000.f);
    CPU_LW(I(0x23, 0, 1), raw, 0x100);
    Seed(0x200, raw, 10.75f, 20.875f, 4000.f);
    CPU_LW(I(0x23, 0, 2), raw, 0x200);
    CPU_MTC0(R(0, 2, 12), raw, raw);
    CPU_CTC2(R(0, 2, 0), raw, raw);
  };
  auto issue = [&](uint32_t inst, uint32_t value, uint32_t address) {
    CPU_LoadDelay(inst, value, address);
    CPU_CancelLoadDelay((inst >> 16) & 31);
    CPU_UpdateLoadDelay();
  };
  for (bool full_cpu : {false, true})
  {
    g_settings.gpu_pgxp_cpu = full_cpu;
    for (uint32_t opcode : {0x20u, 0x24u, 0x21u, 0x25u, 0x22u, 0x23u, 0x26u, 0x10u, 0x12u})
    {
      for (bool control : {false, true})
      {
        if (control && opcode != 0x12)
          continue;
        setup();
        const uint32_t inst = opcode == 0x10 ? I(opcode, 0, 1, 12 << 11) :
          opcode == 0x12 ? I(opcode, control ? 2 : 0, 1, (control ? 0 : 14) << 11) : I(opcode, 0, 1);
        const uint32_t loaded = opcode == 0x20 || opcode == 0x24 || opcode == 0x21 || opcode == 0x25 ? 10 : raw;
        issue(inst, loaded, opcode == 0x22 ? 0x203 : 0x200);
        CPU_MOVE((3 << 8) | 1, raw);
        CPU_CancelLoadDelay(3);
        CPU_SW(I(0x2b, 0, 3), raw, 0x300);
        Check(Vertex(0x300, raw, 10.25f, 20.5f) && CPU_reg[3].z == 1000.f,
              "load slot copies the old geometry even when incoming native bits match");
        CPU_UpdateLoadDelay();
        const bool invalid = opcode == 0x20 || opcode == 0x24 || (opcode == 0x10 && !full_cpu);
        Check(invalid ? CPU_reg[1].flags == 0 :
              CPU_reg[1].x == 10.75f && CPU_reg[1].z == 4000.f && (CPU_reg[1].flags & VALID_0),
              "load precision becomes visible with the native delayed result");
        CPU_UpdateLoadDelay();
        Check(CPU_reg[3].z == 1000.f, "committing a load cannot change an earlier register copy");
      }
    }
  }
  g_settings.gpu_pgxp_cpu = false;
  setup();
  issue(I(0x23, 0, 1), raw, 0x200);
  CPU_MOVE((1 << 8) | 1, raw);
  CPU_CancelLoadDelay(1);
  CPU_UpdateLoadDelay();
  Check(CPU_reg[1].z == 1000.f && CPU_reg[1].x == 10.25f,
        "same-bits write in the load slot cancels incoming precision");

  setup();
  issue(I(0x23, 0, 1), raw, 0x200);
  issue(I(0x23, 0, 1), raw, 0x100);
  Check(CPU_reg[1].z == 1000.f, "consecutive loads to one register suppress the first result");
  CPU_UpdateLoadDelay();
  Check(CPU_reg[1].z == 1000.f, "second delayed result commits after its own slot");

  setup();
  issue(I(0x22, 0, 1), raw, 0x203);
  // LWR at offset two transfers a low half from the high memory half.
  // The retained high half must come from the pending LWL, not visible r1.
  const uint32_t merged = XY(20, 20);
  CPU_LoadDelay(I(0x26, 0, 1), merged, 0x202);
  CPU_CancelLoadDelay(1);
  CPU_UpdateLoadDelay();
  Check(CPU_reg[1].x == 10.25f && CPU_reg[1].z == 1000.f,
        "consecutive merged loads preserve the old visible value in both slots");
  CPU_UpdateLoadDelay();
  Check(CPU_reg[1].x == 20.875f && CPU_reg[1].y == 20.875f && CPU_reg[1].z == 4000.f,
        "merged loads forward pending component precision and matching depth");

  setup();
  issue(I(0x23, 0, 1), raw, 0x200);
  CPU_LoadDelay(I(0x23, 0, 2), raw, 0x100);
  CPU_CancelLoadDelay(2);
  CPU_FlushLoadDelay();
  CPU_UpdateLoadDelay();
  Check(CPU_reg[1].z == 4000.f && CPU_reg[2].z == 4000.f,
        "pipeline flush commits the older load and discards the next one");

  setup();
  CPU_LoadDelay(I(0x23, 0, 0), raw, 0x200);
  CPU_UpdateLoadDelay();
  CPU_UpdateLoadDelay();
  Check(CPU_reg[0].flags == 0, "delayed loads to architectural zero cannot publish geometry");

  for (bool memory_state : {false, true})
  {
    setup();
    issue(I(0x23, 0, 1), raw, 0x200);
    CPU_LoadDelay(I(0x23, 0, 2), raw, 0x100);
    auto stream = ByteStream_CreateGrowableMemoryStream();
    StateWrapper writer(stream.get(), StateWrapper::Mode::Write, 58);
    Check(memory_state ? DoMemoryState(writer) : DoState(writer), "save visible and pending precision separately");
    Reset();
    stream->SeekAbsolute(0);
    StateWrapper reader(stream.get(), StateWrapper::Mode::Read, 58);
    Check((memory_state ? DoMemoryState(reader) : DoState(reader)) && CPU_reg[1].z == 1000.f && CPU_reg[2].z == 4000.f,
          "reload restores the old register values before pending loads commit");
    CPU_UpdateLoadDelay();
    Check(CPU_reg[1].z == 4000.f && CPU_reg[2].z == 4000.f,
          "restored current load commits without publishing the next result early");
    CPU_UpdateLoadDelay();
    Check(CPU_reg[2].z == 1000.f, "restored next load retains its own delay slot");
    stream->Resize(static_cast<uint32_t>(stream->GetSize() - 1));
    stream->SeekAbsolute(0);
    StateWrapper truncated(stream.get(), StateWrapper::Mode::Read, 58);
    Check(!(memory_state ? DoMemoryState(truncated) : DoState(truncated)), "truncated pending precision fails cleanly");
  }
  setup();
  issue(I(0x23, 0, 1), raw, 0x200);
  Reset();
  CPU_UpdateLoadDelay();
  Check(CPU_reg[1].flags == 0, "reset discards precision from the previous CPU pipeline");

  setup();
  auto legacy = ByteStream_CreateGrowableMemoryStream();
  StateWrapper legacy_writer(legacy.get(), StateWrapper::Mode::Write, 57);
  Check(DoState(legacy_writer), "write a legacy precision state without delayed slots");
  CPU::g_state.load_delay_reg = CPU::Reg::at;
  CPU::g_state.next_load_delay_reg = CPU::Reg::count;
  legacy->SeekAbsolute(0);
  StateWrapper legacy_reader(legacy.get(), StateWrapper::Mode::Read, 57);
  Check(DoState(legacy_reader) && CPU_reg[1].flags == 0 && CPU_reg[2].z == 4000.f,
        "legacy pending loads fall back safely without discarding other precision");
  CPU::g_state.load_delay_reg = CPU::Reg::count;
  for (uint32_t invalid_reg : {0u, 32u, UINT32_MAX})
  {
    auto malformed = ByteStream_CreateGrowableMemoryStream();
    const PGXP_value value = {10.75f, 20.875f, 4000.f, {VALID_012}, raw};
    malformed->Write2(&invalid_reg, sizeof(invalid_reg), nullptr);
    malformed->Write2(&value, sizeof(value), nullptr);
    malformed->SeekAbsolute(0);
    StateWrapper malformed_reader(malformed.get(), StateWrapper::Mode::Read, 58);
    Check(!DoLoadDelay(malformed_reader), "pending precision rejects a non-loadable register in saved data");
  }
  setup();
  issue(I(0x23, 0, 1), raw, 0x200);
  *CPU_GetLoadDelayRegister() = static_cast<uint32_t>(CPU::Reg::count);
  CPU_UpdateLoadDelay();
  Check(CPU_reg[1].z == 1000.f, "generated cancellation discards the pending write without replacing visible depth");
  Reset();
}

int main()
{
  using namespace PGXP;
  g_settings.gpu_pgxp_enable = true;
  Initialize();
  CheckHiLoTransfers();
  CheckDivisions();
  CheckMultiplication();
  CheckPartialTransfers();
  CheckGTERegisterTransfers();
  CheckGTECommandWrites();
  CheckPreciseCullingBounds();
  CheckMemoryALU();
  CheckLoadDelays();
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
  CheckMemorySnapshots();

  CheckGPUVertexSnapshots();
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
  CheckMemorySnapshots();

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
