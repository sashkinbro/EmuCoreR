#include "stream_buffer.h"
#include "../align.h"
#include <array>
#include <cstdio>

namespace GL {

StreamBuffer::StreamBuffer(GLenum target, GLuint buffer_id, uint32_t size)
  : m_target(target), m_buffer_id(buffer_id), m_size(size)
{
}

StreamBuffer::~StreamBuffer()
{
  glDeleteBuffers(1, &m_buffer_id);
}

void StreamBuffer::Bind()
{
  glBindBuffer(m_target, m_buffer_id);
}

void StreamBuffer::Unbind()
{
  glBindBuffer(m_target, 0);
}

namespace detail {

// Uses glBufferSubData() to update. Preferred for drivers which don't support {ARB,EXT}_buffer_storage.
class BufferSubDataStreamBuffer final : public StreamBuffer
{
public:
  ~BufferSubDataStreamBuffer() override = default;

  MappingResult Map(uint32_t alignment, uint32_t min_size) override
  {
    return MappingResult{static_cast<void*>(m_cpu_buffer.data()), 0, 0, m_size / alignment};
  }

  void Unmap(uint32_t used_size) override
  {
    if (used_size == 0)
      return;

    glBindBuffer(m_target, m_buffer_id);
    glBufferSubData(m_target, 0, used_size, m_cpu_buffer.data());
  }

  static std::unique_ptr<StreamBuffer> Create(GLenum target, uint32_t size)
  {
    glGetError();

    GLuint buffer_id;
    glGenBuffers(1, &buffer_id);
    glBindBuffer(target, buffer_id);
    glBufferData(target, size, nullptr, GL_STREAM_DRAW);

    GLenum err = glGetError();
    if (err != GL_NO_ERROR)
    {
      glDeleteBuffers(1, &buffer_id);
      return {};
    }

    return std::unique_ptr<StreamBuffer>(new BufferSubDataStreamBuffer(target, buffer_id, size));
  }

private:
  BufferSubDataStreamBuffer(GLenum target, GLuint buffer_id, uint32_t size)
    : StreamBuffer(target, buffer_id, size), m_cpu_buffer(size)
  {
  }

  std::vector<uint8_t> m_cpu_buffer;
};

// Uses BufferData() to orphan the buffer after every update. Used on Mali where BufferSubData forces a sync.
class BufferDataStreamBuffer final : public StreamBuffer
{
public:
  ~BufferDataStreamBuffer() override = default;

  MappingResult Map(uint32_t alignment, uint32_t min_size) override
  {
    return MappingResult{static_cast<void*>(m_cpu_buffer.data()), 0, 0, m_size / alignment};
  }

  void Unmap(uint32_t used_size) override
  {
    if (used_size == 0)
      return;

    glBindBuffer(m_target, m_buffer_id);
    glBufferData(m_target, used_size, m_cpu_buffer.data(), GL_STREAM_DRAW);
  }

  static std::unique_ptr<StreamBuffer> Create(GLenum target, uint32_t size)
  {
    glGetError();

    GLuint buffer_id;
    glGenBuffers(1, &buffer_id);
    glBindBuffer(target, buffer_id);
    glBufferData(target, size, nullptr, GL_STREAM_DRAW);

    GLenum err = glGetError();
    if (err != GL_NO_ERROR)
    {
      glDeleteBuffers(1, &buffer_id);
      return {};
    }

    return std::unique_ptr<StreamBuffer>(new BufferDataStreamBuffer(target, buffer_id, size));
  }

private:
  BufferDataStreamBuffer(GLenum target, GLuint buffer_id, uint32_t size)
    : StreamBuffer(target, buffer_id, size), m_cpu_buffer(size)
  {
  }

  std::vector<uint8_t> m_cpu_buffer;
};

// Base class for implementations which require syncing.
class SyncingStreamBuffer : public StreamBuffer
{
public:
  enum : uint32_t
  {
    NUM_SYNC_POINTS = 16
  };

  virtual ~SyncingStreamBuffer() override
  {
    for (uint32_t i = m_available_block_index; i <= m_used_block_index; i++)
      glDeleteSync(m_sync_objects[i]);
  }

protected:
  SyncingStreamBuffer(GLenum target, GLuint buffer_id, uint32_t size)
    : StreamBuffer(target, buffer_id, size), m_bytes_per_block((size + (NUM_SYNC_POINTS)-1) / NUM_SYNC_POINTS)
  {
  }

  uint32_t GetSyncIndexForOffset(uint32_t offset) { return offset / m_bytes_per_block; }

  void AddSyncsForOffset(uint32_t offset)
  {
    const uint32_t end = GetSyncIndexForOffset(offset);
    for (; m_used_block_index < end; m_used_block_index++)
      m_sync_objects[m_used_block_index] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
  }

  void WaitForSync(GLsync& sync)
  {
    glClientWaitSync(sync, GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
    glDeleteSync(sync);
    sync = nullptr;
  }

  void EnsureSyncsWaitedForOffset(uint32_t offset)
  {
    const uint32_t end = std::min<uint32_t>(GetSyncIndexForOffset(offset) + 1, NUM_SYNC_POINTS);
    for (; m_available_block_index < end; m_available_block_index++)
      WaitForSync(m_sync_objects[m_available_block_index]);
  }

  void AllocateSpace(uint32_t size)
  {
    // add sync objects for writes since the last allocation
    AddSyncsForOffset(m_position);

    // wait for sync objects for the space we want to use
    EnsureSyncsWaitedForOffset(m_position + size);

    // wrap-around?
    if ((m_position + size) > m_size)
    {
      // current position ... buffer end
      AddSyncsForOffset(m_size);

      // rewind, and try again
      m_position = 0;

      // wait for the sync at the start of the buffer
      WaitForSync(m_sync_objects[0]);
      m_available_block_index = 1;

      // and however much more we need to satisfy the allocation
      EnsureSyncsWaitedForOffset(size);
      m_used_block_index = 0;
    }
  }

  uint32_t m_position = 0;
  uint32_t m_used_block_index = 0;
  uint32_t m_available_block_index = NUM_SYNC_POINTS;
  uint32_t m_bytes_per_block;
  std::array<GLsync, NUM_SYNC_POINTS> m_sync_objects{};
};

class BufferStorageStreamBuffer : public SyncingStreamBuffer
{
public:
  ~BufferStorageStreamBuffer() override
  {
    glBindBuffer(m_target, m_buffer_id);
    glUnmapBuffer(m_target);
  }

  MappingResult Map(uint32_t alignment, uint32_t min_size) override
  {
    if (m_position > 0)
      m_position = Common::AlignUp(m_position, alignment);

    AllocateSpace(min_size);

    const uint32_t free_space_in_block = ((m_available_block_index * m_bytes_per_block) - m_position);
    return MappingResult{static_cast<void*>(m_mapped_ptr + m_position), m_position, m_position / alignment,
                         free_space_in_block / alignment};
  }

  void Unmap(uint32_t used_size) override
  {
    if (!m_coherent)
    {
      Bind();
      glFlushMappedBufferRange(m_target, m_position, used_size);
    }

    m_position += used_size;
  }

  static std::unique_ptr<StreamBuffer> Create(GLenum target, uint32_t size, bool coherent = true)
  {
    glGetError();

    GLuint buffer_id;
    glGenBuffers(1, &buffer_id);
    glBindBuffer(target, buffer_id);

    const uint32_t flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | (coherent ? GL_MAP_COHERENT_BIT : 0);
    const uint32_t map_flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | (coherent ? 0 : GL_MAP_FLUSH_EXPLICIT_BIT);
    if (GLAD_GL_VERSION_4_4 || GLAD_GL_ARB_buffer_storage)
      glBufferStorage(target, size, nullptr, flags);
    else if (GLAD_GL_EXT_buffer_storage)
      glBufferStorageEXT(target, size, nullptr, flags);

    GLenum err = glGetError();
    if (err != GL_NO_ERROR)
    {
      glDeleteBuffers(1, &buffer_id);
      return {};
    }

    uint8_t* mapped_ptr = static_cast<uint8_t*>(glMapBufferRange(target, 0, size, map_flags));
    return std::unique_ptr<StreamBuffer>(new BufferStorageStreamBuffer(target, buffer_id, size, mapped_ptr, coherent));
  }

private:
  BufferStorageStreamBuffer(GLenum target, GLuint buffer_id, uint32_t size, uint8_t* mapped_ptr, bool coherent)
    : SyncingStreamBuffer(target, buffer_id, size), m_mapped_ptr(mapped_ptr), m_coherent(coherent)
  {
  }

  uint8_t* m_mapped_ptr;
  bool m_coherent;
};

} // namespace detail

std::unique_ptr<StreamBuffer> StreamBuffer::Create(GLenum target, uint32_t size)
{
  std::unique_ptr<StreamBuffer> buf;
  if (GLAD_GL_VERSION_4_4 || GLAD_GL_ARB_buffer_storage || GLAD_GL_EXT_buffer_storage)
  {
    buf = detail::BufferStorageStreamBuffer::Create(target, size);
    if (buf)
      return buf;
  }

  return detail::BufferDataStreamBuffer::Create(target, size);
}

} // namespace GL
