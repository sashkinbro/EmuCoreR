#pragma once
#include "controller.h"
#include <array>
#include <memory>
#include <optional>
#include <string_view>

class NeGcon final : public Controller
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
    Count
  };

  NeGcon();
  ~NeGcon() override;

  static std::unique_ptr<NeGcon> Create();

  ControllerType GetType() const override;

  void Reset() override;
  bool DoState(StateWrapper& sw, bool apply_input_state) override;

  void SetAxisState(int32_t axis_code, float value) override;

  void SetButtonState(int32_t button_code, bool pressed) override;

  void ResetTransferState() override;
  bool Transfer(const uint8_t data_in, uint8_t* data_out) override;

  void SetAxisState(Axis axis, uint8_t value);
  void SetButtonState(Button button, bool pressed);

  uint32_t GetButtonStateBits() const override;
  std::optional<uint32_t> GetAnalogInputBytes() const override;

  void LoadSettings(const char* section) override;

private:
  enum class TransferState : uint8_t
  {
    Idle,
    Ready,
    IDMSB,
    ButtonsLSB,
    ButtonsMSB,
    AnalogSteering,
    AnalogI,
    AnalogII,
    AnalogL
  };

  std::array<uint8_t, static_cast<uint8_t>(Axis::Count)> m_axis_state{};

  // buttons are active low; bits 0-2, 8-10, 14-15 are not used and are always high
  uint16_t m_button_state = UINT16_C(0xFFFF);

  TransferState m_transfer_state = TransferState::Idle;

  float m_steering_deadzone = 0.00f;
  std::string m_twist_response;
};
