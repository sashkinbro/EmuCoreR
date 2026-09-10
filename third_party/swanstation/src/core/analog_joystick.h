#pragma once
#include "controller.h"
#include <array>
#include <memory>
#include <optional>

class AnalogJoystick final : public Controller
{
public:
  enum class Axis : uint8_t
  {
    LeftX,
    LeftY,
    RightX,
    RightY,
    Count
  };

  enum class Button : uint8_t
  {
    Select = 0,
    L3 = 1,
    R3 = 2,
    Start = 3,
    Up = 4,
    Right = 5,
    Down = 6,
    Left = 7,
    L2 = 8,
    R2 = 9,
    L1 = 10,
    R1 = 11,
    Triangle = 12,
    Circle = 13,
    Cross = 14,
    Square = 15,
    Mode = 16,
    Count
  };

  AnalogJoystick(uint32_t index);
  ~AnalogJoystick() override;

  static std::unique_ptr<AnalogJoystick> Create(uint32_t index);

  ControllerType GetType() const override;

  void Reset() override;
  bool DoState(StateWrapper& sw, bool apply_input_state) override;

  void SetAxisState(int32_t axis_code, float value) override;
  void SetButtonState(int32_t button_code, bool pressed) override;
  uint32_t GetButtonStateBits() const override;
  std::optional<uint32_t> GetAnalogInputBytes() const override;

  void ResetTransferState() override;
  bool Transfer(const uint8_t data_in, uint8_t* data_out) override;

  void LoadSettings(const char* section) override;

  void SetAxisState(Axis axis, uint8_t value);
  void SetButtonState(Button button, bool pressed);

private:
  enum class TransferState : uint8_t
  {
    Idle,
    Ready,
    IDMSB,
    ButtonsLSB,
    ButtonsMSB,
    RightAxisX,
    RightAxisY,
    LeftAxisX,
    LeftAxisY
  };

  uint16_t GetID() const;
  void ToggleAnalogMode();

  uint32_t m_index;

  float m_axis_scale = 1.00f;

  // On original hardware, the mode toggle is a switch rather than a button, so we'll enable Analog Mode by default
  bool m_analog_mode = true;

  // buttons are active low
  uint16_t m_button_state = UINT16_C(0xFFFF);

  std::array<uint8_t, static_cast<uint8_t>(Axis::Count)> m_axis_state{};

  TransferState m_transfer_state = TransferState::Idle;
};
