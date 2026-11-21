#include "analog_joystick.h"
#include "common/log.h"
#include "common/state_wrapper.h"
#include "common/string_util.h"
#include "host_interface.h"
#include "system.h"
#include <cmath>
Log_SetChannel(AnalogJoystick);

AnalogJoystick::AnalogJoystick(uint32_t index)
{
  m_index = index;
  m_axis_state.fill(0x80);
  Reset();
}

AnalogJoystick::~AnalogJoystick() = default;

ControllerType AnalogJoystick::GetType() const
{
  return ControllerType::AnalogJoystick;
}

void AnalogJoystick::Reset()
{
  m_transfer_state = TransferState::Idle;
}

bool AnalogJoystick::DoState(StateWrapper& sw, bool apply_input_state)
{
  if (!Controller::DoState(sw, apply_input_state))
    return false;

  const bool old_analog_mode = m_analog_mode;

  sw.Do(&m_analog_mode);

  uint16_t button_state = m_button_state;
  std::array<uint8_t, static_cast<uint8_t>(Axis::Count)> axis_state = m_axis_state;
  sw.Do(&button_state);
  sw.Do(&axis_state);

  if (apply_input_state)
  {
    m_button_state = button_state;
    m_axis_state = axis_state;
  }

  sw.Do(&m_transfer_state);

  if (sw.IsReading() && (old_analog_mode != m_analog_mode))
  {
    g_host_interface->AddFormattedOSDMessage(
      5.0f,
      m_analog_mode ? g_host_interface->TranslateString("AnalogJoystick", "Controller %u switched to analog mode.") :
                      g_host_interface->TranslateString("AnalogJoystick", "Controller %u switched to digital mode."),
      m_index + 1u);
  }
  return true;
}

void AnalogJoystick::SetAxisState(int32_t axis_code, float value)
{
  if (axis_code < 0 || axis_code >= static_cast<int32_t>(Axis::Count))
    return;

  // -1..1 -> 0..255
  const float scaled_value = std::clamp(value * m_axis_scale, -1.0f, 1.0f);
  const uint8_t u8_value = static_cast<uint8_t>(std::clamp(std::round(((scaled_value + 1.0f) / 2.0f) * 255.0f), 0.0f, 255.0f));

  SetAxisState(static_cast<Axis>(axis_code), u8_value);
}

void AnalogJoystick::SetAxisState(Axis axis, uint8_t value)
{
  if (m_axis_state[static_cast<uint8_t>(axis)] != value)
    System::SetRunaheadReplayFlag();

  m_axis_state[static_cast<uint8_t>(axis)] = value;
}

void AnalogJoystick::SetButtonState(Button button, bool pressed)
{
  if (button == Button::Mode)
  {
    if (pressed)
      ToggleAnalogMode();

    return;
  }

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

void AnalogJoystick::SetButtonState(int32_t button_code, bool pressed)
{
  if (button_code < 0 || button_code >= static_cast<int32_t>(Button::Count))
    return;

  SetButtonState(static_cast<Button>(button_code), pressed);
}

uint32_t AnalogJoystick::GetButtonStateBits() const
{
  return m_button_state ^ 0xFFFF;
}

std::optional<uint32_t> AnalogJoystick::GetAnalogInputBytes() const
{
  return m_axis_state[static_cast<size_t>(Axis::LeftY)] << 24 | m_axis_state[static_cast<size_t>(Axis::LeftX)] << 16 |
         m_axis_state[static_cast<size_t>(Axis::RightY)] << 8 | m_axis_state[static_cast<size_t>(Axis::RightX)];
}

void AnalogJoystick::ResetTransferState()
{
  m_transfer_state = TransferState::Idle;
}

uint16_t AnalogJoystick::GetID() const
{
  static constexpr uint16_t DIGITAL_MODE_ID = 0x5A41;
  static constexpr uint16_t ANALOG_MODE_ID = 0x5A53;

  return m_analog_mode ? ANALOG_MODE_ID : DIGITAL_MODE_ID;
}

void AnalogJoystick::ToggleAnalogMode()
{
  m_analog_mode = !m_analog_mode;

  Log_InfoPrintf("Joystick %u switched to %s mode.", m_index + 1u, m_analog_mode ? "analog" : "digital");
  g_host_interface->AddFormattedOSDMessage(
    5.0f,
    m_analog_mode ? g_host_interface->TranslateString("AnalogJoystick", "Controller %u switched to analog mode.") :
                    g_host_interface->TranslateString("AnalogJoystick", "Controller %u switched to digital mode."),
    m_index + 1u);
}

bool AnalogJoystick::Transfer(const uint8_t data_in, uint8_t* data_out)
{
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
      return false;
    }

    case TransferState::Ready:
    {
      if (data_in == 0x42)
      {
        *data_out = static_cast<uint8_t>(GetID());
        m_transfer_state = TransferState::IDMSB;
        return true;
      }

      *data_out = 0xFF;
      return false;
    }

    case TransferState::IDMSB:
    {
      *data_out = static_cast<uint8_t>(GetID() >> 8);
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

      m_transfer_state = m_analog_mode ? TransferState::RightAxisX : TransferState::Idle;
      return m_analog_mode;
    }

    case TransferState::RightAxisX:
    {
      *data_out = static_cast<uint8_t>(m_axis_state[static_cast<uint8_t>(Axis::RightX)]);
      m_transfer_state = TransferState::RightAxisY;
      return true;
    }

    case TransferState::RightAxisY:
    {
      *data_out = static_cast<uint8_t>(m_axis_state[static_cast<uint8_t>(Axis::RightY)]);
      m_transfer_state = TransferState::LeftAxisX;
      return true;
    }

    case TransferState::LeftAxisX:
    {
      *data_out = static_cast<uint8_t>(m_axis_state[static_cast<uint8_t>(Axis::LeftX)]);
      m_transfer_state = TransferState::LeftAxisY;
      return true;
    }

    case TransferState::LeftAxisY:
    {
      *data_out = static_cast<uint8_t>(m_axis_state[static_cast<uint8_t>(Axis::LeftY)]);
      m_transfer_state = TransferState::Idle;
      return false;
    }

    default:
      return false;
  }
}

std::unique_ptr<AnalogJoystick> AnalogJoystick::Create(uint32_t index)
{
  return std::make_unique<AnalogJoystick>(index);
}

void AnalogJoystick::LoadSettings(const char* section)
{
  Controller::LoadSettings(section);
  m_axis_scale = std::clamp(g_host_interface->GetFloatSettingValue(section, "AxisScale", 1.33f), 0.01f, 1.50f);
}
