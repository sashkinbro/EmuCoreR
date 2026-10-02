#include "controller.h"
#include "analog_controller.h"
#include "analog_joystick.h"
#include "common/state_wrapper.h"
#include "digital_controller.h"
#include "namco_guncon.h"
#include "negcon.h"
#include "negcon_rumble.h"
#include "playstation_mouse.h"

Controller::Controller() = default;

Controller::~Controller() = default;

void Controller::Reset() {}

bool Controller::DoState(StateWrapper& sw, bool apply_input_state)
{
  return !sw.HasError();
}

void Controller::ResetTransferState() {}

bool Controller::Transfer(const uint8_t data_in, uint8_t* data_out)
{
  *data_out = 0xFF;
  return false;
}

void Controller::SetAxisState(int32_t axis_code, float value) {}

void Controller::SetButtonState(int32_t button_code, bool pressed) {}

uint32_t Controller::GetButtonStateBits() const
{
  return 0;
}

std::optional<uint32_t> Controller::GetAnalogInputBytes() const
{
  return std::nullopt;
}

float Controller::GetVibrationMotorStrength(uint32_t motor)
{
  return 0.0f;
}

void Controller::LoadSettings(const char* section) {}

bool Controller::GetSoftwareCursor(const Common::RGBA8Image** image, float* image_scale)
{
  return false;
}

std::unique_ptr<Controller> Controller::Create(ControllerType type, uint32_t index)
{
  switch (type)
  {
    case ControllerType::DigitalController:
      return DigitalController::Create();

    case ControllerType::AnalogController:
      return AnalogController::Create(index);

    case ControllerType::AnalogJoystick:
      return AnalogJoystick::Create(index);

    case ControllerType::NamcoGunCon:
      return NamcoGunCon::Create();

    case ControllerType::PlayStationMouse:
      return PlayStationMouse::Create();

    case ControllerType::NeGcon:
      return NeGcon::Create();

    case ControllerType::NeGconRumble:
      return NeGconRumble::Create(index);

    case ControllerType::None:
    default:
      return {};
  }
}
