#pragma once
#include "common/bitfield.h"
#include "types.h"
#include <array>
#include <memory>

class StateWrapper;

class TimingEvent;
class GPU;

class Timers final
{
public:
  Timers();
  ~Timers();

  void Initialize();
  void Shutdown();
  void Reset();
  bool DoState(StateWrapper& sw);

  void SetGate(uint32_t timer, bool state);

  void CPUClocksChanged();

  // dot clock/hblank/sysclk div 8
  ALWAYS_INLINE bool IsUsingExternalClock(uint32_t timer) const { return m_states[timer].external_counting_enabled; }
  ALWAYS_INLINE bool IsSyncEnabled(uint32_t timer) const { return m_states[timer].mode.sync_enable; }

  // queries for GPU
  ALWAYS_INLINE bool IsExternalIRQEnabled(uint32_t timer) const
  {
    const CounterState& cs = m_states[timer];
    return (cs.external_counting_enabled && (cs.mode.bits & ((1u << 4) | (1u << 5))) != 0);
  }

  TickCount GetTicksUntilIRQ(uint32_t timer) const;

  void AddTicks(uint32_t timer, TickCount ticks);

  uint32_t ReadRegister(uint32_t offset);
  void WriteRegister(uint32_t offset, uint32_t value);

private:
  static constexpr uint32_t NUM_TIMERS = 3;

  enum class SyncMode : uint8_t
  {
    PauseOnGate = 0,
    ResetOnGate = 1,
    ResetAndRunOnGate = 2,
    FreeRunOnGate = 3
  };

  union CounterMode
  {
    uint32_t bits;

    BitField<uint32_t, bool, 0, 1> sync_enable;
    BitField<uint32_t, SyncMode, 1, 2> sync_mode;
    BitField<uint32_t, bool, 3, 1> reset_at_target;
    BitField<uint32_t, bool, 4, 1> irq_at_target;
    BitField<uint32_t, bool, 5, 1> irq_on_overflow;
    BitField<uint32_t, bool, 6, 1> irq_repeat;
    BitField<uint32_t, bool, 7, 1> irq_pulse_n;
    BitField<uint32_t, uint8_t, 8, 2> clock_source;
    BitField<uint32_t, bool, 10, 1> interrupt_request_n;
    BitField<uint32_t, bool, 11, 1> reached_target;
    BitField<uint32_t, bool, 12, 1> reached_overflow;
  };

  struct CounterState
  {
    CounterMode mode;
    uint32_t counter;
    uint32_t target;
    bool gate;
    bool use_external_clock;
    bool external_counting_enabled;
    bool counting_enabled;
    bool irq_done;
  };

  void UpdateCountingEnabled(CounterState& cs);
  void CheckForIRQ(uint32_t index, uint32_t old_counter);
  void UpdateIRQ(uint32_t index);

  void AddSysClkTicks(TickCount sysclk_ticks);

  TickCount GetTicksUntilNextInterrupt() const;
  void UpdateSysClkEvent();

  std::unique_ptr<TimingEvent> m_sysclk_event;

  std::array<CounterState, NUM_TIMERS> m_states{};
  TickCount m_syclk_ticks_carry = 0; // 0 unless overclocking is enabled
  uint32_t m_sysclk_div_8_carry = 0;      // partial ticks for timer 3 with sysclk/8
};

extern Timers g_timers;
