#pragma once
#include "types.h"
#include <type_traits>

#ifdef _WIN32
#include <intrin.h>
#endif

/// Returns the number of zero bits before the first set bit, going MSB->LSB.
template<typename T>
ALWAYS_INLINE unsigned CountLeadingZeros(T value)
{
#ifdef _WIN32
  unsigned long index;
  if constexpr (sizeof(value) >= sizeof(uint64_t))
    _BitScanReverse64(&index, static_cast<uint64_t>(static_cast<typename std::make_unsigned<T>::type>(value)));
  else
    _BitScanReverse(&index, static_cast<uint32_t>(static_cast<typename std::make_unsigned<T>::type>(value)));
  return static_cast<unsigned>(index) ^ static_cast<unsigned>((sizeof(value) * 8u) - 1u);
#else
  if constexpr (sizeof(value) >= sizeof(uint64_t))
    return static_cast<unsigned>(__builtin_clzl(static_cast<uint64_t>(static_cast<typename std::make_unsigned<T>::type>(value))));
  else if constexpr (sizeof(value) == sizeof(uint32_t))
    return static_cast<unsigned>(__builtin_clz(static_cast<uint32_t>(static_cast<typename std::make_unsigned<T>::type>(value))));
  return static_cast<unsigned>(__builtin_clz(static_cast<uint32_t>(static_cast<typename std::make_unsigned<T>::type>(value)))) &
         static_cast<unsigned>((sizeof(value) * 8u) - 1u);
#endif
}
