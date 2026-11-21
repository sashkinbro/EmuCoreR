#include "progress_callback.h"
#include "byte_stream.h"
#include "log.h"
#include <cstdarg>
Log_SetChannel(ProgressCallback);

ProgressCallback::~ProgressCallback() {}

void ProgressCallback::SetFormattedStatusText(const char* Format, ...)
{
  SmallString str;
  std::va_list ap;

  va_start(ap, Format);
  str.FormatVA(Format, ap);
  va_end(ap);

  SetStatusText(str);
}

void ProgressCallback::DisplayFormattedError(const char* format, ...)
{
  SmallString str;
  std::va_list ap;

  va_start(ap, format);
  str.FormatVA(format, ap);
  va_end(ap);

  DisplayError(str);
}

void ProgressCallback::DisplayFormattedWarning(const char* format, ...)
{
  SmallString str;
  std::va_list ap;

  va_start(ap, format);
  str.FormatVA(format, ap);
  va_end(ap);

  DisplayWarning(str);
}

void ProgressCallback::DisplayFormattedInformation(const char* format, ...)
{
  SmallString str;
  std::va_list ap;

  va_start(ap, format);
  str.FormatVA(format, ap);
  va_end(ap);

  DisplayInformation(str);
}

void ProgressCallback::DisplayFormattedDebugMessage(const char* format, ...)
{
  SmallString str;
  std::va_list ap;

  va_start(ap, format);
  str.FormatVA(format, ap);
  va_end(ap);

  DisplayDebugMessage(str);
}

void ProgressCallback::DisplayFormattedModalError(const char* format, ...)
{
  SmallString str;
  std::va_list ap;

  va_start(ap, format);
  str.FormatVA(format, ap);
  va_end(ap);

  ModalError(str);
}

bool ProgressCallback::DisplayFormattedModalConfirmation(const char* format, ...)
{
  SmallString str;
  std::va_list ap;

  va_start(ap, format);
  str.FormatVA(format, ap);
  va_end(ap);

  return ModalConfirmation(str);
}

void ProgressCallback::DisplayFormattedModalInformation(const char* format, ...)
{
  SmallString str;
  std::va_list ap;

  va_start(ap, format);
  str.FormatVA(format, ap);
  va_end(ap);

  ModalInformation(str);
}

void ProgressCallback::UpdateProgressFromStream(ByteStream* pStream)
{
  uint32_t streamSize = (uint32_t)pStream->GetSize();
  uint32_t streamPosition = (uint32_t)pStream->GetPosition();

  SetProgressRange(streamSize);
  SetProgressValue(streamPosition);
}

class NullProgressCallbacks final : public ProgressCallback
{
public:
  void PushState() override {}
  void PopState() override {}

  bool IsCancelled() const override { return false; }
  bool IsCancellable() const override { return false; }

  void SetCancellable(bool cancellable) override {}
  void SetTitle(const char* title) override {}
  void SetStatusText(const char* statusText) override {}
  void SetProgressRange(uint32_t range) override {}
  void SetProgressValue(uint32_t value) override {}
  void IncrementProgressValue() override {}

  void DisplayError(const char* message) override { Log_ErrorPrint(message); }
  void DisplayWarning(const char* message) override { Log_WarningPrint(message); }
  void DisplayInformation(const char* message) override { Log_InfoPrint(message); }
  void DisplayDebugMessage(const char* message) override { }

  void ModalError(const char* message) override { Log_ErrorPrint(message); }
  bool ModalConfirmation(const char* message) override
  {
    Log_InfoPrint(message);
    return false;
  }
  void ModalInformation(const char* message) override { Log_InfoPrint(message); }
};

static NullProgressCallbacks s_nullProgressCallbacks;
ProgressCallback* ProgressCallback::NullProgressCallback = &s_nullProgressCallbacks;

BaseProgressCallback::BaseProgressCallback()
  : m_cancellable(false), m_cancelled(false), m_progress_range(1), m_progress_value(0), m_base_progress_value(0),
    m_saved_state(NULL)
{
}

BaseProgressCallback::~BaseProgressCallback()
{
  State* pNextState = m_saved_state;
  while (pNextState != NULL)
  {
    State* pCurrentState = pNextState;
    pNextState = pCurrentState->next_saved_state;
    delete pCurrentState;
  }
}

void BaseProgressCallback::PushState()
{
  State* pNewState = new State;
  pNewState->cancellable = m_cancellable;
  pNewState->status_text = m_status_text;
  pNewState->progress_range = m_progress_range;
  pNewState->progress_value = m_progress_value;
  pNewState->base_progress_value = m_base_progress_value;
  pNewState->next_saved_state = m_saved_state;
  m_saved_state = pNewState;
}

void BaseProgressCallback::PopState()
{
  State* state = m_saved_state;
  m_saved_state = nullptr;

  // impose the current position into the previous range
  const uint32_t new_progress_value =
    (m_progress_range != 0) ?
      static_cast<uint32_t>(((float)m_progress_value / (float)m_progress_range) * (float)state->progress_range) :
      state->progress_value;

  m_cancellable = state->cancellable;
  m_status_text = std::move(state->status_text);
  m_progress_range = state->progress_range;
  m_progress_value = new_progress_value;

  m_base_progress_value = state->base_progress_value;
  m_saved_state = state->next_saved_state;
  delete state;
}

bool BaseProgressCallback::IsCancelled() const
{
  return m_cancelled;
}

bool BaseProgressCallback::IsCancellable() const
{
  return m_cancellable;
}

void BaseProgressCallback::SetCancellable(bool cancellable)
{
  m_cancellable = cancellable;
}

void BaseProgressCallback::SetStatusText(const char* text)
{
  m_status_text = text;
}

void BaseProgressCallback::SetProgressRange(uint32_t range)
{
  if (m_saved_state)
  {
    // impose the previous range on this range
    m_progress_range = m_saved_state->progress_range * range;
    m_base_progress_value = m_progress_value = m_saved_state->progress_value * range;
  }
  else
  {
    m_progress_range = range;
    m_progress_value = 0;
    m_base_progress_value = 0;
  }
}

void BaseProgressCallback::SetProgressValue(uint32_t value)
{
  m_progress_value = m_base_progress_value + value;
}

void BaseProgressCallback::IncrementProgressValue()
{
  SetProgressValue((m_progress_value - m_base_progress_value) + 1);
}

