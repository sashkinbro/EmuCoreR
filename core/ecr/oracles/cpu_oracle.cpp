// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: GPL-3.0-or-later
//
// EmuCoreR CPU Accuracy Oracle:
//   Golden reference : Interpreter
//   Targets checked  : CachedInterpreter, Recompiler (ARM64 JIT)
//
// Exercises ALU/overflow, MUL/DIV edges, unaligned accesses/load delays/scratchpad,
// branches & delay slots (including branch/exception-in-delay-slot), COP0/RFE,
// self-modifying code JIT invalidation, COP2/GTE matrix/color ops, and optional
// real-BIOS lockstep execution.
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
constexpr uint32_t kResultWords = 256u;

struct EnvConfig {
  std::string sys_dir;
  std::string cpu_mode;
  std::string bios_file = "oracle_bios.bin";
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
      else if (key == "ecr_CPU_ExecutionMode") val = g_env->cpu_mode.c_str();
      else if (key == "ecr_CPU_RecompilerBlockLinking") val = "true";
      else if (key == "ecr_CPU_RecompilerICache") val = "false";
      else if (key == "ecr_CPU_RecompilerMemoryExceptions") val = "true";
      else if (key == "ecr_CPU_FastmemMode") val = "Disabled";
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
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
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
//   [S7] = CAUSE, [S7+4] = EPC, [S7+8] = BADVADDR, [S7+12] = SR,
// increments S7 by 16, and advances EPC past the faulting instruction (4 bytes unless BD is set, +8).
// Returns safely via COP0 EPC+RFE so tests can intentionally trigger & inspect traps!
void EmitExceptionRecorderAt180(BiosBuilder& b) {
  b.Seek(0x180u);
  b.Mfc0(K0, 13u);  // CAUSE
  b.Mfc0(K1, 14u);  // EPC
  // Mask out debug-only CAUSE.BT (bit 30) while preserving architectural CAUSE.BD (bit 31), CE, IP, and ExCode
  b.Li32(AT, 0xBFFFFFFFu);
  b.And(K0, K0, AT);
  b.Sw(K0, 0, S7);
  b.Sw(K1, 4, S7);
  b.Mfc0(AT, 8u);   // BADVADDR
  b.Mfc0(V1, 12u);  // SR
  b.Sw(AT, 8, S7);
  b.Sw(V1, 12, S7);
  b.Addiu(S7, S7, 16);
  // Check CAUSE.BD (bit 31 < 0): skip +8 if inside delay slot, else +4
  b.Bgez(K0, 2);
  b.Nop();
  b.Addiu(K1, K1, 4);
  b.Addiu(K1, K1, 4);
  b.Jr(K1);
  b.Rfe();
}

void EmitCommonPrologue(BiosBuilder& b) {
  EmitExceptionRecorderAt180(b);
  b.Seek(0x0u);
  b.J(0xBFC00400u);
  b.Nop();
  b.Seek(0x400u);
  b.Li32(S6, 0x80010000u);  // S6 = result base pointer
  b.Li32(S7, 0x80010200u);  // S7 = exception log base pointer
  // Keep BEV=1 (0x00400000) and enable COP2 (0x40000000)
  b.Li32(T0, 0x40400000u);
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
  std::function<void(BiosBuilder&)> build;
};

std::vector<SyntheticTest> BuildSuite() {
  std::vector<SyntheticTest> tests;

  // 1. alu_shifts_overflow
  tests.push_back({"alu_shifts_overflow", 4, kMagicBase + 1u, [](BiosBuilder& b) {
    EmitCommonPrologue(b);
    b.Li32(T0, 0x7FFFFFF0u);
    b.Li32(T1, 0x00000020u);
    b.Li32(T2, 0x80000000u);
    b.Li32(T3, 0xFFFFFFFFu);

    b.Addu(V0, T0, T1);  b.Sw(V0, 4, S6);
    b.Subu(V0, T2, T3);  b.Sw(V0, 8, S6);
    b.And(V0, T0, T1);   b.Sw(V0, 12, S6);
    b.Or(V0, T0, T2);    b.Sw(V0, 16, S6);
    b.Xor(V0, T0, T3);   b.Sw(V0, 20, S6);
    b.Nor(V0, T0, T1);   b.Sw(V0, 24, S6);
    b.Slt(V0, T2, T0);   b.Sw(V0, 28, S6);  // 1 (signed min < pos)
    b.Sltu(V0, T2, T0);  b.Sw(V0, 32, S6);  // 0 (unsigned)
    b.Slti(V0, T3, 0);   b.Sw(V0, 36, S6);
    b.Sltiu(V0, T3, 1);  b.Sw(V0, 40, S6);

    // Constant & variable shifts (note sa masked to 5 bits in MIPS)
    b.Li32(T4, 0x81234567u);
    b.Li32(T5, 0x00000105u);  // low 5 bits = 5
    b.Sll(V0, T4, 7);    b.Sw(V0, 44, S6);
    b.Srl(V0, T4, 7);    b.Sw(V0, 48, S6);
    b.Sra(V0, T4, 7);    b.Sw(V0, 52, S6);
    b.Sllv(V0, T4, T5);  b.Sw(V0, 56, S6);
    b.Srlv(V0, T4, T5);  b.Sw(V0, 60, S6);
    b.Srav(V0, T4, T5);  b.Sw(V0, 64, S6);

    // Non-overflow ADD/ADDI/SUB
    b.Li32(V0, 12345);
    b.Addi(V0, V0, -5432); b.Sw(V0, 68, S6);
    b.Add(V0, V0, T1);     b.Sw(V0, 72, S6);
    b.Sub(V0, V0, T1);     b.Sw(V0, 76, S6);

    // Trapping ADD/ADDI/SUB overflow -> sentinel V0 stays unchanged on trap
    b.Li32(V0, 0x11223344u);
    b.Li32(A0, 0x7FFFFFFFu);
    b.Li32(A1, 1u);
    b.Add(V0, A0, A1);     // traps Ov! V0 must stay 0x11223344
    b.Sw(V0, 80, S6);
    b.Addi(V0, A0, 10);    // traps Ov!
    b.Sw(V0, 84, S6);
    b.Li32(A0, 0x80000000u);
    b.Sub(V0, A0, A1);     // traps Ov!
    b.Sw(V0, 88, S6);

    EmitDoneMagic(b, kMagicBase + 1u);
  }});

  // 2. mul_div_edges
  tests.push_back({"mul_div_edges", 4, kMagicBase + 2u, [](BiosBuilder& b) {
    EmitCommonPrologue(b);
    auto StoreHiLo = [&](int off) {
      b.Mfhi(V0);
      b.Mflo(V1);
      b.Sw(V0, off, S6);
      b.Sw(V1, off + 4, S6);
    };

    // Negative signed MULT & large MULTU
    b.Li32(T0, 0x80000001u);
    b.Li32(T1, 0xFFFFFFFDu);  // -3
    b.Mult(T0, T1);  StoreHiLo(4);
    b.Multu(T0, T1); StoreHiLo(12);

    // Normal signed/unsigned DIV
    b.Li32(T0, 0xFFFFFF9Bu);  // -101
    b.Li32(T1, 7u);
    b.Div(T0, T1);   StoreHiLo(20);
    b.Divu(T0, T1);  StoreHiLo(28);

    // Edge case 1: divide positive & negative by ZERO
    b.Li32(T0, 12345u);
    b.Div(T0, ZERO);  StoreHiLo(36);
    b.Li32(T0, 0xFFFFFFF0u);  // -16
    b.Div(T0, ZERO);  StoreHiLo(44);
    b.Li32(T0, 12345u);
    b.Divu(T0, ZERO); StoreHiLo(52);

    // Edge case 2: INT_MIN / -1 signed overflow
    b.Li32(T0, 0x80000000u);
    b.Li32(T1, 0xFFFFFFFFu);
    b.Div(T0, T1);    StoreHiLo(60);

    // MTHI / MTLO roundtrip
    b.Li32(T0, 0xAABBCCDDu);
    b.Li32(T1, 0x11223344u);
    b.Mthi(T0);
    b.Mtlo(T1);
    b.Nop(); b.Nop();
    StoreHiLo(68);

    EmitDoneMagic(b, kMagicBase + 2u);
  }});

  // 3. unaligned_mem_scratchpad
  tests.push_back({"unaligned_mem_scratchpad", 4, kMagicBase + 3u, [](BiosBuilder& b) {
    EmitCommonPrologue(b);
    b.Li32(S0, 0x80010400u);  // unaligned RAM buffer
    b.Li32(S1, 0x1F800000u);  // PS1 1 KiB Scratchpad

    b.Li32(T0, 0x11223344u);
    b.Li32(T1, 0x55667788u);
    b.Sw(T0, 0, S0);
    b.Sw(T1, 4, S0);

    // LB / LBU / LH / LHU across offsets 0..3
    for (int i = 0; i < 4; i++) {
      b.Lb(V0, i, S0);   b.Sw(V0, 4 + i * 16, S6);
      b.Lbu(V1, i, S0);  b.Sw(V1, 8 + i * 16, S6);
    }
    b.Lh(V0, 0, S0); b.Sw(V0, 68, S6);
    b.Lhu(V1, 2, S0); b.Sw(V1, 72, S6);

    // LWL / LWR at every byte alignment 0..3
    for (int align = 0; align < 4; align++) {
      b.Li32(V0, 0xAABBCCDDu);
      b.Lwl(V0, align, S0);
      b.Nop();
      b.Sw(V0, 76 + align * 8, S6);

      b.Li32(V1, 0xEEFF0011u);
      b.Lwr(V1, align, S0);
      b.Nop();
      b.Sw(V1, 80 + align * 8, S6);
    }

    // SWL / SWR into S0+16..S0+48
    for (int align = 0; align < 4; align++) {
      b.Li32(T2, 0x01020304u + static_cast<uint32_t>(align) * 0x11111111u);
      b.Sw(ZERO, 16 + align * 8, S0);
      b.Sw(ZERO, 20 + align * 8, S0);
      b.Swl(T2, 16 + align * 8 + align, S0);
      b.Swr(T2, 16 + align * 8 + align, S0);
      b.Lw(V0, 16 + align * 8, S0);
      b.Sw(V0, 108 + align * 8, S6);
      b.Lw(V1, 20 + align * 8, S0);
      b.Sw(V1, 112 + align * 8, S6);
    }

    // Scratchpad read/write + SB/SH
    b.Li32(T3, 0xCAFEBABEu);
    b.Sw(T3, 0x40, S1);
    b.Sh(T3, 0x44, S1);
    b.Sb(T3, 0x46, S1);
    b.Lw(V0, 0x40, S1);
    b.Lw(V1, 0x44, S1);
    b.Sw(V0, 140, S6);
    b.Sw(V1, 144, S6);

    // Classic MIPS R3000A load-delay slot: value before load completes
    b.Li32(T4, 0x12345678u);
    b.Sw(T4, 0x80, S1);
    b.Li32(A0, 0x0F0E0D0Cu);
    b.Lw(A0, 0x80, S1);
    b.Addu(A1, A0, ZERO);  // On R3000A sees old A0 unless interlocked (checks exact ECR model)
    b.Nop();
    b.Sw(A1, 148, S6);
    b.Sw(A0, 152, S6);

    EmitDoneMagic(b, kMagicBase + 3u);
  }});

  // 4. branches_delay_slots
  tests.push_back({"branches_delay_slots", 4, kMagicBase + 4u, [](BiosBuilder& b) {
    EmitCommonPrologue(b);
    b.Li32(V0, 0u);
    b.Li32(T0, 1u);
    b.Li32(T1, 0xFFFFFFFFu);  // -1

    // BEQ taken + delay slot
    b.Beq(T0, T0, 2);
    b.Addiu(V0, V0, 1);
    b.Addiu(V0, V0, 100);  // skipped
    b.Sw(V0, 4, S6);

    // BNE, BLEZ, BGTZ, BLTZ, BGEZ, BLTZAL, BGEZAL
    b.Bne(T0, T1, 2);
    b.Ori(V0, V0, 0x10u);
    b.Ori(V0, V0, 0x200u);  // skipped

    b.Blez(T1, 2);
    b.Addiu(V0, V0, 2);
    b.Addiu(V0, V0, 200);

    b.Bgtz(T0, 2);
    b.Addiu(V0, V0, 4);
    b.Addiu(V0, V0, 400);

    b.Bltz(T1, 2);
    b.Addiu(V0, V0, 8);
    b.Addiu(V0, V0, 800);

    b.Bgez(ZERO, 2);
    b.Addiu(V0, V0, 16);
    b.Addiu(V0, V0, 1600);
    b.Sw(V0, 8, S6);

    // BGEZAL / JALR
    b.Bgezal(ZERO, 2);
    b.Addiu(V0, V0, 1);
    b.Addiu(V0, V0, 32);
    b.Sw(RA, 12, S6);

    // Exception inside a branch delay slot -> CAUSE.BD (0x80000000) & EPC = branch PC
    b.Li32(A0, 0x7FFFFFFFu);
    b.Li32(A1, 1u);
    b.Beq(ZERO, ZERO, 2);
    b.Add(A2, A0, A1);  // traps Ov IN DELAY SLOT!
    b.Addiu(V0, V0, 500);
    b.Sw(V0, 16, S6);

    // JR back via saved link
    const uint32_t target = b.CurrentAddr() + 20u;
    b.Li32(T3, target);
    b.Jalr(S2, T3);
    b.Addiu(V0, V0, 3);
    b.Addiu(V0, V0, 777);  // skipped if JALR jumped cleanly? Wait target is +20 (= 5 insns):
    // Li32=2, Jalr=1, Delay=1, Skipped=1 => 5*4=20, so JALR lands after Skipped!
    b.Sw(S2, 20, S6);
    b.Sw(V0, 24, S6);

    EmitDoneMagic(b, kMagicBase + 4u);
  }});

  // 5. cop0_exceptions_rfe
  tests.push_back({"cop0_exceptions_rfe", 4, kMagicBase + 5u, [](BiosBuilder& b) {
    EmitCommonPrologue(b);
    // SYSCALL, BREAK, reserved opcode, unaligned LW (AdEL), unaligned SW (AdES)
    b.Syscall(0x123u);
    b.Break(0x456u);
    b.Emit(0x7C00003Bu);  // Reserved instruction (SPECIAL3 / 0x1F)

    b.Li32(T0, 0x80010501u);  // misaligned byte address across block boundary so T0 is a dynamic HostReg
    b.Beq(ZERO, ZERO, 1);
    b.Nop();
    b.Lw(T1, 0, T0);          // AdEL
    b.Sw(T2, 0, T0);          // AdES

    // Verify SR status bits restored after RFE
    b.Mfc0(V0, 12u);
    b.Sw(V0, 4, S6);

    EmitDoneMagic(b, kMagicBase + 5u);
  }});

  // 6. smc_jit_invalidation
  tests.push_back({"smc_jit_invalidation", 4, kMagicBase + 6u, [](BiosBuilder& b) {
    EmitCommonPrologue(b);
    // Copy tiny RAM function at 0x80020000:
    //   addiu v0, a0, 100
    //   jr ra
    //   nop
    b.Li32(S0, 0x80020000u);
    b.Li32(T0, BiosBuilder::I(0x09u, A0, V0, 100));
    b.Sw(T0, 0, S0);
    b.Li32(T0, BiosBuilder::R(RA, 0u, 0u, 0u, 0x08u));
    b.Sw(T0, 4, S0);
    b.Sw(ZERO, 8, S0);

    // Call pass 1 -> JIT compiles block at 0x80020000
    b.Li32(A0, 1000u);
    b.Jalr(RA, S0);
    b.Nop();
    b.Sw(V0, 4, S6);  // expect 1100

    // Self-modify 0x80020000 to `addiu v0, a0, 777` in RAM (triggers CodeCache write invalidation)
    b.Li32(T0, BiosBuilder::I(0x09u, A0, V0, 777));
    b.Sw(T0, 0, S0);

    // Flush the PS1 hardware I-cache line at 0x80020000 via COP0 SR.IsC=1 cache isolation
    b.Li32(T1, 0x40410000u);
    b.Mtc0(T1, 12u);
    b.Nop();
    b.Sw(ZERO, 0, S0);
    b.Li32(T1, 0x40400000u);
    b.Mtc0(T1, 12u);
    b.Nop();

    // Call pass 2 -> JIT MUST invalidate compiled block & return 1777
    b.Li32(A0, 1000u);
    b.Jalr(RA, S0);
    b.Nop();
    b.Sw(V0, 8, S6);  // expect 1777

    EmitDoneMagic(b, kMagicBase + 6u);
  }});

  // 7. cop2_gte_ops
  tests.push_back({"cop2_gte_ops", 4, kMagicBase + 7u, [](BiosBuilder& b) {
    EmitCommonPrologue(b);
    // Load rotation matrix, translation, light, color, background color, H, DQA, DQB, OFX, OFY
    b.Li32(S0, 0x80010600u);
    for (uint32_t i = 0; i < 32; i++) {
      const uint32_t val = 0x10203040u + i * 0x01070305u;
      b.Li32(T0, val);
      b.Sw(T0, static_cast<int32_t>(i * 4u), S0);
    }

    // Set sensible geometry control registers cleanly
    auto SetCtrl = [&](uint32_t reg, uint32_t val) {
      b.Li32(T0, val);
      b.Ctc2(T0, reg);
    };
    // Rotation matrix ~ identity (1.0 = 0x1000 in 4.12 fixed)
    SetCtrl(0u, 0x00001000u);
    SetCtrl(1u, 0x10000000u);
    SetCtrl(2u, 0x00000000u);
    SetCtrl(3u, 0x00000000u);
    SetCtrl(4u, 0x00001000u);
    SetCtrl(5u, 0u); SetCtrl(6u, 0u); SetCtrl(7u, 250u);  // TRX=0, TRY=0, TRZ=250
    // OFX, OFY (16.16 fixed), H, DQA, DQB
    SetCtrl(24u, 160u << 16);
    SetCtrl(25u, 120u << 16);
    SetCtrl(26u, 320u);
    SetCtrl(27u, 0xFF00u);
    SetCtrl(28u, 0x01400000u);
    SetCtrl(29u, 0x1000u);  // ZSF3
    SetCtrl(30u, 0x0800u);  // ZSF4
    // Color matrix + background
    SetCtrl(8u, 0x04000800u);
    SetCtrl(9u, 0x02000300u);
    SetCtrl(13u, 0x100u); SetCtrl(14u, 0x80u); SetCtrl(15u, 0x40u);

    // Load 3 vertices into V0/V1/V2 + RGBC
    b.Li32(T0, 0xFFAC0040u); b.Mtc2(T0, 0u);   // VXY0
    b.Li32(T0, 0x00000030u); b.Mtc2(T0, 1u);   // VZ0
    b.Li32(T0, 0x0050FF80u); b.Mtc2(T0, 2u);   // VXY1
    b.Li32(T0, 0x00000050u); b.Mtc2(T0, 3u);   // VZ1
    b.Li32(T0, 0x00400060u); b.Mtc2(T0, 4u);   // VXY2
    b.Li32(T0, 0x00000070u); b.Mtc2(T0, 5u);   // VZ2
    b.Li32(T0, 0x38C0A080u); b.Mtc2(T0, 6u);   // RGBC
    b.Li32(T0, 0x00000400u); b.Mtc2(T0, 8u);   // IR0
    b.Li32(T0, 0x00000600u); b.Mtc2(T0, 9u);   // IR1
    b.Li32(T0, 0x00000500u); b.Mtc2(T0, 10u);  // IR2
    b.Li32(T0, 0x00000300u); b.Mtc2(T0, 11u);  // IR3
    b.Nop();

    // Exercise core GTE opcodes (valid ECR command encodings with sf/mx/v/cv bits)
    const uint32_t gte_ops[] = {
      0x0180001u,  // RTPS (sf=1, lm=0)
      0x0280030u,  // RTPT (sf=1)
      0x1400006u,  // NCLIP
      0x158002Du,  // AVSZ3 (sf=1)
      0x168002Eu,  // AVSZ4 (sf=1)
      0x0492012u,  // MVMVA (sf=1, lm=1)
      0x0A00428u,  // SQR
      0x0780010u,  // DPCS (sf=1)
      0x0980011u,  // INTPL (sf=1)
      0x170000Cu,  // OP
      0x0E8041Eu,  // NCDS (sf=1, lm=1)
      0x108041Bu,  // NCCS (sf=1, lm=1)
      0x1280414u,  // CDP (sf=1, lm=1)
      0x190003Du,  // GPF (sf=1)
      0x1A0003Eu,  // GPL (sf=1)
    };
    int out_off = 4;
    for (uint32_t op : gte_ops) {
      b.GteCmd(op);
      b.Nop();
      b.Cfc2(V0, 31u);  // GTE FLAG
      b.Mfc2(V1, 25u);  // MAC1
      b.Sw(V0, out_off, S6);
      b.Sw(V1, out_off + 4, S6);
      out_off += 8;
    }

    // Dump all 32 GTE data + 32 GTE control registers into S6 + out_off..
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

    EmitDoneMagic(b, kMagicBase + 7u);
  }});

  return tests;
}

bool RunInstance(EcrApi& api, const std::string& sys_dir, const std::string& bios_name,
                 const std::string& cpu_mode, uint32_t frames, CpuCapture* out) {
  EnvConfig cfg;
  cfg.sys_dir = sys_dir;
  cfg.cpu_mode = cpu_mode;
  cfg.bios_file = bios_name;
  g_env = &cfg;

  api.retro_set_environment(EnvironmentCallback);
  api.retro_set_video_refresh(VideoCallback);
  api.retro_set_audio_sample([](int16_t, int16_t) {});
  api.retro_set_audio_sample_batch([](const int16_t*, size_t n) { return n; });
  api.retro_set_input_poll([]() {});
  api.retro_set_input_state([](unsigned, unsigned, unsigned, unsigned) { return int16_t(0); });

  api.retro_init();
  if (!api.retro_load_game(nullptr)) {
    std::fprintf(stderr, "retro_load_game(nullptr) failed for mode=%s\n", cpu_mode.c_str());
    api.retro_deinit();
    g_env = nullptr;
    return false;
  }

  for (uint32_t f = 0; f < frames; f++) api.retro_run();

  const void* ram = api.retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
  const size_t ram_sz = api.retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
  if (!ram || ram_sz < 2u * 1024u * 1024u) {
    std::fprintf(stderr, "Missing 2 MiB PS1 RAM for mode=%s (sz=%zu)\n", cpu_mode.c_str(), ram_sz);
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
    std::fprintf(stderr, "Usage: %s <libecr_android.so> [--bios <path>] [--frames <N>]\n", argv[0]);
    return 2;
  }

  const char* core_path = argv[1];
  std::string real_bios_path;
  uint32_t real_bios_frames = 120u;
  for (int i = 2; i < argc; i++) {
    if (std::strcmp(argv[i], "--bios") == 0 && i + 1 < argc) {
      real_bios_path = argv[++i];
    } else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
      real_bios_frames = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
      if (real_bios_frames == 0u) real_bios_frames = 120u;
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

  const auto suite = BuildSuite();
  const std::vector<std::string> targets = {"CachedInterpreter", "Recompiler"};
  int total_checks = 0;
  int total_failures = 0;

  std::printf("=== EmuCoreR ECR CPU Accuracy Oracle (Golden: Interpreter) ===\n");
  for (const auto& t : suite) {
    BiosBuilder builder;
    t.build(builder);
    if (!builder.WriteFile(bios_full)) {
      std::fprintf(stderr, "Failed writing BIOS for %s\n", t.name);
      return 2;
    }

    CpuCapture golden;
    if (!RunInstance(api, sys_dir, bios_file, "Interpreter", t.frames, &golden)) {
      std::fprintf(stderr, "FAIL [%s] Golden Interpreter failed to run\n", t.name);
      total_failures++;
      continue;
    }
    if (golden.results[0] != t.magic) {
      std::fprintf(stderr, "FAIL [%s] Golden Interpreter magic=0x%08X expected=0x%08X\n",
                   t.name, golden.results[0], t.magic);
      total_failures++;
      continue;
    }

    for (const auto& mode : targets) {
      total_checks++;
      CpuCapture cand;
      if (!RunInstance(api, sys_dir, bios_file, mode, t.frames, &cand)) {
        std::printf("FAIL %-26s mode=%-18s (run error)\n", t.name, mode.c_str());
        total_failures++;
        continue;
      }
      int first_diff = -1;
      for (size_t w = 0; w < kResultWords; w++) {
        if (golden.results[w] != cand.results[w]) { first_diff = static_cast<int>(w); break; }
      }
      const bool ram_exact = (golden.ram_hash == cand.ram_hash) && (first_diff < 0);
      if (ram_exact) {
        std::printf("PASS %-26s mode=%-18s magic=0x%08X ram_hash=0x%016llX\n",
                    t.name, mode.c_str(), cand.results[0], static_cast<unsigned long long>(cand.ram_hash));
      } else {
        std::printf("FAIL %-26s mode=%-18s diff_word=%d golden=0x%08X cand=0x%08X ram_golden=0x%016llX ram_cand=0x%016llX\n",
                    t.name, mode.c_str(), first_diff,
                    first_diff >= 0 ? golden.results[first_diff] : 0u,
                    first_diff >= 0 ? cand.results[first_diff] : 0u,
                    static_cast<unsigned long long>(golden.ram_hash),
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
      CpuCapture golden;
      CpuCapture cached;
      CpuCapture recomp;
      if (!RunInstance(api, sys_dir, "real_bios.bin", "Interpreter", real_bios_frames, &golden) ||
          !RunInstance(api, sys_dir, "real_bios.bin", "CachedInterpreter", real_bios_frames, &cached) ||
          !RunInstance(api, sys_dir, "real_bios.bin", "Recompiler", real_bios_frames, &recomp)) {
        std::fprintf(stderr, "FAIL [real_bios_lockstep] BIOS run failed\n");
        total_failures++;
      } else {
        // 1) Interpreter vs CachedInterpreter: both boot real BIOS and install the identical PS1 kernel vector table
        //    at 0x80000000..0x800000FF.
        total_checks++;
        const bool boot_ok = (golden.kernel_vectors_hash == cached.kernel_vectors_hash) &&
                             (golden.kernel_vectors_hash != 0ULL) && (golden.ram_hash != 0ULL);
        std::printf("%s %-26s mode=%-18s frames=%u vectors=0x%016llX (interp vectors=0x%016llX ram=0x%016llX)\n",
                    boot_ok ? "PASS" : "FAIL",
                    "real_bios_kernel_boot", "CachedInterpreter", real_bios_frames,
                    static_cast<unsigned long long>(cached.kernel_vectors_hash),
                    static_cast<unsigned long long>(golden.kernel_vectors_hash),
                    static_cast<unsigned long long>(golden.ram_hash));
        if (!boot_ok) total_failures++;

        // 2) CodeCache lockstep (CachedInterpreter C++ ExecuteInstruction vs ARM64 JIT Recompiler):
        //    identical basic-block scheduling -> must match 100% bit-for-bit across all 2 MiB RAM + framebuffer.
        total_checks++;
        const bool lockstep_ok = (cached.ram_hash == recomp.ram_hash) &&
                                 (cached.frame_hash == recomp.frame_hash) &&
                                 (recomp.frame_hash != 0ULL);
        std::printf("%s %-26s mode=%-18s frames=%u ram=0x%016llX fb=0x%016llX (cached ram=0x%016llX fb=0x%016llX)\n",
                    lockstep_ok ? "PASS" : "FAIL",
                    "real_bios_lockstep", "Recompiler", real_bios_frames,
                    static_cast<unsigned long long>(recomp.ram_hash),
                    static_cast<unsigned long long>(recomp.frame_hash),
                    static_cast<unsigned long long>(cached.ram_hash),
                    static_cast<unsigned long long>(cached.frame_hash));
        if (!lockstep_ok) total_failures++;
      }
      std::remove(real_bios_dst.c_str());
    }
  }

  std::remove(bios_full.c_str());
  rmdir(tmp_dir);
  std::printf("CPU Oracle Summary: %d comparisons, %d failures\n", total_checks, total_failures);
  return total_failures == 0 ? 0 : 1;
}
