#pragma once
#include "types.h"

// Contains the information required to create a graphics context in a window.
struct WindowInfo
{
  void* display_connection = nullptr;
  uint32_t surface_width = 0;
  uint32_t surface_height = 0;
};
