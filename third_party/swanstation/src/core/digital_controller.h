#pragma once
#include "controller.h"
#include <memory>
#include <optional>
#include <string_view>

class DigitalController final : public Controller
{
public:
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
    Count
  };

  DigitalController();
  ~DigitalController() override;

  static std::unique_ptr<DigitalController> Create();

  ControllerType GetType() const override;

  void Reset() override;
  bool DoState(StateWrapper& sw, bool apply_input_state) override;

  void SetButtonState(int32_t button_code, bool pressed) override;
  uint32_t GetButtonStateBits() const override;

  void ResetTransferState() override;
  bool Transfer(const uint8_t data_in, uint8_t* data_out) override;

  void SetButtonState(Button button, bool pressed);

private:
  enum class TransferState : uint8_t
  {
    Idle,
    Ready,
    IDMSB,
    ButtonsLSB,
    ButtonsMSB
  };

  // buttons are active low
  uint16_t m_button_state = UINT16_C(0xFFFF);

  TransferState m_transfer_state = TransferState::Idle;
};
