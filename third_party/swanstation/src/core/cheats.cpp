#include "cheats.h"
#include "bus.h"
#include "common/log.h"
#include "controller.h"
#include "cpu_code_cache.h"
#include "cpu_core.h"
#include "system.h"
#include <cstring>
#include <type_traits>
Log_SetChannel(Cheats);
static std::array<uint32_t, 256> cht_register; // Used for D7 ,51 & 52 cheat types

void CheatList::ResetSharedScratchRegisters()
{
  cht_register.fill(0);
}

template<typename T>
static T DoMemoryRead(VirtualMemoryAddress address)
{
  using UnsignedType = typename std::make_unsigned_t<T>;
  T result;
  if constexpr (std::is_same_v<UnsignedType, uint8_t>)
    return CPU::SafeReadMemoryByte(address, &result) ? result : static_cast<T>(0);
  else if constexpr (std::is_same_v<UnsignedType, uint16_t>)
    return CPU::SafeReadMemoryHalfWord(address, &result) ? result : static_cast<T>(0);
  else // if constexpr (std::is_same_v<UnsignedType, uint32_t>)
    return CPU::SafeReadMemoryWord(address, &result) ? result : static_cast<T>(0);
}

template<typename T>
static void DoMemoryWrite(PhysicalMemoryAddress address, T value)
{
  using UnsignedType = typename std::make_unsigned_t<T>;
  if constexpr (std::is_same_v<UnsignedType, uint8_t>)
    CPU::SafeWriteMemoryByte(address, value);
  else if constexpr (std::is_same_v<UnsignedType, uint16_t>)
    CPU::SafeWriteMemoryHalfWord(address, value);
  else // if constexpr (std::is_same_v<UnsignedType, uint32_t>)
    CPU::SafeWriteMemoryWord(address, value);
}

static uint32_t GetControllerButtonBits()
{
  static constexpr std::array<uint16_t, 16> button_mapping = {{
    0x0100, // Select
    0x0200, // L3
    0x0400, // R3
    0x0800, // Start
    0x1000, // Up
    0x2000, // Right
    0x4000, // Down
    0x8000, // Left
    0x0001, // L2
    0x0002, // R2
    0x0004, // L1
    0x0008, // R1
    0x0010, // Triangle
    0x0020, // Circle
    0x0040, // Cross
    0x0080, // Square
  }};

  uint32_t bits = 0;
  for (uint32_t i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
  {
    Controller* controller = System::GetController(i);
    if (!controller)
      continue;

    bits |= controller->GetButtonStateBits();
  }

  uint32_t translated_bits = 0;
  for (uint32_t i = 0, bit = 1; i < static_cast<uint32_t>(button_mapping.size()); i++, bit <<= 1)
  {
    if (bits & bit)
      translated_bits |= button_mapping[i];
  }

  return translated_bits;
}

static uint32_t GetControllerAnalogBits()
{
  // 0x010000 - Right Thumb Up
  // 0x020000 - Right Thumb Right
  // 0x040000 - Right Thumb Down
  // 0x080000 - Right Thumb Left
  // 0x100000 - Left Thumb Up
  // 0x200000 - Left Thumb Right
  // 0x400000 - Left Thumb Down
  // 0x800000 - Left Thumb Left

  uint32_t bits = 0;
  uint8_t l_ypos = 0;
  uint8_t l_xpos = 0;
  uint8_t r_ypos = 0;
  uint8_t r_xpos = 0;

  std::optional<uint32_t> analog = 0;
  for (uint32_t i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
  {
    Controller* controller = System::GetController(i);
    if (!controller)
      continue;

    analog = controller->GetAnalogInputBytes();
    if (analog.has_value())
    {
      l_ypos = static_cast<uint8_t>(analog.value() >> 24);
      l_xpos = static_cast<uint8_t>(analog.value() >> 16);
      r_ypos = static_cast<uint8_t>(analog.value() >> 8);
      r_xpos = static_cast<uint8_t>(analog.value());
      if (l_ypos < 0x50)
        bits |= 0x100000;
      else if (l_ypos > 0xA0)
        bits |= 0x400000;
      if (l_xpos < 0x50)
        bits |= 0x800000;
      else if (l_xpos > 0xA0)
        bits |= 0x200000;
      if (r_ypos < 0x50)
        bits |= 0x10000;
      else if (r_ypos > 0xA0)
        bits |= 0x40000;
      if (r_xpos < 0x50)
        bits |= 0x80000;
      else if (r_xpos > 0xA0)
        bits |= 0x20000;
    }
  }
  return bits;
}

CheatList::CheatList() = default;

CheatList::~CheatList() = default;


static bool IsLibretroSeparator(char ch)
{
  return (ch == ' ' || ch == '-' || ch == ':' || ch == '+');
}

bool CheatList::ParseLibretroCheat(CheatCode* cc, const char* line)
{
  const char* current_ptr = line;
  while (current_ptr)
  {
    char* end_ptr;
    CheatCode::Instruction inst;
    inst.first = static_cast<uint32_t>(std::strtoul(current_ptr, &end_ptr, 16));
    current_ptr = end_ptr;
    if (end_ptr)
    {
      if (!IsLibretroSeparator(*end_ptr))
      {
        Log_WarningPrintf("Malformed code '%s'", line);
        break;
      }

      end_ptr++;
      inst.second = static_cast<uint32_t>(std::strtoul(current_ptr, &end_ptr, 16));
      if (end_ptr && *end_ptr == '\0')
        end_ptr = nullptr;

      if (end_ptr && *end_ptr != '\0')
      {
        if (!IsLibretroSeparator(*end_ptr))
        {
          Log_WarningPrintf("Malformed code '%s'", line);
          break;
        }

        end_ptr++;
      }

      current_ptr = end_ptr;
      cc->instructions.push_back(inst);
    }
  }

  return !cc->instructions.empty();
}

void CheatList::Apply()
{
  for (const CheatCode& code : m_codes)
  {
    if (code.enabled)
      code.Apply();
  }
}

void CheatList::SetCode(uint32_t index, CheatCode cc)
{
  if (index > m_codes.size())
    return;

  if (index == m_codes.size())
  {
    m_codes.push_back(std::move(cc));
    return;
  }

  m_codes[index] = std::move(cc);
}

static bool IsConditionalInstruction(CheatCode::InstructionCode code)
{
  switch (code)
  {
    case CheatCode::InstructionCode::CompareEqual16:       // D0
    case CheatCode::InstructionCode::CompareNotEqual16:    // D1
    case CheatCode::InstructionCode::CompareLess16:        // D2
    case CheatCode::InstructionCode::CompareGreater16:     // D3
    case CheatCode::InstructionCode::CompareEqual8:        // E0
    case CheatCode::InstructionCode::CompareNotEqual8:     // E1
    case CheatCode::InstructionCode::CompareLess8:         // E2
    case CheatCode::InstructionCode::CompareGreater8:      // E3
    case CheatCode::InstructionCode::CompareButtons:       // D4
    case CheatCode::InstructionCode::ExtCompareEqual32:    // A0
    case CheatCode::InstructionCode::ExtCompareNotEqual32: // A1
    case CheatCode::InstructionCode::ExtCompareLess32:     // A2
    case CheatCode::InstructionCode::ExtCompareGreater32:  // A3
      return true;

    default:
      return false;
  }
}

uint32_t CheatCode::GetNextNonConditionalInstruction(uint32_t index) const
{
  const uint32_t count = static_cast<uint32_t>(instructions.size());
  for (; index < count; index++)
  {
    if (!IsConditionalInstruction(instructions[index].code))
    {
      // we've found the first non conditional instruction in the chain, so skip over the instruction following it
      return index + 1;
    }
  }

  return index;
}

void CheatCode::Apply() const
{
  const uint32_t count = static_cast<uint32_t>(instructions.size());
  uint32_t index = 0;
  for (; index < count;)
  {
    const Instruction& inst = instructions[index];
    switch (inst.code)
    {
      case InstructionCode::Nop:
      {
        index++;
      }
      break;

      case InstructionCode::ConstantWrite8:
      {
        DoMemoryWrite<uint8_t>(inst.address, inst.value8);
        index++;
      }
      break;

      case InstructionCode::ConstantWrite16:
      {
        DoMemoryWrite<uint16_t>(inst.address, inst.value16);
        index++;
      }
      break;

      case InstructionCode::ExtConstantWrite32:
      {
        DoMemoryWrite<uint32_t>(inst.address, inst.value32);
        index++;
      }
      break;

      case InstructionCode::ExtConstantBitSet8:
      {
        const uint8_t value = DoMemoryRead<uint8_t>(inst.address) | inst.value8;
        DoMemoryWrite<uint8_t>(inst.address, value);
        index++;
      }
      break;

      case InstructionCode::ExtConstantBitSet16:
      {
        const uint16_t value = DoMemoryRead<uint16_t>(inst.address) | inst.value16;
        DoMemoryWrite<uint16_t>(inst.address, value);
        index++;
      }
      break;

      case InstructionCode::ExtConstantBitSet32:
      {
        const uint32_t value = DoMemoryRead<uint32_t>(inst.address) | inst.value32;
        DoMemoryWrite<uint32_t>(inst.address, value);
        index++;
      }
      break;

      case InstructionCode::ExtConstantBitClear8:
      {
        const uint8_t value = DoMemoryRead<uint8_t>(inst.address) & ~inst.value8;
        DoMemoryWrite<uint8_t>(inst.address, value);
        index++;
      }
      break;

      case InstructionCode::ExtConstantBitClear16:
      {
        const uint16_t value = DoMemoryRead<uint16_t>(inst.address) & ~inst.value16;
        DoMemoryWrite<uint16_t>(inst.address, value);
        index++;
      }
      break;

      case InstructionCode::ExtConstantBitClear32:
      {
        const uint32_t value = DoMemoryRead<uint32_t>(inst.address) & ~inst.value32;
        DoMemoryWrite<uint32_t>(inst.address, value);
        index++;
      }
      break;

      case InstructionCode::ScratchpadWrite16:
      {
        DoMemoryWrite<uint16_t>(CPU::DCACHE_LOCATION | (inst.address & CPU::DCACHE_OFFSET_MASK), inst.value16);
        index++;
      }
      break;

      case InstructionCode::ExtScratchpadWrite32:
      {
        DoMemoryWrite<uint32_t>(CPU::DCACHE_LOCATION | (inst.address & CPU::DCACHE_OFFSET_MASK), inst.value32);
        index++;
      }
      break;

      case InstructionCode::ExtIncrement32:
      {
        const uint32_t value = DoMemoryRead<uint32_t>(inst.address);
        DoMemoryWrite<uint32_t>(inst.address, value + inst.value32);
        index++;
      }
      break;

      case InstructionCode::ExtDecrement32:
      {
        const uint32_t value = DoMemoryRead<uint32_t>(inst.address);
        DoMemoryWrite<uint32_t>(inst.address, value - inst.value32);
        index++;
      }
      break;

      case InstructionCode::Increment16:
      {
        const uint16_t value = DoMemoryRead<uint16_t>(inst.address);
        DoMemoryWrite<uint16_t>(inst.address, value + inst.value16);
        index++;
      }
      break;

      case InstructionCode::Decrement16:
      {
        const uint16_t value = DoMemoryRead<uint16_t>(inst.address);
        DoMemoryWrite<uint16_t>(inst.address, value - inst.value16);
        index++;
      }
      break;

      case InstructionCode::Increment8:
      {
        const uint8_t value = DoMemoryRead<uint8_t>(inst.address);
        DoMemoryWrite<uint8_t>(inst.address, value + inst.value8);
        index++;
      }
      break;

      case InstructionCode::Decrement8:
      {
        const uint8_t value = DoMemoryRead<uint8_t>(inst.address);
        DoMemoryWrite<uint8_t>(inst.address, value - inst.value8);
        index++;
      }
      break;

      case InstructionCode::ExtCompareEqual32:
      {
        const uint32_t value = DoMemoryRead<uint32_t>(inst.address);
        if (value == inst.value32)
          index++;
        else
          index = GetNextNonConditionalInstruction(index);
      }
      break;

      case InstructionCode::ExtCompareNotEqual32:
      {
        const uint32_t value = DoMemoryRead<uint32_t>(inst.address);
        if (value != inst.value32)
          index++;
        else
          index = GetNextNonConditionalInstruction(index);
      }
      break;

      case InstructionCode::ExtCompareLess32:
      {
        const uint32_t value = DoMemoryRead<uint32_t>(inst.address);
        if (value < inst.value32)
          index++;
        else
          index = GetNextNonConditionalInstruction(index);
      }
      break;

      case InstructionCode::ExtCompareGreater32:
      {
        const uint32_t value = DoMemoryRead<uint32_t>(inst.address);
        if (value > inst.value32)
          index++;
        else
          index = GetNextNonConditionalInstruction(index);
      }
      break;

      case InstructionCode::ExtConstantWriteIfMatch16:
      case InstructionCode::ExtConstantWriteIfMatchWithRestore16:
      {
        const uint16_t value = DoMemoryRead<uint16_t>(inst.address);
        const uint16_t comparevalue = static_cast<uint16_t>(inst.value32 >> 16);
        const uint16_t newvalue = static_cast<uint16_t>(inst.value32);
        if (value == comparevalue)
          DoMemoryWrite<uint16_t>(inst.address, newvalue);

        index++;
      }
      break;

      case InstructionCode::ExtConstantForceRange8:
      {
        const uint8_t value = DoMemoryRead<uint8_t>(inst.address);
        const uint8_t min = static_cast<uint8_t>(inst.value32);
        const uint8_t max = static_cast<uint8_t>(inst.value32 >> 8);
        const uint8_t overmin = static_cast<uint8_t>(inst.value32 >> 16);
        const uint8_t overmax = static_cast<uint8_t>(inst.value32 >> 24);
        if ((value < min) || (value < min && min == 0x00u && max < 0xFEu))
          DoMemoryWrite<uint8_t>(inst.address, overmin); // also handles a min value of 0x00
        else if (value > max)
          DoMemoryWrite<uint8_t>(inst.address, overmax);
        index++;
      }
      break;

      case InstructionCode::ExtConstantForceRangeLimits16:
      {
        const uint16_t value = DoMemoryRead<uint16_t>(inst.address);
        const uint16_t min = static_cast<uint16_t>(inst.value32);
        const uint16_t max = static_cast<uint16_t>(inst.value32 >> 16);
        if ((value < min) || (value < min && min == 0x0000u && max < 0xFFFEu))
          DoMemoryWrite<uint16_t>(inst.address, min); // also handles a min value of 0x0000
        else if (value > max)
          DoMemoryWrite<uint16_t>(inst.address, max);
        index++;
      }
      break;

      case InstructionCode::ExtConstantForceRangeRollRound16:
      {
        const uint16_t value = DoMemoryRead<uint16_t>(inst.address);
        const uint16_t min = static_cast<uint16_t>(inst.value32);
        const uint16_t max = static_cast<uint16_t>(inst.value32 >> 16);
        if ((value < min) || (value < min && min == 0x0000u && max < 0xFFFEu))
          DoMemoryWrite<uint16_t>(inst.address, max); // also handles a min value of 0x0000
        else if (value > max)
          DoMemoryWrite<uint16_t>(inst.address, min);
        index++;
      }
      break;

      case InstructionCode::ExtConstantForceRange16:
      {
        const uint16_t min = static_cast<uint16_t>(inst.value32);
        const uint16_t max = static_cast<uint16_t>(inst.value32 >> 16);
        const uint16_t value = DoMemoryRead<uint16_t>(inst.address);
        const Instruction& inst2 = instructions[index + 1];
        const uint16_t overmin = static_cast<uint16_t>(inst2.value32);
        const uint16_t overmax = static_cast<uint16_t>(inst2.value32 >> 16);

        if ((value < min) || (value < min && min == 0x0000u && max < 0xFFFEu))
          DoMemoryWrite<uint16_t>(inst.address, overmin); // also handles a min value of 0x0000
        else if (value > max)
          DoMemoryWrite<uint16_t>(inst.address, overmax);
        index += 2;
      }
      break;

      case InstructionCode::ExtConstantSwap16:
      {
        const uint16_t value1 = static_cast<uint16_t>(inst.value32);
        const uint16_t value2 = static_cast<uint16_t>(inst.value32 >> 16);
        const uint16_t value = DoMemoryRead<uint16_t>(inst.address);

        if (value == value1)
          DoMemoryWrite<uint16_t>(inst.address, value2);
        else if (value == value2)
          DoMemoryWrite<uint16_t>(inst.address, value1);
        index++;
      }
      break;

      case InstructionCode::ExtFindAndReplace:
      {

        if ((index + 4) >= instructions.size())
        {
          Log_ErrorPrintf("Incomplete find/replace instruction");
          return;
        }
        const Instruction& inst2 = instructions[index + 1];
        const Instruction& inst3 = instructions[index + 2];
        const Instruction& inst4 = instructions[index + 3];
        const Instruction& inst5 = instructions[index + 4];

        const uint32_t offset = static_cast<uint16_t>(inst.value32) << 1;
        const uint8_t wildcard = static_cast<uint8_t>(inst.value32 >> 16);
        const uint32_t minaddress = inst.address - offset;
        const uint32_t maxaddress = inst.address + offset;
        const uint8_t f1 = static_cast<uint8_t>(inst2.first >> 24);
        const uint8_t f2 = static_cast<uint8_t>(inst2.first >> 16);
        const uint8_t f3 = static_cast<uint8_t>(inst2.first >> 8);
        const uint8_t f4 = static_cast<uint8_t>(inst2.first);
        const uint8_t f5 = static_cast<uint8_t>(inst2.value32 >> 24);
        const uint8_t f6 = static_cast<uint8_t>(inst2.value32 >> 16);
        const uint8_t f7 = static_cast<uint8_t>(inst2.value32 >> 8);
        const uint8_t f8 = static_cast<uint8_t>(inst2.value32);
        const uint8_t f9 = static_cast<uint8_t>(inst3.first >> 24);
        const uint8_t f10 = static_cast<uint8_t>(inst3.first >> 16);
        const uint8_t f11 = static_cast<uint8_t>(inst3.first >> 8);
        const uint8_t f12 = static_cast<uint8_t>(inst3.first);
        const uint8_t f13 = static_cast<uint8_t>(inst3.value32 >> 24);
        const uint8_t f14 = static_cast<uint8_t>(inst3.value32 >> 16);
        const uint8_t f15 = static_cast<uint8_t>(inst3.value32 >> 8);
        const uint8_t f16 = static_cast<uint8_t>(inst3.value32);
        const uint8_t r1 = static_cast<uint8_t>(inst4.first >> 24);
        const uint8_t r2 = static_cast<uint8_t>(inst4.first >> 16);
        const uint8_t r3 = static_cast<uint8_t>(inst4.first >> 8);
        const uint8_t r4 = static_cast<uint8_t>(inst4.first);
        const uint8_t r5 = static_cast<uint8_t>(inst4.value32 >> 24);
        const uint8_t r6 = static_cast<uint8_t>(inst4.value32 >> 16);
        const uint8_t r7 = static_cast<uint8_t>(inst4.value32 >> 8);
        const uint8_t r8 = static_cast<uint8_t>(inst4.value32);
        const uint8_t r9 = static_cast<uint8_t>(inst5.first >> 24);
        const uint8_t r10 = static_cast<uint8_t>(inst5.first >> 16);
        const uint8_t r11 = static_cast<uint8_t>(inst5.first >> 8);
        const uint8_t r12 = static_cast<uint8_t>(inst5.first);
        const uint8_t r13 = static_cast<uint8_t>(inst5.value32 >> 24);
        const uint8_t r14 = static_cast<uint8_t>(inst5.value32 >> 16);
        const uint8_t r15 = static_cast<uint8_t>(inst5.value32 >> 8);
        const uint8_t r16 = static_cast<uint8_t>(inst5.value32);

        for (uint32_t address = minaddress; address <= maxaddress; address += 2)
        {
          if ((DoMemoryRead<uint8_t>(address) == f1 || f1 == wildcard) &&
              (DoMemoryRead<uint8_t>(address + 1) == f2 || f2 == wildcard) &&
              (DoMemoryRead<uint8_t>(address + 2) == f3 || f3 == wildcard) &&
              (DoMemoryRead<uint8_t>(address + 3) == f4 || f4 == wildcard) &&
              (DoMemoryRead<uint8_t>(address + 4) == f5 || f5 == wildcard) &&
              (DoMemoryRead<uint8_t>(address + 5) == f6 || f6 == wildcard) &&
              (DoMemoryRead<uint8_t>(address + 6) == f7 || f7 == wildcard) &&
              (DoMemoryRead<uint8_t>(address + 7) == f8 || f8 == wildcard) &&
              (DoMemoryRead<uint8_t>(address + 8) == f9 || f9 == wildcard) &&
              (DoMemoryRead<uint8_t>(address + 9) == f10 || f10 == wildcard) &&
              (DoMemoryRead<uint8_t>(address + 10) == f11 || f11 == wildcard) &&
              (DoMemoryRead<uint8_t>(address + 11) == f12 || f12 == wildcard) &&
              (DoMemoryRead<uint8_t>(address + 12) == f13 || f13 == wildcard) &&
              (DoMemoryRead<uint8_t>(address + 13) == f14 || f14 == wildcard) &&
              (DoMemoryRead<uint8_t>(address + 14) == f15 || f15 == wildcard) &&
              (DoMemoryRead<uint8_t>(address + 15) == f16 || f16 == wildcard))
          {
            if (r1 != wildcard)
              DoMemoryWrite<uint8_t>(address, r1);
            if (r2 != wildcard)
              DoMemoryWrite<uint8_t>(address + 1, r2);
            if (r3 != wildcard)
              DoMemoryWrite<uint8_t>(address + 2, r3);
            if (r4 != wildcard)
              DoMemoryWrite<uint8_t>(address + 3, r4);
            if (r5 != wildcard)
              DoMemoryWrite<uint8_t>(address + 4, r5);
            if (r6 != wildcard)
              DoMemoryWrite<uint8_t>(address + 5, r6);
            if (r7 != wildcard)
              DoMemoryWrite<uint8_t>(address + 6, r7);
            if (r8 != wildcard)
              DoMemoryWrite<uint8_t>(address + 7, r8);
            if (r9 != wildcard)
              DoMemoryWrite<uint8_t>(address + 8, r9);
            if (r10 != wildcard)
              DoMemoryWrite<uint8_t>(address + 9, r10);
            if (r11 != wildcard)
              DoMemoryWrite<uint8_t>(address + 10, r11);
            if (r12 != wildcard)
              DoMemoryWrite<uint8_t>(address + 11, r12);
            if (r13 != wildcard)
              DoMemoryWrite<uint8_t>(address + 12, r13);
            if (r14 != wildcard)
              DoMemoryWrite<uint8_t>(address + 13, r14);
            if (r15 != wildcard)
              DoMemoryWrite<uint8_t>(address + 14, r15);
            if (r16 != wildcard)
              DoMemoryWrite<uint8_t>(address + 15, r16);
            address = address + 15;
          }
        }
        index += 5;
      }
      break;

      case InstructionCode::CompareEqual16:
      {
        const uint16_t value = DoMemoryRead<uint16_t>(inst.address);
        if (value == inst.value16)
          index++;
        else
          index = GetNextNonConditionalInstruction(index);
      }
      break;

      case InstructionCode::CompareNotEqual16:
      {
        const uint16_t value = DoMemoryRead<uint16_t>(inst.address);
        if (value != inst.value16)
          index++;
        else
          index = GetNextNonConditionalInstruction(index);
      }
      break;

      case InstructionCode::CompareLess16:
      {
        const uint16_t value = DoMemoryRead<uint16_t>(inst.address);
        if (value < inst.value16)
          index++;
        else
          index = GetNextNonConditionalInstruction(index);
      }
      break;

      case InstructionCode::CompareGreater16:
      {
        const uint16_t value = DoMemoryRead<uint16_t>(inst.address);
        if (value > inst.value16)
          index++;
        else
          index = GetNextNonConditionalInstruction(index);
      }
      break;

      case InstructionCode::CompareEqual8:
      {
        const uint8_t value = DoMemoryRead<uint8_t>(inst.address);
        if (value == inst.value8)
          index++;
        else
          index = GetNextNonConditionalInstruction(index);
      }
      break;

      case InstructionCode::CompareNotEqual8:
      {
        const uint8_t value = DoMemoryRead<uint8_t>(inst.address);
        if (value != inst.value8)
          index++;
        else
          index = GetNextNonConditionalInstruction(index);
      }
      break;

      case InstructionCode::CompareLess8:
      {
        const uint8_t value = DoMemoryRead<uint8_t>(inst.address);
        if (value < inst.value8)
          index++;
        else
          index = GetNextNonConditionalInstruction(index);
      }
      break;

      case InstructionCode::CompareGreater8:
      {
        const uint8_t value = DoMemoryRead<uint8_t>(inst.address);
        if (value > inst.value8)
          index++;
        else
          index = GetNextNonConditionalInstruction(index);
      }
      break;

      case InstructionCode::CompareButtons: // D4
      {
        if (inst.value16 == GetControllerButtonBits())
          index++;
        else
          index = GetNextNonConditionalInstruction(index);
      }
      break;

      case InstructionCode::ExtCheatRegisters: // 51
      {
        const uint32_t poke_value = inst.value32;
        const uint8_t cht_reg_no1 = static_cast<uint8_t>(inst.address);
        const uint8_t cht_reg_no2 = static_cast<uint8_t>(inst.address >> 8);
        const uint8_t cht_reg_no3 = static_cast<uint8_t>(inst.value32);
        const uint8_t sub_type = static_cast<uint8_t>(inst.address >> 16);

        switch (sub_type)
        {
          case 0x00: // Write the uint8_t from cht_register[cht_reg_no1] to address
            DoMemoryWrite<uint8_t>(inst.value32, static_cast<uint8_t>(cht_register[cht_reg_no1]) & 0xFFu);
            break;
          case 0x01: // Read the uint8_t from address to cht_register[cht_reg_no1]
            cht_register[cht_reg_no1] = DoMemoryRead<uint8_t>(inst.value32);
            break;
          case 0x02: // Write the uint8_t from address field to the address stored in cht_register[cht_reg_no1]
            DoMemoryWrite<uint8_t>(cht_register[cht_reg_no1], static_cast<uint8_t>(poke_value));
            break;
          case 0x03: // Write the uint8_t from cht_register[cht_reg_no2] to cht_register[cht_reg_no1]
                     // and add the uint8_t from the address field to it
            cht_register[cht_reg_no1] = static_cast<uint8_t>(cht_register[cht_reg_no2]) + static_cast<uint8_t>(poke_value);
            break;
          case 0x04: // Write the uint8_t from the value stored in cht_register[cht_reg_no2] + poke_value to the address
                     // stored in cht_register[cht_reg_no1]
            DoMemoryWrite<uint8_t>(cht_register[cht_reg_no1],
                              static_cast<uint8_t>(cht_register[cht_reg_no2]) + static_cast<uint8_t>(poke_value));
            break;
          case 0x05: // Write the uint8_t poke value to cht_register[cht_reg_no1]
            cht_register[cht_reg_no1] = static_cast<uint8_t>(poke_value);
            break;
          case 0x06: // Read the uint8_t value from the address (cht_register[cht_reg_no2] + poke_value) to
                     // cht_register[cht_reg_no1]
            cht_register[cht_reg_no1] = DoMemoryRead<uint8_t>(cht_register[cht_reg_no2] + poke_value);
            break;

          case 0x40: // Write the uint16_t from cht_register[cht_reg_no1] to address
            DoMemoryWrite<uint16_t>(inst.value32, static_cast<uint16_t>(cht_register[cht_reg_no1]));
            break;
          case 0x41: // Read the uint16_t from address to cht_register[cht_reg_no1]
            cht_register[cht_reg_no1] = DoMemoryRead<uint16_t>(inst.value32);
            break;
          case 0x42: // Write the uint16_t from address field to the address stored in cht_register[cht_reg_no1]
            DoMemoryWrite<uint16_t>(cht_register[cht_reg_no1], static_cast<uint16_t>(poke_value));
            break;
          case 0x43: // Write the uint16_t from cht_register[cht_reg_no2] to cht_register[cht_reg_no1]
                     // and add the uint16_t from the address field to it
            cht_register[cht_reg_no1] =
              static_cast<uint16_t>(cht_register[cht_reg_no2]) + static_cast<uint16_t>(poke_value);
            break;
          case 0x44: // Write the uint16_t from the value stored in cht_register[cht_reg_no2] + poke_value to the address
                     // stored in cht_register[cht_reg_no1]
            DoMemoryWrite<uint16_t>(cht_register[cht_reg_no1],
                               static_cast<uint16_t>(cht_register[cht_reg_no2]) + static_cast<uint16_t>(poke_value));
            break;
          case 0x45: // Write the uint16_t poke value to cht_register[cht_reg_no1]
            cht_register[cht_reg_no1] = static_cast<uint16_t>(poke_value);
            break;
          case 0x46: // Read the uint16_t value from the address (cht_register[cht_reg_no2] + poke_value) to
                     // cht_register[cht_reg_no1]
            cht_register[cht_reg_no1] = DoMemoryRead<uint16_t>(cht_register[cht_reg_no2] + poke_value);
            break;

          case 0x80: // Write the uint32_t from cht_register[cht_reg_no1] to address
            DoMemoryWrite<uint32_t>(inst.value32, cht_register[cht_reg_no1]);
            break;
          case 0x81: // Read the uint32_t from address to cht_register[cht_reg_no1]
            cht_register[cht_reg_no1] = DoMemoryRead<uint32_t>(inst.value32);
            break;
          case 0x82: // Write the uint32_t from address field to the address stored in cht_register[cht_reg_no]
            DoMemoryWrite<uint32_t>(cht_register[cht_reg_no1], poke_value);
            break;
          case 0x83: // Write the uint32_t from cht_register[cht_reg_no2] to cht_register[cht_reg_no1]
                     // and add the uint32_t from the address field to it
            cht_register[cht_reg_no1] = cht_register[cht_reg_no2] + poke_value;
            break;
          case 0x84: // Write the uint32_t from the value stored in cht_register[cht_reg_no2] + poke_value to the address
                     // stored in cht_register[cht_reg_no1]
            DoMemoryWrite<uint32_t>(cht_register[cht_reg_no1], cht_register[cht_reg_no2] + poke_value);
            break;
          case 0x85: // Write the uint32_t poke value to cht_register[cht_reg_no1]
            cht_register[cht_reg_no1] = poke_value;
            break;
          case 0x86: // Read the uint32_t value from the address (cht_register[cht_reg_no2] + poke_value) to
                     // cht_register[cht_reg_no1]
            cht_register[cht_reg_no1] = DoMemoryRead<uint32_t>(cht_register[cht_reg_no2] + poke_value);
            break;

          case 0xC0: // Reg3 = Reg2 + Reg1
            cht_register[cht_reg_no3] = cht_register[cht_reg_no2] + cht_register[cht_reg_no1];
            break;
          case 0xC1: // Reg3 = Reg2 - Reg1
            cht_register[cht_reg_no3] = cht_register[cht_reg_no2] - cht_register[cht_reg_no1];
            break;
          case 0xC2: // Reg3 = Reg2 * Reg1
            cht_register[cht_reg_no3] = cht_register[cht_reg_no2] * cht_register[cht_reg_no1];
            break;
          case 0xC3: // Reg3 = Reg2 / Reg1 with DIV0 handling
            if (cht_register[cht_reg_no1] == 0)
              cht_register[cht_reg_no3] = 0;
            else
              cht_register[cht_reg_no3] = cht_register[cht_reg_no2] / cht_register[cht_reg_no1];
            break;
          case 0xC4: // Reg3 = Reg2 % Reg1 (with DIV0 handling)
            if (cht_register[cht_reg_no1] == 0)
              cht_register[cht_reg_no3] = cht_register[cht_reg_no2];
            else
              cht_register[cht_reg_no3] = cht_register[cht_reg_no2] % cht_register[cht_reg_no1];
            break;
          case 0xC5: // Reg3 = Reg2 & Reg1
            cht_register[cht_reg_no3] = cht_register[cht_reg_no2] & cht_register[cht_reg_no1];
            break;
          case 0xC6: // Reg3 = Reg2 | Reg1
            cht_register[cht_reg_no3] = cht_register[cht_reg_no2] | cht_register[cht_reg_no1];
            break;
          case 0xC7: // Reg3 = Reg2 ^ Reg1
            cht_register[cht_reg_no3] = cht_register[cht_reg_no2] ^ cht_register[cht_reg_no1];
            break;
          case 0xC8: // Reg3 = ~Reg1
            cht_register[cht_reg_no3] = ~cht_register[cht_reg_no1];
            break;
          case 0xC9: // Reg3 = Reg1 << X
            cht_register[cht_reg_no3] = cht_register[cht_reg_no1] << cht_reg_no2;
            break;
          case 0xCA: // Reg3 = Reg1 >> X
            cht_register[cht_reg_no3] = cht_register[cht_reg_no1] >> cht_reg_no2;
            break;
          // Lots of options exist for expanding into this space
          default:
            break;
        }
        index++;
      }
      break;

      case InstructionCode::SkipIfNotEqual16:      // C0
      case InstructionCode::ExtSkipIfNotEqual32:   // A4
      case InstructionCode::SkipIfButtonsNotEqual: // D5
      case InstructionCode::SkipIfButtonsEqual:    // D6
      case InstructionCode::ExtSkipIfNotLess8:     // C3
      case InstructionCode::ExtSkipIfNotGreater8:  // C4
      case InstructionCode::ExtSkipIfNotLess16:    // C5
      case InstructionCode::ExtSkipIfNotGreater16: // C6
      case InstructionCode::ExtMultiConditionals:  // F6
      {
        index++;

        bool activate_codes;
        switch (inst.code)
        {
          case InstructionCode::SkipIfNotEqual16: // C0
            activate_codes = (DoMemoryRead<uint16_t>(inst.address) == inst.value16);
            break;
          case InstructionCode::ExtSkipIfNotEqual32: // A4
            activate_codes = (DoMemoryRead<uint32_t>(inst.address) == inst.value32);
            break;
          case InstructionCode::SkipIfButtonsNotEqual: // D5
            activate_codes = (GetControllerButtonBits() == inst.value16);
            break;
          case InstructionCode::SkipIfButtonsEqual: // D6
            activate_codes = (GetControllerButtonBits() != inst.value16);
            break;
          case InstructionCode::ExtSkipIfNotLess8: // C3
            activate_codes = (DoMemoryRead<uint8_t>(inst.address) < inst.value8);
            break;
          case InstructionCode::ExtSkipIfNotGreater8: // C4
            activate_codes = (DoMemoryRead<uint8_t>(inst.address) > inst.value8);
            break;
          case InstructionCode::ExtSkipIfNotLess16: // C5
            activate_codes = (DoMemoryRead<uint16_t>(inst.address) < inst.value16);
            break;
          case InstructionCode::ExtSkipIfNotGreater16: // C6
            activate_codes = (DoMemoryRead<uint16_t>(inst.address) > inst.value16);
            break;
          case InstructionCode::ExtMultiConditionals: // F6
          {
            // Ensure any else if or else that are hit outside the if context are skipped
            if ((inst.value32 & 0xFFFFFF00u) != 0x1F000000)
            {
              activate_codes = false;
              break;
            }
            for (;;)
            {
              const uint8_t totalConds = static_cast<uint8_t>(instructions[index - 1].value32);
              const uint8_t conditionType = static_cast<uint8_t>(instructions[index - 1].address);

              bool conditions_check;

              if (conditionType == 0x00 && totalConds > 0) // AND
              {
                conditions_check = true;

                for (int i = 1; totalConds >= i; index++, i++)
                {
                  switch (instructions[index].code)
                  {
                    case InstructionCode::CompareEqual16: // D0
                      conditions_check &=
                        (DoMemoryRead<uint16_t>(instructions[index].address) == instructions[index].value16);
                      break;
                    case InstructionCode::CompareNotEqual16: // D1
                      conditions_check &=
                        (DoMemoryRead<uint16_t>(instructions[index].address) != instructions[index].value16);
                      break;
                    case InstructionCode::CompareLess16: // D2
                      conditions_check &=
                        (DoMemoryRead<uint16_t>(instructions[index].address) < instructions[index].value16);
                      break;
                    case InstructionCode::CompareGreater16: // D3
                      conditions_check &=
                        (DoMemoryRead<uint16_t>(instructions[index].address) > instructions[index].value16);
                      break;
                    case InstructionCode::CompareEqual8: // E0
                      conditions_check &= (DoMemoryRead<uint8_t>(instructions[index].address) == instructions[index].value8);
                      break;
                    case InstructionCode::CompareNotEqual8: // E1
                      conditions_check &= (DoMemoryRead<uint8_t>(instructions[index].address) != instructions[index].value8);
                      break;
                    case InstructionCode::CompareLess8: // E2
                      conditions_check &= (DoMemoryRead<uint8_t>(instructions[index].address) < instructions[index].value8);
                      break;
                    case InstructionCode::CompareGreater8: // E3
                      conditions_check &= (DoMemoryRead<uint8_t>(instructions[index].address) > instructions[index].value8);
                      break;
                    case InstructionCode::ExtCompareEqual32: // A0
                      conditions_check &=
                        (DoMemoryRead<uint32_t>(instructions[index].address) == instructions[index].value32);
                      break;
                    case InstructionCode::ExtCompareNotEqual32: // A1
                      conditions_check &=
                        (DoMemoryRead<uint32_t>(instructions[index].address) != instructions[index].value32);
                      break;
                    case InstructionCode::ExtCompareLess32: // A2
                      conditions_check &=
                        (DoMemoryRead<uint32_t>(instructions[index].address) < instructions[index].value32);
                      break;
                    case InstructionCode::ExtCompareGreater32: // A3
                      conditions_check &=
                        (DoMemoryRead<uint32_t>(instructions[index].address) > instructions[index].value32);
                      break;
                    case InstructionCode::ExtCompareBitsSet8: // E4 Internal to F6
                      conditions_check &=
                        (instructions[index].value8 ==
                         (DoMemoryRead<uint8_t>(instructions[index].address) & instructions[index].value8));
                      break;
                    case InstructionCode::ExtCompareBitsClear8: // E5 Internal to F6
                      conditions_check &=
                        ((DoMemoryRead<uint8_t>(instructions[index].address) & instructions[index].value8) == 0);
                      break;
                    case InstructionCode::ExtBitCompareButtons: // D7
                    {
                      const uint32_t frame_compare_value = instructions[index].address & 0xFFFFu;
                      const uint8_t cht_reg_no = static_cast<uint8_t>(instructions[index].value32 >> 24);
                      const bool bit_comparison_type = ((instructions[index].address & 0x100000u) >> 20);
                      const uint8_t frame_comparison = static_cast<uint8_t>((instructions[index].address & 0xF0000u) >> 16);
                      const uint32_t check_value = (instructions[index].value32 & 0xFFFFFFu);
                      const uint32_t value1 = GetControllerButtonBits();
                      const uint32_t value2 = GetControllerAnalogBits();
                      uint32_t value = value1 | value2;

                      if ((bit_comparison_type == false && check_value == (value & check_value)) // Check Bits are set
                          ||
                          (bit_comparison_type == true && check_value != (value & check_value))) // Check Bits are clear
                      {
                        cht_register[cht_reg_no] += 1;
                        switch (frame_comparison)
                        {
                          case 0x0: // No comparison on frame count, just do it
                            conditions_check &= true;
                            break;
                          case 0x1: // Check if frame_compare_value == current count
                            conditions_check &= (cht_register[cht_reg_no] == frame_compare_value);
                            break;
                          case 0x2: // Check if frame_compare_value < current count
                            conditions_check &= (cht_register[cht_reg_no] < frame_compare_value);
                            break;
                          case 0x3: // Check if frame_compare_value > current count
                            conditions_check &= (cht_register[cht_reg_no] > frame_compare_value);
                            break;
                          case 0x4: // Check if frame_compare_value != current count
                            conditions_check &= (cht_register[cht_reg_no] != frame_compare_value);
                            break;
                          default:
                            conditions_check &= false;
                            break;
                        }
                      }
                      else
                      {
                        cht_register[cht_reg_no] = 0;
                        conditions_check &= false;
                      }
                      break;
                    }
                    default:
                      Log_ErrorPrintf("Incorrect conditional instruction (see chtdb.txt for supported instructions)");
                      return;
                  }
                }
              }
              else if (conditionType == 0x01 && totalConds > 0) // OR
              {
                conditions_check = false;

                for (int i = 1; totalConds >= i; index++, i++)
                {
                  switch (instructions[index].code)
                  {
                    case InstructionCode::CompareEqual16: // D0
                      conditions_check |=
                        (DoMemoryRead<uint16_t>(instructions[index].address) == instructions[index].value16);
                      break;
                    case InstructionCode::CompareNotEqual16: // D1
                      conditions_check |=
                        (DoMemoryRead<uint16_t>(instructions[index].address) != instructions[index].value16);
                      break;
                    case InstructionCode::CompareLess16: // D2
                      conditions_check |=
                        (DoMemoryRead<uint16_t>(instructions[index].address) < instructions[index].value16);
                      break;
                    case InstructionCode::CompareGreater16: // D3
                      conditions_check |=
                        (DoMemoryRead<uint16_t>(instructions[index].address) > instructions[index].value16);
                      break;
                    case InstructionCode::CompareEqual8: // E0
                      conditions_check |= (DoMemoryRead<uint8_t>(instructions[index].address) == instructions[index].value8);
                      break;
                    case InstructionCode::CompareNotEqual8: // E1
                      conditions_check |= (DoMemoryRead<uint8_t>(instructions[index].address) != instructions[index].value8);
                      break;
                    case InstructionCode::CompareLess8: // E2
                      conditions_check |= (DoMemoryRead<uint8_t>(instructions[index].address) < instructions[index].value8);
                      break;
                    case InstructionCode::CompareGreater8: // E3
                      conditions_check |= (DoMemoryRead<uint8_t>(instructions[index].address) > instructions[index].value8);
                      break;
                    case InstructionCode::ExtCompareEqual32: // A0
                      conditions_check |=
                        (DoMemoryRead<uint32_t>(instructions[index].address) == instructions[index].value32);
                      break;
                    case InstructionCode::ExtCompareNotEqual32: // A1
                      conditions_check |=
                        (DoMemoryRead<uint32_t>(instructions[index].address) != instructions[index].value32);
                      break;
                    case InstructionCode::ExtCompareLess32: // A2
                      conditions_check |=
                        (DoMemoryRead<uint32_t>(instructions[index].address) < instructions[index].value32);
                      break;
                    case InstructionCode::ExtCompareGreater32: // A3
                      conditions_check |=
                        (DoMemoryRead<uint32_t>(instructions[index].address) > instructions[index].value32);
                      break;
                    case InstructionCode::ExtCompareBitsSet8: // E4 Internal to F6
                      conditions_check |=
                        (instructions[index].value8 ==
                         (DoMemoryRead<uint8_t>(instructions[index].address) & instructions[index].value8));
                      break;
                    case InstructionCode::ExtCompareBitsClear8: // E5 Internal to F6
                      conditions_check |=
                        ((DoMemoryRead<uint8_t>(instructions[index].address) & instructions[index].value8) == 0);
                      break;
                    case InstructionCode::ExtBitCompareButtons: // D7
                    {
                      const uint32_t frame_compare_value = instructions[index].address & 0xFFFFu;
                      const uint8_t cht_reg_no = static_cast<uint8_t>(instructions[index].value32 >> 24);
                      const bool bit_comparison_type = ((instructions[index].address & 0x100000u) >> 20);
                      const uint8_t frame_comparison = static_cast<uint8_t>((instructions[index].address & 0xF0000u) >> 16);
                      const uint32_t check_value = (instructions[index].value32 & 0xFFFFFFu);
                      const uint32_t value1 = GetControllerButtonBits();
                      const uint32_t value2 = GetControllerAnalogBits();
                      uint32_t value = value1 | value2;

                      if ((bit_comparison_type == false && check_value == (value & check_value)) // Check Bits are set
                          ||
                          (bit_comparison_type == true && check_value != (value & check_value))) // Check Bits are clear
                      {
                        cht_register[cht_reg_no] += 1;
                        switch (frame_comparison)
                        {
                          case 0x0: // No comparison on frame count, just do it
                            conditions_check |= true;
                            break;
                          case 0x1: // Check if frame_compare_value == current count
                            conditions_check |= (cht_register[cht_reg_no] == frame_compare_value);
                            break;
                          case 0x2: // Check if frame_compare_value < current count
                            conditions_check |= (cht_register[cht_reg_no] < frame_compare_value);
                            break;
                          case 0x3: // Check if frame_compare_value > current count
                            conditions_check |= (cht_register[cht_reg_no] > frame_compare_value);
                            break;
                          case 0x4: // Check if frame_compare_value != current count
                            conditions_check |= (cht_register[cht_reg_no] != frame_compare_value);
                            break;
                          default:
                            conditions_check |= false;
                            break;
                        }
                      }
                      else
                      {
                        cht_register[cht_reg_no] = 0;
                        conditions_check |= false;
                      }
                      break;
                    }
                    default:
                      Log_ErrorPrintf("Incorrect conditional instruction (see chtdb.txt for supported instructions)");
                      return;
                  }
                }
              }
              else
              {
                Log_ErrorPrintf("Incomplete multi conditional instruction");
                return;
              }
              if (conditions_check == true)
              {
                activate_codes = true;
                break;
              }
              else
              { // parse through to 00000000 FFFF and peek if next line is a F6 type associated with a ELSE
                activate_codes = false;
                // skip to the next separator (00000000 FFFF), or end
                constexpr uint64_t separator_value = UINT64_C(0x000000000000FFFF);
                constexpr uint64_t else_value = UINT64_C(0x00000000E15E0000);
                constexpr uint64_t elseif_value = UINT64_C(0x00000000E15E1F00);
                while (index < count)
                {
                  const uint64_t bits = instructions[index++].bits;
                  if (bits == separator_value)
                  {
                    const uint64_t bits_ahead = instructions[index].bits;
                    if ((bits_ahead & 0xFFFFFF00u) == elseif_value)
                    {
                      break;
                    }
                    if ((bits_ahead & 0xFFFF0000u) == else_value)
                    {
                      // index++;
                      activate_codes = true;
                      break;
                    }
                    index--;
                    break;
                  }
                  if ((bits & 0xFFFFFF00u) == elseif_value)
                  {
                    // index--;
                    break;
                  }
                  if ((bits & 0xFFFFFFFFu) == else_value)
                  {
                    // index++;
                    activate_codes = true;
                    break;
                  }
                }
                if (activate_codes == true)
                  break;
              }
            }
            break;
          }
          default:
            activate_codes = false;
            break;
        }

        if (activate_codes)
        {
          // execute following instructions
          continue;
        }

        // skip to the next separator (00000000 FFFF), or end
        constexpr uint64_t separator_value = UINT64_C(0x000000000000FFFF);
        while (index < count)
        {
          // we don't want to execute the separator instruction
          const uint64_t bits = instructions[index++].bits;
          if (bits == separator_value)
            break;
        }
      }
      break;

      case InstructionCode::ExtBitCompareButtons: // D7
      {
        index++;
        bool activate_codes;
        const uint32_t frame_compare_value = inst.address & 0xFFFFu;
        const uint8_t cht_reg_no = static_cast<uint8_t>(inst.value32 >> 24);
        const bool bit_comparison_type = ((inst.address & 0x100000u) >> 20);
        const uint8_t frame_comparison = static_cast<uint8_t>((inst.address & 0xF0000u) >> 16);
        const uint32_t check_value = (inst.value32 & 0xFFFFFFu);
        const uint32_t value1 = GetControllerButtonBits();
        const uint32_t value2 = GetControllerAnalogBits();
        uint32_t value = value1 | value2;

        if ((bit_comparison_type == false && check_value == (value & check_value))    // Check Bits are set
            || (bit_comparison_type == true && check_value != (value & check_value))) // Check Bits are clear
        {
          cht_register[cht_reg_no] += 1;
          switch (frame_comparison)
          {
            case 0x0: // No comparison on frame count, just do it
              activate_codes = true;
              break;
            case 0x1: // Check if frame_compare_value == current count
              activate_codes = (cht_register[cht_reg_no] == frame_compare_value);
              break;
            case 0x2: // Check if frame_compare_value < current count
              activate_codes = (cht_register[cht_reg_no] < frame_compare_value);
              break;
            case 0x3: // Check if frame_compare_value > current count
              activate_codes = (cht_register[cht_reg_no] > frame_compare_value);
              break;
            case 0x4: // Check if frame_compare_value != current count
              activate_codes = (cht_register[cht_reg_no] != frame_compare_value);
              break;
            default:
              activate_codes = false;
              break;
          }
        }
        else
        {
          cht_register[cht_reg_no] = 0;
          activate_codes = false;
        }

        if (activate_codes)
        {
          // execute following instructions
          continue;
        }

        // skip to the next separator (00000000 FFFF), or end
        constexpr uint64_t separator_value = UINT64_C(0x000000000000FFFF);
        while (index < count)
        {
          // we don't want to execute the separator instruction
          const uint64_t bits = instructions[index++].bits;
          if (bits == separator_value)
            break;
        }
      }
      break;

      case InstructionCode::ExtCheatRegistersCompare: // 52
      {
        index++;
        bool activate_codes = false;
        const uint8_t cht_reg_no1 = static_cast<uint8_t>(inst.address);
        const uint8_t cht_reg_no2 = static_cast<uint8_t>(inst.address >> 8);
        const uint8_t sub_type = static_cast<uint8_t>(inst.first >> 16);

        switch (sub_type)
        {
          case 0x00:
            activate_codes =
              (static_cast<uint8_t>(cht_register[cht_reg_no2]) == static_cast<uint8_t>(cht_register[cht_reg_no1]));
            break;
          case 0x01:
            activate_codes =
              (static_cast<uint8_t>(cht_register[cht_reg_no2]) != static_cast<uint8_t>(cht_register[cht_reg_no1]));
            break;
          case 0x02:
            activate_codes =
              (static_cast<uint8_t>(cht_register[cht_reg_no2]) > static_cast<uint8_t>(cht_register[cht_reg_no1]));
            break;
          case 0x03:
            activate_codes =
              (static_cast<uint8_t>(cht_register[cht_reg_no2]) >= static_cast<uint8_t>(cht_register[cht_reg_no1]));
            break;
          case 0x04:
            activate_codes =
              (static_cast<uint8_t>(cht_register[cht_reg_no2]) < static_cast<uint8_t>(cht_register[cht_reg_no1]));
            break;
          case 0x05:
            activate_codes =
              (static_cast<uint8_t>(cht_register[cht_reg_no2]) <= static_cast<uint8_t>(cht_register[cht_reg_no1]));
            break;
          case 0x06:
            activate_codes =
              ((static_cast<uint8_t>(cht_register[cht_reg_no2]) & static_cast<uint8_t>(cht_register[cht_reg_no1])) ==
               (static_cast<uint8_t>(cht_register[cht_reg_no1])));
            break;
          case 0x07:
            activate_codes =
              ((static_cast<uint8_t>(cht_register[cht_reg_no2]) & static_cast<uint8_t>(cht_register[cht_reg_no1])) !=
               (static_cast<uint8_t>(cht_register[cht_reg_no1])));
            break;
          case 0x0A:
            activate_codes =
              ((static_cast<uint8_t>(cht_register[cht_reg_no2]) & static_cast<uint8_t>(cht_register[cht_reg_no1])) ==
               (static_cast<uint8_t>(cht_register[cht_reg_no2])));
            break;
          case 0x0B:
            activate_codes =
              ((static_cast<uint8_t>(cht_register[cht_reg_no2]) & static_cast<uint8_t>(cht_register[cht_reg_no1])) !=
               (static_cast<uint8_t>(cht_register[cht_reg_no2])));
            break;
          case 0x10:
            activate_codes = (static_cast<uint8_t>(cht_register[cht_reg_no1]) == inst.value8);
            break;
          case 0x11:
            activate_codes = (static_cast<uint8_t>(cht_register[cht_reg_no1]) != inst.value8);
            break;
          case 0x12:
            activate_codes = (static_cast<uint8_t>(cht_register[cht_reg_no1]) > inst.value8);
            break;
          case 0x13:
            activate_codes = (static_cast<uint8_t>(cht_register[cht_reg_no1]) >= inst.value8);
            break;
          case 0x14:
            activate_codes = (static_cast<uint8_t>(cht_register[cht_reg_no1]) < inst.value8);
            break;
          case 0x15:
            activate_codes = (static_cast<uint8_t>(cht_register[cht_reg_no1]) <= inst.value8);
            break;
          case 0x16:
            activate_codes = ((static_cast<uint8_t>(cht_register[cht_reg_no1]) & inst.value8) == inst.value8);
            break;
          case 0x17:
            activate_codes = ((static_cast<uint8_t>(cht_register[cht_reg_no1]) & inst.value8) != inst.value8);
            break;
          case 0x18:
            activate_codes =
              ((static_cast<uint8_t>(cht_register[cht_reg_no1]) > inst.value8) &&
               (static_cast<uint8_t>(cht_register[cht_reg_no1]) < static_cast<uint8_t>(inst.value32 >> 16)));
            break;
          case 0x19:
            activate_codes =
              ((static_cast<uint8_t>(cht_register[cht_reg_no1]) >= inst.value8) &&
               (static_cast<uint8_t>(cht_register[cht_reg_no1]) <= static_cast<uint8_t>(inst.value32 >> 16)));
            break;
          case 0x1A:
            activate_codes = ((static_cast<uint8_t>(cht_register[cht_reg_no2]) & inst.value8) ==
                              static_cast<uint8_t>(cht_register[cht_reg_no1]));
            break;
          case 0x1B:
            activate_codes = ((static_cast<uint8_t>(cht_register[cht_reg_no1]) & inst.value8) !=
                              static_cast<uint8_t>(cht_register[cht_reg_no1]));
            break;
          case 0x20:
            activate_codes =
              (DoMemoryRead<uint8_t>(cht_register[cht_reg_no2]) == DoMemoryRead<uint8_t>(cht_register[cht_reg_no1]));
            break;
          case 0x21:
            activate_codes =
              (DoMemoryRead<uint8_t>(cht_register[cht_reg_no2]) != DoMemoryRead<uint8_t>(cht_register[cht_reg_no1]));
            break;
          case 0x22:
            activate_codes =
              (DoMemoryRead<uint8_t>(cht_register[cht_reg_no2]) > DoMemoryRead<uint8_t>(cht_register[cht_reg_no1]));
            break;
          case 0x23:
            activate_codes =
              (DoMemoryRead<uint8_t>(cht_register[cht_reg_no2]) >= DoMemoryRead<uint8_t>(cht_register[cht_reg_no1]));
            break;
          case 0x24:
            activate_codes =
              (DoMemoryRead<uint8_t>(cht_register[cht_reg_no2]) < DoMemoryRead<uint8_t>(cht_register[cht_reg_no1]));
            break;
          case 0x25:
            activate_codes =
              (DoMemoryRead<uint8_t>(cht_register[cht_reg_no2]) <= DoMemoryRead<uint8_t>(cht_register[cht_reg_no1]));
            break;
          case 0x26:
            activate_codes = ((DoMemoryRead<uint8_t>(cht_register[cht_reg_no1]) & inst.value8) == inst.value8);
            break;
          case 0x27:
            activate_codes = ((DoMemoryRead<uint8_t>(cht_register[cht_reg_no1]) & inst.value8) != inst.value8);
            break;
          case 0x28:
            activate_codes =
              ((DoMemoryRead<uint8_t>(cht_register[cht_reg_no1]) > inst.value8) &&
               (DoMemoryRead<uint8_t>(cht_register[cht_reg_no1]) < static_cast<uint8_t>(inst.value32 >> 16)));
            break;
          case 0x29:
            activate_codes =
              ((DoMemoryRead<uint8_t>(cht_register[cht_reg_no1]) >= inst.value8) &&
               (DoMemoryRead<uint8_t>(cht_register[cht_reg_no1]) <= static_cast<uint8_t>(inst.value32 >> 16)));
            break;
          case 0x2A:
            activate_codes = ((DoMemoryRead<uint8_t>(cht_register[cht_reg_no1]) & inst.value8) ==
                              DoMemoryRead<uint8_t>(cht_register[cht_reg_no1]));
            break;
          case 0x2B:
            activate_codes = ((DoMemoryRead<uint8_t>(cht_register[cht_reg_no1]) & inst.value8) !=
                              DoMemoryRead<uint8_t>(cht_register[cht_reg_no1]));
            break;
          case 0x30:
            activate_codes = (static_cast<uint8_t>(cht_register[cht_reg_no1]) == DoMemoryRead<uint8_t>(inst.value32));
            break;
          case 0x31:
            activate_codes = (static_cast<uint8_t>(cht_register[cht_reg_no1]) != DoMemoryRead<uint8_t>(inst.value32));
            break;
          case 0x32:
            activate_codes = (static_cast<uint8_t>(cht_register[cht_reg_no1]) > DoMemoryRead<uint8_t>(inst.value32));
            break;
          case 0x33:
            activate_codes = (static_cast<uint8_t>(cht_register[cht_reg_no1]) >= DoMemoryRead<uint8_t>(inst.value32));
            break;
          case 0x34:
            activate_codes = (static_cast<uint8_t>(cht_register[cht_reg_no1]) < DoMemoryRead<uint8_t>(inst.value32));
            break;
          case 0x36:
            activate_codes = ((static_cast<uint8_t>(cht_register[cht_reg_no1]) & DoMemoryRead<uint8_t>(inst.value32)) ==
                              DoMemoryRead<uint8_t>(inst.value32));
            break;
          case 0x37:
            activate_codes = ((static_cast<uint8_t>(cht_register[cht_reg_no1]) & DoMemoryRead<uint8_t>(inst.value32)) !=
                              DoMemoryRead<uint8_t>(inst.value32));
            break;
          case 0x3A:
            activate_codes = ((static_cast<uint8_t>(cht_register[cht_reg_no1]) & DoMemoryRead<uint8_t>(inst.value32)) ==
                              static_cast<uint8_t>(cht_register[cht_reg_no1]));
            break;
          case 0x3B:
            activate_codes = ((static_cast<uint8_t>(cht_register[cht_reg_no1]) & DoMemoryRead<uint8_t>(inst.value32)) !=
                              static_cast<uint8_t>(cht_register[cht_reg_no1]));
            break;
          case 0x40:
            activate_codes =
              (static_cast<uint16_t>(cht_register[cht_reg_no2]) == static_cast<uint16_t>(cht_register[cht_reg_no1]));
            break;
          case 0x41:
            activate_codes =
              (static_cast<uint16_t>(cht_register[cht_reg_no2]) != static_cast<uint16_t>(cht_register[cht_reg_no1]));
            break;
          case 0x42:
            activate_codes =
              (static_cast<uint16_t>(cht_register[cht_reg_no2]) > static_cast<uint16_t>(cht_register[cht_reg_no1]));
            break;
          case 0x43:
            activate_codes =
              (static_cast<uint16_t>(cht_register[cht_reg_no2]) >= static_cast<uint16_t>(cht_register[cht_reg_no1]));
            break;
          case 0x44:
            activate_codes =
              (static_cast<uint16_t>(cht_register[cht_reg_no2]) < static_cast<uint16_t>(cht_register[cht_reg_no1]));
            break;
          case 0x45:
            activate_codes =
              (static_cast<uint16_t>(cht_register[cht_reg_no2]) <= static_cast<uint16_t>(cht_register[cht_reg_no1]));
            break;
          case 0x46:
            activate_codes =
              ((static_cast<uint16_t>(cht_register[cht_reg_no2]) & static_cast<uint16_t>(cht_register[cht_reg_no1])) ==
               static_cast<uint16_t>(cht_register[cht_reg_no1]));
            break;
          case 0x47:
            activate_codes =
              ((static_cast<uint16_t>(cht_register[cht_reg_no2]) & static_cast<uint16_t>(cht_register[cht_reg_no1])) !=
               static_cast<uint16_t>(cht_register[cht_reg_no1]));
            break;
          case 0x4A:
            activate_codes =
              ((static_cast<uint16_t>(cht_register[cht_reg_no2]) & static_cast<uint16_t>(cht_register[cht_reg_no1])) ==
               static_cast<uint16_t>(cht_register[cht_reg_no2]));
            break;
          case 0x4B:
            activate_codes =
              ((static_cast<uint16_t>(cht_register[cht_reg_no2]) & static_cast<uint16_t>(cht_register[cht_reg_no1])) !=
               static_cast<uint16_t>(cht_register[cht_reg_no2]));
            break;
          case 0x50:
            activate_codes = (static_cast<uint16_t>(cht_register[cht_reg_no1]) == inst.value16);
            break;
          case 0x51:
            activate_codes = (static_cast<uint16_t>(cht_register[cht_reg_no1]) != inst.value16);
            break;
          case 0x52:
            activate_codes = (static_cast<uint16_t>(cht_register[cht_reg_no1]) > inst.value16);
            break;
          case 0x53:
            activate_codes = (static_cast<uint16_t>(cht_register[cht_reg_no1]) >= inst.value16);
            break;
          case 0x54:
            activate_codes = (static_cast<uint16_t>(cht_register[cht_reg_no1]) < inst.value16);
            break;
          case 0x55:
            activate_codes = (static_cast<uint16_t>(cht_register[cht_reg_no1]) <= inst.value16);
            break;
          case 0x56:
            activate_codes = ((static_cast<uint16_t>(cht_register[cht_reg_no1]) & inst.value16) == inst.value16);
            break;
          case 0x57:
            activate_codes = ((static_cast<uint16_t>(cht_register[cht_reg_no1]) & inst.value16) != inst.value16);
            break;
          case 0x58:
            activate_codes =
              ((static_cast<uint16_t>(cht_register[cht_reg_no1]) > inst.value16) &&
               (static_cast<uint16_t>(cht_register[cht_reg_no1]) < static_cast<uint16_t>(inst.value32 >> 16)));
            break;
          case 0x59:
            activate_codes =
              ((static_cast<uint16_t>(cht_register[cht_reg_no1]) >= inst.value16) &&
               (static_cast<uint16_t>(cht_register[cht_reg_no1]) <= static_cast<uint16_t>(inst.value32 >> 16)));
            break;
          case 0x5A:
            activate_codes = ((static_cast<uint16_t>(cht_register[cht_reg_no2]) & inst.value16) ==
                              static_cast<uint16_t>(cht_register[cht_reg_no1]));
            break;
          case 0x5B:
            activate_codes = ((static_cast<uint16_t>(cht_register[cht_reg_no1]) & inst.value16) !=
                              static_cast<uint16_t>(cht_register[cht_reg_no1]));
            break;
          case 0x60:
            activate_codes =
              (DoMemoryRead<uint16_t>(cht_register[cht_reg_no2]) == DoMemoryRead<uint16_t>(cht_register[cht_reg_no1]));
            break;
          case 0x61:
            activate_codes =
              (DoMemoryRead<uint16_t>(cht_register[cht_reg_no2]) != DoMemoryRead<uint16_t>(cht_register[cht_reg_no1]));
            break;
          case 0x62:
            activate_codes =
              (DoMemoryRead<uint16_t>(cht_register[cht_reg_no2]) > DoMemoryRead<uint16_t>(cht_register[cht_reg_no1]));
            break;
          case 0x63:
            activate_codes =
              (DoMemoryRead<uint16_t>(cht_register[cht_reg_no2]) >= DoMemoryRead<uint16_t>(cht_register[cht_reg_no1]));
            break;
          case 0x64:
            activate_codes =
              (DoMemoryRead<uint16_t>(cht_register[cht_reg_no2]) < DoMemoryRead<uint16_t>(cht_register[cht_reg_no1]));
            break;
          case 0x65:
            activate_codes =
              (DoMemoryRead<uint16_t>(cht_register[cht_reg_no2]) <= DoMemoryRead<uint16_t>(cht_register[cht_reg_no1]));
            break;
          case 0x66:
            activate_codes = ((DoMemoryRead<uint16_t>(cht_register[cht_reg_no1]) & inst.value16) == inst.value16);
            break;
          case 0x67:
            activate_codes = ((DoMemoryRead<uint16_t>(cht_register[cht_reg_no1]) & inst.value16) != inst.value16);
            break;
          case 0x68:
            activate_codes =
              ((DoMemoryRead<uint16_t>(cht_register[cht_reg_no1]) > inst.value16) &&
               (DoMemoryRead<uint16_t>(cht_register[cht_reg_no1]) < static_cast<uint16_t>(inst.value32 >> 16)));
            break;
          case 0x69:
            activate_codes =
              ((DoMemoryRead<uint16_t>(cht_register[cht_reg_no1]) >= inst.value16) &&
               (DoMemoryRead<uint16_t>(cht_register[cht_reg_no1]) <= static_cast<uint16_t>(inst.value32 >> 16)));
            break;
          case 0x6A:
            activate_codes = ((DoMemoryRead<uint16_t>(cht_register[cht_reg_no1]) & inst.value16) ==
                              DoMemoryRead<uint16_t>(cht_register[cht_reg_no1]));
            break;
          case 0x6B:
            activate_codes = ((DoMemoryRead<uint16_t>(cht_register[cht_reg_no1]) & inst.value16) !=
                              DoMemoryRead<uint16_t>(cht_register[cht_reg_no1]));
            break;
          case 0x70:
            activate_codes = (static_cast<uint16_t>(cht_register[cht_reg_no1]) == DoMemoryRead<uint16_t>(inst.value32));
            break;
          case 0x71:
            activate_codes = (static_cast<uint16_t>(cht_register[cht_reg_no1]) != DoMemoryRead<uint16_t>(inst.value32));
            break;
          case 0x72:
            activate_codes = (static_cast<uint16_t>(cht_register[cht_reg_no1]) > DoMemoryRead<uint16_t>(inst.value32));
            break;
          case 0x73:
            activate_codes = (static_cast<uint16_t>(cht_register[cht_reg_no1]) >= DoMemoryRead<uint16_t>(inst.value32));
            break;
          case 0x74:
            activate_codes = (static_cast<uint16_t>(cht_register[cht_reg_no1]) < DoMemoryRead<uint16_t>(inst.value32));
            break;
          case 0x76:
            activate_codes = ((static_cast<uint16_t>(cht_register[cht_reg_no1]) & DoMemoryRead<uint16_t>(inst.value32)) ==
                              DoMemoryRead<uint16_t>(inst.value32));
            break;
          case 0x77:
            activate_codes = ((static_cast<uint16_t>(cht_register[cht_reg_no1]) & DoMemoryRead<uint16_t>(inst.value32)) !=
                              DoMemoryRead<uint16_t>(inst.value32));
            break;
          case 0x7A:
            activate_codes = ((static_cast<uint16_t>(cht_register[cht_reg_no1]) & DoMemoryRead<uint16_t>(inst.value32)) ==
                              static_cast<uint16_t>(cht_register[cht_reg_no1]));
            break;
          case 0x7B:
            activate_codes = ((static_cast<uint16_t>(cht_register[cht_reg_no1]) & DoMemoryRead<uint16_t>(inst.value32)) !=
                              static_cast<uint16_t>(cht_register[cht_reg_no1]));
            break;
          case 0x80:
            activate_codes = (cht_register[cht_reg_no2] == cht_register[cht_reg_no1]);
            break;
          case 0x81:
            activate_codes = (cht_register[cht_reg_no2] != cht_register[cht_reg_no1]);
            break;
          case 0x82:
            activate_codes = (cht_register[cht_reg_no2] > cht_register[cht_reg_no1]);
            break;
          case 0x83:
            activate_codes = (cht_register[cht_reg_no2] >= cht_register[cht_reg_no1]);
            break;
          case 0x84:
            activate_codes = (cht_register[cht_reg_no2] < cht_register[cht_reg_no1]);
            break;
          case 0x85:
            activate_codes = (cht_register[cht_reg_no2] <= cht_register[cht_reg_no1]);
            break;
          case 0x86:
            activate_codes = ((cht_register[cht_reg_no2] & cht_register[cht_reg_no1]) == cht_register[cht_reg_no1]);
            break;
          case 0x87:
            activate_codes = ((cht_register[cht_reg_no2] & cht_register[cht_reg_no1]) != cht_register[cht_reg_no1]);
            break;
          case 0x8A:
            activate_codes = ((cht_register[cht_reg_no2] & cht_register[cht_reg_no1]) == cht_register[cht_reg_no2]);
            break;
          case 0x8B:
            activate_codes = ((cht_register[cht_reg_no2] & cht_register[cht_reg_no1]) != cht_register[cht_reg_no2]);
            break;
          case 0x90:
            activate_codes = (cht_register[cht_reg_no1] == inst.value32);
            break;
          case 0x91:
            activate_codes = (cht_register[cht_reg_no1] != inst.value32);
            break;
          case 0x92:
            activate_codes = (cht_register[cht_reg_no1] > inst.value32);
            break;
          case 0x93:
            activate_codes = (cht_register[cht_reg_no1] >= inst.value32);
            break;
          case 0x94:
            activate_codes = (cht_register[cht_reg_no1] < inst.value32);
            break;
          case 0x95:
            activate_codes = (cht_register[cht_reg_no1] <= inst.value32);
            break;
          case 0x96:
            activate_codes = ((cht_register[cht_reg_no1] & inst.value32) == inst.value32);
            break;
          case 0x97:
            activate_codes = ((cht_register[cht_reg_no1] & inst.value32) != inst.value32);
            break;
          case 0x9A:
            activate_codes = ((cht_register[cht_reg_no2] & inst.value32) == cht_register[cht_reg_no1]);
            break;
          case 0x9B:
            activate_codes = ((cht_register[cht_reg_no1] & inst.value32) != cht_register[cht_reg_no1]);
            break;
          case 0xA0:
            activate_codes =
              (DoMemoryRead<uint32_t>(cht_register[cht_reg_no2]) == DoMemoryRead<uint32_t>(cht_register[cht_reg_no1]));
            break;
          case 0xA1:
            activate_codes =
              (DoMemoryRead<uint32_t>(cht_register[cht_reg_no2]) != DoMemoryRead<uint32_t>(cht_register[cht_reg_no1]));
            break;
          case 0xA2:
            activate_codes =
              (DoMemoryRead<uint32_t>(cht_register[cht_reg_no2]) > DoMemoryRead<uint32_t>(cht_register[cht_reg_no1]));
            break;
          case 0xA3:
            activate_codes =
              (DoMemoryRead<uint32_t>(cht_register[cht_reg_no2]) >= DoMemoryRead<uint32_t>(cht_register[cht_reg_no1]));
            break;
          case 0xA4:
            activate_codes =
              (DoMemoryRead<uint32_t>(cht_register[cht_reg_no2]) < DoMemoryRead<uint32_t>(cht_register[cht_reg_no1]));
            break;
          case 0xA5:
            activate_codes =
              (DoMemoryRead<uint32_t>(cht_register[cht_reg_no2]) <= DoMemoryRead<uint32_t>(cht_register[cht_reg_no1]));
            break;
          case 0xA6:
            activate_codes = ((DoMemoryRead<uint32_t>(cht_register[cht_reg_no1]) & inst.value32) == inst.value32);
            break;
          case 0xA7:
            activate_codes = ((DoMemoryRead<uint32_t>(cht_register[cht_reg_no1]) & inst.value32) != inst.value32);
            break;
          case 0xAA:
            activate_codes = ((DoMemoryRead<uint32_t>(cht_register[cht_reg_no1]) & inst.value32) ==
                              DoMemoryRead<uint32_t>(cht_register[cht_reg_no1]));
            break;
          case 0xAB:
            activate_codes = ((DoMemoryRead<uint32_t>(cht_register[cht_reg_no1]) & inst.value32) !=
                              DoMemoryRead<uint32_t>(cht_register[cht_reg_no1]));
            break;
          case 0xB0:
            activate_codes = (cht_register[cht_reg_no1] == DoMemoryRead<uint32_t>(inst.value32));
            break;
          case 0xB1:
            activate_codes = (cht_register[cht_reg_no1] != DoMemoryRead<uint32_t>(inst.value32));
            break;
          case 0xB2:
            activate_codes = (cht_register[cht_reg_no1] > DoMemoryRead<uint32_t>(inst.value32));
            break;
          case 0xB3:
            activate_codes = (cht_register[cht_reg_no1] >= DoMemoryRead<uint32_t>(inst.value32));
            break;
          case 0xB4:
            activate_codes = (cht_register[cht_reg_no1] < DoMemoryRead<uint32_t>(inst.value32));
            break;
          case 0xB6:
            activate_codes =
              ((cht_register[cht_reg_no1] & DoMemoryRead<uint32_t>(inst.value32)) == DoMemoryRead<uint32_t>(inst.value32));
            break;
          case 0xB7:
            activate_codes =
              ((cht_register[cht_reg_no1] & DoMemoryRead<uint32_t>(inst.value32)) != DoMemoryRead<uint32_t>(inst.value32));
            break;
          case 0xBA:
            activate_codes =
              ((cht_register[cht_reg_no1] & DoMemoryRead<uint32_t>(inst.value32)) == cht_register[cht_reg_no1]);
            break;
          case 0xBB:
            activate_codes =
              ((cht_register[cht_reg_no1] & DoMemoryRead<uint32_t>(inst.value32)) != cht_register[cht_reg_no1]);
            break;
          default:
            activate_codes = false;
            break;
        }
        if (activate_codes)
        {
          // execute following instructions
          continue;
        }

        // skip to the next separator (00000000 FFFF), or end
        constexpr uint64_t separator_value = UINT64_C(0x000000000000FFFF);
        while (index < count)
        {
          // we don't want to execute the separator instruction
          const uint64_t bits = instructions[index++].bits;
          if (bits == separator_value)
            break;
        }
      }
      break;

      case InstructionCode::DelayActivation: // C1
      {
        // A value of around 4000 or 5000 will usually give you a good 20-30 second delay before codes are activated.
        // Frame number * 0.3 -> (20 * 60) * 10 / 3 => 4000
        const uint32_t comp_value = (System::GetFrameNumber() * 10) / 3;
        if (comp_value < inst.value16)
          index = count;
        else
          index++;
      }
      break;

      case InstructionCode::Slide:
      {
        if ((index + 1) >= instructions.size())
        {
          Log_ErrorPrintf("Incomplete slide instruction");
          return;
        }

        const uint32_t slide_count = (inst.first >> 8) & 0xFFu;
        const uint32_t address_increment = inst.first & 0xFFu;
        const uint16_t value_increment = static_cast<uint16_t>(inst.second);
        const Instruction& inst2 = instructions[index + 1];
        const InstructionCode write_type = inst2.code;
        uint32_t address = inst2.address;
        uint16_t value = inst2.value16;

        if (write_type == InstructionCode::ConstantWrite8)
        {
          for (uint32_t i = 0; i < slide_count; i++)
          {
            DoMemoryWrite<uint8_t>(address, static_cast<uint8_t>(value));
            address += address_increment;
            value += value_increment;
          }
        }
        else if (write_type == InstructionCode::ConstantWrite16)
        {
          for (uint32_t i = 0; i < slide_count; i++)
          {
            DoMemoryWrite<uint16_t>(address, value);
            address += address_increment;
            value += value_increment;
          }
        }
        else
        {
          Log_ErrorPrintf("Invalid command in second slide parameter 0x%02X", static_cast<unsigned>(write_type));
        }

        index += 2;
      }
      break;

      case InstructionCode::ExtImprovedSlide:
      {
        if ((index + 1) >= instructions.size())
        {
          Log_ErrorPrintf("Incomplete slide instruction");
          return;
        }

        const uint32_t slide_count = inst.first & 0xFFFFu;
        const uint32_t address_change = (inst.second >> 16) & 0xFFFFu;
        const uint16_t value_change = static_cast<uint16_t>(inst.second);
        const Instruction& inst2 = instructions[index + 1];
        const InstructionCode write_type = inst2.code;
        const bool address_change_negative = (inst.first >> 20) & 0x1u;
        const bool value_change_negative = (inst.first >> 16) & 0x1u;
        uint32_t address = inst2.address;
        uint32_t value = inst2.value32;

        if (write_type == InstructionCode::ConstantWrite8)
        {
          for (uint32_t i = 0; i < slide_count; i++)
          {
            DoMemoryWrite<uint8_t>(address, static_cast<uint8_t>(value));
            if (address_change_negative)
              address -= address_change;
            else
              address += address_change;
            if (value_change_negative)
              value -= value_change;
            else
              value += value_change;
          }
        }
        else if (write_type == InstructionCode::ConstantWrite16)
        {
          for (uint32_t i = 0; i < slide_count; i++)
          {
            DoMemoryWrite<uint16_t>(address, static_cast<uint16_t>(value));
            if (address_change_negative)
              address -= address_change;
            else
              address += address_change;
            if (value_change_negative)
              value -= value_change;
            else
              value += value_change;
          }
        }
        else if (write_type == InstructionCode::ExtConstantWrite32)
        {
          for (uint32_t i = 0; i < slide_count; i++)
          {
            DoMemoryWrite<uint32_t>(address, value);
            if (address_change_negative)
              address -= address_change;
            else
              address += address_change;
            if (value_change_negative)
              value -= value_change;
            else
              value += value_change;
          }
        }
        else
        {
          Log_ErrorPrintf("Invalid command in second slide parameter 0x%02X", static_cast<unsigned>(write_type));
        }

        index += 2;
      }
      break;

      case InstructionCode::MemoryCopy:
      {
        if ((index + 1) >= instructions.size())
        {
          Log_ErrorPrintf("Incomplete memory copy instruction");
          return;
        }

        const Instruction& inst2 = instructions[index + 1];
        const uint32_t byte_count = inst.value16;
        uint32_t src_address = inst.address;
        uint32_t dst_address = inst2.address;

        for (uint32_t i = 0; i < byte_count; i++)
        {
          uint8_t value = DoMemoryRead<uint8_t>(src_address);
          DoMemoryWrite<uint8_t>(dst_address, value);
          src_address++;
          dst_address++;
        }

        index += 2;
      }
      break;

      default:
      {
        Log_ErrorPrintf("Unhandled instruction code 0x%02X (%08X %08X)", static_cast<uint8_t>(inst.code.GetValue()),
                        inst.first, inst.second);
        index++;
      }
      break;
    }
  }
}

