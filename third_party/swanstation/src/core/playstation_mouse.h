#pragma once
#include "controller.h"
#include <memory>
#include <optional>
#include <string_view>

class PlayStationMouse final : public Controller
{
public:
  enum class Button : uint8_t
  {
    Left = 0,
    Right = 1,
    Count
  };

  PlayStationMouse();
  ~PlayStationMouse() override;

  static std::unique_ptr<PlayStationMouse> Create();

  ControllerType GetType() const override;

  void Reset() override;
  bool DoState(StateWrapper& sw, bool apply_input_state) override;

  void SetButtonState(int32_t button_code, bool pressed) override;

  void ResetTransferState() override;
  bool Transfer(const uint8_t data_in, uint8_t* data_out) override;

  void SetButtonState(Button button, bool pressed);

private:
  void UpdatePosition();

  enum class TransferState : uint8_t
  {
    Idle,
    Ready,
    IDMSB,
    ButtonsLSB,
    ButtonsMSB,
    DeltaX,
    DeltaY
  };

  int32_t m_last_host_position_x = 0;
  int32_t m_last_host_position_y = 0;

  // buttons are active low
  uint16_t m_button_state = UINT16_C(0xFFFF);
  int8_t m_delta_x = 0;
  int8_t m_delta_y = 0;

  TransferState m_transfer_state = TransferState::Idle;
};
