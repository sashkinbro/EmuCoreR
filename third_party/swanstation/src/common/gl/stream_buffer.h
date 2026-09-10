#pragma once
#include "../types.h"
#include <glad.h>
#include <memory>
#include <tuple>
#include <vector>

namespace GL {
class StreamBuffer
{
public:
  virtual ~StreamBuffer();

  ALWAYS_INLINE GLuint GetGLBufferId() const { return m_buffer_id; }
  ALWAYS_INLINE GLenum GetGLTarget() const { return m_target; }
  ALWAYS_INLINE uint32_t GetSize() const { return m_size; }

  void Bind();
  void Unbind();

  struct MappingResult
  {
    void* pointer;
    uint32_t buffer_offset;
    uint32_t index_aligned; // offset / alignment, suitable for base vertex
    uint32_t space_aligned; // remaining space / alignment
  };

  virtual MappingResult Map(uint32_t alignment, uint32_t min_size) = 0;
  virtual void Unmap(uint32_t used_size) = 0;

  static std::unique_ptr<StreamBuffer> Create(GLenum target, uint32_t size);

protected:
  StreamBuffer(GLenum target, GLuint buffer_id, uint32_t size);

  GLenum m_target;
  GLuint m_buffer_id;
  uint32_t m_size;
};
} // namespace GL