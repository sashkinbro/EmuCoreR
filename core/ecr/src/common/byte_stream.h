#pragma once
#include "types.h"
#include <memory>

// base byte stream creation functions
inline constexpr uint32_t BYTESTREAM_OPEN_READ = 1,
  BYTESTREAM_OPEN_WRITE = 2,
  BYTESTREAM_OPEN_TRUNCATE = 8,
  BYTESTREAM_OPEN_CREATE = 16,
  BYTESTREAM_OPEN_ATOMIC_UPDATE = 64,
  BYTESTREAM_OPEN_STREAMED = 256;

// interface class used by readers, writers, etc.
class ByteStream
{
public:
  virtual ~ByteStream() {}

  // reads a single byte from the stream.
  virtual bool ReadByte(uint8_t* pDestByte) = 0;

  // read bytes from this stream. returns the number of bytes read, if this isn't equal to the requested size, an error
  // or EOF occurred.
  virtual uint32_t Read(void* pDestination, uint32_t ByteCount) = 0;

  // read bytes from this stream, optionally returning the number of bytes read.
  virtual bool Read2(void* pDestination, uint32_t ByteCount, uint32_t* pNumberOfBytesRead = nullptr) = 0;

  // writes a single byte to the stream.
  virtual bool WriteByte(uint8_t SourceByte) = 0;

  // write bytes to this stream, returns the number of bytes written. if this isn't equal to the requested size, a
  // buffer overflow, or write error occurred.
  virtual uint32_t Write(const void* pSource, uint32_t ByteCount) = 0;

  // write bytes to this stream, optionally returning the number of bytes written.
  virtual bool Write2(const void* pSource, uint32_t ByteCount, uint32_t* pNumberOfBytesWritten = nullptr) = 0;

  // seeks to the specified position in the stream
  // if seek failed, returns false.
  virtual bool SeekAbsolute(uint64_t Offset) = 0;

  // gets the current offset in the stream
  virtual uint64_t GetPosition() const = 0;

  // gets the size of the stream
  virtual uint64_t GetSize() const = 0;

  // flush any changes to the stream to disk
  virtual bool Flush() = 0;

  // if the file was opened in atomic update mode, discards any changes made to the file
  virtual bool Discard() = 0;

  // if the file was opened in atomic update mode, commits the file and replaces the temporary file
  virtual bool Commit() = 0;

  // state accessors
  inline void SetErrorState() { m_errorState = true; }

protected:
  ByteStream() : m_errorState(false) {}

  // state bits
  bool m_errorState;

  // make it noncopyable
  ByteStream(const ByteStream&) = delete;
  ByteStream& operator=(const ByteStream&) = delete;
};

class MemoryByteStream final : public ByteStream
{
public:
  MemoryByteStream(void* pMemory, uint32_t MemSize);
  ~MemoryByteStream() override;

  bool ReadByte(uint8_t* pDestByte) override;
  uint32_t Read(void* pDestination, uint32_t ByteCount) override;
  bool Read2(void* pDestination, uint32_t ByteCount, uint32_t* pNumberOfBytesRead) override;
  bool WriteByte(uint8_t SourceByte) override;
  uint32_t Write(const void* pSource, uint32_t ByteCount) override;
  bool Write2(const void* pSource, uint32_t ByteCount, uint32_t* pNumberOfBytesWritten) override;
  bool SeekAbsolute(uint64_t Offset) override;
  uint64_t GetSize() const override;
  uint64_t GetPosition() const override;
  bool Flush() override;
  bool Commit() override;
  bool Discard() override;

private:
  uint8_t* m_pMemory;
  uint32_t m_iPosition;
  uint32_t m_iSize;
};

class ReadOnlyMemoryByteStream final : public ByteStream
{
public:
  ReadOnlyMemoryByteStream(const void* pMemory, uint32_t MemSize);
  ~ReadOnlyMemoryByteStream() override;

  bool ReadByte(uint8_t* pDestByte) override;
  uint32_t Read(void* pDestination, uint32_t ByteCount) override;
  bool Read2(void* pDestination, uint32_t ByteCount, uint32_t* pNumberOfBytesRead) override;
  bool WriteByte(uint8_t SourceByte) override;
  uint32_t Write(const void* pSource, uint32_t ByteCount) override;
  bool Write2(const void* pSource, uint32_t ByteCount, uint32_t* pNumberOfBytesWritten) override;
  bool SeekAbsolute(uint64_t Offset) override;
  uint64_t GetSize() const override;
  uint64_t GetPosition() const override;
  bool Flush() override;
  bool Commit() override;
  bool Discard() override;

private:
  const uint8_t* m_pMemory;
  uint32_t m_iPosition;
  uint32_t m_iSize;
};

class GrowableMemoryByteStream final : public ByteStream
{
public:
  GrowableMemoryByteStream(void* pInitialMem, uint32_t InitialMemSize);
  ~GrowableMemoryByteStream() override;

  void Resize(uint32_t new_size);
  void ResizeMemory(uint32_t new_size);

  bool ReadByte(uint8_t* pDestByte) override;
  uint32_t Read(void* pDestination, uint32_t ByteCount) override;
  bool Read2(void* pDestination, uint32_t ByteCount, uint32_t* pNumberOfBytesRead) override;
  bool WriteByte(uint8_t SourceByte) override;
  uint32_t Write(const void* pSource, uint32_t ByteCount) override;
  bool Write2(const void* pSource, uint32_t ByteCount, uint32_t* pNumberOfBytesWritten) override;
  bool SeekAbsolute(uint64_t Offset) override;
  uint64_t GetSize() const override;
  uint64_t GetPosition() const override;
  bool Flush() override;
  bool Commit() override;
  bool Discard() override;

private:
  void Grow(uint32_t MinimumGrowth);

  uint8_t* m_pPrivateMemory;
  uint8_t* m_pMemory;
  uint32_t m_iPosition;
  uint32_t m_iSize;
  uint32_t m_iMemorySize;
};

// base byte stream creation functions
// opens a local file-based stream. fills in error if passed, and returns false if the file cannot be opened.
std::unique_ptr<ByteStream> ByteStream_OpenFileStream(const char* FileName, uint32_t OpenMode);

// memory byte stream, caller is responsible for management, therefore it can be located on either the stack or on the
// heap.
std::unique_ptr<MemoryByteStream> ByteStream_CreateMemoryStream(void* pMemory, uint32_t Size);

// a growable memory byte stream will automatically allocate its own memory if the provided memory is overflowed.
// a "pure heap" buffer, i.e. a buffer completely managed by this implementation, can be created by supplying a NULL
// pointer and initialSize of zero.
std::unique_ptr<GrowableMemoryByteStream> ByteStream_CreateGrowableMemoryStream(void* pInitialMemory, uint32_t InitialSize);
std::unique_ptr<GrowableMemoryByteStream> ByteStream_CreateGrowableMemoryStream();

// readable memory stream
std::unique_ptr<ReadOnlyMemoryByteStream> ByteStream_CreateReadOnlyMemoryStream(const void* pMemory, uint32_t Size);
