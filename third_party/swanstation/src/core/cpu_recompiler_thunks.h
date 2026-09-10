#pragma once
#include "cpu_code_cache.h"
#include "cpu_types.h"

namespace CPU {
struct CodeBlock;
struct CodeBlockInstruction;

namespace Recompiler::Thunks {

//////////////////////////////////////////////////////////////////////////
// Trampolines for calling back from the JIT
// Needed because we can't cast member functions to void*...
// TODO: Abuse carry flag or something else for exception
//////////////////////////////////////////////////////////////////////////
bool InterpretInstruction();
bool InterpretInstructionPGXP();

// Memory access functions for the JIT - MSB is set on exception.
uint64_t ReadMemoryByte(uint32_t address);
uint64_t ReadMemoryHalfWord(uint32_t address);
uint64_t ReadMemoryWord(uint32_t address);
uint32_t WriteMemoryByte(uint32_t address, uint32_t value);
uint32_t WriteMemoryHalfWord(uint32_t address, uint32_t value);
uint32_t WriteMemoryWord(uint32_t address, uint32_t value);

// Unchecked memory access variants. No alignment or bus exceptions.
uint32_t UncheckedReadMemoryByte(uint32_t address);
uint32_t UncheckedReadMemoryHalfWord(uint32_t address);
uint32_t UncheckedReadMemoryWord(uint32_t address);
void UncheckedWriteMemoryByte(uint32_t address, uint32_t value);
void UncheckedWriteMemoryHalfWord(uint32_t address, uint32_t value);
void UncheckedWriteMemoryWord(uint32_t address, uint32_t value);

void ResolveBranch(CodeBlock* block, void* host_pc, void* host_resolve_pc, uint32_t host_pc_size);

} // namespace Recompiler::Thunks

} // namespace CPU
