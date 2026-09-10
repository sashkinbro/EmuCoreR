#include "error.h"
#include <cstdarg>

namespace Common {

Error::Error() = default;

Error::~Error() = default;

void Error::Clear()
{
  m_code_string.Clear();
  m_message.Clear();
}

void Error::SetMessage(const char* msg)
{
  m_code_string.Clear();
  m_message = msg;
}

void Error::SetFormattedMessage(const char* format, ...)
{
  std::va_list ap;
  va_start(ap, format);

  m_code_string.Clear();
  m_message.FormatVA(format, ap);
  va_end(ap);
}

SmallString Error::GetCodeAndMessage() const
{
  SmallString ret;
  GetCodeAndMessage(ret);
  return ret;
}

void Error::GetCodeAndMessage(String& dest) const
{
  if (m_code_string.IsEmpty())
    dest.Assign(m_message);
  else
    dest.Format("[%s]: %s", m_code_string.GetCharArray(), m_message.GetCharArray());
}

} // namespace Common
