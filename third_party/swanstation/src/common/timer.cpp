#include "timer.h"
#ifdef _WIN32
#include "windows_headers.h"
#else
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#endif

namespace Common {

#ifdef _WIN32

static double s_counter_frequency;
static bool s_counter_initialized = false;

// counter frequency does not change after system startup, so we can cache it
static double GetCounterFrequency()
{
  // even if this races, it should still result in the same value..
  if (!s_counter_initialized)
  {
    LARGE_INTEGER Freq;
    QueryPerformanceFrequency(&Freq);
    s_counter_frequency = static_cast<double>(Freq.QuadPart) / 1000000000.0;
    s_counter_initialized = true;
  }
  return s_counter_frequency;
}

Timer::Value Timer::GetValue(void)
{
  GetCounterFrequency();

  Timer::Value ReturnValue;
  QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&ReturnValue));
  return ReturnValue;
}

Timer::Value Timer::ConvertSecondsToValue(double s)
{
  return static_cast<Value>((s * 1000000000.0) * GetCounterFrequency());
}

#else

Timer::Value Timer::GetValue(void)
{
  struct timespec tv;
  clock_gettime(CLOCK_MONOTONIC, &tv);
  return ((Value)tv.tv_nsec + (Value)tv.tv_sec * 1000000000);
}

Timer::Value Timer::ConvertSecondsToValue(double s)
{
  return static_cast<Value>(s * 1000000000.0);
}

#endif

} // namespace Common
