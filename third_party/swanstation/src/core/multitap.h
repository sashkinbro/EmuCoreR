#pragma once
#include "common/state_wrapper.h"
#include "common/types.h"
#include "controller.h"
#include "memory_card.h"
#include <array>

class Multitap final
{
public:
  Multitap();

  void Reset();

  void SetEnable(bool enable, uint32_t base_index);
  ALWAYS_INLINE bool IsEnabled() const { return m_enabled; };

  bool DoState(StateWrapper& sw);

  void ResetTransferState();
  bool Transfer(const uint8_t data_in, uint8_t* data_out);
  ALWAYS_INLINE bool IsReadingMemoryCard() { return IsEnabled() && m_transfer_state == TransferState::MemoryCard; };

private:
  ALWAYS_INLINE static constexpr uint8_t GetMultitapIDByte() { return 0x80; };
  ALWAYS_INLINE static constexpr uint8_t GetStatusByte() { return 0x5A; };

  bool TransferController(uint32_t slot, const uint8_t data_in, uint8_t* data_out) const;
  bool TransferMemoryCard(uint32_t slot, const uint8_t data_in, uint8_t* data_out) const;

  enum class TransferState : uint8_t
  {
    Idle,
    MemoryCard,
    ControllerCommand,
    SingleController,
    AllControllers
  };

  TransferState m_transfer_state = TransferState::Idle;
  uint8_t m_selected_slot = 0;

  uint32_t m_controller_transfer_step = 0;

  bool m_invalid_transfer_all_command = false;
  bool m_transfer_all_controllers = false;
  bool m_current_controller_done = false;

  std::array<uint8_t, 32> m_transfer_buffer{};

  uint32_t m_base_index;
  bool m_enabled = false;
};
