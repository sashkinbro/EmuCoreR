#include "shader_cache.h"
#include "../file_system.h"
#include "../log.h"
#include "../string_util.h"

#include <file/file_path.h>

#include "xxhash.h"

Log_SetChannel(GL::ShaderCache);

namespace GL {

#pragma pack(push, 1)
struct CacheIndexEntry
{
  uint64_t vertex_source_hash_low;
  uint64_t vertex_source_hash_high;
  uint32_t vertex_source_length;
  uint64_t geometry_source_hash_low;
  uint64_t geometry_source_hash_high;
  uint32_t geometry_source_length;
  uint64_t fragment_source_hash_low;
  uint64_t fragment_source_hash_high;
  uint32_t fragment_source_length;
  uint32_t file_offset;
  uint32_t blob_size;
  uint32_t blob_format;
};
#pragma pack(pop)

ShaderCache::ShaderCache() = default;

ShaderCache::~ShaderCache()
{
  Close();
}

bool ShaderCache::CacheIndexKey::operator==(const CacheIndexKey& key) const
{
  return (
    vertex_source_hash_low == key.vertex_source_hash_low && vertex_source_hash_high == key.vertex_source_hash_high &&
    vertex_source_length == key.vertex_source_length && geometry_source_hash_low == key.geometry_source_hash_low &&
    geometry_source_hash_high == key.geometry_source_hash_high &&
    geometry_source_length == key.geometry_source_length && fragment_source_hash_low == key.fragment_source_hash_low &&
    fragment_source_hash_high == key.fragment_source_hash_high && fragment_source_length == key.fragment_source_length);
}

void ShaderCache::Open(bool is_gles, std::string_view base_path, uint32_t version)
{
  m_base_path = base_path;
  m_version = version;
  m_program_binary_supported = is_gles || GLAD_GL_ARB_get_program_binary;
  if (m_program_binary_supported)
  {
    // check that there's at least one format and the extension isn't being "faked"
    GLint num_formats = 0;
    glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &num_formats);
    Log_InfoPrintf("%u program binary formats supported by driver", num_formats);
    m_program_binary_supported = (num_formats > 0);
  }

  if (!m_program_binary_supported)
  {
    Log_WarningPrintf("Your GL driver does not support program binaries. Hopefully it has a built-in cache, otherwise "
                      "startup will be slow due to compiling shaders.");
    return;
  }

  if (!base_path.empty())
  {
    const std::string index_filename = GetIndexFileName();
    const std::string blob_filename = GetBlobFileName();

    if (!ReadExisting(index_filename, blob_filename))
      CreateNew(index_filename, blob_filename);
  }
}

bool ShaderCache::CreateNew(const std::string& index_filename, const std::string& blob_filename)
{
  if (path_is_valid(index_filename.c_str()))
  {
    Log_WarningPrintf("Removing existing index file '%s'", index_filename.c_str());
    filestream_delete(index_filename.c_str());
  }
  if (path_is_valid(blob_filename.c_str()))
  {
    Log_WarningPrintf("Removing existing blob file '%s'", blob_filename.c_str());
    filestream_delete(blob_filename.c_str());
  }

  m_index_file = FileSystem::OpenRFile(index_filename.c_str(), "wb");
  if (!m_index_file)
  {
    Log_ErrorPrintf("Failed to open index file '%s' for writing", index_filename.c_str());
    return false;
  }

  const uint32_t index_version = FILE_VERSION;
  if (rfwrite(&index_version, sizeof(index_version), 1, m_index_file) != 1 ||
      rfwrite(&m_version, sizeof(m_version), 1, m_index_file) != 1)
  {
    Log_ErrorPrintf("Failed to write version to index file '%s'", index_filename.c_str());
    rfclose(m_index_file);
    m_index_file = nullptr;
    filestream_delete(index_filename.c_str());
    return false;
  }

  m_blob_file = FileSystem::OpenRFile(blob_filename.c_str(), "w+b");
  if (!m_blob_file)
  {
    Log_ErrorPrintf("Failed to open blob file '%s' for writing", blob_filename.c_str());
    rfclose(m_index_file);
    m_index_file = nullptr;
    filestream_delete(index_filename.c_str());
    return false;
  }

  return true;
}

bool ShaderCache::ReadExisting(const std::string& index_filename, const std::string& blob_filename)
{
  m_index_file = FileSystem::OpenRFile(index_filename.c_str(), "r+b");
  if (!m_index_file)
    return false;

  uint32_t file_version = 0;
  uint32_t data_version = 0;
  if (rfread(&file_version, sizeof(file_version), 1, m_index_file) != 1 || file_version != FILE_VERSION ||
      rfread(&data_version, sizeof(data_version), 1, m_index_file) != 1 || data_version != m_version)
  {
    Log_ErrorPrintf("Bad file/data version in '%s'", index_filename.c_str());
    rfclose(m_index_file);
    m_index_file = nullptr;
    return false;
  }

  m_blob_file = FileSystem::OpenRFile(blob_filename.c_str(), "a+b");
  if (!m_blob_file)
  {
    Log_ErrorPrintf("Blob file '%s' is missing", blob_filename.c_str());
    rfclose(m_index_file);
    m_index_file = nullptr;
    return false;
  }

  rfseek(m_blob_file, 0, SEEK_END);
  const uint32_t blob_file_size = static_cast<uint32_t>(rftell(m_blob_file));

  for (;;)
  {
    CacheIndexEntry entry;
    if (rfread(&entry, sizeof(entry), 1, m_index_file) != 1 ||
        (entry.file_offset + entry.blob_size) > blob_file_size)
    {
      if (rfeof(m_index_file))
        break;

      Log_ErrorPrintf("Failed to read entry from '%s', corrupt file?", index_filename.c_str());
      m_index.clear();
      rfclose(m_blob_file);
      m_blob_file = nullptr;
      rfclose(m_index_file);
      m_index_file = nullptr;
      return false;
    }

    const CacheIndexKey key{
      entry.vertex_source_hash_low,   entry.vertex_source_hash_high,   entry.vertex_source_length,
      entry.geometry_source_hash_low, entry.geometry_source_hash_high, entry.geometry_source_length,
      entry.fragment_source_hash_low, entry.fragment_source_hash_high, entry.fragment_source_length};
    const CacheIndexData data{entry.file_offset, entry.blob_size, entry.blob_format};
    m_index.emplace(key, data);
  }

  Log_InfoPrintf("Read %zu entries from '%s'", m_index.size(), index_filename.c_str());
  return true;
}

void ShaderCache::Close()
{
  m_index.clear();
  if (m_index_file)
    rfclose(m_index_file);
  if (m_blob_file)
    rfclose(m_blob_file);
}

bool ShaderCache::Recreate()
{
  Close();

  const std::string index_filename = GetIndexFileName();
  const std::string blob_filename = GetBlobFileName();

  return CreateNew(index_filename, blob_filename);
}

ShaderCache::CacheIndexKey ShaderCache::GetCacheKey(const std::string_view& vertex_shader,
                                                    const std::string_view& geometry_shader,
                                                    const std::string_view& fragment_shader)
{
  // Each stage is hashed independently - the cache key stores a separate
  // 128-bit hash per stage - so programs that share one stage (e.g. the
  // same vertex shader) still differ on the others. XXH3 (128-bit)
  // replaces MD5 here: much faster and collision-resistant enough for a
  // local shader-cache key. An empty stage keeps a zero hash, exactly
  // as the old MD5 path did (it skipped empty stages without hashing).
  uint64_t vertex_low = 0, vertex_high = 0;
  uint64_t geometry_low = 0, geometry_high = 0;
  uint64_t fragment_low = 0, fragment_high = 0;

  if (!vertex_shader.empty())
  {
    const XXH128_hash_t h = XXH3_128bits(vertex_shader.data(), vertex_shader.length());
    vertex_low = h.low64;
    vertex_high = h.high64;
  }

  if (!geometry_shader.empty())
  {
    const XXH128_hash_t h = XXH3_128bits(geometry_shader.data(), geometry_shader.length());
    geometry_low = h.low64;
    geometry_high = h.high64;
  }

  if (!fragment_shader.empty())
  {
    const XXH128_hash_t h = XXH3_128bits(fragment_shader.data(), fragment_shader.length());
    fragment_low = h.low64;
    fragment_high = h.high64;
  }

  return CacheIndexKey{vertex_low,   vertex_high,   static_cast<uint32_t>(vertex_shader.length()),
                       geometry_low, geometry_high, static_cast<uint32_t>(geometry_shader.length()),
                       fragment_low, fragment_high, static_cast<uint32_t>(fragment_shader.length())};
}

std::string ShaderCache::GetIndexFileName() const
{
  return StringUtil::StdStringFromFormat("%sgl_programs.idx", m_base_path.c_str());
}

std::string ShaderCache::GetBlobFileName() const
{
  return StringUtil::StdStringFromFormat("%sgl_programs.bin", m_base_path.c_str());
}

std::optional<Program> ShaderCache::GetProgram(const std::string_view vertex_shader,
                                               const std::string_view geometry_shader,
                                               const std::string_view fragment_shader, const PreLinkCallback& callback)
{
  if (!m_program_binary_supported || !m_blob_file)
    return CompileProgram(vertex_shader, geometry_shader, fragment_shader, callback, false);

  const auto key = GetCacheKey(vertex_shader, geometry_shader, fragment_shader);
  auto iter = m_index.find(key);
  if (iter == m_index.end())
    return CompileAndAddProgram(key, vertex_shader, geometry_shader, fragment_shader, callback);

  std::vector<uint8_t> data(iter->second.blob_size);
  if (rfseek(m_blob_file, iter->second.file_offset, SEEK_SET) != 0 ||
      rfread(data.data(), 1, iter->second.blob_size, m_blob_file) != iter->second.blob_size)
  {
    Log_ErrorPrintf("Read blob from file failed");
    return {};
  }

  Program prog;
  if (prog.CreateFromBinary(data.data(), static_cast<uint32_t>(data.size()), iter->second.blob_format))
    return std::optional<Program>(std::move(prog));

  Log_WarningPrintf(
    "Failed to create program from binary, this may be due to a driver or GPU Change. Recreating cache.");
  if (!Recreate())
    return CompileProgram(vertex_shader, geometry_shader, fragment_shader, callback, false);
  else
    return CompileAndAddProgram(key, vertex_shader, geometry_shader, fragment_shader, callback);
}

std::optional<Program> ShaderCache::CompileProgram(const std::string_view& vertex_shader,
                                                   const std::string_view& geometry_shader,
                                                   const std::string_view& fragment_shader,
                                                   const PreLinkCallback& callback, bool set_retrievable)
{
  Program prog;
  if (!prog.Compile(vertex_shader, geometry_shader, fragment_shader))
    return std::nullopt;

  if (callback)
    callback(prog);

  if (set_retrievable)
    prog.SetBinaryRetrievableHint();

  if (!prog.Link())
    return std::nullopt;

  return std::optional<Program>(std::move(prog));
}

std::optional<Program> ShaderCache::CompileAndAddProgram(const CacheIndexKey& key,
                                                         const std::string_view& vertex_shader,
                                                         const std::string_view& geometry_shader,
                                                         const std::string_view& fragment_shader,
                                                         const PreLinkCallback& callback)
{
  std::optional<Program> prog = CompileProgram(vertex_shader, geometry_shader, fragment_shader, callback, true);
  if (!prog)
    return std::nullopt;

  std::vector<uint8_t> prog_data;
  uint32_t prog_format = 0;
  if (!prog->GetBinary(&prog_data, &prog_format))
    return std::nullopt;

  if (!m_blob_file || rfseek(m_blob_file, 0, SEEK_END) != 0)
    return prog;

  CacheIndexData data;
  data.file_offset = static_cast<uint32_t>(rftell(m_blob_file));
  data.blob_size = static_cast<uint32_t>(prog_data.size());
  data.blob_format = prog_format;

  CacheIndexEntry entry = {};
  entry.vertex_source_hash_low = key.vertex_source_hash_low;
  entry.vertex_source_hash_high = key.vertex_source_hash_high;
  entry.vertex_source_length = key.vertex_source_length;
  entry.geometry_source_hash_low = key.geometry_source_hash_low;
  entry.geometry_source_hash_high = key.geometry_source_hash_high;
  entry.geometry_source_length = key.geometry_source_length;
  entry.fragment_source_hash_low = key.fragment_source_hash_low;
  entry.fragment_source_hash_high = key.fragment_source_hash_high;
  entry.fragment_source_length = key.fragment_source_length;
  entry.file_offset = data.file_offset;
  entry.blob_size = data.blob_size;
  entry.blob_format = data.blob_format;

  if (rfwrite(prog_data.data(), 1, entry.blob_size, m_blob_file) != entry.blob_size ||
      filestream_flush(m_blob_file) != 0 || rfwrite(&entry, sizeof(entry), 1, m_index_file) != 1 ||
      filestream_flush(m_index_file) != 0)
  {
    Log_ErrorPrintf("Failed to write shader blob to file");
    return prog;
  }

  m_index.emplace(key, data);
  return prog;
}

} // namespace GL
