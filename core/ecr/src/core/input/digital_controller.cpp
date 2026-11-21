#include "digital_controller.h"
#include "common/state_wrapper.h"
#include "host_interface.h"
#include "system.h"

DigitalController::DigitalController() = default;

DigitalController::~DigitalController() = default;

ControllerType DigitalController::GetType() const
{
  return ControllerType::DigitalController;
}

void DigitalController::Reset()
{
  m_transfer_state = TransferState::Idle;
}

bool DigitalController::DoState(StateWrapper& sw, bool apply_input_state)
{
  if (!Controller::DoState(sw, apply_input_state))
    return false;

  uint16_t button_state = m_button_state;
  sw.Do(&button_state);
  if (apply_input_state)
    m_button_state = button_state;

  sw.Do(&m_transfer_state);
  return true;
}

void DigitalController::SetButtonState(Button button, bool pressed)
{
  const uint16_t bit = uint16_t(1) << static_cast<uint8_t>(button);
  if (pressed)
  {
    if (m_button_state & bit)
      System::SetRunaheadReplayFlag();

    m_button_state &= ~bit;
  }
  else
  {
    if (!(m_button_state & bit))
      System::SetRunaheadReplayFlag();

    m_button_state |= bit;
  }
}

void DigitalController::SetButtonState(int32_t button_code, bool pressed)
{
  if (button_code < 0 || button_code >= static_cast<int32_t>(Button::Count))
    return;

  SetButtonState(static_cast<Button>(button_code), pressed);
}

uint32_t DigitalController::GetButtonStateBits() const
{
  return m_button_state ^ 0xFFFF;
}

void DigitalController::ResetTransferState()
{
  m_transfer_state = TransferState::Idle;
}

bool DigitalController::Transfer(const uint8_t data_in, uint8_t* data_out)
{
  static constexpr uint16_t ID = 0x5A41;

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
      *data_out = static_cast<uint8_t>(m_button_state >> 8);
      m_transfer_state = TransferState::Idle;
      break;

    default:
      break;
  }
  return false;
}

std::unique_ptr<DigitalController> DigitalController::Create()
{
  return std::make_unique<DigitalController>();
}

