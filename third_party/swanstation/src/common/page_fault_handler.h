#pragma once
#include "types.h"

namespace Common::PageFaultHandler {
enum class HandlerResult
{
  ContinueExecution,
  ExecuteNextHandler,
};

using Callback = HandlerResult (*)(void* exception_pc, void* fault_address, bool is_write);

uint32_t GetHandlerCodeSize();

bool InstallHandler(const void* owner, void* start_pc, uint32_t code_size, Callback callback);
bool RemoveHandler(const void* owner);

} // namespace Common::PageFaultHandler
