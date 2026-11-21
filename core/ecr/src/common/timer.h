#pragma once
#include <cstdint>

namespace Common {

// Minimal monotonic timer used only for host-side throttling/sleep decisions
// (never for emulation state - that would break determinism). Time is kept in
// integer Value units; thresholds are produced with ConvertSecondsToValue and
// compared directly, so no time is ever stored in floating point.
class Timer
{
public:
  using Value = std::uint64_t;

  static Value GetValue(void);
  static Value ConvertSecondsToValue(double s);
};

} // namespace Common
