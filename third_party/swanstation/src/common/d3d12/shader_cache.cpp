#include "shader_cache.h"
#include "../file_system.h"
#include "../log.h"
#include <cstring>

#include <file/file_path.h>

#include "xxhash.h"

Log_SetChannel(D3D12::ShaderCache);

namespace D3D12 {

// Pipeline-state-object caching on D3D12 is provided solely by the
// ID3D12PipelineLibrary (a single compact driver-managed file that
// dedups PSO state internally; opened in Open() further down). The
// older per-PSO blob cache - which persisted each PSO's GetCachedBlob
// output to d3d12_pipelines_<sm>.bin and grew to ~75 MB for coverage
// that PCSX2-style caches reach at ~1.5 MB - has been removed entirely.
// It was permanently disabled, and the bytecode it would have saved is
// now pre-baked (D3DCommon::EmbeddedShaders), so driver-side PSO
// assembly from the DXBC is cheap enough to redo each cold start.
// Open() scrubs any d3d12_pipelines_<sm> file left by an older build.

ShaderCache::ShaderCache() = default;

ShaderCache::~ShaderCache()
{
  // Serialize the pipeline library to disk first - while m_pipeline_library
  // is still alive and its backing m_pipeline_library_blob has not been
  // destroyed. Best-effort: a write failure logs and falls through to
  // the rest of the teardown.
  if (m_use_pipeline_library && m_pipeline_library && !m_base_path.empty())
    SerializePipelineLibrary();
}

void ShaderCache::Open(std::string_view base_path, ID3D12Device* device, D3D_FEATURE_LEVEL feature_level,
                       uint32_t version, bool debug)
{
  m_base_path = base_path;
  m_feature_level = feature_level;
  m_debug = debug;
  (void)version; // no on-disk cache to version any more (pre-baked shaders + pipeline library)

  if (!base_path.empty())
  {
    // The shader bytecode cache is gone: every D3D12 shader is
    // pre-baked DXBC consumed from D3DCommon::EmbeddedShaders, so
    // nothing compiles HLSL or calls GetShaderBlob any more, and the
    // D3D11 backend already stopped opening this file. Rather than
    // create an empty d3d_shaders_<sm>.{idx,bin} every launch, scrub
    // the orphaned pair (the unified bytecode cache this backend used
    // to write, shared with the now-removed D3D11 cache). Same cheap
    // path_is_valid+unlink self-healing pattern as the pipeline / legacy
    // scrubs below. The pipeline-library cache (opened further down) is
    // unaffected - it is what makes PSO creation cheap and stays.
    {
      const std::string base_shader_filename = GetCacheBaseFileName(base_path, "shaders", feature_level, debug);
      const std::string shader_index_filename = base_shader_filename + ".idx";
      const std::string shader_blob_filename = base_shader_filename + ".bin";
      if (path_is_valid(shader_index_filename.c_str()))
        filestream_delete(shader_index_filename.c_str());
      if (path_is_valid(shader_blob_filename.c_str()))
        filestream_delete(shader_blob_filename.c_str());
    }

    // Scrub any leftover per-PSO pipeline blob cache from an older
    // build (d3d12_pipelines_<sm>.{idx,bin}). That cache has been
    // removed; the file is never written now, so unlink it rather than
    // leave it sitting in the user's system directory (it reached
    // ~75 MB). Cheap path_is_valid+unlink, self-healing across
    // upgrades / downgrades.
    {
      const std::string base_pipelines_filename = GetCacheBaseFileName(base_path, "pipelines", feature_level, debug);
      const std::string pipelines_index_filename = base_pipelines_filename + ".idx";
      const std::string pipelines_blob_filename = base_pipelines_filename + ".bin";
      if (path_is_valid(pipelines_index_filename.c_str()))
        filestream_delete(pipelines_index_filename.c_str());
      if (path_is_valid(pipelines_blob_filename.c_str()))
        filestream_delete(pipelines_blob_filename.c_str());
    }

    // Scrub the pre-unification per-backend shader bytecode cache
    // (d3d12_shaders_<sm>.{idx,bin}). The shaders cache now lives at
    // the backend-neutral d3d_shaders_<sm>.* shared with the D3D11
    // backend, so the old d3d12_-prefixed file is orphaned. Same
    // self-healing rationale as the pipelines scrub above - cheap
    // path_is_valid+unlink on every Open, removes ~900 KB of dead
    // cache across the one-time upgrade. Built from the literal
    // legacy prefix rather than GetCacheBaseFileName("shaders") since
    // the latter now returns the new neutral name.
    {
      std::string legacy_shaders(base_path);
      legacy_shaders += "d3d12_shaders_";
      switch (feature_level)
      {
        case D3D_FEATURE_LEVEL_10_0: legacy_shaders += "sm40"; break;
        case D3D_FEATURE_LEVEL_10_1: legacy_shaders += "sm41"; break;
        case D3D_FEATURE_LEVEL_11_0: legacy_shaders += "sm50"; break;
        default:                     legacy_shaders += "unk";  break;
      }
      if (debug)
        legacy_shaders += "_debug";
      const std::string legacy_index = legacy_shaders + ".idx";
      const std::string legacy_blob = legacy_shaders + ".bin";
      if (path_is_valid(legacy_index.c_str()))
        filestream_delete(legacy_index.c_str());
      if (path_is_valid(legacy_blob.c_str()))
        filestream_delete(legacy_blob.c_str());
    }

    // ID3D12PipelineLibrary path (D3D12.1, Windows 10 v1703+). We try
    // this whenever a device was provided and supports ID3D12Device1.
    // Independent of m_use_pipeline_cache - the library replaces the
    // per-PSO blob cache with a single driver-managed file, internally
    // dedup'd across PSOs that share shader bytecode. On a fully
    // populated session the library file is comparable in size to the
    // ~5 MB shader bytecode cache rather than the 75 MB the per-PSO
    // cache reached.
    if (device)
    {
      ComPtr<ID3D12Device1> device1;
      HRESULT hr = device->QueryInterface(IID_PPV_ARGS(device1.GetAddressOf()));
      if (SUCCEEDED(hr) && device1)
      {
        m_use_pipeline_library = TryOpenPipelineLibrary(device1.Get(), base_path, feature_level, debug);
      }
    }
  }

  m_open = true;
}

std::string ShaderCache::GetCacheBaseFileName(const std::string_view& base_path, const std::string_view& type,
                                              D3D_FEATURE_LEVEL feature_level, bool debug)
{
  std::string base_filename(base_path);

  // The shader bytecode cache ("shaders") is API-neutral DXBC keyed
  // by HLSL hash, byte-compatible with the D3D11 backend's cache, so
  // it uses the backend-neutral "d3d_shaders_" prefix and both
  // backends share one file. The pipeline caches ("pipelines",
  // "pipeline_library") are D3D12-only - the disabled per-PSO blob
  // cache and the driver-managed ID3D12PipelineLibrary - so they
  // keep the "d3d12_" prefix.
  if (type == "shaders")
    base_filename += "d3d_shaders_";
  else
  {
    base_filename += "d3d12_";
    base_filename += type;
    base_filename += "_";
  }

  switch (feature_level)
  {
    case D3D_FEATURE_LEVEL_10_0:
      base_filename += "sm40";
      break;
    case D3D_FEATURE_LEVEL_10_1:
      base_filename += "sm41";
      break;
    case D3D_FEATURE_LEVEL_11_0:
      base_filename += "sm50";
      break;
    default:
      base_filename += "unk";
      break;
  }

  if (debug)
    base_filename += "_debug";

  return base_filename;
}

ShaderCache::CacheIndexKey ShaderCache::GetPipelineCacheKey(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& gpdesc)
{
  // XXH3 (128-bit) over the pipeline descriptor and the attached shader
  // bytecode, replacing MD5. This hash is the only thing the key is used
  // for now: FormatPipelineLibraryName renders it as the
  // ID3D12PipelineLibrary entry name. XXH128_hash_t maps straight onto
  // the key's two uint64 fields. Far cheaper than a cryptographic digest
  // for a local cache key.
  XXH3_state_t* state = XXH3_createState();
  XXH3_128bits_reset(state);
  uint32_t length = sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC);

  if (gpdesc.VS.BytecodeLength > 0)
  {
    XXH3_128bits_update(state, gpdesc.VS.pShaderBytecode, static_cast<uint32_t>(gpdesc.VS.BytecodeLength));
    length += static_cast<uint32_t>(gpdesc.VS.BytecodeLength);
  }
  if (gpdesc.GS.BytecodeLength > 0)
  {
    XXH3_128bits_update(state, gpdesc.GS.pShaderBytecode, static_cast<uint32_t>(gpdesc.GS.BytecodeLength));
    length += static_cast<uint32_t>(gpdesc.GS.BytecodeLength);
  }
  if (gpdesc.PS.BytecodeLength > 0)
  {
    XXH3_128bits_update(state, gpdesc.PS.pShaderBytecode, static_cast<uint32_t>(gpdesc.PS.BytecodeLength));
    length += static_cast<uint32_t>(gpdesc.PS.BytecodeLength);
  }

  XXH3_128bits_update(state, &gpdesc.BlendState, sizeof(gpdesc.BlendState));
  XXH3_128bits_update(state, &gpdesc.SampleMask, sizeof(gpdesc.SampleMask));
  XXH3_128bits_update(state, &gpdesc.RasterizerState, sizeof(gpdesc.RasterizerState));
  XXH3_128bits_update(state, &gpdesc.DepthStencilState, sizeof(gpdesc.DepthStencilState));

  for (uint32_t i = 0; i < gpdesc.InputLayout.NumElements; i++)
  {
    const D3D12_INPUT_ELEMENT_DESC& ie = gpdesc.InputLayout.pInputElementDescs[i];
    XXH3_128bits_update(state, ie.SemanticName, static_cast<uint32_t>(std::strlen(ie.SemanticName)));
    XXH3_128bits_update(state, &ie.SemanticIndex, sizeof(ie.SemanticIndex));
    XXH3_128bits_update(state, &ie.Format, sizeof(ie.Format));
    XXH3_128bits_update(state, &ie.InputSlot, sizeof(ie.InputSlot));
    XXH3_128bits_update(state, &ie.AlignedByteOffset, sizeof(ie.AlignedByteOffset));
    XXH3_128bits_update(state, &ie.InputSlotClass, sizeof(ie.InputSlotClass));
    XXH3_128bits_update(state, &ie.InstanceDataStepRate, sizeof(ie.InstanceDataStepRate));
    length += sizeof(D3D12_INPUT_ELEMENT_DESC);
  }

  XXH3_128bits_update(state, &gpdesc.IBStripCutValue, sizeof(gpdesc.IBStripCutValue));
  XXH3_128bits_update(state, &gpdesc.PrimitiveTopologyType, sizeof(gpdesc.PrimitiveTopologyType));
  XXH3_128bits_update(state, &gpdesc.NumRenderTargets, sizeof(gpdesc.NumRenderTargets));
  XXH3_128bits_update(state, gpdesc.RTVFormats, sizeof(gpdesc.RTVFormats));
  XXH3_128bits_update(state, &gpdesc.DSVFormat, sizeof(gpdesc.DSVFormat));
  XXH3_128bits_update(state, &gpdesc.SampleDesc, sizeof(gpdesc.SampleDesc));
  XXH3_128bits_update(state, &gpdesc.Flags, sizeof(gpdesc.Flags));

  const XXH128_hash_t h = XXH3_128bits_digest(state);
  XXH3_freeState(state);

  return CacheIndexKey{h.low64, h.high64, length, EntryType::GraphicsPipeline};
}

ShaderCache::ComPtr<ID3D12PipelineState> ShaderCache::GetPipelineState(ID3D12Device* device,
                                                                       const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc)
{
  const auto key = GetPipelineCacheKey(desc);

  // Pipeline library path (D3D12.1+). Try LoadGraphicsPipeline first
  // for a microsecond-cost cache hit; on miss fall through to
  // CreateGraphicsPipelineState and best-effort StorePipeline. The
  // library handles its own internal dedup of shader bytecode across
  // PSOs, so the cached file stays compact (PCSX2-style sizes).
  //
  // ID3D12PipelineLibrary methods are documented thread-safe by
  // Microsoft; no cache mutex is held across LoadGraphicsPipeline,
  // CreateGraphicsPipelineState, or StorePipeline. Lock-free
  // compile property from 75e8269 is preserved.
  //
  // StorePipeline returns E_INVALIDARG when an entry with the same
  // name already exists (race loser, or repeated call). That's an
  // expected outcome, not a failure - the PSO we just compiled is
  // equivalent to whatever's already stored. Ignore the result.
  if (m_use_pipeline_library && m_pipeline_library)
  {
    WCHAR name[33];
    FormatPipelineLibraryName(key, name);

    ComPtr<ID3D12PipelineState> pso;
    HRESULT hr = m_pipeline_library->LoadGraphicsPipeline(name, &desc, IID_PPV_ARGS(pso.GetAddressOf()));
    if (SUCCEEDED(hr))
      return pso;

    hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(pso.GetAddressOf()));
    if (FAILED(hr))
    {
      Log_ErrorPrintf("CreateGraphicsPipelineState failed: %08X", hr);
      return {};
    }

    m_pipeline_library->StorePipeline(name, pso.Get());
    return pso;
  }

  // No pipeline library available (pre-D3D12.1 / older Windows):
  // create the PSO uncached from the pre-baked DXBC. Driver-side PSO
  // assembly is cheap, and the per-PSO on-disk blob cache that used to
  // back this path was permanently disabled, so it has been removed
  // entirely - the ID3D12PipelineLibrary above is the only PSO cache.
  ComPtr<ID3D12PipelineState> pso;
  const HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(pso.GetAddressOf()));
  if (FAILED(hr))
  {
    Log_ErrorPrintf("CreateGraphicsPipelineState failed: %08X", hr);
    return {};
  }
  return pso;
}

std::string ShaderCache::GetPipelineLibraryFilename(const std::string_view& base_path, D3D_FEATURE_LEVEL feature_level,
                                                    bool debug)
{
  return GetCacheBaseFileName(base_path, "pipeline_library", feature_level, debug) + ".bin";
}

void ShaderCache::FormatPipelineLibraryName(const CacheIndexKey& key, WCHAR out[33])
{
  // 16 hex digits per 64-bit half, 32 total + L'\0'. The two
  // (source_hash_low, source_hash_high) values are xxhash output over
  // the relevant fields of D3D12_GRAPHICS_PIPELINE_STATE_DESC, so the
  // name space is fully determined by the descriptor content. Stable
  // across runs and across feature-level upgrades that don't change
  // the desc layout. swprintf_s isn't portable; use a hand-rolled
  // hex format to keep things mingw/MSVC-clean.
  static const wchar_t hex[] = L"0123456789abcdef";
  WCHAR* p = out;
  uint64_t lo = key.source_hash_low;
  uint64_t hi = key.source_hash_high;
  for (int i = 15; i >= 0; i--)
    *p++ = hex[(hi >> (i * 4)) & 0xF];
  for (int i = 15; i >= 0; i--)
    *p++ = hex[(lo >> (i * 4)) & 0xF];
  *p = 0;
}

bool ShaderCache::TryOpenPipelineLibrary(ID3D12Device1* device1, std::string_view base_path,
                                         D3D_FEATURE_LEVEL feature_level, bool debug)
{
  const std::string filename = GetPipelineLibraryFilename(base_path, feature_level, debug);

  // Read the existing blob if any. The library will hold a reference
  // into this buffer for its lifetime per the D3D12 spec, so we keep
  // it in a member vector and never resize it after this point.
  if (path_is_valid(filename.c_str()))
  {
    RFILE* fp = rfopen(filename.c_str(), "rb");
    if (fp)
    {
      if (rfseek(fp, 0, SEEK_END) == 0)
      {
        const int64_t file_size = rftell(fp);
        if (file_size > 0 && rfseek(fp, 0, SEEK_SET) == 0)
        {
          m_pipeline_library_blob.resize(static_cast<size_t>(file_size));
          const int64_t read = rfread(m_pipeline_library_blob.data(), 1, static_cast<size_t>(file_size), fp);
          if (read != file_size)
          {
            Log_WarningPrintf("Short read on pipeline library file (%lld of %lld bytes) - ignoring",
                              static_cast<long long>(read), static_cast<long long>(file_size));
            m_pipeline_library_blob.clear();
          }
        }
      }
      rfclose(fp);
    }
  }

  // Try to create a library from the existing bytes (if any). On
  // D3D12_ERROR_DRIVER_VERSION_MISMATCH / D3D12_ERROR_ADAPTER_NOT_FOUND
  // / any other failure: drop the blob, delete the file, fall through
  // to creating an empty library so the session still benefits from
  // run-time PSO caching even if cold-start doesn't.
  HRESULT hr = E_FAIL;
  if (!m_pipeline_library_blob.empty())
  {
    hr = device1->CreatePipelineLibrary(m_pipeline_library_blob.data(), m_pipeline_library_blob.size(),
                                        IID_PPV_ARGS(m_pipeline_library.GetAddressOf()));
    if (FAILED(hr))
    {
      const char* reason = "unknown";
      if (hr == D3D12_ERROR_DRIVER_VERSION_MISMATCH)
        reason = "driver version mismatch";
      else if (hr == D3D12_ERROR_ADAPTER_NOT_FOUND)
        reason = "adapter not found";
      Log_WarningPrintf("Pipeline library load failed (%s, hr=%08X) - rebuilding from scratch", reason, hr);
      m_pipeline_library_blob.clear();
      m_pipeline_library.Reset();
      if (path_is_valid(filename.c_str()))
        filestream_delete(filename.c_str());
    }
  }

  if (!m_pipeline_library)
  {
    hr = device1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(m_pipeline_library.GetAddressOf()));
    if (FAILED(hr))
    {
      Log_WarningPrintf("CreatePipelineLibrary with empty seed failed: %08X. Falling back to no-PSO-cache path.", hr);
      m_pipeline_library_blob.clear();
      m_pipeline_library.Reset();
      return false;
    }
  }

  return true;
}

void ShaderCache::SerializePipelineLibrary()
{
  const SIZE_T size = m_pipeline_library->GetSerializedSize();
  if (size == 0)
    return;

  std::vector<uint8_t> buffer;
  buffer.resize(size);
  HRESULT hr = m_pipeline_library->Serialize(buffer.data(), size);
  if (FAILED(hr))
  {
    Log_WarningPrintf("ID3D12PipelineLibrary::Serialize failed: %08X", hr);
    return;
  }

  const std::string final_filename = GetPipelineLibraryFilename(m_base_path, m_feature_level, m_debug);
  const std::string tmp_filename = final_filename + ".tmp";

  // Atomic write: dump to .tmp, close, rename over the existing file.
  // If anything fails midway the original is untouched and the .tmp
  // file is cleaned up on the next session via the unconditional
  // delete pass below.
  if (path_is_valid(tmp_filename.c_str()))
    filestream_delete(tmp_filename.c_str());

  RFILE* fp = rfopen(tmp_filename.c_str(), "wb");
  if (!fp)
  {
    Log_WarningPrintf("Failed to open %s for write", tmp_filename.c_str());
    return;
  }
  const bool ok = (rfwrite(buffer.data(), 1, size, fp) == static_cast<int64_t>(size));
  rfclose(fp);
  if (!ok)
  {
    Log_WarningPrintf("Short write on %s - leaving previous pipeline library intact", tmp_filename.c_str());
    filestream_delete(tmp_filename.c_str());
    return;
  }

  // filestream_rename returns 0 on success; if the destination exists
  // it's overwritten atomically on POSIX and via ReplaceFileW-style
  // semantics in libretro-common's Windows VFS.
  if (filestream_rename(tmp_filename.c_str(), final_filename.c_str()) != 0)
  {
    Log_WarningPrintf("Failed to rename %s -> %s", tmp_filename.c_str(), final_filename.c_str());
    filestream_delete(tmp_filename.c_str());
  }
}

} // namespace D3D12
