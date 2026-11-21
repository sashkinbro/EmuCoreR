#pragma once
#include "common/bitfield.h"
#include "common/fifo_queue.h"
#include "multitap.h"
#include "types.h"
#include <array>
#include <memory>

class StateWrapper;

class TimingEvent;
class Controller;
class MemoryCard;

class Pad final
{
public:
  Pad();
  ~Pad();

  void Initialize();
  void Shutdown();
  void Reset();
  bool DoState(StateWrapper& sw);

  Controller* GetController(uint32_t slot) const { return m_controllers[slot].get(); }
  void SetController(uint32_t slot, std::unique_ptr<Controller> dev);

  MemoryCard* GetMemoryCard(uint32_t slot) { return m_memory_cards[slot].get(); }
  void SetMemoryCard(uint32_t slot, std::unique_ptr<MemoryCard> dev);

  Multitap* GetMultitap(uint32_t slot) { return &m_multitaps[slot]; };

  uint32_t ReadRegister(uint32_t offset);
  void WriteRegister(uint32_t offset, uint32_t value);

  ALWAYS_INLINE bool IsTransmitting() const { return m_state != State::Idle; }

private:
  enum class State : uint32_t
  {
    Idle,
    Transmitting,
    WaitingForACK
  };

  enum class ActiveDevice : uint8_t
  {
    None,
    Controller,
    MemoryCard,
    Multitap
  };

  union JOY_CTRL
  {
    uint16_t bits;

    BitField<uint16_t, bool, 0, 1> TXEN;
    BitField<uint16_t, bool, 1, 1> SELECT;
    BitField<uint16_t, bool, 2, 1> RXEN;
    BitField<uint16_t, bool, 4, 1> ACK;
    BitField<uint16_t, bool, 6, 1> RESET;
    BitField<uint16_t, uint8_t, 8, 2> RXIMODE;
    BitField<uint16_t, bool, 10, 1> TXINTEN;
    BitField<uint16_t, bool, 11, 1> RXINTEN;
    BitField<uint16_t, bool, 12, 1> ACKINTEN;
    BitField<uint16_t, uint8_t, 13, 1> SLOT;
  };

  union JOY_STAT
  {
    uint32_t bits;

    BitField<uint32_t, bool, 0, 1> TXRDY;
    BitField<uint32_t, bool, 1, 1> RXFIFONEMPTY;
    BitField<uint32_t, bool, 2, 1> TXDONE;
    BitField<uint32_t, bool, 7, 1> ACKINPUT;
    BitField<uint32_t, bool, 9, 1> INTR;
    BitField<uint32_t, uint32_t, 11, 21> TMR;
  };

  union JOY_MODE
  {
    uint16_t bits;

    BitField<uint16_t, uint8_t, 0, 2> reload_factor;
    BitField<uint16_t, uint8_t, 2, 2> character_length;
    BitField<uint16_t, bool, 4, 1> parity_enable;
    BitField<uint16_t, uint8_t, 5, 1> parity_type;
    BitField<uint16_t, uint8_t, 8, 1> clk_polarity;
  };

  ALWAYS_INLINE bool CanTransfer() const { return m_transmit_buffer_full && m_JOY_CTRL.SELECT && m_JOY_CTRL.TXEN; }

  ALWAYS_INLINE TickCount GetTransferTicks() const { return static_cast<TickCount>(m_JOY_BAUD * 8); }

  // From @JaCzekanski
  // ACK lasts ~96 ticks or approximately 2.84us at master clock (not implemented).
  // ACK delay is between 6.8us-13.7us, or ~338 ticks at master clock for approximately 9.98us.
  // Memory card responds faster, approximately 5us or ~170 ticks.
  static constexpr TickCount GetACKTicks(bool memory_card) { return memory_card ? 170 : 450; }

  void SoftReset();
  void UpdateJoyStat();
  void TransferEvent(TickCount ticks_late);
  void BeginTransfer();
  void DoTransfer(TickCount ticks_late);
  void DoACK();
  void EndTransfer();
  void ResetDeviceTransferState();

  bool DoStateController(StateWrapper& sw, uint32_t i);
  bool DoStateMemcard(StateWrapper& sw, uint32_t i);

  std::array<std::unique_ptr<Controller>, NUM_CONTROLLER_AND_CARD_PORTS> m_controllers;
  std::array<std::unique_ptr<MemoryCard>, NUM_CONTROLLER_AND_CARD_PORTS> m_memory_cards;

  std::array<Multitap, NUM_MULTITAPS> m_multitaps;

  std::unique_ptr<TimingEvent> m_transfer_event;
  State m_state = State::Idle;

  JOY_CTRL m_JOY_CTRL = {};
  JOY_STAT m_JOY_STAT = {};
  JOY_MODE m_JOY_MODE = {};
  uint16_t m_JOY_BAUD = 0;

  ActiveDevice m_active_device = ActiveDevice::None;
  uint8_t m_receive_buffer = 0;
  uint8_t m_transmit_buffer = 0;
  uint8_t m_transmit_value = 0;
  bool m_receive_buffer_full = false;
  bool m_transmit_buffer_full = false;
};

extern Pad g_pad;
