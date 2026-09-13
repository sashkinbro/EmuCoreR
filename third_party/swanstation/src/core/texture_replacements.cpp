#include "texture_replacements.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/platform.h"
#include "common/string_util.h"
#include "common/timer.h"
#include "core/host_interface.h"
#include "gpu.h"
#include "host_interface.h"
#include "settings.h"
#include "xxhash.h"
#if defined(CPU_X86) || defined(CPU_X64)
#include "xxh_x86dispatch.h"
#endif
#include <algorithm>
#include <cmath>
#include <cinttypes>
#include <cstring>
#include <file/file_path.h>
#include <iterator>
#include <mutex>
#include <optional>
#include <utility>
Log_SetChannel(TextureReplacements);

// Decoded replacement textures are kept in memory, and large HD packs can be
// hundreds of megabytes, so cap the cache and evict the oldest entries first.
static constexpr size_t TEXTURE_CACHE_BUDGET_BYTES = 128u * 1024u * 1024u;

// Composited texture pages (RGBA8, upscaled) get their own budget. A single
// full-page composite is 256x256x4 bytes at 1x, or up to ~64 MiB at 4K packs.
static constexpr size_t TEXTURE_PAGE_CACHE_BUDGET_BYTES = 256u * 1024u * 1024u;

// VRAM page layout (matches DuckStation's gpu_types.h).
static constexpr uint32_t VRAM_PAGE_WIDTH = 64;
static constexpr uint32_t VRAM_PAGE_HEIGHT = 256;
static constexpr uint32_t TEXPAGE_NATIVE_WIDTH = 256;  // expanded texels, all modes
static constexpr uint32_t TEXPAGE_NATIVE_HEIGHT = 256;

static constexpr const char* s_texture_replacement_mode_names[] = {"P4",   "P8",   "C16",   "C16",
                                                                   "STP4", "STP8", "STC16", "STC16"};


TextureReplacements g_texture_replacements;

// EmuCoreR Android frontend hook: when set, replacement textures are read from
// this base directory instead of the core's shader-cache location.
namespace {
std::mutex s_texture_path_override_mutex;
std::string s_texture_path_override;
}  // namespace

void SetTextureReplacementsPathOverride(std::string path)
{
  std::lock_guard<std::mutex> lock(s_texture_path_override_mutex);
  s_texture_path_override = std::move(path);
}

static std::string GetTextureReplacementsPathOverride()
{
  std::lock_guard<std::mutex> lock(s_texture_path_override_mutex);
  return s_texture_path_override;
}

static constexpr uint32_t VRAMRGBA5551ToRGBA8888(uint16_t color)
{
  uint8_t r = static_cast<uint8_t>(color & 31);
  uint8_t g = static_cast<uint8_t>((color >> 5) & 31);
  uint8_t b = static_cast<uint8_t>((color >> 10) & 31);
  uint8_t a = static_cast<uint8_t>((color >> 15) & 1);

  // 00012345 -> 1234545
  b = (b << 3) | (b & 0b111);
  g = (g << 3) | (g & 0b111);
  r = (r << 3) | (r & 0b111);
  a = a ? 255 : 0;

  return static_cast<uint32_t>(r) | (static_cast<uint32_t>(g) << 8) | (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(a) << 24);
}

std::string TextureReplacementHash::ToString() const
{
  return StringUtil::StdStringFromFormat("%" PRIx64 "%" PRIx64, high, low);
}

bool TextureReplacementHash::ParseString(const std::string_view& sv)
{
  if (sv.length() != 32)
    return false;

  std::optional<uint64_t> high_value = StringUtil::FromChars<uint64_t>(sv.substr(0, 16), 16);
  std::optional<uint64_t> low_value = StringUtil::FromChars<uint64_t>(sv.substr(16), 16);
  if (!high_value.has_value() || !low_value.has_value())
    return false;

  low = low_value.value();
  high = high_value.value();
  return true;
}

TextureReplacements::TextureReplacements() = default;

TextureReplacements::~TextureReplacements() = default;

void TextureReplacements::SetGameID(std::string game_id)
{
  if (m_game_id == game_id)
    return;

  m_game_id = game_id;
  Reload();
}

const TextureReplacementTexture* TextureReplacements::GetVRAMWriteReplacement(uint32_t width, uint32_t height, const void* pixels)
{
  const TextureReplacementHash hash = GetVRAMWriteHash(width, height, pixels);

  const auto it = m_vram_write_replacements.find(hash);
  if (it == m_vram_write_replacements.end())
    return nullptr;

  return LoadTexture(it->second);
}

void TextureReplacements::Shutdown()
{
  m_texture_cache.clear();
  m_texture_lru.clear();
  m_texture_lru_positions.clear();
  m_texture_cache_bytes = 0;
  m_vram_write_replacements.clear();
  for (auto& entries : m_texpage_replacements)
    entries.clear();
  m_texupload_replacement_count = 0;
  m_page_cache.clear();
  m_page_cache_bytes = 0;
  m_vram = nullptr;
  m_game_id.clear();
}

std::string TextureReplacements::GetSourceDirectory() const
{
  // EmuCoreR: the Android frontend supplies its own texture root so the
  // installed packs and the core share one directory.
  const std::string override_path = GetTextureReplacementsPathOverride();
  if (!override_path.empty())
    return override_path + FS_OSPATH_SEPARATOR_STR + m_game_id;

  // Use the shader cache path as base for the textures folder
  std::string cache_folder = g_host_interface_storage.GetShaderCacheBasePath();
  return g_host_interface->GetUserDirectoryRelativePath("%s" "textures" FS_OSPATH_SEPARATOR_STR "%s", cache_folder.c_str(), m_game_id.c_str());
}

TextureReplacementHash TextureReplacements::GetVRAMWriteHash(uint32_t width, uint32_t height, const void* pixels) const
{
  XXH128_hash_t hash = XXH3_128bits(pixels, width * height * sizeof(uint16_t));
  return {hash.low64, hash.high64};
}

void TextureReplacements::Reload()
{
  m_vram_write_replacements.clear();
  for (auto& entries : m_texpage_replacements)
    entries.clear();
  m_texupload_replacement_count = 0;
  m_page_cache.clear();
  m_page_cache_bytes = 0;

  if (g_settings.texture_replacements.AnyReplacementsEnabled())
    FindTextures(GetSourceDirectory());

  if (g_settings.texture_replacements.preload_textures)
    PreloadTextures();

  PurgeUnreferencedTexturesFromCache();
}

void TextureReplacements::PurgeUnreferencedTexturesFromCache()
{
  TextureCache old_map = std::move(m_texture_cache);
  for (const auto& it : m_vram_write_replacements)
  {
    auto it2 = old_map.find(it.second);
    if (it2 != old_map.end())
    {
      m_texture_cache[it.second] = std::move(it2->second);
      old_map.erase(it2);
    }
  }

  ResetTextureCacheOrdering();
}

void TextureReplacements::ResetTextureCacheOrdering()
{
  m_texture_lru.clear();
  m_texture_lru_positions.clear();
  m_texture_cache_bytes = 0;
  for (auto& it : m_texture_cache)
  {
    m_texture_lru.push_back(it.first);
    m_texture_lru_positions.emplace(it.first, std::prev(m_texture_lru.end()));
    m_texture_cache_bytes += static_cast<size_t>(it.second.GetWidth()) * it.second.GetHeight() * 4;
  }
}

void TextureReplacements::TouchTextureCacheEntry(const std::string& filename)
{
  const auto pos = m_texture_lru_positions.find(filename);
  if (pos == m_texture_lru_positions.end())
    return;

  m_texture_lru.splice(m_texture_lru.end(), m_texture_lru, pos->second);
}

void TextureReplacements::EvictTexturesForBudget(size_t incoming_bytes, const std::string& keep_filename)
{
  while ((m_texture_cache_bytes + incoming_bytes) > TEXTURE_CACHE_BUDGET_BYTES && !m_texture_lru.empty())
  {
    const std::string victim = m_texture_lru.front();
    if (victim == keep_filename && m_texture_lru.size() == 1)
      break;

    m_texture_lru.pop_front();
    m_texture_lru_positions.erase(victim);

    const auto it = m_texture_cache.find(victim);
    if (it != m_texture_cache.end())
    {
      m_texture_cache_bytes -= static_cast<size_t>(it->second.GetWidth()) * it->second.GetHeight() * 4;
      m_texture_cache.erase(it);
    }
  }
}

bool TextureReplacements::ParseReplacementFilename(const std::string& filename,
                                                   TextureReplacementHash* replacement_hash,
                                                   ReplacmentType* replacement_type)
{
  const char* extension = std::strrchr(filename.c_str(), '.');
  const char* title = std::strrchr(filename.c_str(), '/');
#ifdef _WIN32
  const char* title2 = std::strrchr(filename.c_str(), '\\');
  if (title2 && (!title || title2 > title))
    title = title2;
#endif

  if (!title || !extension)
    return false;

  title++;

  const char* hashpart;

  if (StringUtil::Strncasecmp(title, "vram-write-", 11) == 0)
  {
    hashpart = title + 11;
    *replacement_type = ReplacmentType::VRAMWrite;
  }
  else
  {
    return false;
  }

  if (!replacement_hash->ParseString(std::string_view(hashpart, static_cast<size_t>(extension - hashpart))))
    return false;

  extension++;

  bool valid_extension = false;
  for (const char* test_extension : {"png", "jpg", "tga", "bmp"})
  {
    if (StringUtil::Strcasecmp(extension, test_extension) == 0)
    {
      valid_extension = true;
      break;
    }
  }

  return valid_extension;
}

static std::string_view GetFileTitle(const std::string& filename)
{
  const char* file_name = filename.c_str();
  const char* slash = std::strrchr(file_name, '/');
  const char* backslash = std::strrchr(file_name, '\\');
  if (backslash && (!slash || backslash > slash))
    slash = backslash;
  if (slash)
    file_name = slash + 1;

  const char* dot = std::strrchr(file_name, '.');
  return (dot && dot != file_name) ? std::string_view(file_name, static_cast<size_t>(dot - file_name)) :
                                     std::string_view(file_name);
}

bool TextureReplacements::IsSupportedExtension(const std::string_view extension)
{
  for (const char* test_extension : {"png", "jpg", "tga", "bmp"})
  {
    const size_t test_length = std::strlen(test_extension);
    if (extension.size() == test_length &&
        StringUtil::Strncasecmp(extension.data(), test_extension, test_length) == 0)
    {
      return true;
    }
  }

  return false;
}

void TextureReplacements::FindTextures(const std::string& dir)
{
  FileSystem::FindResultsArray files;
  FileSystem::FindFiles(dir.c_str(), "*", FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_RECURSIVE, &files);

  for (FILESYSTEM_FIND_DATA& fd : files)
  {
    const char* extension = std::strrchr(fd.FileName.c_str(), '.');
    if (!extension || !IsSupportedExtension(std::string_view(extension + 1)))
      continue;

    const std::string_view file_title = GetFileTitle(fd.FileName);

    if (file_title.size() >= 11 && StringUtil::Strncasecmp(file_title.data(), "vram-write-", 11) == 0)
    {
      TextureReplacementHash hash;
      ReplacmentType type;
      if (!ParseReplacementFilename(fd.FileName, &hash, &type))
        continue;

      switch (type)
      {
        case ReplacmentType::VRAMWrite:
        {
          auto it = m_vram_write_replacements.find(hash);
          if (it != m_vram_write_replacements.end())
          {
            Log_WarningPrintf("Duplicate VRAM write replacement: '%s' and '%s'", it->second.c_str(),
                              fd.FileName.c_str());
            continue;
          }

          m_vram_write_replacements.emplace(hash, std::move(fd.FileName));
        }
        break;
      }
      continue;
    }

    const bool is_texpage = (file_title.size() >= 8 && StringUtil::Strncasecmp(file_title.data(), "texpage-", 8) == 0);
    const bool is_texupload =
      (file_title.size() >= 10 && StringUtil::Strncasecmp(file_title.data(), "texupload-", 10) == 0);
    if (!is_texpage && !is_texupload)
      continue;

    TexturePageReplacementName name;
    if (!name.Parse(file_title))
    {
      Log_WarningPrintf("Invalid texture replacement filename: '%s'", fd.FileName.c_str());
      continue;
    }

    if (is_texupload)
    {
      // Parsed for completeness, but texupload (VRAM write) replacements are
      // not matched in this pass; only texpage-* packs are supported.
      m_texupload_replacement_count++;
      continue;
    }

    // ST* variants share the bucket with their base mode (matching ignores the
    // semitransparency bit; it only affects how the replacement is composited).
    uint32_t entry_mode = name.texture_mode & 3u;
    if (entry_mode == static_cast<uint32_t>(GPUTextureMode::Reserved_Direct16Bit))
      entry_mode = static_cast<uint32_t>(GPUTextureMode::Direct16Bit);

    auto& entries = m_texpage_replacements[entry_mode];
    if (std::find_if(entries.begin(), entries.end(),
                     [&name](const TexturePageReplacementEntry& e) { return (e.name == name); }) != entries.end())
    {
      Log_WarningPrintf("Duplicate texture page replacement: '%s'", fd.FileName.c_str());
      continue;
    }

    TexturePageReplacementEntry entry;
    entry.name = name;
    entry.filename = std::move(fd.FileName);
    entries.push_back(std::move(entry));
  }

  size_t texpage_count = 0;
  for (const auto& entries : m_texpage_replacements)
    texpage_count += entries.size();

  Log_InfoPrintf("Found %zu replacement VRAM writes, %zu texture pages and %u texture uploads for '%s'",
                 m_vram_write_replacements.size(), texpage_count, m_texupload_replacement_count, m_game_id.c_str());
}

const TextureReplacementTexture* TextureReplacements::LoadTexture(const std::string& filename)
{
  auto it = m_texture_cache.find(filename);
  if (it != m_texture_cache.end())
  {
    TouchTextureCacheEntry(filename);
    return &it->second;
  }

  Common::RGBA8Image image;
  if (!Common::LoadImageFromFile(&image, filename.c_str()))
  {
    Log_ErrorPrintf("Failed to load '%s'", filename.c_str());
    return nullptr;
  }

  const size_t image_bytes = static_cast<size_t>(image.GetWidth()) * image.GetHeight() * 4;
  Log_InfoPrintf("Loaded '%s': %ux%u", filename.c_str(), image.GetWidth(), image.GetHeight());

  EvictTexturesForBudget(image_bytes, filename);

  it = m_texture_cache.emplace(filename, std::move(image)).first;
  m_texture_lru.push_back(filename);
  m_texture_lru_positions[filename] = std::prev(m_texture_lru.end());
  m_texture_cache_bytes += image_bytes;
  return &it->second;
}

void TextureReplacements::PreloadTextures()
{
  const Common::Timer::Value update_interval = Common::Timer::ConvertSecondsToValue(1.0);
  Common::Timer::Value last_update_time = Common::Timer::GetValue();
  uint32_t num_textures_loaded = 0;
  uint32_t total_textures = static_cast<uint32_t>(m_vram_write_replacements.size());
  for (const auto& entries : m_texpage_replacements)
    total_textures += static_cast<uint32_t>(entries.size());

#define UPDATE_PROGRESS()                                                                                              \
  if ((Common::Timer::GetValue() - last_update_time) >= update_interval)                                              \
  {                                                                                                                    \
    g_host_interface->DisplayLoadingScreen("Preloading replacement textures...", 0, static_cast<int>(total_textures),  \
                                           static_cast<int>(num_textures_loaded));                                     \
    last_update_time = Common::Timer::GetValue();                                                                     \
  }

  for (const auto& it : m_vram_write_replacements)
  {
    UPDATE_PROGRESS();

    LoadTexture(it.second);
    num_textures_loaded++;
  }

  for (const auto& entries : m_texpage_replacements)
  {
    for (const TexturePageReplacementEntry& entry : entries)
    {
      UPDATE_PROGRESS();

      LoadTexture(entry.filename);
      num_textures_loaded++;
    }
  }

#undef UPDATE_PROGRESS
}

//////////////////////////////////////////////////////////////////////////
// Texture page (texpage-*) replacements
//////////////////////////////////////////////////////////////////////////

bool TextureReplacements::TexturePageReplacementName::operator==(const TexturePageReplacementName& rhs) const
{
  return src_hash == rhs.src_hash && pal_hash == rhs.pal_hash && src_width == rhs.src_width &&
         src_height == rhs.src_height && texture_mode == rhs.texture_mode && offset_x == rhs.offset_x &&
         offset_y == rhs.offset_y && width == rhs.width && height == rhs.height && pal_min == rhs.pal_min &&
         pal_max == rhs.pal_max;
}

// Port of DuckStation's TextureReplacementName::Parse. The token order differs
// between paletted and direct16 modes: paletted entries carry a palette hash
// and a palette index range, direct16 entries do not.
bool TextureReplacements::TexturePageReplacementName::Parse(const std::string_view file_title)
{
  std::string_view::size_type start_pos = 0;
  std::string_view::size_type end_pos = file_title.find("-", start_pos);
  if (end_pos == std::string_view::npos)
    return false;

  // type
  const std::string_view type_token = file_title.substr(start_pos, end_pos - start_pos);
  if (type_token != "texpage" && type_token != "texupload")
    return false;
  start_pos = end_pos + 1;
  end_pos = file_title.find("-", start_pos + 1);
  if (end_pos == std::string_view::npos)
    return false;

  // mode
  const std::string_view mode_token = file_title.substr(start_pos, end_pos - start_pos);
  std::optional<uint8_t> mode_opt;
  for (uint8_t i = 0; i < 8; i++)
  {
    if (mode_token == s_texture_replacement_mode_names[i])
    {
      mode_opt = i;
      break; // first match wins: C16 -> 2, STC16 -> 6
    }
  }
  if (!mode_opt.has_value())
    return false;
  texture_mode = mode_opt.value();
  start_pos = end_pos + 1;
  end_pos = file_title.find("-", start_pos + 1);
  if (end_pos == std::string_view::npos)
    return false;

  // src_hash
  const std::string_view src_hash_token = file_title.substr(start_pos, end_pos - start_pos);
  std::optional<uint64_t> val64;
  if (src_hash_token.size() != 16 || !(val64 = StringUtil::FromChars<uint64_t>(src_hash_token, 16)).has_value())
    return false;
  src_hash = val64.value();

  std::optional<uint16_t> val16;
  std::optional<uint8_t> val8;

  if (GetTextureMode() < GPUTextureMode::Direct16Bit)
  {
    start_pos = end_pos + 1;
    end_pos = file_title.find("-", start_pos + 1);
    if (end_pos == std::string_view::npos)
      return false;

    // pal_hash
    const std::string_view pal_hash_token = file_title.substr(start_pos, end_pos - start_pos);
    if (pal_hash_token.size() != 16 || !(val64 = StringUtil::FromChars<uint64_t>(pal_hash_token, 16)).has_value())
      return false;
    pal_hash = val64.value();
    start_pos = end_pos + 1;
    end_pos = file_title.find("x", start_pos + 1);
    if (end_pos == std::string_view::npos)
      return false;

    // src_width
    const std::string_view src_width_token = file_title.substr(start_pos, end_pos - start_pos);
    if (!(val16 = StringUtil::FromChars<uint16_t>(src_width_token)).has_value())
      return false;
    src_width = val16.value();
    if (src_width == 0)
      return false;
    start_pos = end_pos + 1;
    end_pos = file_title.find("-", start_pos + 1);
    if (end_pos == std::string_view::npos)
      return false;

    // src_height
    const std::string_view src_height_token = file_title.substr(start_pos, end_pos - start_pos);
    if (!(val16 = StringUtil::FromChars<uint16_t>(src_height_token)).has_value())
      return false;
    src_height = val16.value();
    if (src_height == 0)
      return false;
    start_pos = end_pos + 1;
    end_pos = file_title.find("-", start_pos + 1);
    if (end_pos == std::string_view::npos)
      return false;

    // offset_x
    const std::string_view offset_x_token = file_title.substr(start_pos, end_pos - start_pos);
    if (!(val16 = StringUtil::FromChars<uint16_t>(offset_x_token)).has_value())
      return false;
    offset_x = val16.value();
    start_pos = end_pos + 1;
    end_pos = file_title.find("-", start_pos + 1);
    if (end_pos == std::string_view::npos)
      return false;

    // offset_y
    const std::string_view offset_y_token = file_title.substr(start_pos, end_pos - start_pos);
    if (!(val16 = StringUtil::FromChars<uint16_t>(offset_y_token)).has_value())
      return false;
    offset_y = val16.value();
    start_pos = end_pos + 1;
    end_pos = file_title.find("x", start_pos + 1);
    if (end_pos == std::string_view::npos)
      return false;

    // width
    const std::string_view width_token = file_title.substr(start_pos, end_pos - start_pos);
    if (!(val16 = StringUtil::FromChars<uint16_t>(width_token)).has_value())
      return false;
    width = val16.value();
    if (width == 0)
      return false;
    start_pos = end_pos + 1;
    end_pos = file_title.find("-", start_pos + 1);
    if (end_pos == std::string_view::npos)
      return false;

    // height
    const std::string_view height_token = file_title.substr(start_pos, end_pos - start_pos);
    if (!(val16 = StringUtil::FromChars<uint16_t>(height_token)).has_value())
      return false;
    height = val16.value();
    if (height == 0)
      return false;
    start_pos = end_pos + 1;
    end_pos = file_title.find("-", start_pos + 1);
    if (end_pos == std::string_view::npos || file_title[start_pos] != 'P')
      return false;

    // pal_min
    const std::string_view pal_min_token = file_title.substr(start_pos + 1, end_pos - start_pos - 1);
    if (!(val8 = StringUtil::FromChars<uint8_t>(pal_min_token)).has_value())
      return false;
    pal_min = val8.value();
    start_pos = end_pos + 1;

    // pal_max
    const std::string_view pal_max_token = file_title.substr(start_pos);
    if (!(val8 = StringUtil::FromChars<uint8_t>(pal_max_token)).has_value())
      return false;
    pal_max = val8.value();
    if (pal_min > pal_max)
      return false;
  }
  else
  {
    start_pos = end_pos + 1;
    end_pos = file_title.find("x", start_pos + 1);
    if (end_pos == std::string_view::npos)
      return false;

    // src_width
    const std::string_view src_width_token = file_title.substr(start_pos, end_pos - start_pos);
    if (!(val16 = StringUtil::FromChars<uint16_t>(src_width_token)).has_value())
      return false;
    src_width = val16.value();
    if (src_width == 0)
      return false;
    start_pos = end_pos + 1;
    end_pos = file_title.find("-", start_pos + 1);
    if (end_pos == std::string_view::npos)
      return false;

    // src_height
    const std::string_view src_height_token = file_title.substr(start_pos, end_pos - start_pos);
    if (!(val16 = StringUtil::FromChars<uint16_t>(src_height_token)).has_value())
      return false;
    src_height = val16.value();
    if (src_height == 0)
      return false;
    start_pos = end_pos + 1;
    end_pos = file_title.find("-", start_pos + 1);
    if (end_pos == std::string_view::npos)
      return false;

    // offset_x
    const std::string_view offset_x_token = file_title.substr(start_pos, end_pos - start_pos);
    if (!(val16 = StringUtil::FromChars<uint16_t>(offset_x_token)).has_value())
      return false;
    offset_x = val16.value();
    start_pos = end_pos + 1;
    end_pos = file_title.find("-", start_pos + 1);
    if (end_pos == std::string_view::npos)
      return false;

    // offset_y
    const std::string_view offset_y_token = file_title.substr(start_pos, end_pos - start_pos);
    if (!(val16 = StringUtil::FromChars<uint16_t>(offset_y_token)).has_value())
      return false;
    offset_y = val16.value();
    start_pos = end_pos + 1;
    end_pos = file_title.find("x", start_pos + 1);
    if (end_pos == std::string_view::npos)
      return false;

    // width
    const std::string_view width_token = file_title.substr(start_pos, end_pos - start_pos);
    if (!(val16 = StringUtil::FromChars<uint16_t>(width_token)).has_value())
      return false;
    width = val16.value();
    if (width == 0)
      return false;
    start_pos = end_pos + 1;

    // height
    const std::string_view height_token = file_title.substr(start_pos);
    if (!(val16 = StringUtil::FromChars<uint16_t>(height_token)).has_value())
      return false;
    height = val16.value();
    if (height == 0)
      return false;
  }

  return true;
}

size_t TextureReplacements::PageCacheKeyHash::operator()(const PageCacheKey& k) const
{
  size_t seed = std::hash<uint32_t>{}(k.page);
  hash_combine(seed, k.mode, k.palette_x, k.palette_y);
  return seed;
}

void TextureReplacements::SetVRAM(const uint16_t* vram)
{
  if (m_vram == vram)
    return;

  m_vram = vram;
  m_page_cache.clear();
  m_page_cache_bytes = 0;
}

void TextureReplacements::SetMaxTextureSize(uint32_t size)
{
  if (size == 0 || m_max_texture_size == size)
    return;

  m_max_texture_size = size;
  m_page_cache.clear();
  m_page_cache_bytes = 0;
}

bool TextureReplacements::HasTexturePageReplacements() const
{
  for (const auto& entries : m_texpage_replacements)
  {
    if (!entries.empty())
      return true;
  }

  return false;
}

const uint16_t* TextureReplacements::GetPagePointer(uint32_t page) const
{
  const uint32_t start_x = (page & 15u) * VRAM_PAGE_WIDTH;
  const uint32_t start_y = (page >> 4) * VRAM_PAGE_HEIGHT;
  return m_vram + (start_y * VRAM_WIDTH) + start_x;
}

const uint16_t* TextureReplacements::GetPalettePointer(uint32_t palette_x, uint32_t palette_y) const
{
  return m_vram + (palette_y * VRAM_WIDTH) + palette_x;
}

uint64_t TextureReplacements::HashPage(uint32_t page, GPUTextureMode mode) const
{
  XXH3_state_t state;
  XXH3_64bits_reset(&state);

  const uint16_t* page_ptr = GetPagePointer(page);

  switch (static_cast<uint8_t>(mode) & 3u)
  {
    case static_cast<uint8_t>(GPUTextureMode::Palette4Bit):
    {
      for (uint32_t y = 0; y < VRAM_PAGE_HEIGHT; y++)
      {
        XXH3_64bits_update(&state, page_ptr, VRAM_PAGE_WIDTH * sizeof(uint16_t));
        page_ptr += VRAM_WIDTH;
      }
    }
    break;

    case static_cast<uint8_t>(GPUTextureMode::Palette8Bit):
    {
      for (uint32_t y = 0; y < VRAM_PAGE_HEIGHT; y++)
      {
        XXH3_64bits_update(&state, page_ptr, VRAM_PAGE_WIDTH * 2 * sizeof(uint16_t));
        page_ptr += VRAM_WIDTH;
      }
    }
    break;

    default:
    {
      for (uint32_t y = 0; y < VRAM_PAGE_HEIGHT; y++)
      {
        XXH3_64bits_update(&state, page_ptr, VRAM_PAGE_WIDTH * 4 * sizeof(uint16_t));
        page_ptr += VRAM_WIDTH;
      }
    }
    break;
  }

  return XXH3_64bits_digest(&state);
}

uint64_t TextureReplacements::HashPalette(uint32_t palette_x, uint32_t palette_y, GPUTextureMode mode) const
{
  const uint16_t* base = GetPalettePointer(palette_x, palette_y);

  if ((static_cast<uint8_t>(mode) & 3u) == static_cast<uint8_t>(GPUTextureMode::Palette4Bit))
    return XXH3_64bits(base, sizeof(uint16_t) * 16);

  // 8-bit palettes can wrap around the right edge of VRAM; only hash what
  // actually exists there (games like Metal Gear Solid rely on this).
  if ((palette_x + 256) > VRAM_WIDTH)
    return XXH3_64bits(base, sizeof(uint16_t) * (VRAM_WIDTH - palette_x));

  return XXH3_64bits(base, sizeof(uint16_t) * 256);
}

uint64_t TextureReplacements::HashPartialPalette(uint32_t palette_x, uint32_t palette_y, uint32_t min, uint32_t max) const
{
  const uint32_t size = max - min + 1;
  return XXH3_64bits(GetPalettePointer(palette_x, palette_y), sizeof(uint16_t) * size);
}

uint64_t TextureReplacements::HashRect(uint32_t left, uint32_t top, uint32_t width, uint32_t height) const
{
  XXH3_state_t state;
  XXH3_64bits_reset(&state);

  const uint16_t* ptr = m_vram + (top * VRAM_WIDTH) + left;
  for (uint32_t y = 0; y < height; y++)
  {
    XXH3_64bits_update(&state, ptr, width * sizeof(uint16_t));
    ptr += VRAM_WIDTH;
  }

  return XXH3_64bits_digest(&state);
}

bool TextureReplacements::IsMatchingReplacementPalette(uint64_t full_palette_hash, GPUTextureMode mode,
                                                       uint32_t palette_x, uint32_t palette_y,
                                                       const TexturePageReplacementName& name) const
{
  if (!TextureModeHasPalette(mode))
    return true;

  const uint32_t full_pal_max = GetPaletteWidth(mode) - 1u;
  if (name.pal_min == 0 && name.pal_max == full_pal_max)
    return (name.pal_hash == full_palette_hash);

  // If the range goes off the edge of VRAM, it's not a match.
  if ((palette_x + name.pal_max) >= VRAM_WIDTH)
    return false;

  const uint64_t partial_hash = HashPartialPalette(palette_x, palette_y, name.pal_min, name.pal_max);
  return (partial_hash == name.pal_hash);
}

void TextureReplacements::FindTexturePageMatches(std::vector<ReplacementMatch>& matches, uint32_t page,
                                                 GPUTextureMode mode, uint32_t palette_x, uint32_t palette_y,
                                                 uint64_t page_hash, uint64_t full_palette_hash)
{
  const auto& entries = m_texpage_replacements[static_cast<uint8_t>(mode) & 3u];
  if (entries.empty())
    return;

  const bool has_palette = TextureModeHasPalette(mode);
  const uint32_t page_start_x = (page & 15u) * VRAM_PAGE_WIDTH;
  const uint32_t page_start_y = (page >> 4) * VRAM_PAGE_HEIGHT;
  const uint32_t shift = GetTextureModeShift(mode);

  for (const TexturePageReplacementEntry& entry : entries)
  {
    const TexturePageReplacementName& name = entry.name;

    if (has_palette && !IsMatchingReplacementPalette(full_palette_hash, mode, palette_x, palette_y, name))
      continue;

    uint32_t dst_x, dst_y, dst_width, dst_height;
    if (name.width == TEXPAGE_NATIVE_WIDTH && name.height == TEXPAGE_NATIVE_HEIGHT)
    {
      // Entire page: the already-computed page hash is enough.
      if (name.src_hash != page_hash)
        continue;

      dst_x = 0;
      dst_y = 0;
      dst_width = TEXPAGE_NATIVE_WIDTH;
      dst_height = TEXPAGE_NATIVE_HEIGHT;
    }
    else
    {
      // Sub-rectangle: hash the rectangle the name describes, mapped into VRAM
      // coordinates exactly like DuckStation's GetTexturePageTextureReplacements.
      dst_x = name.offset_x;
      dst_y = name.offset_y;
      dst_width = name.width;
      dst_height = name.height;

      const uint32_t left = page_start_x + (dst_x >> shift);
      const uint32_t top = page_start_y + dst_y;
      const uint32_t right = page_start_x + ((dst_x + dst_width) >> shift);
      if (right <= left || right > VRAM_WIDTH || (top + dst_height) > VRAM_HEIGHT)
      {
        // Malformed/out-of-range rectangle; never hash out of bounds.
        continue;
      }

      const uint64_t hash = HashRect(left, top, right - left, dst_height);
      if (name.src_hash != hash)
        continue;
    }

    const TextureReplacementTexture* image = LoadTexture(entry.filename);
    if (!image || image->GetWidth() == 0 || image->GetHeight() == 0)
      continue;

    ReplacementMatch match;
    match.entry = &entry;
    match.scale_x = static_cast<float>(image->GetWidth()) / static_cast<float>(std::max<uint32_t>(1, name.width));
    match.scale_y = static_cast<float>(image->GetHeight()) / static_cast<float>(std::max<uint32_t>(1, name.height));
    match.dst_x = dst_x;
    match.dst_y = dst_y;
    match.dst_width = dst_width;
    match.dst_height = dst_height;
    matches.push_back(match);
  }
}

void TextureReplacements::DecodePage(std::vector<uint32_t>& pixels, uint32_t page, GPUTextureMode mode,
                                     uint32_t palette_x, uint32_t palette_y) const
{
  pixels.resize(TEXPAGE_NATIVE_WIDTH * TEXPAGE_NATIVE_HEIGHT);

  const uint16_t* page_ptr = GetPagePointer(page);
  const uint16_t* palette = TextureModeHasPalette(mode) ? GetPalettePointer(palette_x, palette_y) : nullptr;
  // DuckStation reads palette entries contiguously (a wrapped 8-bit palette
  // runs into the next VRAM row); clamp to the end of VRAM so a malformed
  // palette at the very last row can never read out of bounds.
  const size_t palette_remaining =
    TextureModeHasPalette(mode) ? ((static_cast<size_t>(VRAM_WIDTH) * VRAM_HEIGHT) -
                                   ((static_cast<size_t>(palette_y) * VRAM_WIDTH) + palette_x)) :
                                  0;
  uint32_t* dest = pixels.data();

  switch (static_cast<uint8_t>(mode) & 3u)
  {
    case static_cast<uint8_t>(GPUTextureMode::Palette4Bit):
    {
      for (uint32_t y = 0; y < TEXPAGE_NATIVE_HEIGHT; y++)
      {
        const uint16_t* row = page_ptr;
        for (uint32_t x = 0; x < TEXPAGE_NATIVE_WIDTH; x++)
        {
          const uint16_t word = row[x >> 2];
          uint32_t index = (word >> ((x & 3u) * 4u)) & 0x0Fu;
          if (index >= palette_remaining)
            index = 0;
          *(dest++) = VRAMRGBA5551ToRGBA8888(palette[index]);
        }

        page_ptr += VRAM_WIDTH;
      }
    }
    break;

    case static_cast<uint8_t>(GPUTextureMode::Palette8Bit):
    {
      for (uint32_t y = 0; y < TEXPAGE_NATIVE_HEIGHT; y++)
      {
        const uint16_t* row = page_ptr;
        for (uint32_t x = 0; x < TEXPAGE_NATIVE_WIDTH; x++)
        {
          const uint16_t word = row[x >> 1];
          uint32_t index = (word >> ((x & 1u) * 8u)) & 0xFFu;
          if (index >= palette_remaining)
            index = 0;
          *(dest++) = VRAMRGBA5551ToRGBA8888(palette[index]);
        }

        page_ptr += VRAM_WIDTH;
      }
    }
    break;

    default:
    {
      for (uint32_t y = 0; y < TEXPAGE_NATIVE_HEIGHT; y++)
      {
        const uint16_t* row = page_ptr;
        for (uint32_t x = 0; x < TEXPAGE_NATIVE_WIDTH; x++)
          *(dest++) = VRAMRGBA5551ToRGBA8888(row[x]);

        page_ptr += VRAM_WIDTH;
      }
    }
    break;
  }
}

static uint32_t LerpRGBA8(uint32_t a, uint32_t b, float t)
{
  const float r = static_cast<float>(a & 0xFFu) + ((static_cast<float>(b & 0xFFu) - static_cast<float>(a & 0xFFu)) * t);
  const float g = static_cast<float>((a >> 8) & 0xFFu) +
                  ((static_cast<float>((b >> 8) & 0xFFu) - static_cast<float>((a >> 8) & 0xFFu)) * t);
  const float bval = static_cast<float>((a >> 16) & 0xFFu) +
                     ((static_cast<float>((b >> 16) & 0xFFu) - static_cast<float>((a >> 16) & 0xFFu)) * t);
  const float al = static_cast<float>((a >> 24) & 0xFFu) +
                   ((static_cast<float>((b >> 24) & 0xFFu) - static_cast<float>((a >> 24) & 0xFFu)) * t);

  const uint32_t ri = static_cast<uint32_t>(std::clamp(r + 0.5f, 0.0f, 255.0f));
  const uint32_t gi = static_cast<uint32_t>(std::clamp(g + 0.5f, 0.0f, 255.0f));
  const uint32_t bi = static_cast<uint32_t>(std::clamp(bval + 0.5f, 0.0f, 255.0f));
  const uint32_t ai = static_cast<uint32_t>(std::clamp(al + 0.5f, 0.0f, 255.0f));
  return ri | (gi << 8) | (bi << 16) | (ai << 24);
}

static uint32_t SampleImageBilinear(const uint32_t* pixels, uint32_t width, uint32_t height, float u, float v)
{
  const float fx = (u * static_cast<float>(width)) - 0.5f;
  const float fy = (v * static_cast<float>(height)) - 0.5f;
  const int32_t x0 = static_cast<int32_t>(std::floor(fx));
  const int32_t y0 = static_cast<int32_t>(std::floor(fy));
  const float tx = fx - static_cast<float>(x0);
  const float ty = fy - static_cast<float>(y0);

  const int32_t x1 = std::clamp<int32_t>(x0 + 1, 0, static_cast<int32_t>(width) - 1);
  const int32_t y1 = std::clamp<int32_t>(y0 + 1, 0, static_cast<int32_t>(height) - 1);
  const int32_t cx0 = std::clamp<int32_t>(x0, 0, static_cast<int32_t>(width) - 1);
  const int32_t cy0 = std::clamp<int32_t>(y0, 0, static_cast<int32_t>(height) - 1);

  const uint32_t p00 = pixels[cy0 * width + cx0];
  const uint32_t p10 = pixels[cy0 * width + x1];
  const uint32_t p01 = pixels[y1 * width + cx0];
  const uint32_t p11 = pixels[y1 * width + x1];

  return LerpRGBA8(LerpRGBA8(p00, p10, tx), LerpRGBA8(p01, p11, tx), ty);
}

std::shared_ptr<Common::RGBA8Image> TextureReplacements::ComposePage(const std::vector<ReplacementMatch>& matches,
                                                                     uint32_t page, GPUTextureMode mode,
                                                                     uint32_t palette_x, uint32_t palette_y,
                                                                     float* out_scale_x, float* out_scale_y)
{
  std::vector<uint32_t> base_pixels;
  DecodePage(base_pixels, page, mode, palette_x, palette_y);

  float max_scale_x = 1.0f;
  float max_scale_y = 1.0f;
  for (const ReplacementMatch& match : matches)
  {
    max_scale_x = std::max(max_scale_x, match.scale_x);
    max_scale_y = std::max(max_scale_y, match.scale_y);
  }

  // Clamp to the largest texture the renderer can bind. DuckStation does the
  // same against the device's max texture size.
  const float max_possible_scale = static_cast<float>(m_max_texture_size) / static_cast<float>(TEXPAGE_NATIVE_WIDTH);
  max_scale_x = std::min(max_scale_x, max_possible_scale);
  max_scale_y = std::min(max_scale_y, max_possible_scale);
  if (!(max_scale_x > 0.0f) || !(max_scale_y > 0.0f))
    return nullptr;

  const uint32_t out_width =
    std::max<uint32_t>(1, static_cast<uint32_t>(std::ceil(static_cast<float>(TEXPAGE_NATIVE_WIDTH) * max_scale_x)));
  const uint32_t out_height =
    std::max<uint32_t>(1, static_cast<uint32_t>(std::ceil(static_cast<float>(TEXPAGE_NATIVE_HEIGHT) * max_scale_y)));

  auto image = std::make_shared<Common::RGBA8Image>();
  image->SetSize(out_width, out_height);
  uint32_t* out_pixels = image->GetPixels();

  // Upscale the decoded page with nearest filtering (matches DuckStation's
  // nearest upscale of the source texture page).
  for (uint32_t y = 0; y < out_height; y++)
  {
    const uint32_t src_y = std::min<uint32_t>(
      static_cast<uint32_t>((static_cast<uint64_t>(y) * TEXPAGE_NATIVE_HEIGHT) / out_height), TEXPAGE_NATIVE_HEIGHT - 1);
    const uint32_t* src_row = &base_pixels[static_cast<size_t>(src_y) * TEXPAGE_NATIVE_WIDTH];
    uint32_t* dst_row = out_pixels + (static_cast<size_t>(y) * out_width);
    for (uint32_t x = 0; x < out_width; x++)
    {
      const uint32_t src_x = std::min<uint32_t>(
        static_cast<uint32_t>((static_cast<uint64_t>(x) * TEXPAGE_NATIVE_WIDTH) / out_width), TEXPAGE_NATIVE_WIDTH - 1);
      dst_row[x] = src_row[src_x];
    }
  }

  // Overlay each matching replacement image, scaled to its destination rect.
  for (const ReplacementMatch& match : matches)
  {
    const TextureReplacementTexture* tex = LoadTexture(match.entry->filename);
    if (!tex || tex->GetWidth() == 0 || tex->GetHeight() == 0)
      continue;

    const uint32_t dst_x0 = std::min<uint32_t>(static_cast<uint32_t>(static_cast<float>(match.dst_x) * max_scale_x), out_width);
    const uint32_t dst_x1 =
      std::min<uint32_t>(static_cast<uint32_t>(static_cast<float>(match.dst_x + match.dst_width) * max_scale_x), out_width);
    const uint32_t dst_y0 = std::min<uint32_t>(static_cast<uint32_t>(static_cast<float>(match.dst_y) * max_scale_y), out_height);
    const uint32_t dst_y1 =
      std::min<uint32_t>(static_cast<uint32_t>(static_cast<float>(match.dst_y + match.dst_height) * max_scale_y), out_height);
    if (dst_x1 <= dst_x0 || dst_y1 <= dst_y0)
      continue;

    const bool semitransparent = match.entry->name.IsSemitransparent();
    const uint32_t src_width = tex->GetWidth();
    const uint32_t src_height = tex->GetHeight();
    const uint32_t* src_pixels = tex->GetPixels();
    const float rcp_width = 1.0f / static_cast<float>(dst_x1 - dst_x0);
    const float rcp_height = 1.0f / static_cast<float>(dst_y1 - dst_y0);

    for (uint32_t y = dst_y0; y < dst_y1; y++)
    {
      const float v = (static_cast<float>(y - dst_y0) + 0.5f) * rcp_height;
      uint32_t* dst_row = out_pixels + (static_cast<size_t>(y) * out_width);
      for (uint32_t x = dst_x0; x < dst_x1; x++)
      {
        const float u = (static_cast<float>(x - dst_x0) + 0.5f) * rcp_width;
        const uint32_t pixel = SampleImageBilinear(src_pixels, src_width, src_height, u, v);

        if (semitransparent)
        {
          // Semitransparent replacement images encode opacity; map anything
          // which isn't fully opaque to the PSX STP bit (bit 15). 0000h stays
          // fully transparent. Mirrors DuckStation's replacement merge shader.
          if (pixel == 0)
            dst_row[x] = 0;
          else
            dst_row[x] = (pixel & 0x00FFFFFFu) | (((pixel >> 24) <= 242u) ? 0xFF000000u : 0x00000000u);
        }
        else
        {
          // Opaque replacements: sub-0.5 alpha becomes transparent, everything
          // else becomes an opaque texel. The low alpha value keeps the texel
          // from being transparency-culled while leaving bit 15 clear.
          if ((pixel >> 24) >= 128u)
            dst_row[x] = (pixel & 0x00FFFFFFu) | 0x02000000u;
          else
            dst_row[x] = 0;
        }
      }
    }
  }

  *out_scale_x = max_scale_x;
  *out_scale_y = max_scale_y;
  return image;
}

void TextureReplacements::EvictPageReplacementsForBudget(size_t incoming_bytes)
{
  while ((m_page_cache_bytes + incoming_bytes) > TEXTURE_PAGE_CACHE_BUDGET_BYTES && !m_page_cache.empty())
  {
    auto oldest = m_page_cache.begin();
    for (auto it = std::next(m_page_cache.begin()); it != m_page_cache.end(); ++it)
    {
      if (it->second.last_used < oldest->second.last_used)
        oldest = it;
    }

    m_page_cache_bytes -= oldest->second.size_bytes;
    m_page_cache.erase(oldest);
  }
}

const TexturePageReplacement* TextureReplacements::GetTexturePageReplacement(GPUTextureMode mode, uint32_t texture_page_x,
                                                                             uint32_t texture_page_y, uint32_t palette_x,
                                                                             uint32_t palette_y)
{
  if (!m_vram || m_game_id.empty() || texture_page_x >= VRAM_WIDTH || texture_page_y >= VRAM_HEIGHT ||
      palette_x >= VRAM_WIDTH || palette_y >= VRAM_HEIGHT)
  {
    return nullptr;
  }

  // Canonicalize the "reserved" direct16 mode; the runtime never uses it, but
  // the register can technically hold it. The raw_texture/ST bit does not
  // participate in matching (mirrors DuckStation's GetIndex()), so it is
  // dropped here; the entry's own bit selects the alpha compositing path.
  uint32_t mode_bits = static_cast<uint8_t>(mode) & 3u;
  if (mode_bits == static_cast<uint8_t>(GPUTextureMode::Reserved_Direct16Bit))
    mode_bits = static_cast<uint8_t>(GPUTextureMode::Direct16Bit);

  const uint32_t mode_index = mode_bits;
  if (m_texpage_replacements[mode_index].empty())
    return nullptr;

  const uint32_t page = ((texture_page_y / VRAM_PAGE_HEIGHT) * 16u) + (texture_page_x / VRAM_PAGE_WIDTH);
  const uint64_t generation = g_gpu ? static_cast<uint64_t>(g_gpu->GetVRAMGeneration()) : 0;

  const PageCacheKey key = {page, mode_index, palette_x, palette_y};
  auto it = m_page_cache.find(key);
  if (it != m_page_cache.end() && it->second.vram_generation == generation)
  {
    it->second.last_used = ++m_page_cache_used_counter;
    return (it->second.replacement.id != 0) ? &it->second.replacement : nullptr;
  }

  const GPUTextureMode canonical_mode = static_cast<GPUTextureMode>(mode_index);
  const uint64_t page_hash = HashPage(page, canonical_mode);
  const uint64_t full_palette_hash =
    TextureModeHasPalette(canonical_mode) ? HashPalette(palette_x, palette_y, canonical_mode) : 0;

  std::vector<ReplacementMatch> matches;
  FindTexturePageMatches(matches, page, canonical_mode, palette_x, palette_y, page_hash, full_palette_hash);

  // The signature covers the matched replacement entries *and* the hashed
  // source data, because the composite also contains the decoded base page.
  size_t match_signature = 0;
  hash_combine(match_signature, page_hash, full_palette_hash);
  for (const ReplacementMatch& match : matches)
    hash_combine(match_signature, match.entry);

  if (it != m_page_cache.end() && match_signature == it->second.match_signature)
  {
    // Same match set as last time; the generation bump was caused by VRAM
    // writes elsewhere, so the cached composite is still valid.
    it->second.vram_generation = generation;
    it->second.last_used = ++m_page_cache_used_counter;
    return (it->second.replacement.id != 0) ? &it->second.replacement : nullptr;
  }

  CachedPage cached;
  cached.match_signature = match_signature;
  cached.vram_generation = generation;
  cached.last_used = ++m_page_cache_used_counter;
  cached.replacement.id = 0;

  if (!matches.empty())
  {
    float scale_x = 1.0f;
    float scale_y = 1.0f;
    std::shared_ptr<Common::RGBA8Image> image =
      ComposePage(matches, page, canonical_mode, palette_x, palette_y, &scale_x, &scale_y);
    if (image)
    {
      cached.replacement.image = std::move(image);
      cached.replacement.id = m_next_replacement_id++;
      cached.replacement.vram_page_start_x = (page & 15u) * VRAM_PAGE_WIDTH;
      cached.replacement.vram_page_start_y = (page >> 4) * VRAM_PAGE_HEIGHT;
      // In expanded texel space a page is 256x256 for every mode.
      cached.replacement.source_width = TEXPAGE_NATIVE_WIDTH;
      cached.replacement.source_height = TEXPAGE_NATIVE_HEIGHT;
      cached.replacement.scale_x = scale_x;
      cached.replacement.scale_y = scale_y;
      cached.size_bytes = static_cast<size_t>(cached.replacement.image->GetWidth()) *
                          static_cast<size_t>(cached.replacement.image->GetHeight()) * sizeof(uint32_t);

      Log_InfoPrintf("TexPage replacement: page=%u mode=%s %ux%u scale=%.2fx%.2f matches=%zu",
                     page, s_texture_replacement_mode_names[mode_index], cached.replacement.image->GetWidth(),
                     cached.replacement.image->GetHeight(), scale_x, scale_y, matches.size());
    }
  }

  if (it != m_page_cache.end())
  {
    m_page_cache_bytes -= it->second.size_bytes;
    it->second = std::move(cached);
  }
  else
  {
    EvictPageReplacementsForBudget(cached.size_bytes);
    it = m_page_cache.emplace(key, std::move(cached)).first;
  }

  m_page_cache_bytes += it->second.size_bytes;
  return (it->second.replacement.id != 0) ? &it->second.replacement : nullptr;
}
