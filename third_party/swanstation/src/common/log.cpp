#include "log.h"
#include <cstdio>
#include <mutex>
#include <vector>

namespace Log {

struct RegisteredCallback
{
  CallbackFunctionType Function;
  void* Parameter;
};

std::vector<RegisteredCallback> s_callbacks;
static std::mutex s_callback_mutex;

static LogLevel s_filter_level = LogLevel::Trace;

void RegisterCallback(CallbackFunctionType callbackFunction, void* pUserParam)
{
  RegisteredCallback Callback;
  Callback.Function = callbackFunction;
  Callback.Parameter = pUserParam;

  std::lock_guard<std::mutex> guard(s_callback_mutex);
  s_callbacks.push_back(std::move(Callback));
}

void UnregisterCallback(CallbackFunctionType callbackFunction, void* pUserParam)
{
  std::lock_guard<std::mutex> guard(s_callback_mutex);

  for (auto iter = s_callbacks.begin(); iter != s_callbacks.end(); ++iter)
  {
    if (iter->Function == callbackFunction && iter->Parameter == pUserParam)
    {
      s_callbacks.erase(iter);
      break;
    }
  }
}

static void ExecuteCallbacks(const char* channelName, const char* functionName, LogLevel level, const char* message)
{
  std::lock_guard<std::mutex> guard(s_callback_mutex);
  for (RegisteredCallback& callback : s_callbacks)
    callback.Function(callback.Parameter, channelName, functionName, level, message);
}

void SetFilterLevel(LogLevel level)
{
  s_filter_level = level;
}

void Write(const char* channelName, const char* functionName, LogLevel level, const char* message)
{
  if (level > s_filter_level)
    return;

  ExecuteCallbacks(channelName, functionName, level, message);
}

void Writef(const char* channelName, const char* functionName, LogLevel level, const char* format, ...)
{
  if (level > s_filter_level)
    return;

  std::va_list ap;
  va_start(ap, format);
  Writev(channelName, functionName, level, format, ap);
  va_end(ap);
}

void Writev(const char* channelName, const char* functionName, LogLevel level, const char* format, std::va_list ap)
{
  if (level > s_filter_level)
    return;

  std::va_list apCopy;
  va_copy(apCopy, ap);

#ifdef _WIN32
  uint32_t requiredSize = static_cast<uint32_t>(_vscprintf(format, apCopy));
#else
  uint32_t requiredSize = std::vsnprintf(nullptr, 0, format, apCopy);
#endif
  va_end(apCopy);

  if (requiredSize < 256)
  {
    char buffer[256];
    std::vsnprintf(buffer, countof(buffer), format, ap);
    ExecuteCallbacks(channelName, functionName, level, buffer);
  }
  else
  {
    char* buffer = new char[requiredSize + 1];
    std::vsnprintf(buffer, requiredSize + 1, format, ap);
    ExecuteCallbacks(channelName, functionName, level, buffer);
    delete[] buffer;
  }
}

} // namespace Log
