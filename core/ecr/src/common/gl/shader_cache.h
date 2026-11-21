#pragma once
#include "../file_system.h"
#include "../hash_combine.h"
#include "../types.h"
#include "program.h"
#include <cstdio>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace GL {

class ShaderCache
{
public:
  using PreLinkCallback = std::function<void(Program&)>;

  ShaderCache();
  ~ShaderCache();

  void Open(bool is_gles, std::string_view base_path, uint32_t version);

  // Returns whether Open() has already been called successfully on
  // this instance. Used by the lazy-compile path in GPU_HW_OpenGL
  // to avoid re-reading the same on-disk index on UpdateSettings
  // round-trips through CompilePrograms, which would both leak
  // file handles and double-count m_index entries. Mirrors the
  // equivalent accessor on D3D11::ShaderCache and D3D12::ShaderCache.
  bool IsOpen() const { return m_index_file != nullptr; }

  std::optional<Program> GetProgram(const std::string_view vertex_shader, const std::string_view geometry_shader,
                                    const std::string_view fragment_shader, const PreLinkCallback& callback = {});

private:
  // Bumped 4 -> 5 when the cache key hash moved from MD5 to XXH3_128bits;
  // the change alters every key, so an old (v4) gl_programs cache is
  // discarded and rebuilt once on first run with this build.
  static constexpr uint32_t FILE_VERSION = 5;

  struct CacheIndexKey
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

    bool operator==(const CacheIndexKey& key) const;
  };

  struct CacheIndexEntryHasher
  {
    std::size_t operator()(const CacheIndexKey& e) const noexcept
    {
      std::size_t h = 0;
      hash_combine(h, e.vertex_source_hash_low, e.vertex_source_hash_high, e.vertex_source_length,
                   e.geometry_source_hash_low, e.geometry_source_hash_high, e.geometry_source_length,
                   e.fragment_source_hash_low, e.fragment_source_hash_high, e.fragment_source_length);
      return h;
    }
  };

  struct CacheIndexData
  {
    uint32_t file_offset;
    uint32_t blob_size;
    uint32_t blob_format;
  };

  using CacheIndex = std::unordered_map<CacheIndexKey, CacheIndexData, CacheIndexEntryHasher>;

  static CacheIndexKey GetCacheKey(const std::string_view& vertex_shader, const std::string_view& geometry_shader,
                                   const std::string_view& fragment_shader);

  std::string GetIndexFileName() const;
  std::string GetBlobFileName() const;

  bool CreateNew(const std::string& index_filename, const std::string& blob_filename);
  bool ReadExisting(const std::string& index_filename, const std::string& blob_filename);
  void Close();
  bool Recreate();

  std::optional<Program> CompileProgram(const std::string_view& vertex_shader, const std::string_view& geometry_shader,
                                        const std::string_view& fragment_shader, const PreLinkCallback& callback,
                                        bool set_retrievable);
  std::optional<Program> CompileAndAddProgram(const CacheIndexKey& key, const std::string_view& vertex_shader,
                                              const std::string_view& geometry_shader,
                                              const std::string_view& fragment_shader, const PreLinkCallback& callback);

  std::string m_base_path;
  RFILE* m_index_file = nullptr;
  RFILE* m_blob_file = nullptr;

  CacheIndex m_index;
  uint32_t m_version = 0;
  bool m_program_binary_supported = false;
};

} // namespace GL
