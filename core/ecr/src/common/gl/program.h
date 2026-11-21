#pragma once
#include "../types.h"
#include "glad.h"
#include <string_view>
#include <vector>

namespace GL {
class Program
{
public:
  Program();
  Program(const Program&) = delete;
  Program(Program&& prog);
  ~Program();

  static GLuint CompileShader(GLenum type, const std::string_view source);
  static void ResetLastProgram();

  bool Compile(const std::string_view vertex_shader, const std::string_view geometry_shader,
               const std::string_view fragment_shader);

  bool CreateFromBinary(const void* data, uint32_t data_length, uint32_t data_format);

  bool GetBinary(std::vector<uint8_t>* out_data, uint32_t* out_data_format);
  void SetBinaryRetrievableHint();

  void BindAttribute(GLuint index, const char* name);

  void BindFragData(GLuint index = 0, const char* name = "o_col0");
  void BindFragDataIndexed(GLuint color_number = 0, const char* name = "o_col0");

  bool Link();

  void Bind() const;

  // Returns true if the program has a non-zero linked GL handle.
  // Used by the lazy-compile path in GPU_HW_OpenGL to distinguish a
  // never-touched matrix slot (program id 0) from one that has
  // already been compiled and is ready for Bind().
  bool IsValid() const { return m_program_id != 0; }

  void Destroy();

  int RegisterUniform(const char* name);
  void Uniform1i(int index, int32_t x) const;
  void Uniform4f(int index, float x, float y, float z, float w) const;

  void Uniform1i(const char* name, int32_t x) const;

  void BindUniformBlock(const char* name, uint32_t index);

  Program& operator=(const Program&) = delete;
  Program& operator=(Program&& prog);

private:
  static uint32_t s_last_program_id;

  GLuint m_program_id = 0;
  GLuint m_vertex_shader_id = 0;
  GLuint m_fragment_shader_id = 0;

  std::vector<GLint> m_uniform_locations;
};

} // namespace GL
