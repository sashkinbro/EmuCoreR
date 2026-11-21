#include "libecr_settings_interface.h"
#include "common/string_util.h"
#include "core/host_interface.h"
#include <type_traits>

template<typename T, typename DefaultValueType>
static T GetVariable(const char* section, const char* key, DefaultValueType default_value)
{

  TinyString full_key;
  full_key.Format("ecr_%s_%s", section, key);

  retro_variable rv = {full_key.GetCharArray(), nullptr};
  if (!g_retro_environment_callback(RETRO_ENVIRONMENT_GET_VARIABLE, &rv) || !rv.value)
  {
    // Legacy SwanStation frontend option key fallback for external libretro hosts.
    full_key.Format("swanstation_%s_%s", section, key);
    rv.key = full_key.GetCharArray();
    rv.value = nullptr;
    if (!g_retro_environment_callback(RETRO_ENVIRONMENT_GET_VARIABLE, &rv) || !rv.value)
      return T(default_value);
  }

  if constexpr (std::is_same_v<T, std::string>)
  {
    return T(rv.value);
  }
  else if constexpr (std::is_same_v<T, bool>)
  {
    return (StringUtil::Strcasecmp(rv.value, "true") == 0 || StringUtil::Strcasecmp(rv.value, "1") == 0);
  }
  else if constexpr (std::is_same_v<T, float>)
  {
    return std::strtof(rv.value, nullptr);
  }
  else
  {
    std::optional<T> parsed = StringUtil::FromChars<T>(rv.value);
    if (!parsed.has_value())
      return T(default_value);

    return parsed.value();
  }
}

int LibecrSettingsInterface::GetIntValue(const char* section, const char* key, int default_value /*= 0*/)
{
  return GetVariable<int>(section, key, default_value);
}

float LibecrSettingsInterface::GetFloatValue(const char* section, const char* key, float default_value /*= 0.0f*/)
{
  return GetVariable<float>(section, key, default_value);
}

bool LibecrSettingsInterface::GetBoolValue(const char* section, const char* key, bool default_value /*= false*/)
{
  return GetVariable<bool>(section, key, default_value);
}

std::string LibecrSettingsInterface::GetStringValue(const char* section, const char* key,
                                                      const char* default_value /*= ""*/)
{
  return GetVariable<std::string>(section, key, default_value);
}
