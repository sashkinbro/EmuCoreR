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

namespace PGXP {

void Initialize();
void Reset();
void Shutdown();

// -- GTE functions
// Transforms
void GTE_PushSXYZ2f(float x, float y, float z, uint32_t v);
int GTE_NCLIP_valid(uint32_t sxy0, uint32_t sxy1, uint32_t sxy2);
float GTE_NCLIP();

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
void CPU_SB(uint32_t instr, uint8_t rtVal, uint32_t addr);
void CPU_SH(uint32_t instr, uint16_t rtVal, uint32_t addr);
void CPU_SW(uint32_t instr, uint32_t rtVal, uint32_t addr);
void CPU_MOVE(uint32_t rd_and_rs, uint32_t rsVal);

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
void CPU_MTHI(uint32_t instr, uint32_t rdVal);
void CPU_MFLO(uint32_t instr, uint32_t loVal);
void CPU_MTLO(uint32_t instr, uint32_t rdVal);

// CP0 Data transfer tracking
void CPU_MFC0(uint32_t instr, uint32_t rdVal);
void CPU_MTC0(uint32_t instr, uint32_t rdVal, uint32_t rtVal);

} // namespace PGXP