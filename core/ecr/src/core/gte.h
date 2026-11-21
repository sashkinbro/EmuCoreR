#pragma once
#include "gte_types.h"

class StateWrapper;

namespace GTE {

void Initialize();
void Reset();
bool DoState(StateWrapper& sw);
void UpdateAspectRatio();

// control registers are offset by +32
uint32_t ReadRegister(uint32_t index);
void WriteRegister(uint32_t index, uint32_t value);

void ExecuteInstruction(uint32_t inst_bits);

using InstructionImpl = void (*)(Instruction);
InstructionImpl GetInstructionImpl(uint32_t inst_bits, TickCount* ticks);

} // namespace GTE
