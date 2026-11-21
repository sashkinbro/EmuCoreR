// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: GPL-3.0-or-later
//
// EmuCoreR ECR CPU Accuracy Oracle:
//   Golden reference : Interpreter (and CachedInterpreter_ICache for ICache=true)
//   Targets checked  :
//     - CachedInterpreter
//     - Recompiler_Safe     (Fastmem=Disabled, MemoryExceptions=true)
//     - Recompiler_LUT      (Fastmem=LUT,      MemoryExceptions=false)
//     - Recompiler_MMap     (Fastmem=MMap,     MemoryExceptions=false)
//     - Recompiler_ICache   (Fastmem=MMap,     MemoryExceptions=false, ICache=true)
//     - Recompiler_PGXP_Mem (Fastmem=MMap,     PGXPEnable=true, PGXPCPU=false)
//     - Recompiler_PGXP_CPU (Fastmem=MMap,     PGXPEnable=true, PGXPCPU=true)
//
// Also supports:
//   --bios <path> [--frames N]
//   --game <path> [--game-frames N]
#include "mips_emitter.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace ecr_oracle;

namespace {

constexpr uint32_t kResultRamOffset = 0x10000u;  // 0x80010000
constexpr uint32_t kMagicBase = 0xCAFE1000u;
constexpr uint32_t kResultWords = 512u;

struct CpuTargetConfig {
  const char* name;
  const char* cpu_mode;
  const char* fastmem_mode;
  bool memory_exceptions;
  bool block_linking;
  bool icache;
  bool pgxp_enable;
  bool pgxp_cpu;
  bool use_icache_golden;
};

struct EnvConfig {
  std::string sys_dir;
  std::string bios_file = "oracle_bios.bin";
  CpuTargetConfig target{};
  std::vector<uint8_t> last_frame;
  unsigned last_w = 0;
  unsigned last_h = 0;
};

EnvConfig* g_env = nullptr;

bool EnvironmentCallback(unsigned command, void* data) {
  switch (command) {
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
      *static_cast<const char**>(data) = g_env->sys_dir.c_str();
      return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
      auto* var = static_cast<retro_variable*>(data);
      const std::string key(var->key ? var->key : "");
      const char* val = nullptr;
      if (key == "ecr_GPU_Renderer") val = "Software";
      else if (key == "ecr_GPU_UseThread") val = "false";
      else if (key == "ecr_CPU_ExecutionMode") val = g_env->target.cpu_mode;
      else if (key == "ecr_CPU_RecompilerBlockLinking") val = g_env->target.block_linking ? "true" : "false";
      else if (key == "ecr_CPU_RecompilerICache") val = g_env->target.icache ? "true" : "false";
      else if (key == "ecr_CPU_RecompilerMemoryExceptions") val = g_env->target.memory_exceptions ? "true" : "false";
      else if (key == "ecr_CPU_FastmemMode") val = g_env->target.fastmem_mode;
      else if (key == "ecr_GPU_PGXPEnable") val = g_env->target.pgxp_enable ? "true" : "false";
      else if (key == "ecr_GPU_PGXPCPU") val = g_env->target.pgxp_cpu ? "true" : "false";
      else if (key == "ecr_GPU_PGXPCulling") val = "true";
      else if (key == "ecr_GPU_PGXPTextureCorrection") val = "true";
      else if (key == "ecr_GPU_PGXPPreserveProjFP") val = "true";
      else if (key == "ecr_Console_Region") val = "NTSC-U";
      else if (key == "ecr_Console_Enable8MBRAM") val = "false";
      else if (key == "ecr_BIOS_PathNTSCU") val = g_env->bios_file.c_str();
      else if (key == "ecr_BIOS_PathNTSCJ") val = g_env->bios_file.c_str();
      else if (key == "ecr_BIOS_PathPAL") val = g_env->bios_file.c_str();
      else if (key == "ecr_BIOS_PatchFastBoot") val = "false";
      else if (key == "ecr_MemoryCards_Card1Type" || key == "ecr_MemoryCards_Card2Type") val = "None";
      else if (key == "ecr_Main_ApplyGameSettings") val = "false";
      else if (key == "ecr_CDROM_ReadThread") val = "false";
      else if (key == "ecr_Audio_FastHook") val = "false";
      var->value = val;
      return val != nullptr;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
      *static_cast<bool*>(data) = false;
      return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
      *static_cast<bool*>(data) = true;
      return true;
    case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
      *static_cast<int*>(data) = 3;  // VIDEO | AUDIO
      return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK:
    case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE:
    case RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE:
      return true;
    default:
      return false;
  }
}

void VideoCallback(const void* data, unsigned width, unsigned height, size_t pitch) {
  if (!data || data == RETRO_HW_FRAME_BUFFER_VALID || width == 0 || height == 0 || !g_env) return;
  g_env->last_w = width;
  g_env->last_h = height;
  const size_t row_bytes = width * 2u;  // RGB565 default in LibecrHostDisplay
  g_env->last_frame.resize(height * row_bytes);
  const auto* src = static_cast<const uint8_t*>(data);
  for (unsigned y = 0; y < height; y++) {
    std::memcpy(g_env->last_frame.data() + y * row_bytes, src + y * pitch, row_bytes);
  }
}

struct CpuCapture {
  std::vector<uint32_t> results;
  uint64_t ram_hash = 0;
  uint64_t kernel_vectors_hash = 0;
  uint64_t frame_hash = 0;
  size_t ram_bytes = 0;
};

// Emit an exception handler at 0xBFC00180 (SR.BEV=1 reset default) that records:
//   [S7] = CAUSE (masked to clear debug-only BT), [S7+4] = EPC, [S7+8] = BADVADDR, [S7+12] = SR,
// increments S7 by 16, and advances EPC past the faulting instruction (+8 if BD=1, +4 otherwise).
// Note: if ExCode == 11 (CpU, COP2 disabled), enables COP2 in SR so subsequent instructions run.
// Also if software interrupt (ExCode == 0), clears CAUSE.IP0/IP1 via MTC0 to avoid re-entry loop.
void EmitExceptionRecorderAt180(BiosBuilder& b) {
  b.Seek(0x180u);
  b.Mfc0(K0, 13u);  // CAUSE
  b.Mfc0(K1, 14u);  // EPC
  b.Li32(AT, 0xBFFFFFFFu);
  b.And(K0, K0, AT);
  b.Sw(K0, 0, S7);
  b.Sw(K1, 4, S7);
  b.Mfc0(AT, 8u);   // BADVADDR
  b.Mfc0(V1, 12u);  // SR
  b.Sw(AT, 8, S7);
  b.Sw(V1, 12, S7);
  b.Addiu(S7, S7, 16);

  // Extract ExCode = (CAUSE >> 2) & 0x1F
  b.Srl(AT, K0, 2);
  b.Andi(AT, AT, 0x1Fu);

  // If ExCode == 0 (Int), clear CAUSE software interrupts (IP0..IP1) and do NOT skip EPC!
  BiosBuilder::Label not_int;
  b.Bne(AT, ZERO, not_int);
  b.Nop();
  b.Mtc0(ZERO, 13u);
  b.Nop();
  b.Jr(K1);
  b.Rfe();

  b.Bind(not_int);
  // If ExCode == 11 (CpU), set SR bit 30 (CU2) before RFE so test can proceed cleanly
  BiosBuilder::Label not_cpu;
  b.Addiu(T9, ZERO, 11);
  b.Bne(AT, T9, not_cpu);
  b.Nop();
  b.Li32(T9, 0x40000000u);
  b.Or(V1, V1, T9);
  b.Mtc0(V1, 12u);
  b.Nop();

  b.Bind(not_cpu);
  // Skip +8 if CAUSE.BD (bit 31 < 0), else +4
  b.Bgez(K0, 2);
  b.Nop();
  b.Addiu(K1, K1, 4);
  b.Addiu(K1, K1, 4);
  b.Jr(K1);
  b.Rfe();
}

void EmitCommonPrologue(BiosBuilder& b, bool enable_cop2 = true) {
  EmitExceptionRecorderAt180(b);
  b.Seek(0x0u);
  b.J(0xBFC00400u);
  b.Nop();
  b.Seek(0x400u);
  b.Li32(S6, 0x80010000u);  // S6 = result base pointer
  b.Li32(S7, 0x80010200u);  // S7 = exception log base pointer
  b.Li32(SP, 0x801FFF00u);
  b.Li32(T0, enable_cop2 ? 0x40400000u : 0x00400000u);  // BEV=1 (+ CU2)
  b.Mtc0(T0, 12u);
  b.Nop();
}

void EmitDoneMagic(BiosBuilder& b, uint32_t magic) {
  b.Li32(T0, magic);
  b.Sw(T0, 0, S6);
  b.HaltLoop();
}

struct SyntheticTest {
  const char* name;
  uint32_t frames;
  uint32_t magic;
  bool requires_mem_exceptions;
  bool compare_codecache_only;
  std::function<void(BiosBuilder&)> build;
};

std::vector<SyntheticTest> BuildSuite() {
  std::vector<SyntheticTest> tests;

  // 1. alu_shifts_overflow
  tests.push_back({"alu_shifts_overflow", 4, kMagicBase + 1u, false, false, [](BiosBuilder& b) {
    EmitCommonPrologue(b);
    b.Li32(T0, 0x7FFFFFF0u);
    b.Li32(T1, 0x00000020u);
    b.Li32(T2, 0x80000000u);
    b.Li32(T3, 0xFFFFFFFFu);

    int off = 4;
    auto SaveV0 = [&]() { b.Sw(V0, off, S6); off += 4; };

    b.Addu(V0, T0, T1);  SaveV0();
    b.Subu(V0, T2, T3);  SaveV0();
    b.And(V0, T0, T1);   SaveV0();
    b.And(V0, T0, ZERO); SaveV0();
    b.And(V0, T0, T3);   SaveV0();
    b.Or(V0, T0, T2);    SaveV0();
    b.Or(V0, T0, ZERO);  SaveV0();
    b.Xor(V0, T0, T3);   SaveV0();
    b.Xor(V0, T0, T0);   SaveV0();
    b.Nor(V0, T0, T1);   SaveV0();
    b.Nor(V0, T0, ZERO); SaveV0();
    b.Slt(V0, T2, T0);   SaveV0();
    b.Sltu(V0, T2, T0);  SaveV0();
    b.Slt(V0, T0, T2);   SaveV0();
    b.Slti(V0, T3, 0);   SaveV0();
    b.Sltiu(V0, T3, 1);  SaveV0();
    b.Sltiu(V0, T3, -1); SaveV0();

    // Negative immediates (tests AArch64 negative immediate folding too)
    b.Li32(V0, 100);
    b.Addiu(V0, V0, -1);     SaveV0();
    b.Addiu(V0, V0, -4095);  SaveV0();
    b.Addiu(V0, V0, -4096);  SaveV0();
    b.Addiu(V0, V0, -32768); SaveV0();
    b.Andi(V0, T0, 0xFFFFu); SaveV0();
    b.Ori(V0, T0, 0u);       SaveV0();
    b.Xori(V0, T0, 0xA55Au); SaveV0();

    // Constant & variable shifts across counts 0,1,5,15,31,32,37
    b.Li32(T4, 0x81234567u);
    const uint32_t counts[] = {0u, 1u, 15u, 31u, 32u, 37u};
    b.Sll(V0, T4, 0);   SaveV0();
    b.Srl(V0, T4, 0);   SaveV0();
    b.Sra(V0, T4, 0);   SaveV0();
    b.Sll(V0, T4, 31);  SaveV0();
    b.Srl(V0, T4, 31);  SaveV0();
    b.Sra(V0, T4, 31);  SaveV0();
    for (uint32_t c : counts) {
      b.Li32(T5, c);
      b.Sllv(V0, T4, T5); SaveV0();
      b.Srlv(V0, T4, T5); SaveV0();
      b.Srav(V0, T4, T5); SaveV0();
    }

    // Non-overflow ADD/ADDI/SUB
    b.Li32(V0, 12345);
    b.Addi(V0, V0, -5432); SaveV0();
    b.Add(V0, V0, T1);     SaveV0();
    b.Sub(V0, V0, T1);     SaveV0();

    // Trapping ADD/ADDI/SUB overflow + boundary non-trapping cases
    b.Li32(V0, 0x11223344u);
    b.Li32(A0, 0x7FFFFFFFu);
    b.Li32(A1, 1u);
    b.Add(V0, A0, A1);     // traps Ov! V0 stays 0x11223344
    SaveV0();
    b.Addi(V0, A0, 10);    // traps Ov!
    SaveV0();
    b.Li32(A0, 0x80000000u);
    b.Addi(V0, A0, -1);    // traps Ov!
    SaveV0();
    b.Sub(V0, A0, A1);     // traps Ov!
    SaveV0();
    b.Add(V0, A0, A1);     // does NOT trap (0x80000001)
    SaveV0();

    EmitDoneMagic(b, kMagicBase + 1u);
  }});

  // 2. mul_div_edges
  tests.push_back({"mul_div_edges", 4, kMagicBase + 2u, false, false, [](BiosBuilder& b) {
    EmitCommonPrologue(b);
    int off = 4;
    auto StoreHiLo = [&]() {
      b.Mfhi(V0);
      b.Mflo(V1);
      b.Sw(V0, off, S6);
      b.Sw(V1, off + 4, S6);
      off += 8;
    };

    b.Li32(T0, 0x80000001u);
    b.Li32(T1, 0xFFFFFFFDu);  // -3
    b.Mult(T0, T1);  StoreHiLo();
    b.Multu(T0, T1); StoreHiLo();

    // Dynamic vs constant across block boundaries
    b.Beq(ZERO, ZERO, 1); b.Nop();
    b.Li32(T2, 0xFFFFFF9Bu);  // -101
    b.Li32(T3, 7u);
    b.Div(T2, T3);   StoreHiLo();
    b.Divu(T2, T3);  StoreHiLo();

    // 0 / nonzero, + / 0, - / 0, 0 / 0
    b.Li32(T4, 12345u);
    b.Div(ZERO, T3);  StoreHiLo();
    b.Divu(ZERO, T3); StoreHiLo();
    b.Div(T4, ZERO);  StoreHiLo();
    b.Li32(T5, 0xFFFFFFF0u);
    b.Div(T5, ZERO);  StoreHiLo();
    b.Divu(T4, ZERO); StoreHiLo();
    b.Div(ZERO, ZERO);  StoreHiLo();
    b.Divu(ZERO, ZERO); StoreHiLo();

    // INT_MIN / -1 signed overflow (both constant and register across branch)
    b.Li32(T0, 0x80000000u);
    b.Li32(T1, 0xFFFFFFFFu);
    b.Div(T0, T1);    StoreHiLo();
    b.Beq(ZERO, ZERO, 1); b.Nop();
    b.Div(T0, T1);    StoreHiLo();

    // Back-to-back MTHI/MTLO
    b.Li32(T0, 0xAABBCCDDu);
    b.Li32(T1, 0x11223344u);
    b.Mthi(T0);
    b.Mtlo(T1);
    b.Mthi(T1);
    b.Mtlo(T0);
    StoreHiLo();

    EmitDoneMagic(b, kMagicBase + 2u);
  }});

  // 3. unaligned_mem_scratchpad
  tests.push_back({"unaligned_mem_scratchpad", 4, kMagicBase + 3u, false, false, [](BiosBuilder& b) {
    EmitCommonPrologue(b);
    b.Li32(S0, 0x80010400u);  // KSEG0 RAM
    b.Li32(S2, 0x00010400u);  // KUSEG RAM mirror
    b.Li32(S3, 0xA0010400u);  // KSEG1 RAM mirror
    b.Li32(S4, 0x80210400u);  // 2 MiB mirror wrap
    b.Li32(S1, 0x1F800000u);  // Scratchpad
    b.Li32(S5, 0x9F800000u);  // Scratchpad KSEG0 mirror

    int off = 4;
    auto Out = [&](uint32_t r) { b.Sw(r, off, S6); off += 4; };

    b.Li32(T0, 0x91A2B3C4u);
    b.Li32(T1, 0x55667788u);
    b.Sw(T0, 0, S0);
    b.Sw(T1, 4, S0);

    // Verify RAM mirror aliasing
    b.Lw(V0, 0, S2); b.Nop(); Out(V0);
    b.Lw(V0, 4, S3); b.Nop(); Out(V0);
    b.Lw(V0, 0, S4); b.Nop(); Out(V0);

    // LB / LBU / LH / LHU across offsets (negative & positive sign bits)
    for (int i = 0; i < 4; i++) {
      b.Lb(V0, i, S0);
      b.Lbu(V1, i, S0);
      b.Nop();
      Out(V0);
      Out(V1);
    }
    b.Lh(V0, 0, S0); b.Lhu(V1, 0, S0); b.Nop(); Out(V0); Out(V1);
    b.Lh(V0, 2, S0); b.Lhu(V1, 2, S0); b.Nop(); Out(V0); Out(V1);

    // LWL / LWR at every byte alignment 0..3
    for (int align = 0; align < 4; align++) {
      b.Li32(V0, 0xAABBCCDDu);
      b.Lwl(V0, align, S0);
      b.Nop();
      Out(V0);

      b.Li32(V1, 0xEEFF0011u);
      b.Lwr(V1, align, S0);
      b.Nop();
      Out(V1);
    }

    // Back-to-back LWL + LWR pair (canonical unaligned 32-bit load)
    for (int align = 0; align < 4; align++) {
      b.Li32(V0, 0xDEAD1234u);
      b.Lwl(V0, align + 3, S0);
      b.Lwr(V0, align, S0);
      b.Nop();
      Out(V0);
    }

    // SWL / SWR
    for (int align = 0; align < 4; align++) {
      b.Li32(T2, 0x01020304u + static_cast<uint32_t>(align) * 0x11111111u);
      b.Sw(ZERO, 16 + align * 8, S0);
      b.Sw(ZERO, 20 + align * 8, S0);
      b.Swl(T2, 16 + align * 8 + align, S0);
      b.Swr(T2, 16 + align * 8 + align, S0);
      b.Lw(V0, 16 + align * 8, S0); b.Nop(); Out(V0);
      b.Lw(V1, 20 + align * 8, S0); b.Nop(); Out(V1);
    }

    // Scratchpad read/write + mirror + LB/LH/SB/SH
    b.Li32(T3, 0xCAFEBABEu);
    b.Sw(T3, 0x40, S1);
    b.Sh(T3, 0x44, S1);
    b.Sb(T3, 0x46, S1);
    b.Lw(V0, 0x40, S5); b.Nop(); Out(V0);
    b.Lh(V0, 0x40, S1); b.Lhu(V1, 0x42, S5); b.Nop(); Out(V0); Out(V1);
    b.Lw(V1, 0x44, S1); b.Nop(); Out(V1);

    EmitDoneMagic(b, kMagicBase + 3u);
  }});

  // 4. load_delay_hazards
  tests.push_back({"load_delay_hazards", 4, kMagicBase + 4u, false, false, [](BiosBuilder& b) {
    EmitCommonPrologue(b);
    b.Li32(S0, 0x1F800100u);  // Scratchpad
    b.Li32(T0, 0x11112222u);
    b.Li32(T1, 0x33334444u);
    b.Li32(T2, 0x55556666u);
    b.Sw(T0, 0, S0);
    b.Sw(T1, 4, S0);
    b.Sw(T2, 8, S0);

    int off = 4;
    auto Out = [&](uint32_t r) { b.Sw(r, off, S6); off += 4; };

    // Case 1: Immediate read in load-delay slot sees OLD A0, instruction after sees NEW A0
    b.Li32(A0, 0xAAAAAAA0u);
    b.Lw(A0, 0, S0);
    b.Addu(A1, A0, ZERO);  // sees old A0
    b.Addu(A2, A0, ZERO);  // sees new A0
    Out(A1);
    Out(A2);

    // Case 2: Overwriting same register in load-delay slot CANCELS pending load
    b.Li32(A0, 0xBBBBBBB0u);
    b.Lw(A0, 0, S0);
    b.Li32(A0, 0x99998888u);  // LUI cancels load delay! (and ORI completes)
    b.Nop();
    Out(A0);

    // Case 3: Back-to-back LW to same reg: first LW completes, overwritten by second LW
    b.Li32(A0, 0xCCCC0000u);
    b.Lw(A0, 0, S0);       // 0x11112222
    b.Lw(A0, 4, S0);       // 0x33334444; slot reads A0?
    b.Addu(A1, A0, ZERO);  // sees 0x11112222 or old? On R3000A first load writes back during second's slot, so A1 gets 0x11112222? Check golden Interpreter!
    b.Nop();
    Out(A1);
    Out(A0);

    // Case 4: Load delay across branch boundary (LW in delay slot, next block reads target)
    BiosBuilder::Label blk2;
    b.Li32(A0, 0xDDDD0000u);
    b.Beq(ZERO, ZERO, blk2);
    b.Lw(A0, 8, S0);
    b.Bind(blk2);
    b.Addu(A1, A0, ZERO);  // still in load delay slot of delay-slot LW!
    b.Nop();
    Out(A1);
    Out(A0);

    // Case 5: COP0 MFC0 & COP2 MFC2 load delays
    b.Li32(T3, 0x12345678u);
    b.Mtc2(T3, 24u);  // MAC0 = 0x12345678
    b.Nop();
    b.Li32(A0, 0x77770000u);
    b.Mfc2(A0, 24u);
    b.Addu(A1, A0, ZERO);
    b.Nop();
    Out(A1);
    Out(A0);

    EmitDoneMagic(b, kMagicBase + 4u);
  }});

  // 5. branches_delay_slots (all 12 branch/jump opcodes + JALR rX,rX + chained loop linking)
  tests.push_back({"branches_delay_slots", 4, kMagicBase + 5u, false, false, [](BiosBuilder& b) {
    EmitCommonPrologue(b);
    b.Li32(V0, 0u);
    b.Li32(T0, 1u);
    b.Li32(T1, 0xFFFFFFFFu);  // -1

    int off = 4;
    auto Out = [&](uint32_t r) { b.Sw(r, off, S6); off += 4; };

    // BEQ/BNE taken and NOT-taken
    b.Beq(T0, T0, 2); b.Addiu(V0, V0, 1); b.Addiu(V0, V0, 100);
    b.Beq(T0, T1, 2); b.Addiu(V0, V0, 2); b.Addiu(V0, V0, 4);
    b.Bne(T0, T1, 2); b.Addiu(V0, V0, 8); b.Addiu(V0, V0, 200);
    b.Bne(T0, T0, 2); b.Addiu(V0, V0, 16); b.Addiu(V0, V0, 32);
    Out(V0);

    // BLEZ / BGTZ / BLTZ / BGEZ (taken + untaken + zero boundary)
    b.Li32(V0, 0u);
    b.Blez(ZERO, 2); b.Addiu(V0, V0, 1); b.Addiu(V0, V0, 1000);
    b.Blez(T0, 2);   b.Addiu(V0, V0, 2); b.Addiu(V0, V0, 4);
    b.Bgtz(T0, 2);   b.Addiu(V0, V0, 8); b.Addiu(V0, V0, 1000);
    b.Bgtz(ZERO, 2); b.Addiu(V0, V0, 16); b.Addiu(V0, V0, 32);
    b.Bltz(T1, 2);   b.Addiu(V0, V0, 64); b.Addiu(V0, V0, 1000);
    b.Bltz(ZERO, 2); b.Addiu(V0, V0, 128); b.Addiu(V0, V0, 256);
    b.Bgez(ZERO, 2); b.Addiu(V0, V0, 512); b.Addiu(V0, V0, 1000);
    b.Bgez(T1, 2);   b.Addiu(V0, V0, 1024); b.Addiu(V0, V0, 2048);
    Out(V0);

    // BLTZAL & BGEZAL (untaken STILL writes $ra!)
    b.Li32(RA, 0x11111111u);
    b.Bltzal(T0, 2);  // NOT taken, still writes $ra
    b.Nop();
    b.Nop();
    Out(RA);

    b.Li32(RA, 0x22222222u);
    b.Bgezal(T1, 2);  // NOT taken, still writes $ra
    b.Nop();
    b.Nop();
    Out(RA);

    b.Bgezal(ZERO, 2);  // taken
    b.Addiu(V0, V0, 1);
    b.Addiu(V0, V0, 999);
    Out(RA);

    // JALR rX, rX (rd == rs): must read old rX for target, write return PC into rX
    const uint32_t target = b.CurrentAddr() + 20u;
    b.Li32(S2, target);
    b.Jalr(S2, S2);
    b.Addiu(V0, V0, 3);
    b.Addiu(V0, V0, 777);  // skipped
    Out(S2);
    Out(V0);

    // Exception inside delay slot -> CAUSE.BD & EPC = branch PC
    b.Li32(A0, 0x7FFFFFFFu);
    b.Li32(A1, 1u);
    b.Beq(ZERO, ZERO, 2);
    b.Add(A2, A0, A1);  // Ov trap in delay slot
    b.Addiu(V0, V0, 500);
    Out(V0);

    // Multi-block loop (1000 iterations) to exercise block linking + downcount exit/resume
    b.Li32(T4, 1000u);
    b.Li32(T5, 0u);
    BiosBuilder::Label loop;
    b.Bind(loop);
    b.Addiu(T4, T4, -1);
    b.Bne(T4, ZERO, loop);
    b.Addu(T5, T5, T4);
    Out(T5);

    EmitDoneMagic(b, kMagicBase + 5u);
  }});

  // 6. cop0_exceptions_traps (non-alignment traps so runs on Fastmem modes too!)
  tests.push_back({"cop0_exceptions_traps", 4, kMagicBase + 6u, false, false, [](BiosBuilder& b) {
    EmitCommonPrologue(b, false);  // CU2=0 initially to test CpU!
    b.Syscall(0x123u);
    b.Break(0x456u);
    b.Emit(0x7C00003Bu);  // Reserved instruction

    // Trigger CpU (COP2 disabled) -> handler enables CU2 and returns past instruction
    b.Li32(T0, 0xABCDEF01u);
    b.Mtc2(T0, 0u);  // traps CpU!
    b.Mtc2(T0, 0u);  // now succeeds because handler enabled CU2
    b.Mfc2(V1, 0u);
    b.Nop();
    b.Sw(V1, 4, S6);

    // Trigger software interrupt (CAUSE.IP0=1 first with IEc=0, then enable IEc=1 in a
    // branch delay slot so pending-interrupt EPC aligns identically at the branch target
    // across Interpreter (per-insn) and CachedInterpreter/Recompiler (block boundary)).
    b.Li32(T2, 0x00000100u);  // CAUSE.IP0 = 1
    b.Mtc0(T2, 13u);
    b.Nop();
    b.Li32(T1, 0x40400101u);  // CU2 | BEV | IM0 | IEc=1
    BiosBuilder::Label after_sw_irq;
    b.Beq(ZERO, ZERO, after_sw_irq);
    b.Mtc0(T1, 12u);
    b.Bind(after_sw_irq);
    b.Nop();

    b.Mfc0(V0, 12u);
    b.Nop();
    b.Sw(V0, 8, S6);

    EmitDoneMagic(b, kMagicBase + 6u);
  }});

  // 7. mem_align_exceptions (AdEL / AdES - requires MemoryExceptions=true)
  tests.push_back({"mem_align_exceptions", 4, kMagicBase + 7u, true, false, [](BiosBuilder& b) {
    EmitCommonPrologue(b);
    b.Li32(T0, 0x80010501u);
    b.Beq(ZERO, ZERO, 1);
    b.Nop();
    b.Lw(T1, 0, T0);   // AdEL (word)
    b.Lh(T1, 0, T0);   // AdEL (half)
    b.Lhu(T1, 0, T0);  // AdEL (half unsigned)
    b.Sw(T2, 0, T0);   // AdES (word)
    b.Sh(T2, 0, T0);   // AdES (half)

    // Unaligned JR target (AdEL on instruction fetch) -> handler advances EPC, wait: BadVaddr check!
    // Put a normal instruction after JR delay slot that handler skips to? No, EPC is the misaligned PC!
    // Handler adds +4 to K1 (making it aligned? no, +4 keeps bit 0!), so don't test unaligned JR with simple +4 handler;
    // instead verify SR & BadVaddr after data AdEL/AdES:
    b.Mfc0(V0, 12u);
    b.Sw(V0, 4, S6);
    b.Mfc0(V1, 8u);
    b.Sw(V1, 8, S6);

    EmitDoneMagic(b, kMagicBase + 7u);
  }});

  // 8. smc_jit_invalidation (with PS1 I-cache flush, matches Interpreter across all modes)
  tests.push_back({"smc_jit_invalidation", 4, kMagicBase + 8u, false, false, [](BiosBuilder& b) {
    EmitCommonPrologue(b);
    // RAM function at 0x80020000: addiu v0, a0, 100; jr ra; nop
    b.Li32(S0, 0x80020000u);
    b.Li32(T0, BiosBuilder::I(0x09u, A0, V0, 100));
    b.Sw(T0, 0, S0);
    b.Li32(T0, BiosBuilder::R(RA, 0u, 0u, 0u, 0x08u));
    b.Sw(T0, 4, S0);
    b.Sw(ZERO, 8, S0);

    // Pass 1: returns 1100
    b.Li32(A0, 1000u);
    b.Jalr(RA, S0);
    b.Nop();
    b.Sw(V0, 4, S6);

    // Pass 2: patch WITH PS1 I-cache flush -> 1777
    b.Li32(T0, BiosBuilder::I(0x09u, A0, V0, 777));
    b.Sw(T0, 0, S0);
    b.Li32(T1, 0x40410000u);
    b.Mtc0(T1, 12u); b.Nop();
    b.Sw(ZERO, 0, S0);
    b.Li32(T1, 0x40400000u);
    b.Mtc0(T1, 12u); b.Nop();

    b.Li32(A0, 1000u);
    b.Jalr(RA, S0);
    b.Nop();
    b.Sw(V0, 8, S6);

    // Pass 3..18: Repeated SMC in a loop (16 times) WITH I-cache isolation
    b.Li32(S1, 0u);  // accumulator
    b.Li32(S2, 16u);
    b.Li32(S3, 1u);
    b.Li32(S4, BiosBuilder::I(0x09u, A0, V0, 0));
    b.Li32(S5, 0x40410000u);
    b.Li32(T8, 0x40400000u);
    BiosBuilder::Label smc_loop;
    b.Bind(smc_loop);
    b.Or(T0, S4, S3);
    b.Sw(T0, 0, S0);       // patch addiu v0, a0, S3
    b.Mtc0(S5, 12u); b.Nop();
    b.Sw(ZERO, 0, S0);     // flush PS1 I-cache line
    b.Mtc0(T8, 12u); b.Nop();
    b.Li32(A0, 100u);
    b.Jalr(RA, S0);
    b.Nop();
    b.Addu(S1, S1, V0);
    b.Addiu(S3, S3, 1);
    b.Addiu(S2, S2, -1);
    b.Bne(S2, ZERO, smc_loop);
    b.Nop();
    b.Sw(S1, 12, S6);

    EmitDoneMagic(b, kMagicBase + 8u);
  }});

  // 8b. smc_codecache_invalidation (16x repeated RAM code patch WITHOUT PS1 I-cache flush,
  //     stresses CodeCache write-invalidation / MMap write-fault / slowmem backpatch vs CachedInterpreter)
  tests.push_back({"smc_codecache_invalid", 4, kMagicBase + 15u, false, true, [](BiosBuilder& b) {
    EmitCommonPrologue(b);
    b.Li32(S0, 0x80020100u);
    b.Li32(T0, BiosBuilder::I(0x09u, A0, V0, 100));
    b.Sw(T0, 0, S0);
    b.Li32(T0, BiosBuilder::R(RA, 0u, 0u, 0u, 0x08u));
    b.Sw(T0, 4, S0);
    b.Sw(ZERO, 8, S0);

    b.Li32(S1, 0u);
    b.Li32(S2, 20u);
    b.Li32(S3, 1u);
    b.Li32(S4, BiosBuilder::I(0x09u, A0, V0, 0));
    BiosBuilder::Label smc_cc_loop;
    b.Bind(smc_cc_loop);
    b.Or(T0, S4, S3);
    b.Sw(T0, 0, S0);       // un-flushed SMC write!
    b.Li32(A0, 100u);
    b.Jalr(RA, S0);
    b.Nop();
    b.Addu(S1, S1, V0);
    b.Addiu(S3, S3, 1);
    b.Addiu(S2, S2, -1);
    b.Bne(S2, ZERO, smc_cc_loop);
    b.Nop();
    b.Sw(S1, 4, S6);

    EmitDoneMagic(b, kMagicBase + 15u);
  }});

  // 9. cop2_gte_full (all 22 opcodes + LWC2/SWC2 + SXYP + IRGB/ORGB + LZCS/LZCR + FLAG + 64-reg dump)
  tests.push_back({"cop2_gte_full", 4, kMagicBase + 9u, false, false, [](BiosBuilder& b) {
    EmitCommonPrologue(b);
    b.Li32(S0, 0x80010600u);
    b.Li32(T0, 0xFFAC0040u); b.Sw(T0, 0, S0);   // VXY0
    b.Li32(T0, 0x00000030u); b.Sw(T0, 4, S0);   // VZ0
    b.Li32(T0, 0x0050FF80u); b.Sw(T0, 8, S0);   // VXY1
    b.Li32(T0, 0x00000050u); b.Sw(T0, 12, S0);  // VZ1
    b.Li32(T0, 0x00400060u); b.Sw(T0, 16, S0);  // VXY2
    b.Li32(T0, 0x00000070u); b.Sw(T0, 20, S0);  // VZ2

    // Load V0/V1/V2 via LWC2
    b.Lwc2(0u, 0, S0);
    b.Lwc2(1u, 4, S0);
    b.Lwc2(2u, 8, S0);
    b.Lwc2(3u, 12, S0);
    b.Lwc2(4u, 16, S0);
    b.Lwc2(5u, 20, S0);
    b.Nop();

    auto SetCtrl = [&](uint32_t reg, uint32_t val) {
      b.Li32(T0, val);
      b.Ctc2(T0, reg);
    };
    SetCtrl(0u, 0x00001000u);
    SetCtrl(1u, 0x10000200u);
    SetCtrl(2u, 0xFE000000u);
    SetCtrl(3u, 0x00000000u);
    SetCtrl(4u, 0x00001000u);
    SetCtrl(5u, 10u); SetCtrl(6u, 0xFFFFFFF6u); SetCtrl(7u, 250u);  // TRX/TRY/TRZ
    SetCtrl(8u, 0x08000400u);
    SetCtrl(9u, 0x03000200u);
    SetCtrl(10u, 0x01000600u);
    SetCtrl(11u, 0x04000500u);
    SetCtrl(12u, 0x00000700u);
    SetCtrl(13u, 0x100u); SetCtrl(14u, 0x80u); SetCtrl(15u, 0x40u);  // BK
    SetCtrl(16u, 0x0C000400u); SetCtrl(17u, 0x02000800u);
    SetCtrl(18u, 0x04000200u); SetCtrl(19u, 0x06000A00u);
    SetCtrl(20u, 0x00000800u);
    SetCtrl(21u, 0x200u); SetCtrl(22u, 0x180u); SetCtrl(23u, 0x100u);  // RFC/GFC/BFC
    SetCtrl(24u, 160u << 16);
    SetCtrl(25u, 120u << 16);
    SetCtrl(26u, 320u);
    SetCtrl(27u, 0xFF00u);
    SetCtrl(28u, 0x01400000u);
    SetCtrl(29u, 0x1000u);
    SetCtrl(30u, 0x0800u);

    b.Li32(T0, 0x38C0A080u); b.Mtc2(T0, 6u);   // RGBC
    b.Li32(T0, 0x00000400u); b.Mtc2(T0, 8u);   // IR0
    b.Li32(T0, 0x00000600u); b.Mtc2(T0, 9u);   // IR1
    b.Li32(T0, 0x00000500u); b.Mtc2(T0, 10u);  // IR2
    b.Li32(T0, 0x00000300u); b.Mtc2(T0, 11u);  // IR3
    b.Nop();

    // All 22 GTE opcodes (including multiple MVMVA variants)
    const uint32_t gte_ops[] = {
      0x0180001u,  // 0x01 RTPS (sf=1)
      0x0280030u,  // 0x30 RTPT (sf=1)
      0x1400006u,  // 0x06 NCLIP
      0x158002Du,  // 0x2D AVSZ3 (sf=1)
      0x168002Eu,  // 0x2E AVSZ4 (sf=1)
      0x0492012u,  // 0x12 MVMVA (sf=1, mx=0 RT, v=0 V0, cv=1 BK, lm=1)
      0x042A412u,  // 0x12 MVMVA (sf=0, mx=1 LLM, v=1 V1, cv=2 FC, lm=0)
      0x04C6812u,  // 0x12 MVMVA (sf=1, mx=2 LCM, v=3 IR, cv=0 TR, lm=0)
      0x178000Cu,  // 0x0C OP (sf=1)
      0x0A00428u,  // 0x28 SQR
      0x0780010u,  // 0x10 DPCS (sf=1)
      0x0980011u,  // 0x11 INTPL (sf=1)
      0x0E80413u,  // 0x13 NCDS (sf=1, lm=1)
      0x1280414u,  // 0x14 CDP (sf=1, lm=1)
      0x0F80416u,  // 0x16 NCDT (sf=1, lm=1)
      0x108041Bu,  // 0x1B NCCS (sf=1, lm=1)
      0x138041Cu,  // 0x1C CC (sf=1, lm=1)
      0x0C8041Eu,  // 0x1E NCS (sf=1, lm=1)
      0x0D80420u,  // 0x20 NCT (sf=1, lm=1)
      0x0680029u,  // 0x29 DCPL (sf=1)
      0x0F8002Au,  // 0x2A DPCT (sf=1)
      0x190003Du,  // 0x3D GPF (sf=1)
      0x1A0003Eu,  // 0x3E GPL (sf=1)
      0x118043Fu,  // 0x3F NCCT (sf=1, lm=1)
    };
    int out_off = 4;
    for (uint32_t op : gte_ops) {
      b.GteCmd(op);
      b.Nop();
      b.Cfc2(V0, 31u);  // FLAG
      b.Mfc2(V1, 25u);  // MAC1
      b.Sw(V0, out_off, S6);
      b.Sw(V1, out_off + 4, S6);
      out_off += 8;
    }

    // SXYP FIFO push & SWC2 readback
    b.Li32(T0, 0x00100020u); b.Mtc2(T0, 15u);
    b.Li32(T0, 0x00300040u); b.Mtc2(T0, 15u);
    b.Swc2(12u, 32, S0);
    b.Swc2(13u, 36, S0);
    b.Swc2(14u, 40, S0);
    b.Swc2(15u, 44, S0);
    b.Lw(V0, 32, S0); b.Sw(V0, out_off, S6); out_off += 4;
    b.Lw(V0, 44, S0); b.Sw(V0, out_off, S6); out_off += 4;

    // IRGB <-> ORGB roundtrip + LZCS/LZCR edge cases
    b.Li32(T0, 0x00002A15u);
    b.Mtc2(T0, 28u);  // IRGB sets IR1..IR3
    b.Nop();
    b.Mfc2(V0, 29u);  // ORGB reads clamped 5-bit RGB from IR1..IR3
    b.Nop();
    b.Sw(V0, out_off, S6); out_off += 4;

    const uint32_t lz_inputs[] = {0u, 1u, 0xFFFFFFFFu, 0x80000000u, 0x7FFFFFFFu, 0x000F0000u};
    for (uint32_t v : lz_inputs) {
      b.Li32(T0, v);
      b.Mtc2(T0, 30u);  // LZCS
      b.Nop();
      b.Mfc2(V0, 31u);  // LZCR
      b.Nop();
      b.Sw(V0, out_off, S6);
      out_off += 4;
    }

    // FLAG manual write & error bit 31 auto-update
    b.Li32(T0, 0x40000000u);  // mac1_overflow -> sets bit 31
    b.Ctc2(T0, 31u);
    b.Nop();
    b.Cfc2(V0, 31u);
    b.Nop();
    b.Sw(V0, out_off, S6); out_off += 4;

    b.Li32(T0, 0x00001000u);  // ir0_saturated ONLY -> bit 31 = 0!
    b.Ctc2(T0, 31u);
    b.Nop();
    b.Cfc2(V0, 31u);
    b.Nop();
    b.Sw(V0, out_off, S6); out_off += 4;

    // Full 64-reg dump
    for (uint32_t r = 0; r < 32u; r++) {
      b.Mfc2(V0, r);
      b.Nop();
      b.Sw(V0, out_off, S6);
      out_off += 4;
    }
    for (uint32_t r = 0; r < 32u; r++) {
      b.Cfc2(V0, r);
      b.Nop();
      b.Sw(V0, out_off, S6);
      out_off += 4;
    }

    EmitDoneMagic(b, kMagicBase + 9u);
  }});

  // 10. timers_irq_controller
  tests.push_back({"timers_irq_controller", 4, kMagicBase + 10u, false, false, [](BiosBuilder& b) {
    EmitCommonPrologue(b);
    // Clear I_MASK, ack I_STAT
    b.SwImm32(0x1F801074u, 0x0000u);
    b.SwImm32(0x1F801070u, 0x0000u);

    // Timer 2: reset at target (bit 3), irq at target (bit 4), irq_repeat (bit 6), sysclk/8 (bit 9 = 0x200)
    // Mode = 0x0258, target = 16
    b.SwImm32(0x1F801128u, 16u);
    b.SwImm32(0x1F801124u, 0x0258u);
    b.SwImm32(0x1F801120u, 0u);

    // Enable I_MASK bit 6 (TMR2) = 0x40 (keep SR.IEc=0 so no CPU interrupt exception, just poll I_STAT)
    b.SwImm32(0x1F801074u, 0x0040u);

    // Busy loop for enough cycles that Timer 2 hits target 16 (16 * 8 = 128 sysclk ticks)
    b.Li32(T0, 120u);
    BiosBuilder::Label wait_tmr;
    b.Bind(wait_tmr);
    b.Addiu(T0, T0, -1);
    b.Bne(T0, ZERO, wait_tmr);
    b.Nop();

    b.Li32(S0, 0x1F801070u);
    b.Lw(V0, 0, S0);       // I_STAT should have TMR2 (bit 6) set
    b.Nop();
    b.Sw(V0, 4, S6);
    b.Li32(T1, ~0x40u);
    b.Sw(T1, 0, S0);       // Ack TMR2
    b.Lw(V1, 0, S0);       // I_STAT after ack
    b.Nop();
    b.Sw(V1, 8, S6);
    b.Li32(S1, 0x1F801124u);
    b.Lw(V0, 0, S1);       // Read TMR2 mode (clears reached_target bit 11 on read)
    b.Nop();
    b.Sw(V0, 12, S6);

    EmitDoneMagic(b, kMagicBase + 10u);
  }});

  // 11. dma_otc_and_gpu
  tests.push_back({"dma_otc_and_gpu", 4, kMagicBase + 11u, false, false, [](BiosBuilder& b) {
    EmitCommonPrologue(b);
    b.SwImm32(0x1F801814u, 0x00000000u);  // GP1 Reset GPU
    b.SwImm32(0x1F801814u, 0x04000002u);  // GP1 DMA dir = CPU->GP0
    b.SwImm32(0x1F8010F0u, 0x0F654B21u);  // DPCR: enable master bit (bit 3) for DMA2 & DMA6 (and DMA0..5 base)

    // Build an OTC 16-entry reverse table ending at 0x8003003C
    b.SwImm32(0x1F8010E0u, 0x0003003Cu);  // DMA6 MADR
    b.SwImm32(0x1F8010E4u, 16u);          // DMA6 BCR = 16 words
    b.SwImm32(0x1F8010E8u, 0x11000002u);  // DMA6 CHCR: start + busy + reverse

    b.Li32(S0, 0x1F8010E8u);
    b.Li32(T1, 0x01000000u);
    BiosBuilder::Label wait_otc;
    b.Bind(wait_otc);
    b.Lw(T0, 0, S0);
    b.Nop();
    b.And(T0, T0, T1);
    b.Bne(T0, ZERO, wait_otc);
    b.Nop();

    // Verify generated OTC table at 0x80030000..0x8003003C
    b.Li32(S1, 0x80030000u);
    for (int i = 0; i < 16; i++) {
      b.Lw(V0, i * 4, S1);
      b.Nop();
      b.Sw(V0, 4 + i * 4, S6);
    }

    // Build a tiny GPU linked-list packet at 0x80031000 -> 0x80031020 -> end (0x00FFFFFF)
    b.SwImm32(0x80031000u, (4u << 24) | 0x00031020u);
    b.SwImm32(0x80031004u, 0xE1000400u);
    b.SwImm32(0x80031008u, 0x02112233u);  // FillRect
    b.SwImm32(0x8003100Cu, 0x00100010u);
    b.SwImm32(0x80031010u, 0x00200020u);

    b.SwImm32(0x80031020u, (1u << 24) | 0x00FFFFFFu);
    b.SwImm32(0x80031024u, 0xE6000001u);

    b.SwImm32(0x1F8010A0u, 0x00031000u);  // DMA2 MADR
    b.SwImm32(0x1F8010A4u, 0u);
    b.SwImm32(0x1F8010A8u, 0x01000401u);  // DMA2 CHCR: busy, linked-list, RAM->GPU

    b.Li32(S0, 0x1F8010A8u);
    b.Li32(T1, 0x01000000u);
    BiosBuilder::Label wait_gpu_dma;
    b.Bind(wait_gpu_dma);
    b.Lw(T0, 0, S0);
    b.Nop();
    b.And(T0, T0, T1);
    b.Bne(T0, ZERO, wait_gpu_dma);
    b.Nop();

    // Wait until GPU finishes FillRect + E6 command (ready_for_cmd bit 26 & ready_for_dma bit 28)
    b.Li32(S2, 0x1F801814u);
    b.Li32(T2, 0x14000000u);
    BiosBuilder::Label wait_gpu_idle;
    b.Bind(wait_gpu_idle);
    b.Lw(V0, 0, S2);
    b.Nop();
    b.And(T0, V0, T2);
    b.Bne(T0, T2, wait_gpu_idle);
    b.Nop();

    b.Li32(T3, 0x7FFFFFFFu);  // mask out bit 31 interlace/odd-line parity (cycle-phase dependent)
    b.And(V0, V0, T3);
    b.Sw(V0, 68, S6);

    EmitDoneMagic(b, kMagicBase + 11u);
  }});

  // 12. spu_ram_and_voices
  tests.push_back({"spu_ram_and_voices", 4, kMagicBase + 12u, false, false, [](BiosBuilder& b) {
    EmitCommonPrologue(b);
    b.Li32(S0, 0x1F801C00u);
    // Enable SPU, unmuted, stop transfer first
    b.Li32(T0, 0xC000u);
    b.Sh(T0, 0x1AA, S0);  // SPUCNT
    b.Li32(T0, 0x0004u);
    b.Sh(T0, 0x1AC, S0);  // transfer control

    // Write 16 halfwords to SPU RAM at 0x1000 * 8 = 0x8000 via manual FIFO write
    b.Li32(T0, 0x1000u);
    b.Sh(T0, 0x1A6, S0);  // transfer addr
    for (int i = 0; i < 16; i++) {
      b.Li32(T0, 0xA500u + static_cast<uint32_t>(i) * 17u);
      b.Sh(T0, 0x1A8, S0);
    }
    // Select ManualWrite mode (bits 5:4 = 1 -> 0xC010)
    b.Li32(T0, 0xC010u);
    b.Sh(T0, 0x1AA, S0);

    // Wait for SPUSTAT busy bit (bit 10) to clear (insert Nop for MIPS load delay!)
    BiosBuilder::Label wait_spu;
    b.Bind(wait_spu);
    b.Lhu(T0, 0x1AE, S0);
    b.Nop();
    b.Andi(T0, T0, 0x0400u);
    b.Bne(T0, ZERO, wait_spu);
    b.Nop();

    // Configure Voice 0 registers + key-on, read back regs + SPUSTAT
    b.Li32(T0, 0x3FFFu);
    b.Sh(T0, 0x00, S0);
    b.Sh(T0, 0x02, S0);
    b.Li32(T0, 0x1000u);
    b.Sh(T0, 0x04, S0);
    b.Sh(T0, 0x06, S0);
    b.Li32(T0, 0x80FFu);
    b.Sh(T0, 0x08, S0);
    b.Li32(T0, 0x0001u);
    b.Sw(T0, 0x188, S0);  // Key on voice 0

    b.Lhu(V0, 0x1AA, S0); b.Nop(); b.Sw(V0, 4, S6);
    b.Lhu(V0, 0x1AE, S0); b.Nop(); b.Sw(V0, 8, S6);
    b.Lhu(V0, 0x04, S0);  b.Nop(); b.Sw(V0, 12, S6);
    b.Lhu(V0, 0x06, S0);  b.Nop(); b.Sw(V0, 16, S6);

    EmitDoneMagic(b, kMagicBase + 12u);
  }});

  // 13. mdec_decode (quant + scale tables + 24-bit macroblock decode via CPU I/O)
  tests.push_back({"mdec_decode", 4, kMagicBase + 13u, false, false, [](BiosBuilder& b) {
    EmitCommonPrologue(b);
    b.Li32(S0, 0x1F801820u);  // MDEC data/cmd (0), ctrl/stat (4)
    b.Li32(T0, 0x80000000u);
    b.Sw(T0, 4, S0);          // Soft reset

    // Command 2: SetIqTab (Y + UV = 32 words = 64 bytes)
    b.Li32(T0, 0x40000001u);
    b.Sw(T0, 0, S0);
    for (uint32_t i = 0; i < 32u; i++) {
      b.Li32(T0, 0x08080808u + (i & 7u) * 0x01010101u);
      b.Sw(T0, 0, S0);
    }

    // Command 3: SetScale (32 words = 64 halfwords)
    b.Li32(T0, 0x60000000u);
    b.Sw(T0, 0, S0);
    for (uint32_t i = 0; i < 32u; i++) {
      uint32_t val = (i == 0 || i == 4 || i == 8) ? 0x20002000u : 0x0400FC00u;
      b.Li32(T0, val);
      b.Sw(T0, 0, S0);
    }

    // Command 1: DecodeMacroblock 24-bit unsigned (depth=2 -> bits 28:27 = 2 -> 0x30000000), 12 words (6 blocks * 2 words)
    b.Li32(T0, 0x3000000Cu);
    b.Sw(T0, 0, S0);
    // Feed 6 minimal blocks: each block has DC q=8/dc=0x20 (0x2020) + EOB (0xFE00) -> 0xFE002020 + 0xFE00FE00
    for (int blk = 0; blk < 6; blk++) {
      b.Li32(T0, 0xFE001020u + static_cast<uint32_t>(blk * 4));
      b.Sw(T0, 0, S0);
      b.Li32(T0, 0xFE00FE00u);
      b.Sw(T0, 0, S0);
    }

    // Poll MDECSTAT bit 31 (data_out_fifo_empty) until CopyOutBlock fires (Nop for load delay!), then read 192 words
    BiosBuilder::Label wait_mdec_ready;
    b.Bind(wait_mdec_ready);
    b.Lw(V0, 4, S0);
    b.Nop();
    b.Bltz(V0, wait_mdec_ready);
    b.Nop();

    b.Li32(T2, 192u);
    b.Li32(T3, 0u);  // rolling hash
    BiosBuilder::Label mdec_read;
    b.Bind(mdec_read);
    b.Lw(V0, 0, S0);
    b.Sll(T5, T3, 5);
    b.Addu(T3, T3, T5);
    b.Xor(T3, T3, V0);
    b.Addiu(T2, T2, -1);
    b.Bne(T2, ZERO, mdec_read);
    b.Nop();

    b.Sw(T3, 4, S6);
    b.Lw(V1, 4, S0);  // MDEC status after drain
    b.Nop();
    b.Sw(V1, 8, S6);

    EmitDoneMagic(b, kMagicBase + 13u);
  }});

  // 14. fastmem_mixed_io_stress (RAM, Scratchpad, BIOS, MMIO interleaved in hot loop)
  tests.push_back({"fastmem_mixed_io_stress", 4, kMagicBase + 14u, false, false, [](BiosBuilder& b) {
    EmitCommonPrologue(b);
    b.Li32(S0, 0x80018000u);  // RAM buffer
    b.Li32(S1, 0x1F800200u);  // Scratchpad
    b.Li32(S2, 0xBFC00000u);  // BIOS ROM
    b.Li32(S3, 0x1F801074u);  // I_MASK MMIO

    // Dynamically compute pointer in loop that alternates RAM <-> Scratchpad <-> BIOS <-> MMIO
    b.Li32(T0, 0u);  // idx
    b.Li32(T1, 64u);
    b.Li32(T2, 0x13579BDFu);  // checksum
    BiosBuilder::Label loop;
    b.Bind(loop);
    b.Sll(T3, T0, 2);
    b.Addu(T4, S0, T3);
    b.Sw(T2, 0, T4);          // RAM write
    b.Lw(V0, 0, T4);          // RAM read
    b.Addu(T5, S1, T3);
    b.Sw(V0, 0, T5);          // Scratchpad write
    b.Lw(V1, 0, T5);          // Scratchpad read
    b.Addu(A0, S2, T3);
    b.Lw(A1, 0, A0);          // BIOS ROM read
    b.Sw(T0, 0, S3);          // I_MASK write (MMIO)
    b.Lw(A2, 0, S3);          // I_MASK read (MMIO)
    b.Xor(T2, T2, V1);
    b.Addu(T2, T2, A1);
    b.Xor(T2, T2, A2);
    b.Addiu(T0, T0, 1);
    b.Bne(T0, T1, loop);
    b.Nop();

    b.Sw(ZERO, 0, S3);
    b.Sw(T2, 4, S6);

    EmitDoneMagic(b, kMagicBase + 14u);
  }});

  return tests;
}

bool RunInstance(EcrApi& api, const std::string& sys_dir, const std::string& bios_name,
                 const CpuTargetConfig& target, uint32_t frames, const std::string& game_path,
                 CpuCapture* out) {
  EnvConfig cfg;
  cfg.sys_dir = sys_dir;
  cfg.bios_file = bios_name;
  cfg.target = target;
  g_env = &cfg;

  api.retro_set_environment(EnvironmentCallback);
  api.retro_set_video_refresh(VideoCallback);
  api.retro_set_audio_sample([](int16_t, int16_t) {});
  api.retro_set_audio_sample_batch([](const int16_t*, size_t n) { return n; });
  api.retro_set_input_poll([]() {});
  api.retro_set_input_state([](unsigned, unsigned, unsigned, unsigned) { return int16_t(0); });

  api.retro_init();
  retro_game_info gi{};
  gi.path = game_path.empty() ? nullptr : game_path.c_str();
  if (!api.retro_load_game(game_path.empty() ? nullptr : &gi)) {
    std::fprintf(stderr, "retro_load_game(%s) failed for target=%s\n",
                 game_path.empty() ? "BIOS" : game_path.c_str(), target.name);
    api.retro_deinit();
    g_env = nullptr;
    return false;
  }

  for (uint32_t f = 0; f < frames; f++) api.retro_run();

  const void* ram = api.retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
  const size_t ram_sz = api.retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
  if (!ram || ram_sz < 2u * 1024u * 1024u) {
    std::fprintf(stderr, "Missing 2 MiB PS1 RAM for target=%s (sz=%zu)\n", target.name, ram_sz);
    api.retro_unload_game();
    api.retro_deinit();
    g_env = nullptr;
    return false;
  }

  out->ram_bytes = ram_sz;
  out->ram_hash = Fnv1a64(ram, 2u * 1024u * 1024u);
  out->kernel_vectors_hash = Fnv1a64(ram, 0x100u);
  out->frame_hash = cfg.last_frame.empty() ? 0ULL : Fnv1a64(cfg.last_frame.data(), cfg.last_frame.size());
  out->results.resize(kResultWords);
  std::memcpy(out->results.data(), static_cast<const uint8_t*>(ram) + kResultRamOffset,
              kResultWords * sizeof(uint32_t));

  api.retro_unload_game();
  api.retro_deinit();
  g_env = nullptr;
  return true;
}

bool CopyBinaryFile(const std::string& src, const std::string& dst) {
  FILE* in = std::fopen(src.c_str(), "rb");
  if (!in) return false;
  FILE* out = std::fopen(dst.c_str(), "wb");
  if (!out) { std::fclose(in); return false; }
  std::vector<uint8_t> buf(64 * 1024);
  size_t n = 0;
  while ((n = std::fread(buf.data(), 1, buf.size(), in)) > 0) {
    if (std::fwrite(buf.data(), 1, n, out) != n) {
      std::fclose(in); std::fclose(out); return false;
    }
  }
  std::fclose(in);
  std::fclose(out);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "Usage: %s <libecr_android.so> [--bios <path>] [--frames <N>] [--game <path>] [--game-frames <N>]\n", argv[0]);
    return 2;
  }

  const char* core_path = argv[1];
  std::string real_bios_path;
  std::string game_path;
  uint32_t real_bios_frames = 120u;
  uint32_t game_frames = 180u;
  for (int i = 2; i < argc; i++) {
    if (std::strcmp(argv[i], "--bios") == 0 && i + 1 < argc) {
      real_bios_path = argv[++i];
    } else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
      real_bios_frames = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
      if (real_bios_frames == 0u) real_bios_frames = 120u;
    } else if (std::strcmp(argv[i], "--game") == 0 && i + 1 < argc) {
      game_path = argv[++i];
    } else if (std::strcmp(argv[i], "--game-frames") == 0 && i + 1 < argc) {
      game_frames = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
      if (game_frames == 0u) game_frames = 180u;
    }
  }

  EcrApi api;
  if (!api.Load(core_path)) return 2;

  char tmp_template[] = "/data/local/tmp/ecr-cpu-oracle-XXXXXX";
  char* tmp_dir = mkdtemp(tmp_template);
  if (!tmp_dir) { std::perror("mkdtemp"); return 2; }
  const std::string sys_dir(tmp_dir);
  const std::string bios_file = "oracle_bios.bin";
  const std::string bios_full = sys_dir + "/" + bios_file;

  const CpuTargetConfig golden_cfg = {"Interpreter", "Interpreter", "Disabled", true, false, false, false, false, false};
  const CpuTargetConfig golden_icache_cfg = {"CachedInterp_IC", "CachedInterpreter", "Disabled", true, true, true, false, false, true};

  const std::vector<CpuTargetConfig> targets = {
    {"CachedInterpreter", "CachedInterpreter", "Disabled", true,  true,  false, false, false, false},
    {"Recompiler_Safe",   "Recompiler",        "Disabled", true,  true,  false, false, false, false},
    {"Recompiler_LUT",    "Recompiler",        "LUT",      false, true,  false, false, false, false},
    {"Recompiler_MMap",   "Recompiler",        "MMap",     false, true,  false, false, false, false},
    {"Recompiler_ICache", "Recompiler",        "MMap",     false, true,  true,  false, false, true},
    {"Recompiler_PGXP_M", "Recompiler",        "MMap",     false, true,  false, true,  false, false},
    {"Recompiler_PGXP_C", "Recompiler",        "MMap",     false, true,  false, true,  true,  false},
  };

  const auto suite = BuildSuite();
  int total_checks = 0;
  int total_failures = 0;

  std::printf("=== EmuCoreR ECR Expanded CPU Accuracy Oracle (Golden: Interpreter) ===\n");
  for (const auto& t : suite) {
    BiosBuilder builder;
    t.build(builder);
    if (!builder.WriteFile(bios_full)) {
      std::fprintf(stderr, "Failed writing BIOS for %s\n", t.name);
      return 2;
    }

    CpuCapture golden;
    if (!RunInstance(api, sys_dir, bios_file, t.compare_codecache_only ? targets[0] : golden_cfg,
                     t.frames, "", &golden)) {
      std::fprintf(stderr, "FAIL [%s] Golden %s failed to run\n", t.name,
                   t.compare_codecache_only ? "CachedInterpreter" : "Interpreter");
      total_failures++;
      continue;
    }
    if (golden.results[0] != t.magic) {
      std::fprintf(stderr, "FAIL [%s] Golden %s magic=0x%08X expected=0x%08X\n",
                   t.name, t.compare_codecache_only ? "CachedInterpreter" : "Interpreter",
                   golden.results[0], t.magic);
      total_failures++;
      continue;
    }

    CpuCapture golden_icache;
    bool have_icache_golden = false;

    for (size_t idx = t.compare_codecache_only ? 1u : 0u; idx < targets.size(); idx++) {
      const auto& mode = targets[idx];
      if (t.requires_mem_exceptions && !mode.memory_exceptions) continue;
      if (t.compare_codecache_only && mode.icache) continue;
      total_checks++;

      const CpuCapture* ref = &golden;
      if (!t.compare_codecache_only && mode.use_icache_golden) {
        if (!have_icache_golden) {
          have_icache_golden = RunInstance(api, sys_dir, bios_file, golden_icache_cfg, t.frames, "", &golden_icache);
        }
        if (!have_icache_golden) {
          std::printf("FAIL %-25s mode=%-18s (icache golden error)\n", t.name, mode.name);
          total_failures++;
          continue;
        }
        ref = &golden_icache;
      }

      CpuCapture cand;
      if (!RunInstance(api, sys_dir, bios_file, mode, t.frames, "", &cand)) {
        std::printf("FAIL %-25s mode=%-18s (run error)\n", t.name, mode.name);
        total_failures++;
        continue;
      }
      int first_diff = -1;
      for (size_t w = 0; w < kResultWords; w++) {
        if (ref->results[w] != cand.results[w]) { first_diff = static_cast<int>(w); break; }
      }
      const bool ram_exact = (ref->ram_hash == cand.ram_hash) && (first_diff < 0);
      if (ram_exact) {
        std::printf("PASS %-25s mode=%-18s magic=0x%08X ram_hash=0x%016llX\n",
                    t.name, mode.name, cand.results[0], static_cast<unsigned long long>(cand.ram_hash));
      } else {
        std::printf("FAIL %-25s mode=%-18s diff_word=%d ref=0x%08X cand=0x%08X ram_ref=0x%016llX ram_cand=0x%016llX\n",
                    t.name, mode.name, first_diff,
                    first_diff >= 0 ? ref->results[first_diff] : 0u,
                    first_diff >= 0 ? cand.results[first_diff] : 0u,
                    static_cast<unsigned long long>(ref->ram_hash),
                    static_cast<unsigned long long>(cand.ram_hash));
        total_failures++;
      }
    }
  }

  if (!real_bios_path.empty()) {
    const std::string real_bios_dst = sys_dir + "/real_bios.bin";
    if (!CopyBinaryFile(real_bios_path, real_bios_dst)) {
      std::fprintf(stderr, "FAIL [real_bios_lockstep] Cannot read BIOS: %s\n", real_bios_path.c_str());
      total_failures++;
    } else {
      CpuCapture golden, cached;
      if (!RunInstance(api, sys_dir, "real_bios.bin", golden_cfg, real_bios_frames, "", &golden) ||
          !RunInstance(api, sys_dir, "real_bios.bin", targets[0], real_bios_frames, "", &cached)) {
        std::fprintf(stderr, "FAIL [real_bios_lockstep] BIOS golden/cached run failed\n");
        total_failures++;
      } else {
        total_checks++;
        const bool boot_ok = (golden.kernel_vectors_hash == cached.kernel_vectors_hash) &&
                             (golden.kernel_vectors_hash != 0ULL) && (golden.ram_hash != 0ULL);
        std::printf("%s %-25s mode=%-18s frames=%u vectors=0x%016llX (interp=0x%016llX)\n",
                    boot_ok ? "PASS" : "FAIL",
                    "real_bios_kernel_boot", targets[0].name, real_bios_frames,
                    static_cast<unsigned long long>(cached.kernel_vectors_hash),
                    static_cast<unsigned long long>(golden.kernel_vectors_hash));
        if (!boot_ok) total_failures++;

        for (size_t idx = 1; idx < targets.size(); idx++) {
          const auto& mode = targets[idx];
          if (mode.icache) continue;  // ICache timing differs from non-icache CachedInterpreter
          total_checks++;
          CpuCapture cand;
          if (!RunInstance(api, sys_dir, "real_bios.bin", mode, real_bios_frames, "", &cand)) {
            std::printf("FAIL %-25s mode=%-18s (run error)\n", "real_bios_lockstep", mode.name);
            total_failures++;
            continue;
          }
          const bool lockstep_ok = (cached.ram_hash == cand.ram_hash) &&
                                   (cached.frame_hash == cand.frame_hash) &&
                                   (cand.frame_hash != 0ULL);
          std::printf("%s %-25s mode=%-18s frames=%u ram=0x%016llX fb=0x%016llX (cached ram=0x%016llX fb=0x%016llX)\n",
                      lockstep_ok ? "PASS" : "FAIL",
                      "real_bios_lockstep", mode.name, real_bios_frames,
                      static_cast<unsigned long long>(cand.ram_hash),
                      static_cast<unsigned long long>(cand.frame_hash),
                      static_cast<unsigned long long>(cached.ram_hash),
                      static_cast<unsigned long long>(cached.frame_hash));
          if (!lockstep_ok) total_failures++;
        }
      }

      if (!game_path.empty()) {
        CpuCapture game_cached;
        if (!RunInstance(api, sys_dir, "real_bios.bin", targets[0], game_frames, game_path, &game_cached)) {
          std::fprintf(stderr, "FAIL [real_game_lockstep] CachedInterpreter game run failed: %s\n", game_path.c_str());
          total_failures++;
        } else {
          for (size_t idx = 1; idx < targets.size(); idx++) {
            const auto& mode = targets[idx];
            if (mode.icache) continue;
            total_checks++;
            CpuCapture cand;
            if (!RunInstance(api, sys_dir, "real_bios.bin", mode, game_frames, game_path, &cand)) {
              std::printf("FAIL %-25s mode=%-18s (run error)\n", "real_game_lockstep", mode.name);
              total_failures++;
              continue;
            }
            const bool lockstep_ok = (game_cached.ram_hash == cand.ram_hash) &&
                                     (game_cached.frame_hash == cand.frame_hash) &&
                                     (cand.frame_hash != 0ULL);
            std::printf("%s %-25s mode=%-18s frames=%u ram=0x%016llX fb=0x%016llX (cached ram=0x%016llX fb=0x%016llX)\n",
                        lockstep_ok ? "PASS" : "FAIL",
                        "real_game_lockstep", mode.name, game_frames,
                        static_cast<unsigned long long>(cand.ram_hash),
                        static_cast<unsigned long long>(cand.frame_hash),
                        static_cast<unsigned long long>(game_cached.ram_hash),
                        static_cast<unsigned long long>(game_cached.frame_hash));
            if (!lockstep_ok) total_failures++;
          }
        }
      }
      std::remove(real_bios_dst.c_str());
    }
  }

  std::remove(bios_full.c_str());
  rmdir(tmp_dir);
  std::printf("CPU Oracle Summary: %d comparisons, %d failures\n", total_checks, total_failures);
  return total_failures == 0 ? 0 : 1;
}
