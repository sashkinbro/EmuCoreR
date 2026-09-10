#pragma once
#include "common/image.h"
#include "settings.h"
#include "types.h"
#include <memory>
#include <optional>

class StateWrapper;
class HostInterface;

class Controller
{
public:
  Controller();
  virtual ~Controller();

  /// Returns the type of controller.
  virtual ControllerType GetType() const = 0;

  virtual void Reset();
  virtual bool DoState(StateWrapper& sw, bool apply_input_state);

  // Resets all state for the transferring to/from the device.
  virtual void ResetTransferState();

  // Returns the value of ACK, as well as filling out_data.
  virtual bool Transfer(const uint8_t data_in, uint8_t* data_out);

  /// Changes the specified axis state. Values are normalized from -1..1.
  virtual void SetAxisState(int32_t axis_code, float value);

  /// Changes the specified button state.
  virtual void SetButtonState(int32_t button_code, bool pressed);

  /// Returns a bitmask of the current button states, 1 = on.
  virtual uint32_t GetButtonStateBits() const;

  /// Returns analog input bytes packed as a uint32_t. Values are specific to controller type.
  virtual std::optional<uint32_t> GetAnalogInputBytes() const;

  /// Returns the number of vibration motors.
  /// Queries the state of the specified vibration motor. Values are normalized from 0..1.
  virtual float GetVibrationMotorStrength(uint32_t motor);

  /// Loads/refreshes any per-controller settings.
  virtual void LoadSettings(const char* section);

  /// Returns the software cursor to use for this controller, if any.
  virtual bool GetSoftwareCursor(const Common::RGBA8Image** image, float* image_scale);

  /// Creates a new controller of the specified type.
  static std::unique_ptr<Controller> Create(ControllerType type, uint32_t index);
};
