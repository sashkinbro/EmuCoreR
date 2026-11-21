#pragma once
#include "common/bitfield.h"
#include "cpu_types.h"
#include "gte_types.h"
#include "types.h"
#include <array>
#include <optional>
#include <vector>

class StateWrapper;

namespace CPU {

inline constexpr VirtualMemoryAddress RESET_VECTOR = UINT32_C(0xBFC00000);

inline constexpr PhysicalMemoryAddress DCACHE_LOCATION = UINT32_C(0x1F800000),
                                       DCACHE_LOCATION_MASK = UINT32_C(0xFFFFFC00),
                                       DCACHE_OFFSET_MASK = UINT32_C(0x000003FF), DCACHE_SIZE = UINT32_C(0x00000400),
                                       ICACHE_SIZE = UINT32_C(0x00001000),
                                       ICACHE_LINE_SIZE = 16, ICACHE_LINES = ICACHE_SIZE / ICACHE_LINE_SIZE,
                                       ICACHE_TAG_ADDRESS_MASK = 0xFFFFFFF0u, ICACHE_INVALID_BITS = 0x0Fu;

union CacheControl
{
  uint32_t bits;

  BitField<uint32_t, bool, 0, 1> lock_mode;
  BitField<uint32_t, bool, 1, 1> invalidate_mode;
  BitField<uint32_t, bool, 2, 1> tag_test_mode;
  BitField<uint32_t, bool, 3, 1> dcache_scratchpad;
  BitField<uint32_t, bool, 7, 1> dcache_enable;
  BitField<uint32_t, uint8_t, 8, 2> icache_fill_size; // actually dcache? icache always fills to 16 bytes
  BitField<uint32_t, bool, 11, 1> icache_enable;
};

struct State
{
  // ticks the CPU has executed
  TickCount downcount = 0;
  TickCount pending_ticks = 0;
  TickCount gte_completion_tick = 0;

  Registers regs = {};
  Cop0Registers cop0_regs = {};
  Instruction next_instruction = {};

  // address of the instruction currently being executed
  Instruction current_instruction = {};
  uint32_t current_instruction_pc = 0;
  bool current_instruction_in_branch_delay_slot = false;
  bool current_instruction_was_branch_taken = false;
  bool next_instruction_is_branch_delay_slot = false;
  bool branch_was_taken = false;
  bool exception_raised = false;
  bool interrupt_delay = false;
  bool frame_done = false;

  // load delays
  Reg load_delay_reg = Reg::count;
  uint32_t load_delay_value = 0;
  Reg next_load_delay_reg = Reg::count;
  uint32_t next_load_delay_value = 0;

  CacheControl cache_control{0};

  // GTE registers are stored here so we can access them on ARM with a single instruction
  GTE::Regs gte_regs = {};

  // 4 bytes of padding here on x64
  bool use_debug_dispatcher = false;

  uint8_t* fastmem_base = nullptr;

  // data cache (used as scratchpad)
  std::array<uint8_t, DCACHE_SIZE> dcache = {};
  std::array<uint32_t, ICACHE_LINES> icache_tags = {};
  std::array<uint8_t, ICACHE_SIZE> icache_data = {};

  // Compute the byte offset of a register within the State struct, for the
  // recompiler's EmitLoadCPUStructField / EmitStoreCPUStructField helpers.
  // These are deliberately NOT constexpr.
  //
  // Apple Clang rejects offsetof through a union member in a constexpr
  // context ("cannot access field of null pointer") even when the
  // composition uses only direct members - r and r32 are members of
  // anonymous/named unions inside Registers / GTE::Regs, and Clang's
  // constexpr evaluator walks them as null-pointer accesses through the
  // union. GCC chooses to elide that check; Clang chooses to enforce it.
  // It is a longstanding Clang/GCC divergence on __builtin_offsetof
  // constexpr semantics, not specific to any deployment target.
  //
  // Dropping constexpr costs nothing at runtime: every call site passes a
  // runtime uint32_t (guest_reg / GTE register index), so the value is computed
  // at runtime in either case. The two inner offsetof calls remain
  // compile-time constants regardless of the constexpr keyword on the
  // wrapper, and the compiler folds them through normal optimization just
  // as well as it would through constexpr evaluation. Verified at -O3
  // with GCC: object files for cpu_recompiler_code_generator{,_generic}.o
  // are bit-identical with and without the keyword.
  static uint32_t GPRRegisterOffset(uint32_t index)
  {
    return offsetof(State, regs) + offsetof(Registers, r) + (sizeof(uint32_t) * index);
  }
  static uint32_t GTERegisterOffset(uint32_t index)
  {
    return offsetof(State, gte_regs) + offsetof(GTE::Regs, r32) + (sizeof(uint32_t) * index);
  }
};

extern State g_state;
extern bool g_using_interpreter;

void Initialize();
void Shutdown();
void Reset();
bool DoState(StateWrapper& sw);
void ClearICache();
void UpdateFastmemBase();

/// Executes interpreter loop.
void Execute();
void ExecuteDebug();

// Forces an early exit from the CPU dispatcher.
void ForceDispatcherExit();

ALWAYS_INLINE TickCount GetPendingTicks()
{
  return g_state.pending_ticks;
}
ALWAYS_INLINE void ResetPendingTicks()
{
  g_state.gte_completion_tick =
    (g_state.pending_ticks < g_state.gte_completion_tick) ? (g_state.gte_completion_tick - g_state.pending_ticks) : 0;
  g_state.pending_ticks = 0;
}
ALWAYS_INLINE void AddPendingTicks(TickCount ticks)
{
  g_state.pending_ticks += ticks;
}

// state helpers
ALWAYS_INLINE bool InUserMode()
{
  return g_state.cop0_regs.sr.KUc;
}

// Memory reads variants which do not raise exceptions.
// These methods do not support writing to MMIO addresses with side effects, and are
// thus safe to call from the UI thread in debuggers, for example.
bool SafeReadMemoryByte(VirtualMemoryAddress addr, uint8_t* value);
bool SafeReadMemoryHalfWord(VirtualMemoryAddress addr, uint16_t* value);
bool SafeReadMemoryWord(VirtualMemoryAddress addr, uint32_t* value);
bool SafeWriteMemoryByte(VirtualMemoryAddress addr, uint8_t value);
bool SafeWriteMemoryHalfWord(VirtualMemoryAddress addr, uint16_t value);
bool SafeWriteMemoryWord(VirtualMemoryAddress addr, uint32_t value);

// External IRQs
void SetExternalInterrupt(uint8_t bit);
void ClearExternalInterrupt(uint8_t bit);

// Breakpoints
void ClearBreakpoints();

} // namespace CPU
