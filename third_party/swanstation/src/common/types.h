#pragma once
#include <cstddef>
#include <cstdint>
#include <type_traits>

// Force inline helper
#ifndef ALWAYS_INLINE
#if defined(_MSC_VER)
#define ALWAYS_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define ALWAYS_INLINE __attribute__((always_inline)) inline
#else
#define ALWAYS_INLINE inline
#endif
#endif

// Force inline in non-debug helper
#ifdef _DEBUG
#define ALWAYS_INLINE_RELEASE inline
#else
#define ALWAYS_INLINE_RELEASE ALWAYS_INLINE
#endif

// countof macro
#ifndef countof
#ifdef _countof
#define countof _countof
#else
template<typename T, size_t N>
char (&__countof_ArraySizeHelper(T (&array)[N]))[N];
#define countof(array) (sizeof(__countof_ArraySizeHelper(array)))
#endif
#endif

#ifdef __GNUC__
#ifndef _WIN32
#define printflike(n,m) __attribute__((format(printf,n,m)))
#else
#define printflike(n,m)
#endif
#else
#define printflike(n,m)
#endif

// disable warnings that show up at warning level 4
// TODO: Move to build system instead
#ifdef _MSC_VER
#pragma warning(disable : 4201) // warning C4201: nonstandard extension used : nameless struct/union
#pragma warning(disable : 4100) // warning C4100: 'Platform' : unreferenced formal parameter
#pragma warning(disable : 4355) // warning C4355: 'this' : used in base member initializer list
#endif

// BCD helpers
ALWAYS_INLINE constexpr uint8_t BinaryToBCD(uint8_t value)
{
  return ((value / 10) << 4) + (value % 10);
}
ALWAYS_INLINE constexpr uint8_t PackedBCDToBinary(uint8_t value)
{
  return ((value >> 4) * 10) + (value % 16);
}
ALWAYS_INLINE constexpr bool IsValidBCDDigit(uint8_t digit)
{
  return (digit <= 9);
}
ALWAYS_INLINE constexpr bool IsValidPackedBCD(uint8_t value)
{
  return IsValidBCDDigit(value & 0x0F) && IsValidBCDDigit(value >> 4);
}

// Enum class bitwise operators
#define IMPLEMENT_ENUM_CLASS_BITWISE_OPERATORS(type_)                                                                  \
  ALWAYS_INLINE constexpr type_ operator&(type_ lhs, type_ rhs)                                                        \
  {                                                                                                                    \
    return static_cast<type_>(static_cast<std::underlying_type<type_>::type>(lhs) &                                    \
                              static_cast<std::underlying_type<type_>::type>(rhs));                                    \
  }                                                                                                                    \
  ALWAYS_INLINE constexpr type_ operator|(type_ lhs, type_ rhs)                                                        \
  {                                                                                                                    \
    return static_cast<type_>(static_cast<std::underlying_type<type_>::type>(lhs) |                                    \
                              static_cast<std::underlying_type<type_>::type>(rhs));                                    \
  }                                                                                                                    \
  ALWAYS_INLINE constexpr type_ operator~(type_ val)                                                                   \
  {                                                                                                                    \
    return static_cast<type_>(~static_cast<std::underlying_type<type_>::type>(val));                                   \
  }                                                                                                                    \
  ALWAYS_INLINE constexpr type_& operator&=(type_& lhs, type_ rhs)                                                     \
  {                                                                                                                    \
    lhs = static_cast<type_>(static_cast<std::underlying_type<type_>::type>(lhs) &                                     \
                             static_cast<std::underlying_type<type_>::type>(rhs));                                     \
    return lhs;                                                                                                        \
  }                                                                                                                    \
  ALWAYS_INLINE constexpr type_& operator|=(type_& lhs, type_ rhs)                                                     \
  {                                                                                                                    \
    lhs = static_cast<type_>(static_cast<std::underlying_type<type_>::type>(lhs) |                                     \
                             static_cast<std::underlying_type<type_>::type>(rhs));                                     \
    return lhs;                                                                                                        \
  }

// Host endianness selection.
//
// swanstation currently only ships to little-endian targets (x86, x64,
// AArch32 and AArch64 in their normal LE configurations). To keep the
// hot paths honest we don't autodetect the host byte order, and we
// don't define a positive marker for the LE case either: little-endian
// is simply the unmarked default.
//
// Big-endian is opt-in. Pass -DMSB_FIRST=1 to the compiler (via the
// build system or by hand) to flag a big-endian build. Code that
// needs byte-order-dependent work should branch on `#ifdef MSB_FIRST`
// only - the LE path stays the unconditional default and pays no
// cost on the targets we actually support. There is no LSB_FIRST
// macro and there should never be one.
