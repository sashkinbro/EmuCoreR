// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Shared MIPS R3000A BIOS emitter, FNV-1a hasher, and dynamic libecr loader
// used by the EmuCoreR CPU and GPU accuracy oracles.
#pragma once

#include <libretro.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <vector>

namespace ecr_oracle {

// Common MIPS R3000A general-purpose register indices.
enum Reg : uint32_t {
  ZERO = 0, AT = 1, V0 = 2, V1 = 3, A0 = 4, A1 = 5, A2 = 6, A3 = 7,
  T0 = 8, T1 = 9, T2 = 10, T3 = 11, T4 = 12, T5 = 13, T6 = 14, T7 = 15,
  S0 = 16, S1 = 17, S2 = 18, S3 = 19, S4 = 20, S5 = 21, S6 = 22, S7 = 23,
  T8 = 24, T9 = 25, K0 = 26, K1 = 27, GP = 28, SP = 29, FP = 30, RA = 31
};

struct BiosBuilder {
  static constexpr uint32_t kBiosBase = 0xBFC00000u;
  static constexpr uint32_t kBiosWords = (512u * 1024u) / 4u;
  std::vector<uint32_t> words;
  uint32_t cursor = 0;  // word index

  BiosBuilder() : words(kBiosWords, 0u) {}

  void Seek(uint32_t byte_offset) { cursor = byte_offset / 4u; }
  uint32_t CurrentAddr() const { return kBiosBase + cursor * 4u; }

  void Emit(uint32_t insn) {
    if (cursor < kBiosWords) words[cursor++] = insn;
  }

  void Nop() { Emit(0u); }

  static constexpr uint32_t R(uint32_t rs, uint32_t rt, uint32_t rd, uint32_t sa, uint32_t fn) {
    return ((rs & 31u) << 21) | ((rt & 31u) << 16) | ((rd & 31u) << 11) |
           ((sa & 31u) << 6) | (fn & 63u);
  }
  static constexpr uint32_t I(uint32_t op, uint32_t rs, uint32_t rt, int32_t imm) {
    return ((op & 63u) << 26) | ((rs & 31u) << 21) | ((rt & 31u) << 16) |
           (static_cast<uint32_t>(imm) & 0xFFFFu);
  }

  void Lui(uint32_t rt, uint32_t imm16) { Emit(I(0x0Fu, 0u, rt, static_cast<int32_t>(imm16))); }
  void Ori(uint32_t rt, uint32_t rs, uint32_t imm16) { Emit(I(0x0Du, rs, rt, static_cast<int32_t>(imm16))); }
  void Andi(uint32_t rt, uint32_t rs, uint32_t imm16) { Emit(I(0x0Cu, rs, rt, static_cast<int32_t>(imm16))); }
  void Xori(uint32_t rt, uint32_t rs, uint32_t imm16) { Emit(I(0x0Eu, rs, rt, static_cast<int32_t>(imm16))); }
  void Li32(uint32_t rt, uint32_t value) {
    Lui(rt, (value >> 16) & 0xFFFFu);
    if ((value & 0xFFFFu) != 0u) Ori(rt, rt, value & 0xFFFFu);
  }

  void Add(uint32_t rd, uint32_t rs, uint32_t rt) { Emit(R(rs, rt, rd, 0u, 0x20u)); }
  void Addu(uint32_t rd, uint32_t rs, uint32_t rt) { Emit(R(rs, rt, rd, 0u, 0x21u)); }
  void Sub(uint32_t rd, uint32_t rs, uint32_t rt) { Emit(R(rs, rt, rd, 0u, 0x22u)); }
  void Subu(uint32_t rd, uint32_t rs, uint32_t rt) { Emit(R(rs, rt, rd, 0u, 0x23u)); }
  void And(uint32_t rd, uint32_t rs, uint32_t rt) { Emit(R(rs, rt, rd, 0u, 0x24u)); }
  void Or(uint32_t rd, uint32_t rs, uint32_t rt) { Emit(R(rs, rt, rd, 0u, 0x25u)); }
  void Xor(uint32_t rd, uint32_t rs, uint32_t rt) { Emit(R(rs, rt, rd, 0u, 0x26u)); }
  void Nor(uint32_t rd, uint32_t rs, uint32_t rt) { Emit(R(rs, rt, rd, 0u, 0x27u)); }
  void Slt(uint32_t rd, uint32_t rs, uint32_t rt) { Emit(R(rs, rt, rd, 0u, 0x2Au)); }
  void Sltu(uint32_t rd, uint32_t rs, uint32_t rt) { Emit(R(rs, rt, rd, 0u, 0x2Bu)); }

  void Sll(uint32_t rd, uint32_t rt, uint32_t sa) { Emit(R(0u, rt, rd, sa, 0x00u)); }
  void Srl(uint32_t rd, uint32_t rt, uint32_t sa) { Emit(R(0u, rt, rd, sa, 0x02u)); }
  void Sra(uint32_t rd, uint32_t rt, uint32_t sa) { Emit(R(0u, rt, rd, sa, 0x03u)); }
  void Sllv(uint32_t rd, uint32_t rt, uint32_t rs) { Emit(R(rs, rt, rd, 0u, 0x04u)); }
  void Srlv(uint32_t rd, uint32_t rt, uint32_t rs) { Emit(R(rs, rt, rd, 0u, 0x06u)); }
  void Srav(uint32_t rd, uint32_t rt, uint32_t rs) { Emit(R(rs, rt, rd, 0u, 0x07u)); }

  void Addi(uint32_t rt, uint32_t rs, int32_t imm) { Emit(I(0x08u, rs, rt, imm)); }
  void Addiu(uint32_t rt, uint32_t rs, int32_t imm) { Emit(I(0x09u, rs, rt, imm)); }
  void Slti(uint32_t rt, uint32_t rs, int32_t imm) { Emit(I(0x0Au, rs, rt, imm)); }
  void Sltiu(uint32_t rt, uint32_t rs, int32_t imm) { Emit(I(0x0Bu, rs, rt, imm)); }

  void Mult(uint32_t rs, uint32_t rt) { Emit(R(rs, rt, 0u, 0u, 0x18u)); }
  void Multu(uint32_t rs, uint32_t rt) { Emit(R(rs, rt, 0u, 0u, 0x19u)); }
  void Div(uint32_t rs, uint32_t rt) { Emit(R(rs, rt, 0u, 0u, 0x1Au)); }
  void Divu(uint32_t rs, uint32_t rt) { Emit(R(rs, rt, 0u, 0u, 0x1Bu)); }
  void Mfhi(uint32_t rd) { Emit(R(0u, 0u, rd, 0u, 0x10u)); }
  void Mthi(uint32_t rs) { Emit(R(rs, 0u, 0u, 0u, 0x11u)); }
  void Mflo(uint32_t rd) { Emit(R(0u, 0u, rd, 0u, 0x12u)); }
  void Mtlo(uint32_t rs) { Emit(R(rs, 0u, 0u, 0u, 0x13u)); }

  void Lb(uint32_t rt, int32_t off, uint32_t base) { Emit(I(0x20u, base, rt, off)); }
  void Lh(uint32_t rt, int32_t off, uint32_t base) { Emit(I(0x21u, base, rt, off)); }
  void Lwl(uint32_t rt, int32_t off, uint32_t base) { Emit(I(0x22u, base, rt, off)); }
  void Lw(uint32_t rt, int32_t off, uint32_t base) { Emit(I(0x23u, base, rt, off)); }
  void Lbu(uint32_t rt, int32_t off, uint32_t base) { Emit(I(0x24u, base, rt, off)); }
  void Lhu(uint32_t rt, int32_t off, uint32_t base) { Emit(I(0x25u, base, rt, off)); }
  void Lwr(uint32_t rt, int32_t off, uint32_t base) { Emit(I(0x26u, base, rt, off)); }

  void Sb(uint32_t rt, int32_t off, uint32_t base) { Emit(I(0x28u, base, rt, off)); }
  void Sh(uint32_t rt, int32_t off, uint32_t base) { Emit(I(0x29u, base, rt, off)); }
  void Swl(uint32_t rt, int32_t off, uint32_t base) { Emit(I(0x2Au, base, rt, off)); }
  void Sw(uint32_t rt, int32_t off, uint32_t base) { Emit(I(0x2Bu, base, rt, off)); }
  void Swr(uint32_t rt, int32_t off, uint32_t base) { Emit(I(0x2Eu, base, rt, off)); }

  void J(uint32_t addr) { Emit((0x02u << 26) | ((addr >> 2) & 0x03FFFFFFu)); }
  void Jal(uint32_t addr) { Emit((0x03u << 26) | ((addr >> 2) & 0x03FFFFFFu)); }
  void Jr(uint32_t rs) { Emit(R(rs, 0u, 0u, 0u, 0x08u)); }
  void Jalr(uint32_t rd, uint32_t rs) { Emit(R(rs, 0u, rd, 0u, 0x09u)); }

  void Beq(uint32_t rs, uint32_t rt, int32_t rel_words) { Emit(I(0x04u, rs, rt, rel_words)); }
  void Bne(uint32_t rs, uint32_t rt, int32_t rel_words) { Emit(I(0x05u, rs, rt, rel_words)); }
  void Blez(uint32_t rs, int32_t rel_words) { Emit(I(0x06u, rs, 0u, rel_words)); }
  void Bgtz(uint32_t rs, int32_t rel_words) { Emit(I(0x07u, rs, 0u, rel_words)); }
  void Bltz(uint32_t rs, int32_t rel_words) { Emit(I(0x01u, rs, 0u, rel_words)); }
  void Bgez(uint32_t rs, int32_t rel_words) { Emit(I(0x01u, rs, 1u, rel_words)); }
  void Bltzal(uint32_t rs, int32_t rel_words) { Emit(I(0x01u, rs, 0x10u, rel_words)); }
  void Bgezal(uint32_t rs, int32_t rel_words) { Emit(I(0x01u, rs, 0x11u, rel_words)); }

  void Syscall(uint32_t code = 0u) { Emit(((code & 0xFFFFFu) << 6) | 0x0Cu); }
  void Break(uint32_t code = 0u) { Emit(((code & 0xFFFFFu) << 6) | 0x0Du); }

  void Mfc0(uint32_t rt, uint32_t rd) { Emit((0x10u << 26) | (0u << 21) | ((rt & 31u) << 16) | ((rd & 31u) << 11)); }
  void Mtc0(uint32_t rt, uint32_t rd) { Emit((0x10u << 26) | (4u << 21) | ((rt & 31u) << 16) | ((rd & 31u) << 11)); }
  void Rfe() { Emit((0x10u << 26) | (1u << 25) | 0x10u); }

  void Mfc2(uint32_t rt, uint32_t rd) { Emit((0x12u << 26) | (0u << 21) | ((rt & 31u) << 16) | ((rd & 31u) << 11)); }
  void Cfc2(uint32_t rt, uint32_t rd) { Emit((0x12u << 26) | (2u << 21) | ((rt & 31u) << 16) | ((rd & 31u) << 11)); }
  void Mtc2(uint32_t rt, uint32_t rd) { Emit((0x12u << 26) | (4u << 21) | ((rt & 31u) << 16) | ((rd & 31u) << 11)); }
  void Ctc2(uint32_t rt, uint32_t rd) { Emit((0x12u << 26) | (6u << 21) | ((rt & 31u) << 16) | ((rd & 31u) << 11)); }
  void GteCmd(uint32_t cmd25) { Emit((0x12u << 26) | (1u << 25) | (cmd25 & 0x01FFFFFFu)); }

  void HaltLoop() {
    Beq(ZERO, ZERO, -1);
    Nop();
  }

  bool WriteFile(const std::string& path) const {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const size_t written = std::fwrite(words.data(), sizeof(uint32_t), words.size(), f);
    std::fclose(f);
    return written == words.size();
  }
};

inline uint64_t Fnv1a64(const void* data, size_t size) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  uint64_t hash = 14695981039346656037ULL;
  for (size_t i = 0; i < size; i++) {
    hash ^= bytes[i];
    hash *= 1099511628211ULL;
  }
  return hash;
}

struct EcrApi {
  void* handle = nullptr;
  decltype(&::retro_set_environment) retro_set_environment = nullptr;
  decltype(&::retro_set_video_refresh) retro_set_video_refresh = nullptr;
  decltype(&::retro_set_audio_sample) retro_set_audio_sample = nullptr;
  decltype(&::retro_set_audio_sample_batch) retro_set_audio_sample_batch = nullptr;
  decltype(&::retro_set_input_poll) retro_set_input_poll = nullptr;
  decltype(&::retro_set_input_state) retro_set_input_state = nullptr;
  decltype(&::retro_init) retro_init = nullptr;
  decltype(&::retro_deinit) retro_deinit = nullptr;
  decltype(&::retro_load_game) retro_load_game = nullptr;
  decltype(&::retro_unload_game) retro_unload_game = nullptr;
  decltype(&::retro_run) retro_run = nullptr;
  decltype(&::retro_get_memory_data) retro_get_memory_data = nullptr;
  decltype(&::retro_get_memory_size) retro_get_memory_size = nullptr;

  bool Load(const char* so_path) {
    handle = dlopen(so_path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
      std::fprintf(stderr, "dlopen(%s) failed: %s\n", so_path, dlerror());
      return false;
    }
#define LOAD_SYM(name) \
  name = reinterpret_cast<decltype(name)>(dlsym(handle, #name)); \
  if (!name) { std::fprintf(stderr, "Missing symbol: %s\n", #name); return false; }
    LOAD_SYM(retro_set_environment);
    LOAD_SYM(retro_set_video_refresh);
    LOAD_SYM(retro_set_audio_sample);
    LOAD_SYM(retro_set_audio_sample_batch);
    LOAD_SYM(retro_set_input_poll);
    LOAD_SYM(retro_set_input_state);
    LOAD_SYM(retro_init);
    LOAD_SYM(retro_deinit);
    LOAD_SYM(retro_load_game);
    LOAD_SYM(retro_unload_game);
    LOAD_SYM(retro_run);
    LOAD_SYM(retro_get_memory_data);
    LOAD_SYM(retro_get_memory_size);
#undef LOAD_SYM
    return true;
  }

  ~EcrApi() {
    if (handle) dlclose(handle);
  }
};

}  // namespace ecr_oracle
