#include "program.h"
#include "../byte_stream.h"
#include "../log.h"
#include "../string_util.h"
#include <array>
Log_SetChannel(GL);

namespace GL {

GLuint Program::s_last_program_id = 0;
static GLuint s_next_bad_shader_id = 1;

Program::Program() = default;

Program::Program(Program&& prog)
{
  m_program_id = prog.m_program_id;
  prog.m_program_id = 0;
  m_vertex_shader_id = prog.m_vertex_shader_id;
  prog.m_vertex_shader_id = 0;
  m_fragment_shader_id = prog.m_fragment_shader_id;
  prog.m_fragment_shader_id = 0;
  m_uniform_locations = std::move(prog.m_uniform_locations);
}

Program::~Program()
{
  Destroy();
}

GLuint Program::CompileShader(GLenum type, const std::string_view source)
{
  GLuint id = glCreateShader(type);

  std::array<const GLchar*, 1> sources = {{source.data()}};
  std::array<GLint, 1> source_lengths = {{static_cast<GLint>(source.size())}};
  glShaderSource(id, static_cast<GLsizei>(sources.size()), sources.data(), source_lengths.data());
  glCompileShader(id);

  GLint status = GL_FALSE;
  glGetShaderiv(id, GL_COMPILE_STATUS, &status);

  GLint info_log_length = 0;
  glGetShaderiv(id, GL_INFO_LOG_LENGTH, &info_log_length);

  if (status == GL_FALSE || info_log_length > 0)
  {
    std::string info_log;
    info_log.resize(info_log_length + 1);
    glGetShaderInfoLog(id, info_log_length, &info_log_length, &info_log[0]);

    if (status == GL_TRUE)
    {
      Log_ErrorPrintf("Shader compiled with warnings:\n%s", info_log.c_str());
    }
    else
    {
      Log_ErrorPrintf("Shader failed to compile:\n%s", info_log.c_str());

      std::unique_ptr<ByteStream> ofs = ByteStream_OpenFileStream(
        StringUtil::StdStringFromFormat("bad_shader_%u.txt", s_next_bad_shader_id++).c_str(),
        BYTESTREAM_OPEN_CREATE | BYTESTREAM_OPEN_WRITE | BYTESTREAM_OPEN_TRUNCATE);
      if (ofs)
      {
        ofs->Write(sources[0], static_cast<uint32_t>(source_lengths[0]));
        static const char tail[] = "\n\nCompile failed, info log:\n";
        ofs->Write(tail, sizeof(tail) - 1);
        ofs->Write(info_log.c_str(), static_cast<uint32_t>(info_log.size()));
        ofs->Commit();
      }

      glDeleteShader(id);
      return 0;
    }
  }

  return id;
}

void Program::ResetLastProgram()
{
  s_last_program_id = 0;
}

bool Program::Compile(const std::string_view vertex_shader, const std::string_view geometry_shader,
                      const std::string_view fragment_shader)
{
  GLuint vertex_shader_id = 0;
  if (!vertex_shader.empty())
  {
    vertex_shader_id = CompileShader(GL_VERTEX_SHADER, vertex_shader);
    if (vertex_shader_id == 0)
      return false;
  }

  GLuint geometry_shader_id = 0;
  if (!geometry_shader.empty())
  {
    geometry_shader_id = CompileShader(GL_GEOMETRY_SHADER, geometry_shader);
    if (geometry_shader_id == 0)
      return false;
  }

  GLuint fragment_shader_id = 0;
  if (!fragment_shader.empty())
  {
    fragment_shader_id = CompileShader(GL_FRAGMENT_SHADER, fragment_shader);
    if (fragment_shader_id == 0)
    {
      glDeleteShader(vertex_shader_id);
      return false;
    }
  }

  m_program_id = glCreateProgram();
  if (vertex_shader_id != 0)
    glAttachShader(m_program_id, vertex_shader_id);
  if (geometry_shader_id != 0)
    glAttachShader(m_program_id, geometry_shader_id);
  if (fragment_shader_id != 0)
    glAttachShader(m_program_id, fragment_shader_id);
  return true;
}

bool Program::CreateFromBinary(const void* data, uint32_t data_length, uint32_t data_format)
{
  GLuint prog = glCreateProgram();
  glProgramBinary(prog, static_cast<GLenum>(data_format), data, data_length);

  GLint link_status;
  glGetProgramiv(prog, GL_LINK_STATUS, &link_status);
  if (link_status != GL_TRUE)
  {
    Log_ErrorPrintf("Failed to create GL program from binary: status %d", link_status);
    glDeleteProgram(prog);
    return false;
  }

  m_program_id = prog;
  return true;
}

bool Program::GetBinary(std::vector<uint8_t>* out_data, uint32_t* out_data_format)
{
  GLint binary_size = 0;
  glGetProgramiv(m_program_id, GL_PROGRAM_BINARY_LENGTH, &binary_size);
  if (binary_size == 0)
  {
    Log_WarningPrint("glGetProgramiv(GL_PROGRAM_BINARY_LENGTH) returned 0");
    return false;
  }

  GLenum format = 0;
  out_data->resize(static_cast<size_t>(binary_size));
  glGetProgramBinary(m_program_id, binary_size, &binary_size, &format, out_data->data());
  if (binary_size == 0)
  {
    Log_WarningPrint("glGetProgramBinary() failed");
    return false;
  }
  else if (static_cast<size_t>(binary_size) != out_data->size())
  {
    Log_WarningPrintf("Size changed from %zu to %d after glGetProgramBinary()", out_data->size(), binary_size);
    out_data->resize(static_cast<size_t>(binary_size));
  }

  *out_data_format = static_cast<uint32_t>(format);
  Log_InfoPrintf("Program binary retrieved, %zu bytes, format %u", out_data->size(), *out_data_format);
  return true;
}

void Program::SetBinaryRetrievableHint()
{
  glProgramParameteri(m_program_id, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
}

void Program::BindAttribute(GLuint index, const char* name)
{
  glBindAttribLocation(m_program_id, index, name);
}

void Program::BindFragData(GLuint index /*= 0*/, const char* name /*= "o_col0"*/)
{
  glBindFragDataLocation(m_program_id, index, name);
}

void Program::BindFragDataIndexed(GLuint color_number /*= 0*/, const char* name /*= "o_col0"*/)
{
  if (GLAD_GL_VERSION_3_3 || GLAD_GL_ARB_blend_func_extended)
  {
    glBindFragDataLocationIndexed(m_program_id, color_number, 0, name);
    return;
  }
  else if (GLAD_GL_EXT_blend_func_extended)
  {
    glBindFragDataLocationIndexedEXT(m_program_id, color_number, 0, name);
    return;
  }

  Log_ErrorPrintf("BindFragDataIndexed() called without ARB or EXT extension, we'll probably crash.");
  glBindFragDataLocationIndexed(m_program_id, color_number, 0, name);
}

bool Program::Link()
{
  glLinkProgram(m_program_id);

  if (m_vertex_shader_id != 0)
    glDeleteShader(m_vertex_shader_id);
  m_vertex_shader_id = 0;
  if (m_fragment_shader_id != 0)
    glDeleteShader(m_fragment_shader_id);
  m_fragment_shader_id = 0;

  GLint status = GL_FALSE;
  glGetProgramiv(m_program_id, GL_LINK_STATUS, &status);

  GLint info_log_length = 0;
  glGetProgramiv(m_program_id, GL_INFO_LOG_LENGTH, &info_log_length);

  if (status == GL_FALSE || info_log_length > 0)
  {
    std::string info_log;
    info_log.resize(info_log_length + 1);
    glGetProgramInfoLog(m_program_id, info_log_length, &info_log_length, &info_log[0]);

    if (status == GL_TRUE)
    {
      Log_ErrorPrintf("Program linked with warnings:\n%s", info_log.c_str());
    }
    else
    {
      Log_ErrorPrintf("Program failed to link:\n%s", info_log.c_str());
      glDeleteProgram(m_program_id);
      m_program_id = 0;
      return false;
    }
  }

  return true;
}

void Program::Bind() const
{
  if (s_last_program_id == m_program_id)
    return;

  glUseProgram(m_program_id);
  s_last_program_id = m_program_id;
}

void Program::Destroy()
{
  if (m_vertex_shader_id != 0)
  {
    glDeleteShader(m_vertex_shader_id);
    m_vertex_shader_id = 0;
  }
  if (m_fragment_shader_id != 0)
  {
    glDeleteShader(m_fragment_shader_id);
    m_fragment_shader_id = 0;
  }
  if (m_program_id != 0)
  {
    glDeleteProgram(m_program_id);
    m_program_id = 0;
  }

  m_uniform_locations.clear();
}

int Program::RegisterUniform(const char* name)
{
  int id = static_cast<int>(m_uniform_locations.size());
  m_uniform_locations.push_back(glGetUniformLocation(m_program_id, name));
  return id;
}

void Program::Uniform1i(int index, int32_t x) const
{
  const GLint location = m_uniform_locations[index];
  if (location >= 0)
    glUniform1i(location, x);
}

void Program::Uniform4f(int index, float x, float y, float z, float w) const
{
  const GLint location = m_uniform_locations[index];
  if (location >= 0)
    glUniform4f(location, x, y, z, w);
}

void Program::Uniform1i(const char* name, int32_t x) const
{
  const GLint location = glGetUniformLocation(m_program_id, name);
  if (location >= 0)
    glUniform1i(location, x);
}

void Program::BindUniformBlock(const char* name, uint32_t index)
{
  const GLint location = glGetUniformBlockIndex(m_program_id, name);
  if (location >= 0)
    glUniformBlockBinding(m_program_id, location, index);
}

Program& Program::operator=(Program&& prog)
{
  Destroy();
  m_program_id = prog.m_program_id;
  prog.m_program_id = 0;
  m_vertex_shader_id = prog.m_vertex_shader_id;
  prog.m_vertex_shader_id = 0;
  m_fragment_shader_id = prog.m_fragment_shader_id;
  prog.m_fragment_shader_id = 0;
  m_uniform_locations = std::move(prog.m_uniform_locations);
  return *this;
}

} // namespace GL
