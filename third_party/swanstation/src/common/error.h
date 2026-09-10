#pragma once
#include "string.h"
#include "types.h"

namespace Common {

// Lightweight error container - stores a code string and a free-form message.
class Error
{
public:
  Error();
  ~Error();

  // setter functions
  void Clear();
  void SetMessage(const char* msg);
  void SetFormattedMessage(const char* format, ...) printflike(2, 3);

  // get code and description, e.g. "[0x00000002]: File not Found"
  SmallString GetCodeAndMessage() const;
  void GetCodeAndMessage(String& dest) const;

private:
  StackString<16> m_code_string;
  TinyString m_message;
};

} // namespace Common
