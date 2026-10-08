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

#pragma once
#include "types.h"

class StateWrapper;

namespace PGXP {

void Initialize();
void Reset();
void Shutdown();

/// Serializes the tracked precision state so loading a save state does not
/// throw away the vertex data the game has already computed.
bool DoState(StateWrapper& sw);

/// Compact precision snapshots for same-process runahead states.
bool DoMemoryState(StateWrapper& sw);

// GPU FIFO words retain their precision independently of later RAM writes.
inline constexpr uint32_t GPU_VERTEX_SNAPSHOT_COUNT = 4096;
inline constexpr uint32_t INVALID_GPU_VERTEX_TOKEN = 0xffffffffu;
uint32_t CaptureGPUVertex(uint32_t address, uint32_t value);
uint32_t ConsumeGPUWrite(uint32_t value);

// -- GTE functions
// Transforms
void GTE_PushSXYZ2f(float x, float y, float z, uint32_t v);
// Arithmetic results replace old register geometry even when raw bits match.
void GTE_SetDataRegister(uint32_t index, uint32_t value);
void GTE_PushDataFIFO(uint32_t first, uint32_t count, uint32_t value);
int GTE_NCLIP_valid(uint32_t sxy0, uint32_t sxy1, uint32_t sxy2);
double GTE_NCLIP();

// Data transfer tracking
void CPU_MFC2(uint32_t instr, uint32_t rtVal, uint32_t rdVal); // copy GTE data reg to GPR reg (MFC2)
void CPU_MTC2(uint32_t instr, uint32_t rdVal, uint32_t rtVal); // copy GPR reg to GTE data reg (MTC2)
void CPU_CFC2(uint32_t instr, uint32_t rtVal, uint32_t rdVal); // copy GTE ctrl reg to GPR reg (CFC2)
void CPU_CTC2(uint32_t instr, uint32_t rdVal, uint32_t rtVal); // copy GPR reg to GTE ctrl reg (CTC2)
// Memory Access
void CPU_LWC2(uint32_t instr, uint32_t rtVal, uint32_t addr); // copy memory to GTE reg
void CPU_SWC2(uint32_t instr, uint32_t rtVal, uint32_t addr); // copy GTE reg to memory

bool GetPreciseVertex(uint32_t addr, uint32_t value, int x, int y, int xOffs, int yOffs, float* out_x, float* out_y,
                      float* out_w);

// -- CPU functions
void CPU_LW(uint32_t instr, uint32_t rtVal, uint32_t addr);
void CPU_LHx(uint32_t instr, uint32_t rtVal, uint32_t addr);
void CPU_LBx(uint32_t instr, uint32_t rtVal, uint32_t addr);
// Stage a successful CPU load without replacing the value visible in its slot.
void CPU_LoadDelay(uint32_t instr, uint32_t value, uint32_t addr);
void CPU_UpdateLoadDelay();
void CPU_CommitLoadDelay();
void CPU_FlushLoadDelay();
void CPU_CancelLoadDelay(uint32_t reg);
uint32_t* CPU_GetLoadDelayRegister();
void CPU_SB(uint32_t instr, uint8_t rtVal, uint32_t addr);
void CPU_SH(uint32_t instr, uint16_t rtVal, uint32_t addr);
void CPU_SW(uint32_t instr, uint32_t rtVal, uint32_t addr);
void CPU_MOVE(uint32_t rd_and_rs, uint32_t rsVal);
// Memory mode follows exact copies and invalidates other integer results.
void CPU_MemoryALU(uint32_t instr, uint32_t rsVal, uint32_t rtVal);
void CPU_InvalidateRegister(uint32_t reg);
// The fixed precision array also permits generated flag stores without a call.
uint32_t* CPU_GetRegisterFlags(uint32_t reg);

// Arithmetic with immediate value
void CPU_ADDI(uint32_t instr, uint32_t rsVal);
void CPU_ANDI(uint32_t instr, uint32_t rsVal);
void CPU_ORI(uint32_t instr, uint32_t rsVal);
void CPU_XORI(uint32_t instr, uint32_t rsVal);
void CPU_SLTI(uint32_t instr, uint32_t rsVal);
void CPU_SLTIU(uint32_t instr, uint32_t rsVal);

// Load Upper
void CPU_LUI(uint32_t instr);

// Register Arithmetic
void CPU_ADD(uint32_t instr, uint32_t rsVal, uint32_t rtVal);
void CPU_SUB(uint32_t instr, uint32_t rsVal, uint32_t rtVal);
void CPU_AND_(uint32_t instr, uint32_t rsVal, uint32_t rtVal);
void CPU_OR_(uint32_t instr, uint32_t rsVal, uint32_t rtVal);
void CPU_XOR_(uint32_t instr, uint32_t rsVal, uint32_t rtVal);
void CPU_NOR(uint32_t instr, uint32_t rsVal, uint32_t rtVal);
void CPU_SLT(uint32_t instr, uint32_t rsVal, uint32_t rtVal);
void CPU_SLTU(uint32_t instr, uint32_t rsVal, uint32_t rtVal);

// Register mult/div
void CPU_MULT(uint32_t instr, uint32_t rsVal, uint32_t rtVal);
void CPU_MULTU(uint32_t instr, uint32_t rsVal, uint32_t rtVal);
void CPU_DIV(uint32_t instr, uint32_t rsVal, uint32_t rtVal);
void CPU_DIVU(uint32_t instr, uint32_t rsVal, uint32_t rtVal);

// Shift operations (sa)
void CPU_SLL(uint32_t instr, uint32_t rtVal);
void CPU_SRL(uint32_t instr, uint32_t rtVal);
void CPU_SRA(uint32_t instr, uint32_t rtVal);

// Shift operations variable
void CPU_SLLV(uint32_t instr, uint32_t rtVal, uint32_t rsVal);
void CPU_SRLV(uint32_t instr, uint32_t rtVal, uint32_t rsVal);
void CPU_SRAV(uint32_t instr, uint32_t rtVal, uint32_t rsVal);

// Move registers
void CPU_MFHI(uint32_t instr, uint32_t hiVal);
void CPU_MTHI(uint32_t instr, uint32_t rsVal);
void CPU_MFLO(uint32_t instr, uint32_t loVal);
void CPU_MTLO(uint32_t instr, uint32_t rsVal);

// CP0 Data transfer tracking
void CPU_MFC0(uint32_t instr, uint32_t rdVal);
void CPU_MTC0(uint32_t instr, uint32_t rdVal, uint32_t rtVal);

} // namespace PGXP
