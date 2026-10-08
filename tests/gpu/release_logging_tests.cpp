#include "common/log.h"
#include <cstdio>

Log_SetChannel(ReleaseLoggingTests);

static int s_messages = 0;
static void CountMessage(void*, const char*, const char*, LogLevel, const char*)
{
  ++s_messages;
}

int main()
{
  int failures = 0;
  const auto check = [&failures](bool passed, const char* name) {
    std::printf("%s %s\n", passed ? "PASS" : "FAIL", name);
    failures += !passed;
  };
  Log::RegisterCallback(CountMessage, nullptr);
  Log::SetFilterLevel(LogLevel::Trace);
  int evaluated_arguments = 0;
  Log_InfoPrintf("release diagnostic %d", ++evaluated_arguments);
  check(evaluated_arguments == 0, "release info logs do not evaluate arguments");
  check(s_messages == 0, "release info logs do not dispatch callbacks");
  Log::Write("Test", "main", LogLevel::Debug, "direct debug message");
  Log::Writef("Test", "main", LogLevel::Info, "%s", "direct info message");
  check(s_messages == 0, "release filtering cannot enable diagnostic messages");
  Log_WarningPrint("warning");
  Log_ErrorPrint("error");
  check(s_messages == 2, "warnings and errors remain available");
  Log::SetFilterLevel(LogLevel::None);
  Log_ErrorPrint("muted");
  check(s_messages == 2, "explicit silence still suppresses errors");
  Log::UnregisterCallback(CountMessage, nullptr);
  return failures ? 1 : 0;
}
