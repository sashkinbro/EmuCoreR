#pragma once
#include "common/bitfield.h"
#include "types.h"
#include <optional>

namespace CPU {

// Memory address mask used for fetching as well as loadstores (removes cached/uncached/user/kernel bits).
inline constexpr uint32_t PHYSICAL_MEMORY_ADDRESS_MASK = 0x1FFFFFFF;

inline constexpr uint32_t INSTRUCTION_SIZE = sizeof(uint32_t);

enum class Segment
{
  KUSEG, // virtual memory
  KSEG0, // physical memory cached
  KSEG1, // physical memory uncached
  KSEG2
};

enum class Reg : uint8_t
{
  zero,
  at,
  v0,
  v1,
  a0,
  a1,
  a2,
  a3,
  t0,
  t1,
  t2,
  t3,
  t4,
  t5,
  t6,
  t7,
  s0,
  s1,
  s2,
  s3,
  s4,
  s5,
  s6,
  s7,
  t8,
  t9,
  k0,
  k1,
  gp,
  sp,
  fp,
  ra,

  // not accessible to instructions
  hi,
  lo,
  pc,
  npc,

  count
};

enum class InstructionOp : uint8_t
{
  funct = 0,
  b = 1, // i.rt 0 - bltz, 1 - bgez, 16 - bltzal, 17 - bgezal
  j = 2,
  jal = 3,
  beq = 4,
  bne = 5,
  blez = 6,
  bgtz = 7,
  addi = 8,
  addiu = 9,
  slti = 10,
  sltiu = 11,
  andi = 12,
  ori = 13,
  xori = 14,
  lui = 15,
  cop0 = 16,
  cop1 = 17,
  cop2 = 18,
  cop3 = 19,
  lb = 32,
  lh = 33,
  lwl = 34,
  lw = 35,
  lbu = 36,
  lhu = 37,
  lwr = 38,
  sb = 40,
  sh = 41,
  swl = 42,
  sw = 43,
  swr = 46,
  lwc0 = 48,
  lwc1 = 49,
  lwc2 = 50,
  lwc3 = 51,
  swc0 = 56,
  swc1 = 57,
  swc2 = 58,
  swc3 = 59,
};

enum class InstructionFunct : uint8_t
{
  sll = 0,
  srl = 2,
  sra = 3,
  sllv = 4,
  srlv = 6,
  srav = 7,
  jr = 8,
  jalr = 9,
  syscall = 12,
  break_ = 13,
  mfhi = 16,
  mthi = 17,
  mflo = 18,
  mtlo = 19,
  mult = 24,
  multu = 25,
  div = 26,
  divu = 27,
  add = 32,
  addu = 33,
  sub = 34,
  subu = 35,
  and_ = 36,
  or_ = 37,
  xor_ = 38,
  nor = 39,
  slt = 42,
  sltu = 43
};

enum class CopCommonInstruction : uint32_t
{
  mfcn = 0b0000,
  cfcn = 0b0010,
  mtcn = 0b0100,
  ctcn = 0b0110,
};

enum class Cop0Instruction : uint32_t
{
  tlbr = 0x01,
  tlbwi = 0x02,
  tlbwr = 0x04,
  tlbp = 0x08,
  rfe = 0x10,
};

union Instruction
{
  uint32_t bits;

  BitField<uint32_t, InstructionOp, 26, 6> op; // function/instruction

  union
  {
    BitField<uint32_t, Reg, 21, 5> rs;
    BitField<uint32_t, Reg, 16, 5> rt;
    BitField<uint32_t, uint16_t, 0, 16> imm;

    ALWAYS_INLINE uint32_t imm_sext32() const { return static_cast<uint32_t>(static_cast<int16_t>(imm.GetValue())); }
    ALWAYS_INLINE uint32_t imm_zext32() const { return static_cast<uint32_t>(imm.GetValue()); }
  } i;

  union
  {
    BitField<uint32_t, uint32_t, 0, 26> target;
  } j;

  union
  {
    BitField<uint32_t, Reg, 21, 5> rs;
    BitField<uint32_t, Reg, 16, 5> rt;
    BitField<uint32_t, Reg, 11, 5> rd;
    BitField<uint32_t, uint8_t, 6, 5> shamt;
    BitField<uint32_t, InstructionFunct, 0, 6> funct;
  } r;

  union
  {
    uint32_t bits;
    BitField<uint32_t, uint8_t, 26, 2> cop_n;
    BitField<uint32_t, uint16_t, 0, 16> imm16;
    BitField<uint32_t, uint32_t, 0, 25> imm25;

    ALWAYS_INLINE bool IsCommonInstruction() const { return (bits & (UINT32_C(1) << 25)) == 0; }

    ALWAYS_INLINE CopCommonInstruction CommonOp() const
    {
      return static_cast<CopCommonInstruction>((bits >> 21) & UINT32_C(0b1111));
    }

    ALWAYS_INLINE Cop0Instruction Cop0Op() const { return static_cast<Cop0Instruction>(bits & UINT32_C(0x3F)); }
  } cop;

  bool IsCop2Instruction() const
  {
    return (op == InstructionOp::cop2 || op == InstructionOp::lwc2 || op == InstructionOp::swc2);
  }
};

// Instruction helpers.
bool IsNopInstruction(const Instruction& instruction);
bool IsBranchInstruction(const Instruction& instruction);
bool IsUnconditionalBranchInstruction(const Instruction& instruction);
bool IsDirectBranchInstruction(const Instruction& instruction);
VirtualMemoryAddress GetDirectBranchTarget(const Instruction& instruction, VirtualMemoryAddress instruction_pc);
bool IsCallInstruction(const Instruction& instruction);
bool IsReturnInstruction(const Instruction& instruction);
bool IsMemoryLoadInstruction(const Instruction& instruction);
bool IsMemoryStoreInstruction(const Instruction& instruction);
bool InstructionHasLoadDelay(const Instruction& instruction);
bool IsExitBlockInstruction(const Instruction& instruction);
bool CanInstructionTrap(const Instruction& instruction, bool in_user_mode);
bool IsInvalidInstruction(const Instruction& instruction);

struct Registers
{
  union
  {
    uint32_t r[static_cast<uint8_t>(Reg::count)];

    struct
    {
      uint32_t zero; // r0
      uint32_t at;   // r1
      uint32_t v0;   // r2
      uint32_t v1;   // r3
      uint32_t a0;   // r4
      uint32_t a1;   // r5
      uint32_t a2;   // r6
      uint32_t a3;   // r7
      uint32_t t0;   // r8
      uint32_t t1;   // r9
      uint32_t t2;   // r10
      uint32_t t3;   // r11
      uint32_t t4;   // r12
      uint32_t t5;   // r13
      uint32_t t6;   // r14
      uint32_t t7;   // r15
      uint32_t s0;   // r16
      uint32_t s1;   // r17
      uint32_t s2;   // r18
      uint32_t s3;   // r19
      uint32_t s4;   // r20
      uint32_t s5;   // r21
      uint32_t s6;   // r22
      uint32_t s7;   // r23
      uint32_t t8;   // r24
      uint32_t t9;   // r25
      uint32_t k0;   // r26
      uint32_t k1;   // r27
      uint32_t gp;   // r28
      uint32_t sp;   // r29
      uint32_t fp;   // r30
      uint32_t ra;   // r31

      // not accessible to instructions
      uint32_t hi;
      uint32_t lo;
      uint32_t pc;  // at execution time: the address of the next instruction to execute (already fetched)
      uint32_t npc; // at execution time: the address of the next instruction to fetch
    };
  };
};

std::optional<VirtualMemoryAddress> GetLoadStoreEffectiveAddress(const Instruction& instruction, const Registers* regs);

enum class Cop0Reg : uint8_t
{
  BPC = 3,
  BDA = 5,
  JUMPDEST = 6,
  DCIC = 7,
  BadVaddr = 8,
  BDAM = 9,
  BPCM = 11,
  SR = 12,
  CAUSE = 13,
  EPC = 14,
  PRID = 15
};

enum class Exception : uint8_t
{
  INT = 0x00,     // interrupt
  MOD = 0x01,     // tlb modification
  TLBL = 0x02,    // tlb load
  TLBS = 0x03,    // tlb store
  AdEL = 0x04,    // address error, data load/instruction fetch
  AdES = 0x05,    // address error, data store
  IBE = 0x06,     // bus error on instruction fetch
  DBE = 0x07,     // bus error on data load/store
  Syscall = 0x08, // system call instruction
  BP = 0x09,      // break instruction
  RI = 0x0A,      // reserved instruction
  CpU = 0x0B,     // coprocessor unusable
  Ov = 0x0C,      // arithmetic overflow
};

struct Cop0Registers
{
  uint32_t BPC;      // breakpoint on execute
  uint32_t BDA;      // breakpoint on data access
  uint32_t TAR;      // randomly memorized jump address
  uint32_t BadVaddr; // bad virtual address value
  uint32_t BDAM;     // data breakpoint mask
  uint32_t BPCM;     // execute breakpoint mask
  uint32_t EPC;      // return address from trap
  uint32_t PRID;     // processor ID

  union SR
  {
    uint32_t bits;
    BitField<uint32_t, bool, 0, 1> IEc;  // current interrupt enable
    BitField<uint32_t, bool, 1, 1> KUc;  // current kernel/user mode, user = 1
    BitField<uint32_t, bool, 2, 1> IEp;  // previous interrupt enable
    BitField<uint32_t, bool, 3, 1> KUp;  // previous kernel/user mode, user = 1
    BitField<uint32_t, bool, 4, 1> IEo;  // old interrupt enable
    BitField<uint32_t, bool, 5, 1> KUo;  // old kernel/user mode, user = 1
    BitField<uint32_t, uint8_t, 8, 8> Im;     // interrupt mask, set to 1 = allowed to trigger
    BitField<uint32_t, bool, 16, 1> Isc; // isolate cache, no writes to memory occur
    BitField<uint32_t, bool, 17, 1> Swc; // swap data and instruction caches
    BitField<uint32_t, bool, 18, 1> PZ;  // zero cache parity bits
    BitField<uint32_t, bool, 19, 1> CM;  // last isolated load contains data from memory (tag matches?)
    BitField<uint32_t, bool, 20, 1> PE;  // cache parity error
    BitField<uint32_t, bool, 21, 1> TS;  // tlb shutdown - matched two entries
    BitField<uint32_t, bool, 22, 1> BEV; // boot exception vectors, 0 = KSEG0, 1 = KSEG1
    BitField<uint32_t, bool, 25, 1> RE;  // reverse endianness in user mode
    BitField<uint32_t, bool, 28, 1> CU0; // coprocessor 0 enable in user mode
    BitField<uint32_t, bool, 29, 1> CE1; // coprocessor 1 enable
    BitField<uint32_t, bool, 30, 1> CE2; // coprocessor 2 enable
    BitField<uint32_t, bool, 31, 1> CE3; // coprocessor 3 enable

    BitField<uint32_t, uint8_t, 0, 6> mode_bits;
    BitField<uint32_t, uint8_t, 28, 2> coprocessor_enable_mask;

    static constexpr uint32_t WRITE_MASK = 0b1111'0010'0111'1111'1111'1111'0011'1111;
  } sr;

  union CAUSE
  {
    uint32_t bits;
    BitField<uint32_t, Exception, 2, 5> Excode; // which exception occurred
    BitField<uint32_t, uint8_t, 8, 8> Ip;            // interrupt pending
    BitField<uint32_t, uint8_t, 28, 2> CE;           // coprocessor number if caused by a coprocessor
    BitField<uint32_t, bool, 30, 1> BT;         // exception occurred in branch delay slot, and the branch was taken
    BitField<uint32_t, bool, 31, 1> BD;         // exception occurred in branch delay slot, but pushed IP is for branch

    static constexpr uint32_t WRITE_MASK = 0b0000'0000'0000'0000'0000'0011'0000'0000;
    static constexpr uint32_t EXCEPTION_WRITE_MASK = 0b1111'0000'0000'0000'0000'0000'0111'1100;

    static uint32_t MakeValueForException(Exception excode, bool BD, bool BT, uint8_t CE)
    {
      CAUSE c = {};
      c.Excode = excode;
      c.BD = BD;
      c.BT = BT;
      c.CE = CE;
      return c.bits;
    }
  } cause;

  union DCIC
  {
    uint32_t bits;
    BitField<uint32_t, bool, 0, 1> status_any_break;
    BitField<uint32_t, bool, 1, 1> status_bpc_code_break;
    BitField<uint32_t, bool, 2, 1> status_bda_data_break;
    BitField<uint32_t, bool, 3, 1> status_bda_data_read_break;
    BitField<uint32_t, bool, 4, 1> status_bda_data_write_break;
    BitField<uint32_t, bool, 5, 1> status_any_jump_break;
    BitField<uint32_t, uint8_t, 12, 2> jump_redirection;
    BitField<uint32_t, bool, 23, 1> super_master_enable_1;
    BitField<uint32_t, bool, 24, 1> execution_breakpoint_enable;
    BitField<uint32_t, bool, 25, 1> data_access_breakpoint;
    BitField<uint32_t, bool, 26, 1> break_on_data_read;
    BitField<uint32_t, bool, 27, 1> break_on_data_write;
    BitField<uint32_t, bool, 28, 1> break_on_any_jump;
    BitField<uint32_t, bool, 29, 1> master_enable_any_jump;
    BitField<uint32_t, bool, 30, 1> master_enable_break;
    BitField<uint32_t, bool, 31, 1> super_master_enable_2;

    static constexpr uint32_t WRITE_MASK = 0b1111'1111'1000'0000'1111'0000'0011'1111;

    static constexpr uint32_t ANY_BREAKPOINTS_ENABLED_BITS = (1u << 24) | (1u << 26) | (1u << 27) | (1u << 28);
    static constexpr uint32_t MASTER_ENABLE_BITS = (1u << 23) | (1u << 31);

    constexpr bool ExecutionBreakpointsEnabled() const
    {
      const uint32_t mask = (1u << 23) | (1u << 24) | (1u << 31);
      return ((bits & mask) == mask);
    }

    constexpr bool DataReadBreakpointsEnabled() const
    {
      const uint32_t mask = (1u << 23) | (1u << 25) | (1u << 26) | (1u << 31);
      return ((bits & mask) == mask);
    }

    constexpr bool DataWriteBreakpointsEnabled() const
    {
      const uint32_t mask = (1u << 23) | (1u << 25) | (1u << 27) | (1u << 31);
      return ((bits & mask) == mask);
    }
  } dcic;
};

} // namespace CPU
