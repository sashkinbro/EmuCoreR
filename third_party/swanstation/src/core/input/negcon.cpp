#include "negcon.h"
#include "common/state_wrapper.h"
#include "host_interface.h"
#include "system.h"
#include <array>
#include <cmath>

NeGcon::NeGcon()
{
  m_axis_state.fill(0x00);
  m_axis_state[static_cast<uint8_t>(Axis::Steering)] = 0x80;
}

NeGcon::~NeGcon() = default;

ControllerType NeGcon::GetType() const
{
  return ControllerType::NeGcon;
}

void NeGcon::Reset()
{
  m_transfer_state = TransferState::Idle;
}

bool NeGcon::DoState(StateWrapper& sw, bool apply_input_state)
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

void NeGcon::SetAxisState(int32_t axis_code, float value)
{
  if (axis_code < 0 || axis_code >= static_cast<int32_t>(Axis::Count))
    return;

  // Steering Axis: -1..1 -> 0..255
  if (axis_code == static_cast<int32_t>(Axis::Steering))
  {
    float float_value =
      (std::abs(value) < m_steering_deadzone) ?
        0.0f :
        std::copysign((std::abs(value) - m_steering_deadzone) / (1.0f - m_steering_deadzone), value);

    if (m_twist_response == "quadratic")
    {
        if (float_value < 0.0f)
            float_value = -(float_value * float_value);
        else
            float_value = float_value * float_value;
    }
    else if (m_twist_response == "cubic")
    {
        float_value = float_value * float_value * float_value;
    }

    const uint8_t u8_value = static_cast<uint8_t>(std::clamp(std::round(((float_value + 1.0f) / 2.0f) * 255.0f), 0.0f, 255.0f));
    SetAxisState(static_cast<Axis>(axis_code), u8_value);

    return;
  }

  // I, II, L: -1..1 -> 0..255
  const uint8_t u8_value = static_cast<uint8_t>(std::clamp(value * 255.0f, 0.0f, 255.0f));

  SetAxisState(static_cast<Axis>(axis_code), u8_value);
}

void NeGcon::SetAxisState(Axis axis, uint8_t value)
{
  if (value != m_axis_state[static_cast<uint8_t>(axis)])
  {
    // Runahead-replay flag - same rationale as in SetButtonState. The
    // NeGcon's steering axis is the primary input for the racing games
    // it ships with, so a missed flag here means runahead replays the
    // wrong steering for one frame on every twist of the wheel.
    System::SetRunaheadReplayFlag();
    m_axis_state[static_cast<uint8_t>(axis)] = value;
  }
}

void NeGcon::SetButtonState(int32_t button_code, bool pressed)
{
  if (button_code < 0 || button_code >= static_cast<int32_t>(Button::Count))
    return;

  SetButtonState(static_cast<Button>(button_code), pressed);
}

void NeGcon::SetButtonState(Button button, bool pressed)
{
  // Mapping of Button to index of corresponding bit in m_button_state
  static constexpr std::array<uint8_t, static_cast<size_t>(Button::Count)> indices = {3, 4, 5, 6, 7, 11, 12, 13};

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

uint32_t NeGcon::GetButtonStateBits() const
{
  return m_button_state ^ 0xFFFF;
}

std::optional<uint32_t> NeGcon::GetAnalogInputBytes() const
{
  return m_axis_state[static_cast<size_t>(Axis::L)] << 24 | m_axis_state[static_cast<size_t>(Axis::II)] << 16 |
         m_axis_state[static_cast<size_t>(Axis::I)] << 8 | m_axis_state[static_cast<size_t>(Axis::Steering)];
}

void NeGcon::ResetTransferState()
{
  m_transfer_state = TransferState::Idle;
}

bool NeGcon::Transfer(const uint8_t data_in, uint8_t* data_out)
{
  static constexpr uint16_t ID = 0x5A23;

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
      m_transfer_state = TransferState::AnalogSteering;
      return true;
    }

    case TransferState::AnalogSteering:
    {
      *data_out = static_cast<uint8_t>(m_axis_state[static_cast<uint8_t>(Axis::Steering)]);
      m_transfer_state = TransferState::AnalogI;
      return true;
    }

    case TransferState::AnalogI:
    {
      *data_out = static_cast<uint8_t>(m_axis_state[static_cast<uint8_t>(Axis::I)]);
      m_transfer_state = TransferState::AnalogII;
      return true;
    }

    case TransferState::AnalogII:
    {
      *data_out = static_cast<uint8_t>(m_axis_state[static_cast<uint8_t>(Axis::II)]);
      m_transfer_state = TransferState::AnalogL;
      return true;
    }

    case TransferState::AnalogL:
    {
      *data_out = static_cast<uint8_t>(m_axis_state[static_cast<uint8_t>(Axis::L)]);
      m_transfer_state = TransferState::Idle;
      break;
    }

    default:
      break;
  }

  return false;
}

std::unique_ptr<NeGcon> NeGcon::Create()
{
  return std::make_unique<NeGcon>();
}

void NeGcon::LoadSettings(const char* section)
{
  Controller::LoadSettings(section);
  m_steering_deadzone = g_host_interface->GetFloatSettingValue(section, "SteeringDeadzone", 0.10f);
  m_twist_response = g_host_interface->GetStringSettingValue(section, "TwistResponse");
}
