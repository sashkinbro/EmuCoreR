#include "byte_stream.h"
#include "file_system.h"
#include "string_util.h"
#include <config.h>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

#if defined(_WIN32)
#include "windows_headers.h"
#include <malloc.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#  if defined(HAVE_ALLOCA_H)
#    include <alloca.h>
#  endif
#endif

#include <file/file_path.h>

class FileByteStream : public ByteStream
{
public:
  FileByteStream(RFILE* pFile) : m_pFile(pFile) { }

  virtual ~FileByteStream() override { rfclose(m_pFile); }

  bool ReadByte(uint8_t* pDestByte) override
  {
    if (m_errorState)
      return false;

    if (rfread(pDestByte, 1, 1, m_pFile) != 1)
    {
      m_errorState = true;
      return false;
    }

    return true;
  }

  uint32_t Read(void* pDestination, uint32_t ByteCount) override
  {
    if (m_errorState)
      return 0;

    uint32_t readCount = (uint32_t)rfread(pDestination, 1, ByteCount, m_pFile);
    if (readCount != ByteCount && filestream_error(m_pFile) != 0)
      m_errorState = true;

    return readCount;
  }

  bool Read2(void* pDestination, uint32_t ByteCount, uint32_t* pNumberOfBytesRead) override
  {
    if (m_errorState)
      return false;

    uint32_t bytesRead = Read(pDestination, ByteCount);

    if (pNumberOfBytesRead != nullptr)
      *pNumberOfBytesRead = bytesRead;

    if (bytesRead != ByteCount)
    {
      m_errorState = true;
      return false;
    }

    return true;
  }

  bool WriteByte(uint8_t SourceByte) override
  {
    if (m_errorState)
      return false;

    if (rfwrite(&SourceByte, 1, 1, m_pFile) != 1)
    {
      m_errorState = true;
      return false;
    }

    return true;
  }

  uint32_t Write(const void* pSource, uint32_t ByteCount) override
  {
    if (m_errorState)
      return 0;

    uint32_t writeCount = (uint32_t)rfwrite(pSource, 1, ByteCount, m_pFile);
    if (writeCount != ByteCount)
      m_errorState = true;

    return writeCount;
  }

  bool Write2(const void* pSource, uint32_t ByteCount, uint32_t* pNumberOfBytesWritten) override
  {
    if (m_errorState)
      return false;

    uint32_t bytesWritten = Write(pSource, ByteCount);

    if (pNumberOfBytesWritten != nullptr)
      *pNumberOfBytesWritten = bytesWritten;

    if (bytesWritten != ByteCount)
    {
      m_errorState = true;
      return false;
    }

    return true;
  }

  bool SeekAbsolute(uint64_t Offset) override
  {
    if (m_errorState)
      return false;

    if (rfseek(m_pFile, static_cast<int64_t>(Offset), SEEK_SET) != 0)
    {
      m_errorState = true;
      return false;
    }

    return true;
  }

  uint64_t GetPosition() const override { return static_cast<uint64_t>(rftell(m_pFile)); }

  uint64_t GetSize() const override
  {
    int64_t OldPos = rftell(m_pFile);
    rfseek(m_pFile, 0, SEEK_END);
    int64_t Size = rftell(m_pFile);
    rfseek(m_pFile, OldPos, SEEK_SET);
    return (uint64_t)Size;
  }

  bool Flush() override
  {
    if (m_errorState)
      return false;

    // Closed (post-commit) streams have nothing left to flush.
    if (!m_pFile)
      return true;

    if (filestream_flush(m_pFile) != 0)
    {
      m_errorState = true;
      return false;
    }

    return true;
  }

  virtual bool Commit() override { return true; }

  virtual bool Discard() override { return false; }

protected:
  /// Close the underlying handle early (idempotent).  Windows cannot
  /// rename or delete a file that is held open by a handle without
  /// FILE_SHARE_DELETE - which is what the VFS-opened handle is - so
  /// the atomic-update commit path must close before renaming.  The
  /// rf* wrappers are null-safe, so subsequent stream calls degrade
  /// to errors rather than crashes.
  void CloseFile()
  {
    rfclose(m_pFile);
    m_pFile = nullptr;
  }

  RFILE* m_pFile;
};

class AtomicUpdatedFileByteStream final : public FileByteStream
{
public:
  AtomicUpdatedFileByteStream(RFILE* pFile, std::string originalFileName, std::string temporaryFileName)
    : FileByteStream(pFile), m_committed(false), m_discarded(false), m_originalFileName(std::move(originalFileName)),
      m_temporaryFileName(std::move(temporaryFileName))
  {
  }

  ~AtomicUpdatedFileByteStream() override
  {
    if (m_discarded)
    {
      // The handle must be closed before the temporary file can be
      // deleted on Windows (no FILE_SHARE_DELETE on VFS handles).
      CloseFile();
      filestream_delete(m_temporaryFileName.c_str());
    }
    else if (!m_committed)
    {
      Commit();
    }

    // rfclose called by FileByteStream destructor (null-safe if
    // Commit already closed the handle)
  }

  bool Commit() override
  {
    if (m_committed)
      return true;

    // The pre-VFS Windows path opened the temporary with
    // FILE_SHARE_DELETE and renamed it over the original while still
    // open (MoveFileExW).  The VFS handle is a plain _wfopen without
    // that sharing mode, so renaming - or deleting - the temporary
    // while it is open fails with a sharing violation, leaving the
    // new save stranded in .tmp and the original untouched.  Flush
    // and close first, then rename through the VFS (which also
    // routes correctly for frontend-backed paths).
    filestream_flush(m_pFile);
    CloseFile();

    if (filestream_rename(m_temporaryFileName.c_str(), m_originalFileName.c_str()) != 0)
    {
      // A VFS backend without replace-existing rename semantics
      // (e.g. a frontend whose Windows rename is plain _wrename)
      // fails when the destination exists: fall back to
      // delete-then-rename.  Not atomic, but strictly better than
      // losing the new data; the atomic single-rename path is
      // preserved wherever the backend supports it.
      filestream_delete(m_originalFileName.c_str());
      if (filestream_rename(m_temporaryFileName.c_str(), m_originalFileName.c_str()) != 0)
      {
        m_discarded = true;
        filestream_delete(m_temporaryFileName.c_str());
        return false;
      }
    }

    m_committed = true;
    return true;
  }

  bool Discard() override
  {
    m_discarded = true;
    return true;
  }

private:
  bool m_committed;
  bool m_discarded;
  std::string m_originalFileName;
  std::string m_temporaryFileName;
};

MemoryByteStream::MemoryByteStream(void* pMemory, uint32_t MemSize)
{
  m_iPosition = 0;
  m_iSize = MemSize;
  m_pMemory = (uint8_t*)pMemory;
}

MemoryByteStream::~MemoryByteStream() {}

bool MemoryByteStream::ReadByte(uint8_t* pDestByte)
{
  if (m_iPosition < m_iSize)
  {
    *pDestByte = m_pMemory[m_iPosition++];
    return true;
  }

  return false;
}

uint32_t MemoryByteStream::Read(void* pDestination, uint32_t ByteCount)
{
  uint32_t sz = ByteCount;
  if ((m_iPosition + ByteCount) > m_iSize)
    sz = m_iSize - m_iPosition;

  if (sz > 0)
  {
    std::memcpy(pDestination, m_pMemory + m_iPosition, sz);
    m_iPosition += sz;
  }

  return sz;
}

bool MemoryByteStream::Read2(void* pDestination, uint32_t ByteCount, uint32_t* pNumberOfBytesRead /* = nullptr */)
{
  uint32_t r = Read(pDestination, ByteCount);
  if (pNumberOfBytesRead != NULL)
    *pNumberOfBytesRead = r;

  return (r == ByteCount);
}

bool MemoryByteStream::WriteByte(uint8_t SourceByte)
{
  if (m_iPosition < m_iSize)
  {
    m_pMemory[m_iPosition++] = SourceByte;
    return true;
  }

  return false;
}

uint32_t MemoryByteStream::Write(const void* pSource, uint32_t ByteCount)
{
  uint32_t sz = ByteCount;
  if ((m_iPosition + ByteCount) > m_iSize)
    sz = m_iSize - m_iPosition;

  if (sz > 0)
  {
    std::memcpy(m_pMemory + m_iPosition, pSource, sz);
    m_iPosition += sz;
  }

  return sz;
}

bool MemoryByteStream::Write2(const void* pSource, uint32_t ByteCount, uint32_t* pNumberOfBytesWritten /* = nullptr */)
{
  uint32_t r = Write(pSource, ByteCount);
  if (pNumberOfBytesWritten != nullptr)
    *pNumberOfBytesWritten = r;

  return (r == ByteCount);
}

bool MemoryByteStream::SeekAbsolute(uint64_t Offset)
{
  uint32_t Offset32 = (uint32_t)Offset;
  if (Offset32 > m_iSize)
    return false;

  m_iPosition = Offset32;
  return true;
}

uint64_t MemoryByteStream::GetSize() const
{
  return (uint64_t)m_iSize;
}

uint64_t MemoryByteStream::GetPosition() const
{
  return (uint64_t)m_iPosition;
}

bool MemoryByteStream::Flush()
{
  return true;
}

bool MemoryByteStream::Commit()
{
  return true;
}

bool MemoryByteStream::Discard()
{
  return false;
}

ReadOnlyMemoryByteStream::ReadOnlyMemoryByteStream(const void* pMemory, uint32_t MemSize)
{
  m_iPosition = 0;
  m_iSize = MemSize;
  m_pMemory = reinterpret_cast<const uint8_t*>(pMemory);
}

ReadOnlyMemoryByteStream::~ReadOnlyMemoryByteStream() {}

bool ReadOnlyMemoryByteStream::ReadByte(uint8_t* pDestByte)
{
  if (m_iPosition < m_iSize)
  {
    *pDestByte = m_pMemory[m_iPosition++];
    return true;
  }

  return false;
}

uint32_t ReadOnlyMemoryByteStream::Read(void* pDestination, uint32_t ByteCount)
{
  uint32_t sz = ByteCount;
  if ((m_iPosition + ByteCount) > m_iSize)
    sz = m_iSize - m_iPosition;

  if (sz > 0)
  {
    std::memcpy(pDestination, m_pMemory + m_iPosition, sz);
    m_iPosition += sz;
  }

  return sz;
}

bool ReadOnlyMemoryByteStream::Read2(void* pDestination, uint32_t ByteCount, uint32_t* pNumberOfBytesRead /* = nullptr */)
{
  uint32_t r = Read(pDestination, ByteCount);
  if (pNumberOfBytesRead != nullptr)
    *pNumberOfBytesRead = r;

  return (r == ByteCount);
}

bool ReadOnlyMemoryByteStream::WriteByte(uint8_t SourceByte)
{
  return false;
}

uint32_t ReadOnlyMemoryByteStream::Write(const void* pSource, uint32_t ByteCount)
{
  return 0;
}

bool ReadOnlyMemoryByteStream::Write2(const void* pSource, uint32_t ByteCount, uint32_t* pNumberOfBytesWritten /* = nullptr */)
{
  return false;
}

bool ReadOnlyMemoryByteStream::SeekAbsolute(uint64_t Offset)
{
  uint32_t Offset32 = (uint32_t)Offset;
  if (Offset32 > m_iSize)
    return false;

  m_iPosition = Offset32;
  return true;
}

uint64_t ReadOnlyMemoryByteStream::GetSize() const
{
  return (uint64_t)m_iSize;
}

uint64_t ReadOnlyMemoryByteStream::GetPosition() const
{
  return (uint64_t)m_iPosition;
}

bool ReadOnlyMemoryByteStream::Flush()
{
  return false;
}

bool ReadOnlyMemoryByteStream::Commit()
{
  return false;
}

bool ReadOnlyMemoryByteStream::Discard()
{
  return false;
}

GrowableMemoryByteStream::GrowableMemoryByteStream(void* pInitialMem, uint32_t InitialMemSize)
{
  m_iPosition = 0;
  m_iSize = 0;

  if (pInitialMem != nullptr)
  {
    m_iMemorySize = InitialMemSize;
    m_pPrivateMemory = nullptr;
    m_pMemory = (uint8_t*)pInitialMem;
  }
  else
  {
    m_iMemorySize = std::max(InitialMemSize, (uint32_t)64);
    m_pPrivateMemory = m_pMemory = (uint8_t*)std::malloc(m_iMemorySize);
    if (m_pMemory == nullptr)
    {
      m_iMemorySize = 0;
      SetErrorState();
    }
  }
}

GrowableMemoryByteStream::~GrowableMemoryByteStream()
{
  if (m_pPrivateMemory != nullptr)
    std::free(m_pPrivateMemory);
}

void GrowableMemoryByteStream::Resize(uint32_t new_size)
{
  if (new_size > m_iMemorySize)
    ResizeMemory(new_size);

  m_iSize = new_size;
}

void GrowableMemoryByteStream::ResizeMemory(uint32_t new_size)
{
  if (new_size == m_iMemorySize)
    return;

  if (m_pPrivateMemory == nullptr)
  {
    uint8_t* new_memory = (uint8_t*)std::malloc(new_size);
    if (new_memory == nullptr)
    {
      SetErrorState();
      return;
    }

    std::memcpy(new_memory, m_pMemory, m_iSize);
    m_pPrivateMemory = new_memory;
    m_pMemory = m_pPrivateMemory;
    m_iMemorySize = new_size;
  }
  else
  {
    uint8_t* new_memory = (uint8_t*)std::realloc(m_pPrivateMemory, new_size);
    if (new_memory == nullptr)
    {
      // original buffer is still valid and owned by m_pPrivateMemory
      SetErrorState();
      return;
    }

    m_pPrivateMemory = m_pMemory = new_memory;
    m_iMemorySize = new_size;
  }
}

bool GrowableMemoryByteStream::ReadByte(uint8_t* pDestByte)
{
  if (m_iPosition < m_iSize)
  {
    *pDestByte = m_pMemory[m_iPosition++];
    return true;
  }

  return false;
}

uint32_t GrowableMemoryByteStream::Read(void* pDestination, uint32_t ByteCount)
{
  uint32_t sz = ByteCount;
  if ((m_iPosition + ByteCount) > m_iSize)
    sz = m_iSize - m_iPosition;

  if (sz > 0)
  {
    std::memcpy(pDestination, m_pMemory + m_iPosition, sz);
    m_iPosition += sz;
  }

  return sz;
}

bool GrowableMemoryByteStream::Read2(void* pDestination, uint32_t ByteCount, uint32_t* pNumberOfBytesRead /* = nullptr */)
{
  uint32_t r = Read(pDestination, ByteCount);
  if (pNumberOfBytesRead != NULL)
    *pNumberOfBytesRead = r;

  return (r == ByteCount);
}

bool GrowableMemoryByteStream::WriteByte(uint8_t SourceByte)
{
  if (m_iPosition == m_iMemorySize)
    Grow(1);

  if (m_errorState || m_pMemory == nullptr)
    return false;

  m_pMemory[m_iPosition++] = SourceByte;
  m_iSize = std::max(m_iSize, m_iPosition);
  return true;
}

uint32_t GrowableMemoryByteStream::Write(const void* pSource, uint32_t ByteCount)
{
  if ((m_iPosition + ByteCount) > m_iMemorySize)
    Grow(ByteCount);

  if (m_errorState || m_pMemory == nullptr)
    return 0;

  std::memcpy(m_pMemory + m_iPosition, pSource, ByteCount);
  m_iPosition += ByteCount;
  m_iSize = std::max(m_iSize, m_iPosition);
  return ByteCount;
}

bool GrowableMemoryByteStream::Write2(const void* pSource, uint32_t ByteCount, uint32_t* pNumberOfBytesWritten /* = nullptr */)
{
  uint32_t r = Write(pSource, ByteCount);
  if (pNumberOfBytesWritten != nullptr)
    *pNumberOfBytesWritten = r;

  return (r == ByteCount);
}

bool GrowableMemoryByteStream::SeekAbsolute(uint64_t Offset)
{
  uint32_t Offset32 = (uint32_t)Offset;
  if (Offset32 > m_iSize)
    return false;

  m_iPosition = Offset32;
  return true;
}

uint64_t GrowableMemoryByteStream::GetSize() const
{
  return (uint64_t)m_iSize;
}

uint64_t GrowableMemoryByteStream::GetPosition() const
{
  return (uint64_t)m_iPosition;
}

bool GrowableMemoryByteStream::Flush()
{
  return true;
}

bool GrowableMemoryByteStream::Commit()
{
  return true;
}

bool GrowableMemoryByteStream::Discard()
{
  return false;
}

void GrowableMemoryByteStream::Grow(uint32_t MinimumGrowth)
{
  uint32_t NewSize = std::max(m_iMemorySize + MinimumGrowth, m_iMemorySize * 2);
  ResizeMemory(NewSize);
}

std::unique_ptr<ByteStream> ByteStream_OpenFileStream(const char* fileName, uint32_t openMode)
{
  if ((openMode & (BYTESTREAM_OPEN_CREATE | BYTESTREAM_OPEN_WRITE)) == BYTESTREAM_OPEN_WRITE)
  {
    // if opening with write but not create, the path must exist.
    if (!path_is_valid(fileName))
      return nullptr;
  }

  char modeString[16];
  uint32_t modeStringLength = 0;

  if (openMode & BYTESTREAM_OPEN_WRITE)
  {
    if (openMode & BYTESTREAM_OPEN_TRUNCATE)
      modeString[modeStringLength++] = 'w';
    else
      modeString[modeStringLength++] = 'a';

    modeString[modeStringLength++] = 'b';

    if (openMode & BYTESTREAM_OPEN_READ)
      modeString[modeStringLength++] = '+';
  }
  else if (openMode & BYTESTREAM_OPEN_READ)
  {
    modeString[modeStringLength++] = 'r';
    modeString[modeStringLength++] = 'b';
  }

  modeString[modeStringLength] = 0;

  if (openMode & BYTESTREAM_OPEN_ATOMIC_UPDATE)
  {
    // generate the temporary file name
    const uint32_t fileNameLength = static_cast<uint32_t>(std::strlen(fileName));
    char* temporaryFileName = (char*)alloca(fileNameLength + 8);
    std::snprintf(temporaryFileName, fileNameLength + 8, "%s.tmp", fileName);

    // open the temporary file through libretro VFS.  The temporary
    // must always be created fresh (pre-VFS this was CREATE_NEW):
    // passing the caller's append mode through maps to VFS
    // UPDATE_EXISTING, which fails when the tmp does not exist yet -
    // and would be wrong anyway, since the copy-in loop below is what
    // provides the original content for non-truncating opens.
    RFILE* pTemporaryFile = rfopen(temporaryFileName, (openMode & BYTESTREAM_OPEN_READ) ? "w+b" : "wb");
    if (!pTemporaryFile)
      return nullptr;

    // create the stream pointer
    std::unique_ptr<AtomicUpdatedFileByteStream> pStream =
      std::make_unique<AtomicUpdatedFileByteStream>(pTemporaryFile, fileName, temporaryFileName);

    // do we need to copy the existing file into this one?
    if (!(openMode & BYTESTREAM_OPEN_TRUNCATE))
    {
      RFILE* pOriginalFile = rfopen(fileName, "rb");
      if (!pOriginalFile)
      {
        // this will delete the temporary file
        pStream->Discard();
        return nullptr;
      }

      static const size_t BUFFERSIZE = 4096;
      uint8_t buffer[BUFFERSIZE];
      while (!rfeof(pOriginalFile))
      {
        int64_t nBytes = rfread(buffer, sizeof(uint8_t), BUFFERSIZE, pOriginalFile);
        if (nBytes <= 0)
          break;

        if (pStream->Write(buffer, (uint32_t)nBytes) != (uint32_t)nBytes)
        {
          pStream->Discard();
          rfclose(pOriginalFile);
          return nullptr;
        }
      }

      // close original file
      rfclose(pOriginalFile);
    }

    // return pointer
    return pStream;
  }
  else
  {
    RFILE* pFile = rfopen(fileName, modeString);
    if (!pFile)
      return nullptr;

    return std::make_unique<FileByteStream>(pFile);
  }
}

std::unique_ptr<MemoryByteStream> ByteStream_CreateMemoryStream(void* pMemory, uint32_t Size)
{
  return std::make_unique<MemoryByteStream>(pMemory, Size);
}

std::unique_ptr<ReadOnlyMemoryByteStream> ByteStream_CreateReadOnlyMemoryStream(const void* pMemory, uint32_t Size)
{
  return std::make_unique<ReadOnlyMemoryByteStream>(pMemory, Size);
}

std::unique_ptr<GrowableMemoryByteStream> ByteStream_CreateGrowableMemoryStream(void* pInitialMemory, uint32_t InitialSize)
{
  return std::make_unique<GrowableMemoryByteStream>(pInitialMemory, InitialSize);
}

std::unique_ptr<GrowableMemoryByteStream> ByteStream_CreateGrowableMemoryStream()
{
  return std::make_unique<GrowableMemoryByteStream>(nullptr, 0);
}
