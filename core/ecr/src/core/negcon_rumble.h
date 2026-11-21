#pragma once
#include "controller.h"
#include <array>
#include <memory>
#include <optional>
#include <string_view>

class NeGconRumble final : public Controller
{
public:
  enum class Axis : uint8_t
  {
    Steering = 0,
    I = 1,
    II = 2,
    L = 3,
    Count
  };

  enum class Button : uint8_t
  {
    Start = 0,
    Up = 1,
    Right = 2,
    Down = 3,
    Left = 4,
    R = 5,
    B = 6,
    A = 7,
    Analog = 8,
    Count
  };

  static constexpr uint8_t NUM_MOTORS = 2;

  NeGconRumble(uint32_t index);
  ~NeGconRumble() override;

  static std::unique_ptr<NeGconRumble> Create(uint32_t index);

  ControllerType GetType() const override;

  void Reset() override;
  bool DoState(StateWrapper& sw, bool ignore_input_state) override;

  void SetAxisState(int32_t axis_code, float value) override;
  void SetButtonState(int32_t button_code, bool pressed) override;
  uint32_t GetButtonStateBits() const override;
  std::optional<uint32_t> GetAnalogInputBytes() const override;

  void ResetTransferState() override;
  bool Transfer(const uint8_t data_in, uint8_t* data_out) override;

  void SetAxisState(Axis axis, uint8_t value);
  void SetButtonState(Button button, bool pressed);

  float GetVibrationMotorStrength(uint32_t motor) override;

  void LoadSettings(const char* section) override;

private:
  using MotorState = std::array<uint8_t, NUM_MOTORS>;

  enum class Command : uint8_t
  {
    Idle,
    Ready,
    ReadPad,           // 0x42
    ConfigModeSetMode, // 0x43
    SetAnalogMode,     // 0x44
    GetAnalogMode,     // 0x45
    Command46,         // 0x46
    Command47,         // 0x47
    Command4C,         // 0x4C
    GetSetRumble       // 0x4D
  };

  Command m_command = Command::Idle;
  int m_command_step = 0;

  // Transmit and receive buffers, not including the first Hi-Z/ack response byte
  static constexpr uint32_t MAX_RESPONSE_LENGTH = 8;
  std::array<uint8_t, MAX_RESPONSE_LENGTH> m_rx_buffer;
  std::array<uint8_t, MAX_RESPONSE_LENGTH> m_tx_buffer;
  uint32_t m_response_length = 0;

  // Get number of response halfwords (excluding the initial controller info halfword)
  uint8_t GetResponseNumHalfwords() const;

  uint8_t GetModeID() const;
  uint8_t GetIDByte() const;

  void SetAnalogMode(bool enabled);
  void ProcessAnalogModeToggle();
  void SetMotorState(uint8_t motor, uint8_t value);
  uint8_t GetExtraButtonMaskLSB() const;
  void ResetRumbleConfig();
  void SetMotorStateForConfigIndex(int index, uint8_t value);

  uint32_t m_index;

  uint8_t m_rumble_bias = 8;

  bool m_analog_mode = false;
  bool m_analog_locked = false;
  bool m_dualshock_enabled = false;
  bool m_configuration_mode = false;

  std::array<uint8_t, static_cast<uint8_t>(Axis::Count)> m_axis_state{};

  static constexpr uint8_t LargeMotor = 0, SmallMotor = 1;

  std::array<uint8_t, 6> m_rumble_config{};
  int m_rumble_config_large_motor_index = -1;
  int m_rumble_config_small_motor_index = -1;

  bool m_analog_toggle_queued = false;
  uint8_t m_status_byte = 0x5A;

  // buttons are active low
  uint16_t m_button_state = UINT16_C(0xFFFF);

  MotorState m_motor_state{};

  // Member variables that are no longer used, but kept and serialized for compatibility with older save states
  float m_steering_deadzone = 0.00f;
  std::string m_twist_response;
};
