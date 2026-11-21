#pragma once
#include "types.h"
#include <type_traits>

// Disable MSVC warnings that we actually handle
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4800) // warning C4800: 'int': forcing value to bool 'true' or 'false' (performance warning)
#endif

template<typename BackingDataType, typename DataType, unsigned BitIndex, unsigned BitCount>
struct BitField
{
  ALWAYS_INLINE constexpr BackingDataType GetMask() const
  {
    return ((static_cast<BackingDataType>(~0)) >> (8 * sizeof(BackingDataType) - BitCount)) << BitIndex;
  }

  ALWAYS_INLINE constexpr operator DataType() const { return GetValue(); }

  ALWAYS_INLINE constexpr BitField& operator=(DataType value)
  {
    SetValue(value);
    return *this;
  }

  ALWAYS_INLINE constexpr BitField& operator+=(DataType rhs)
  {
    SetValue(GetValue() + rhs);
    return *this;
  }

  ALWAYS_INLINE constexpr BitField& operator-=(DataType rhs)
  {
    SetValue(GetValue() - rhs);
    return *this;
  }

  ALWAYS_INLINE constexpr BitField& operator/=(DataType rhs)
  {
    SetValue(GetValue() / rhs);
    return *this;
  }

  ALWAYS_INLINE constexpr BitField& operator&=(DataType rhs)
  {
    SetValue(GetValue() & rhs);
    return *this;
  }

  ALWAYS_INLINE constexpr BitField& operator|=(DataType rhs)
  {
    SetValue(GetValue() | rhs);
    return *this;
  }

  ALWAYS_INLINE constexpr BitField& operator^=(DataType rhs)
  {
    SetValue(GetValue() ^ rhs);
    return *this;
  }

  ALWAYS_INLINE constexpr BitField& operator<<=(DataType rhs)
  {
    SetValue(GetValue() << rhs);
    return *this;
  }

  ALWAYS_INLINE constexpr DataType GetValue() const
  {
    if constexpr (std::is_same_v<DataType, bool>)
    {
      return static_cast<DataType>(!!((data & GetMask()) >> BitIndex));
    }
    else if constexpr (std::is_signed_v<DataType>)
    {
      // Sign-extend the BitCount-wide field to a full DataType. Shift left
      // to put bit (BitCount-1) at the sign-bit position of DataType, then
      // arithmetic-shift right to replicate the sign bit. The left shift
      // is performed in the unsigned domain to avoid C++<20 UB on signed
      // left shifts of values whose product would not fit in DataType
      // (well-defined since C++20).
      using UnsignedT = std::make_unsigned_t<DataType>;
      constexpr int shift = 8 * sizeof(DataType) - BitCount;
      return static_cast<DataType>(static_cast<UnsignedT>(data >> BitIndex) << shift) >> shift;
    }
    else
    {
      return static_cast<DataType>((data & GetMask()) >> BitIndex);
    }
  }

  ALWAYS_INLINE constexpr void SetValue(DataType value)
  {
    data = (data & ~GetMask()) | ((static_cast<BackingDataType>(value) << BitIndex) & GetMask());
  }

  BackingDataType data;
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif
