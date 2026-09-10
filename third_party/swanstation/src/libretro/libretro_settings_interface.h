#pragma once
#include "core/types.h"
#include <string>

class LibretroSettingsInterface
{
public:
  int GetIntValue(const char* section, const char* key, int default_value = 0);
  float GetFloatValue(const char* section, const char* key, float default_value = 0.0f);
  bool GetBoolValue(const char* section, const char* key, bool default_value = false);
  std::string GetStringValue(const char* section, const char* key, const char* default_value = "");
};
