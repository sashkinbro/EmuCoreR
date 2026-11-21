#pragma once
#include "../types.h"
#include <glad.h>

namespace GL {
class Texture
{
public:
  Texture();
  Texture(Texture&& moved);
  ~Texture();

  bool Create(uint32_t width, uint32_t height, uint32_t samples, GLenum internal_format, GLenum format, GLenum type,
              const void* data = nullptr, bool linear_filter = false, bool wrap = false);
  void Replace(uint32_t width, uint32_t height, GLenum internal_format, GLenum format, GLenum type, const void* data);
  bool CreateFramebuffer();

  void Destroy();

  bool IsValid() const { return m_id != 0; }
  bool IsMultisampled() const { return m_samples > 1; }
  GLuint GetGLId() const { return m_id; }
  uint32_t GetWidth() const { return m_width; }
  uint32_t GetHeight() const { return m_height; }
  uint32_t GetSamples() const { return m_samples; }

  GLenum GetGLTarget() const { return IsMultisampled() ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D; }

  void Bind();
  void BindFramebuffer(GLenum target = GL_DRAW_FRAMEBUFFER);

  static void Unbind();

  Texture& operator=(const Texture& copy) = delete;
  Texture& operator=(Texture&& moved);

private:
  GLuint m_id = 0;
  uint32_t m_width = 0;
  uint32_t m_height = 0;
  uint32_t m_samples = 0;

  GLuint m_fbo_id = 0;
};

} // namespace GL
