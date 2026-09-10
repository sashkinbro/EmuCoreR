#include "string.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#define CASE_N_COMPARE _strnicmp
#else
#define CASE_N_COMPARE strncasecmp
#endif

// globals
const String::StringData String::s_EmptyStringData = {const_cast<char*>(""), 0, 1, -1, true};

// helper functions
static String::StringData* StringDataAllocate(uint32_t allocSize)
{
  String::StringData* pStringData =
    reinterpret_cast<String::StringData*>(std::malloc(sizeof(String::StringData) + allocSize));
  if (pStringData == nullptr)
    std::abort();
  pStringData->pBuffer = reinterpret_cast<char*>(pStringData + 1);
  pStringData->StringLength = 0;
  pStringData->BufferSize = allocSize;
  pStringData->ReadOnly = false;
  pStringData->ReferenceCount = 1;

  // if in debug build, set all to zero, otherwise only the first to zero.
#ifdef _DEBUG
  std::memset(pStringData->pBuffer, 0, allocSize);
#else
  pStringData->pBuffer[0] = 0;
  if (allocSize > 1)
    pStringData->pBuffer[allocSize - 1] = 0;
#endif

  return pStringData;
}

static inline void StringDataRelease(String::StringData* pStringData)
{
  if (pStringData->ReferenceCount == -1)
    return;

  uint32_t newRefCount = --pStringData->ReferenceCount;
  if (!newRefCount)
    std::free(pStringData);
}

static String::StringData* StringDataClone(const String::StringData* pStringData, uint32_t newSize, bool copyPastString)
{
  String::StringData* pClone = StringDataAllocate(newSize);
  if (pStringData->StringLength > 0)
  {
    uint32_t copyLength;

    if (copyPastString)
    {
      copyLength = std::min(newSize, pStringData->BufferSize);
      if (copyLength > 0)
      {
        std::memcpy(pClone->pBuffer, pStringData->pBuffer, copyLength);
        if (copyLength < pStringData->BufferSize)
          pClone->pBuffer[copyLength - 1] = 0;
      }
    }
    else
    {
      copyLength = std::min(newSize, pStringData->StringLength);
      if (copyLength > 0)
      {
        std::memcpy(pClone->pBuffer, pStringData->pBuffer, copyLength);
        pClone->pBuffer[copyLength] = 0;
      }
    }

    pClone->StringLength = copyLength;
  }

  return pClone;
}

static String::StringData* StringDataReallocate(String::StringData* pStringData, uint32_t newSize)
{
  // perform realloc
  String::StringData* pNewStringData =
    reinterpret_cast<String::StringData*>(std::realloc(pStringData, sizeof(String::StringData) + newSize));
  if (pNewStringData == nullptr)
    std::abort();
  pStringData = pNewStringData;
  pStringData->pBuffer = reinterpret_cast<char*>(pStringData + 1);

  // zero bytes in debug
#ifdef _DEBUG
  if (newSize > pStringData->BufferSize)
  {
    uint32_t bytesToZero = newSize - pStringData->BufferSize;
    std::memset(pStringData->pBuffer + (newSize - bytesToZero), 0, bytesToZero);
  }
#else
  if (newSize > pStringData->BufferSize)
    pStringData->pBuffer[newSize - 1] = 0;
#endif

  // update size
  pStringData->BufferSize = newSize;
  return pStringData;
}

static bool StringDataIsSharable(const String::StringData* pStringData)
{
  return pStringData->ReadOnly || pStringData->ReferenceCount != -1;
}

static bool StringDataIsShared(const String::StringData* pStringData)
{
  return pStringData->ReferenceCount > 1;
}

String::String() : m_pStringData(const_cast<String::StringData*>(&s_EmptyStringData)) {}

String::String(const String& copyString)
{
  // special case: empty strings
  if (copyString.IsEmpty())
  {
    m_pStringData = const_cast<String::StringData*>(&s_EmptyStringData);
  }
  // is the string data sharable?
  else if (StringDataIsSharable(copyString.m_pStringData))
  {
    m_pStringData = copyString.m_pStringData;
    if (!m_pStringData->ReadOnly)
      ++m_pStringData->ReferenceCount;
  }
  // create a clone for ourselves
  else
  {
    // since we're going to the effort of creating a clone, we might as well create it as the smallest size possible
    m_pStringData = StringDataClone(copyString.m_pStringData, copyString.m_pStringData->StringLength + 1, false);
  }
}

String::String(const char* Text) : m_pStringData(const_cast<String::StringData*>(&s_EmptyStringData))
{
  Assign(Text);
}

String::String(String&& moveString)
{
  Assign(moveString);
}

String::~String()
{
  StringDataRelease(m_pStringData);
}

void String::EnsureOwnWritableCopy()
{
  if (StringDataIsShared(m_pStringData) || m_pStringData->ReadOnly)
  {
    StringData* pNewStringData = StringDataClone(m_pStringData, m_pStringData->StringLength + 1, false);
    StringDataRelease(m_pStringData);
    m_pStringData = pNewStringData;
  }
}

void String::EnsureRemainingSpace(uint32_t spaceRequired)
{
  StringData* pNewStringData;
  uint32_t requiredReserve = m_pStringData->StringLength + spaceRequired + 1;

  if (StringDataIsShared(m_pStringData) || m_pStringData->ReadOnly)
  {
    pNewStringData = StringDataClone(m_pStringData, std::max(requiredReserve, m_pStringData->BufferSize), false);
    StringDataRelease(m_pStringData);
    m_pStringData = pNewStringData;
  }
  else if (m_pStringData->BufferSize < requiredReserve)
  {
    uint32_t newSize = std::max(requiredReserve, m_pStringData->BufferSize * 2);

    // if we are the only owner of the buffer, we can simply realloc it
    if (m_pStringData->ReferenceCount == 1)
    {
      // do realloc and update pointer
      m_pStringData = StringDataReallocate(m_pStringData, newSize);
    }
    else
    {
      // clone and release old
      pNewStringData = StringDataClone(m_pStringData, std::max(requiredReserve, newSize), false);
      StringDataRelease(m_pStringData);
      m_pStringData = pNewStringData;
    }
  }
}

void String::InternalAppend(const char* pString, uint32_t Length)
{
  EnsureRemainingSpace(Length);

  std::memcpy(m_pStringData->pBuffer + m_pStringData->StringLength, pString, Length);
  m_pStringData->StringLength += Length;
  m_pStringData->pBuffer[m_pStringData->StringLength] = 0;
}

void String::AppendString(const String& appendStr)
{
  if (appendStr.GetLength() > 0)
    InternalAppend(appendStr.GetCharArray(), appendStr.GetLength());
}

void String::AppendString(const char* appendText)
{
  uint32_t textLength = static_cast<uint32_t>(std::strlen(appendText));
  if (textLength > 0)
    InternalAppend(appendText, textLength);
}

void String::AppendString(const char* appendString, uint32_t Count)
{
  if (Count > 0)
    InternalAppend(appendString, Count);
}

void String::Format(const char* FormatString, ...)
{
  std::va_list ap;
  va_start(ap, FormatString);
  FormatVA(FormatString, ap);
  va_end(ap);
}

void String::FormatVA(const char* FormatString, std::va_list ArgPtr)
{
  if (GetLength() > 0)
    Clear();

  // We have a 1KB byte buffer on the stack here. If this is too little, we'll grow it via the heap,
  // but 1KB should be enough for most strings.
  char stackBuffer[1024];
  char* pHeapBuffer = NULL;
  char* pBuffer = stackBuffer;
  uint32_t currentBufferSize = countof(stackBuffer);
  uint32_t charsWritten;

  for (;;)
  {
    std::va_list ArgPtrCopy;
    va_copy(ArgPtrCopy, ArgPtr);
    int ret = std::vsnprintf(pBuffer, currentBufferSize, FormatString, ArgPtrCopy);
    va_end(ArgPtrCopy);
    if (ret < 0 || ((uint32_t)ret >= (currentBufferSize - 1)))
    {
      currentBufferSize *= 2;
      char* pNewBuffer = reinterpret_cast<char*>(std::realloc(pHeapBuffer, currentBufferSize));
      if (pNewBuffer == nullptr)
      {
        if (pHeapBuffer != NULL)
          std::free(pHeapBuffer);
        std::abort();
      }
      pBuffer = pHeapBuffer = pNewBuffer;
      continue;
    }

    charsWritten = (uint32_t)ret;
    break;
  }

  InternalAppend(pBuffer, charsWritten);

  if (pHeapBuffer != NULL)
    std::free(pHeapBuffer);
}

void String::Assign(const String& copyString)
{
  // special case: empty strings
  if (copyString.IsEmpty())
  {
    m_pStringData = const_cast<String::StringData*>(&s_EmptyStringData);
  }
  // is the string data sharable?
  else if (StringDataIsSharable(copyString.m_pStringData))
  {
    m_pStringData = copyString.m_pStringData;
    if (!m_pStringData->ReadOnly)
      ++m_pStringData->ReferenceCount;
  }
  // create a clone for ourselves
  else
  {
    // since we're going to the effort of creating a clone, we might as well create it as the smallest size possible
    m_pStringData = StringDataClone(copyString.m_pStringData, copyString.m_pStringData->StringLength + 1, false);
  }
}

void String::Assign(const char* copyText)
{
  Clear();
  AppendString(copyText);
}

void String::Assign(String&& moveString)
{
  Clear();
  m_pStringData = moveString.m_pStringData;
  moveString.m_pStringData = const_cast<String::StringData*>(&s_EmptyStringData);
}

bool String::Compare(const char* otherText) const
{
  return (std::strcmp(m_pStringData->pBuffer, otherText) == 0);
}

void String::Clear()
{
  if (m_pStringData == &s_EmptyStringData)
    return;

  // Do we have a shared buffer? If so, cancel it and allocate a new one when we need to.
  // Otherwise, clear the current buffer.
  if (StringDataIsShared(m_pStringData) || m_pStringData->ReadOnly)
  {
    // replace with empty string data
    StringDataRelease(m_pStringData);
    m_pStringData = const_cast<StringData*>(&s_EmptyStringData);
  }
  else
  {
    // in debug, zero whole string, in release, zero only the first character
#if _DEBUG
    std::memset(m_pStringData->pBuffer, 0, m_pStringData->BufferSize);
#else
    m_pStringData->pBuffer[0] = '\0';
#endif
    m_pStringData->StringLength = 0;
  }
}

// Internal: ensure the buffer can hold at least newReserve characters plus the
// terminator. Only used by Replace below.
static void StringReserve(String::StringData*& pStringData, uint32_t newReserve)
{
  uint32_t newSize = std::max(newReserve + 1, pStringData->BufferSize);
  String::StringData* pNewStringData;

  if (StringDataIsShared(pStringData) || pStringData->ReadOnly)
  {
    pNewStringData = StringDataClone(pStringData, newSize, false);
    StringDataRelease(pStringData);
    pStringData = pNewStringData;
  }
  else
  {
    // skip if smaller
    if (newSize <= pStringData->BufferSize)
      return;

    // if we are the only owner of the buffer, we can simply realloc it
    if (pStringData->ReferenceCount == 1)
    {
      // do realloc and update pointer
      pStringData = StringDataReallocate(pStringData, newSize);
    }
    else
    {
      // clone and release old
      pNewStringData = StringDataClone(pStringData, newSize, false);
      StringDataRelease(pStringData);
      pStringData = pNewStringData;
    }
  }
}

void String::Resize(uint32_t newSize, char fillerCharacter /* = ' ' */)
{
  StringData* pNewStringData;

  // if going larger, or we don't own the buffer, realloc
  if (StringDataIsShared(m_pStringData) || m_pStringData->ReadOnly || newSize >= m_pStringData->BufferSize)
  {
    pNewStringData = StringDataClone(m_pStringData, newSize + 1, true);
    StringDataRelease(m_pStringData);
    m_pStringData = pNewStringData;

    if (m_pStringData->StringLength < newSize)
    {
      std::memset(m_pStringData->pBuffer + m_pStringData->StringLength, fillerCharacter,
                  m_pStringData->BufferSize - m_pStringData->StringLength - 1);
    }

    m_pStringData->StringLength = newSize;
  }
  else
  {
    // owns the buffer, and going smaller
    // update length and terminator
#if _DEBUG
    std::memset(m_pStringData->pBuffer + newSize, 0, m_pStringData->BufferSize - newSize);
#else
    m_pStringData->pBuffer[newSize] = 0;
#endif
    m_pStringData->StringLength = newSize;
  }
}

void String::UpdateSize()
{
  EnsureOwnWritableCopy();
  m_pStringData->StringLength = static_cast<uint32_t>(std::strlen(m_pStringData->pBuffer));
}

uint32_t String::Replace(const char* searchString, const char* replaceString)
{
  uint32_t nReplacements = 0;
  uint32_t searchStringLength = static_cast<uint32_t>(std::strlen(searchString));

  // TODO: Fastpath if strlen(searchString) == strlen(replaceString)

  String tempString;
  char* pStart = m_pStringData->pBuffer;
  char* pCurrent = std::strstr(pStart, searchString);
  char* pLast = NULL;
  while (pCurrent != NULL)
  {
    if ((nReplacements++) == 0)
      StringReserve(tempString.m_pStringData, m_pStringData->StringLength);

    // append [pStart, pCurrent) range from this string into tempString
    uint32_t chunkLen = static_cast<uint32_t>(pCurrent - pStart);
    if (chunkLen > 0)
      tempString.InternalAppend(pStart, chunkLen);
    tempString.AppendString(replaceString);
    pLast = pCurrent + searchStringLength;
    pStart = pLast;
    nReplacements++;

    pCurrent = std::strstr(pLast, searchString);
  }

  if (pLast != NULL)
  {
    // append remainder [pStart, end) into tempString
    uint32_t remainLen = m_pStringData->StringLength - static_cast<uint32_t>(pStart - m_pStringData->pBuffer);
    if (remainLen > 0)
      tempString.InternalAppend(pStart, remainLen);
  }

  if (nReplacements)
    std::swap(m_pStringData, tempString.m_pStringData);

  return nReplacements;
}

String String::FromFormat(const char* FormatString, ...)
{
  String returnStr;
  std::va_list ap;

  va_start(ap, FormatString);
  returnStr.FormatVA(FormatString, ap);
  va_end(ap);

  return returnStr;
}
