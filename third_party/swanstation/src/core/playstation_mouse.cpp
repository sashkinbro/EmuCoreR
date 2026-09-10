#include "playstation_mouse.h"
#include "common/state_wrapper.h"
#include "gpu.h"
#include "host_display.h"
#include "host_interface.h"
#include "system.h"
#include <array>
#include <limits>

PlayStationMouse::PlayStationMouse()
{
  m_last_host_position_x = g_host_interface->GetDisplay()->GetMousePositionX();
  m_last_host_position_y = g_host_interface->GetDisplay()->GetMousePositionY();
}

PlayStationMouse::~PlayStationMouse() = default;

ControllerType PlayStationMouse::GetType() const
{
  return ControllerType::PlayStationMouse;
}

void PlayStationMouse::Reset()
{
  m_transfer_state = TransferState::Idle;
}

bool PlayStationMouse::DoState(StateWrapper& sw, bool apply_input_state)
{
  if (!Controller::DoState(sw, apply_input_state))
    return false;

  uint16_t button_state = m_button_state;
  uint8_t delta_x = m_delta_x;
  uint8_t delta_y = m_delta_y;
  sw.Do(&button_state);
  sw.Do(&delta_x);
  sw.Do(&delta_y);
  if (apply_input_state)
  {
    m_button_state = button_state;
    m_delta_x = delta_x;
    m_delta_y = delta_y;
  }

  sw.Do(&m_transfer_state);
  return true;
}

void PlayStationMouse::SetButtonState(Button button, bool pressed)
{
  static constexpr std::array<uint8_t, static_cast<size_t>(Button::Count)> indices = {{11, 10}};
  const uint16_t bit = uint16_t(1) << indices[static_cast<uint8_t>(button)];
  const uint16_t new_state = pressed ? (m_button_state & ~bit) : (m_button_state | bit);
  if (new_state != m_button_state)
  {
    // The runahead simulation needs to re-run any frame where input changed
    // between the original poll and now; signalling here lets it detect that.
    System::SetRunaheadReplayFlag();
    m_button_state = new_state;
  }
}

void PlayStationMouse::SetButtonState(int32_t button_code, bool pressed)
{
  if (button_code < 0 || button_code >= static_cast<int32_t>(Button::Count))
    return;

  SetButtonState(static_cast<Button>(button_code), pressed);
}

void PlayStationMouse::ResetTransferState()
{
  m_transfer_state = TransferState::Idle;
}

bool PlayStationMouse::Transfer(const uint8_t data_in, uint8_t* data_out)
{
  static constexpr uint16_t ID = 0x5A12;

  switch (m_transfer_state)
  {
    case TransferState::Idle:
    {
      *data_out = 0xFF;

      if (data_in == 0x01)
      {
        m_transfer_state = TransferState::Ready;
        return true;
      }
      break;
    }

    case TransferState::Ready:
    {
      if (data_in == 0x42)
      {
        *data_out = static_cast<uint8_t>(ID);
        m_transfer_state = TransferState::IDMSB;
        return true;
      }

      *data_out = 0xFF;
      break;
    }

    case TransferState::IDMSB:
    {
      *data_out = static_cast<uint8_t>(ID >> 8);
      m_transfer_state = TransferState::ButtonsLSB;
      return true;
    }

    case TransferState::ButtonsLSB:
    {
      *data_out = static_cast<uint8_t>(m_button_state);
      m_transfer_state = TransferState::ButtonsMSB;
      return true;
    }

    case TransferState::ButtonsMSB:
    {
      *data_out = static_cast<uint8_t>(m_button_state >> 8);
      m_transfer_state = TransferState::DeltaX;
      return true;
    }

    case TransferState::DeltaX:
    {
      UpdatePosition();
      *data_out = static_cast<uint8_t>(m_delta_x);
      m_transfer_state = TransferState::DeltaY;
      return true;
    }

    case TransferState::DeltaY:
    {
      *data_out = static_cast<uint8_t>(m_delta_y);
      m_transfer_state = TransferState::Idle;
      break;
    }

    default:
      break;
  }

  return false;
}

void PlayStationMouse::UpdatePosition()
{
  // get screen coordinates
  const HostDisplay* display = g_host_interface->GetDisplay();
  const int32_t mouse_x = display->GetMousePositionX();
  const int32_t mouse_y = display->GetMousePositionY();
  const int32_t delta_x = mouse_x - m_last_host_position_x;
  const int32_t delta_y = mouse_y - m_last_host_position_y;
  m_last_host_position_x = mouse_x;
  m_last_host_position_y = mouse_y;

  m_delta_x = static_cast<int8_t>(std::clamp<int32_t>(delta_x, std::numeric_limits<int8_t>::min(), std::numeric_limits<int8_t>::max()));
  m_delta_y = static_cast<int8_t>(std::clamp<int32_t>(delta_y, std::numeric_limits<int8_t>::min(), std::numeric_limits<int8_t>::max()));
}

std::unique_ptr<PlayStationMouse> PlayStationMouse::Create()
{
  return std::make_unique<PlayStationMouse>();
}

