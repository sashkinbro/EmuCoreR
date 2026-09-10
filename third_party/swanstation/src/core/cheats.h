#pragma once
#include "common/bitfield.h"
#include "types.h"
#include <string>
#include <vector>

struct CheatCode
{
  enum class InstructionCode : uint8_t
  {
    Nop = 0x00,
    ConstantWrite8 = 0x30,
    ConstantWrite16 = 0x80,
    ScratchpadWrite16 = 0x1F,
    Increment16 = 0x10,
    Decrement16 = 0x11,
    Increment8 = 0x20,
    Decrement8 = 0x21,
    DelayActivation = 0xC1,
    SkipIfNotEqual16 = 0xC0,
    SkipIfButtonsNotEqual = 0xD5,
    SkipIfButtonsEqual = 0xD6,
    CompareButtons = 0xD4,
    CompareEqual16 = 0xD0,
    CompareNotEqual16 = 0xD1,
    CompareLess16 = 0xD2,
    CompareGreater16 = 0xD3,
    CompareEqual8 = 0xE0,
    CompareNotEqual8 = 0xE1,
    CompareLess8 = 0xE2,
    CompareGreater8 = 0xE3,
    Slide = 0x50,
    MemoryCopy = 0xC2,
    ExtImprovedSlide = 0x53,

    // Extension opcodes, not present on original GameShark.
    ExtConstantWrite32 = 0x90,
    ExtScratchpadWrite32 = 0xA5,
    ExtCompareEqual32 = 0xA0,
    ExtCompareNotEqual32 = 0xA1,
    ExtCompareLess32 = 0xA2,
    ExtCompareGreater32 = 0xA3,
    ExtSkipIfNotEqual32 = 0xA4,
    ExtIncrement32 = 0x60,
    ExtDecrement32 = 0x61,
    ExtConstantWriteIfMatch16 = 0xA6,
    ExtConstantWriteIfMatchWithRestore16 = 0xA7,
    ExtConstantForceRange8 = 0xF0,
    ExtConstantForceRangeLimits16 = 0xF1,
    ExtConstantForceRangeRollRound16 = 0xF2,
    ExtConstantForceRange16 = 0xF3,
    ExtFindAndReplace = 0xF4,
    ExtConstantSwap16 = 0xF5,

    ExtConstantBitSet8 = 0x31,
    ExtConstantBitClear8 = 0x32,
    ExtConstantBitSet16 = 0x81,
    ExtConstantBitClear16 = 0x82,
    ExtConstantBitSet32 = 0x91,
    ExtConstantBitClear32 = 0x92,

    ExtBitCompareButtons = 0xD7,
    ExtSkipIfNotLess8 = 0xC3,
    ExtSkipIfNotGreater8 = 0xC4,
    ExtSkipIfNotLess16 = 0xC5,
    ExtSkipIfNotGreater16 = 0xC6,
    ExtMultiConditionals = 0xF6,

    ExtCheatRegisters = 0x51,
    ExtCheatRegistersCompare = 0x52,
    
    ExtCompareBitsSet8 = 0xE4,   //Only used inside ExtMultiConditionals
    ExtCompareBitsClear8 = 0xE5, //Only used inside ExtMultiConditionals
  };

  union Instruction
  {
    uint64_t bits;

    struct
    {
      uint32_t second;
      uint32_t first;
    };

    BitField<uint64_t, InstructionCode, 32 + 24, 8> code;
    BitField<uint64_t, uint32_t, 32, 24> address;
    BitField<uint64_t, uint32_t, 0, 32> value32;
    BitField<uint64_t, uint16_t, 0, 16> value16;
    BitField<uint64_t, uint8_t, 0, 8> value8;
  };

  std::string description;
  std::vector<Instruction> instructions;
  bool enabled = false;

  ALWAYS_INLINE bool Valid() const { return !instructions.empty() && !description.empty(); }

  uint32_t GetNextNonConditionalInstruction(uint32_t index) const;

  void Apply() const;
};

class CheatList final
{
public:
  CheatList();
  ~CheatList();

  void SetCode(uint32_t index, CheatCode cc);

  static bool ParseLibretroCheat(CheatCode* cc, const char* line);

  // Reset the global scratch register file used by the D7/0x51/0x52 cheat
  // instruction families. The registers are TU-local statics that persist
  // for the lifetime of the process - if the host loads a new game without
  // restarting the core, register-using cheats from the previous game would
  // otherwise see leftover values. Call this when the cheat lifecycle
  // begins fresh (currently from System::Shutdown after the cheat list is
  // released).
  static void ResetSharedScratchRegisters();

  void Apply();

private:
  std::vector<CheatCode> m_codes;
};
