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
static constexpr size_t TEXTURE_CACHE_BUDGET_BYTES = 256u * 1024u * 1024u;

// Composited texture pages (RGBA8, upscaled) get their own budget. A single
// full-page composite is 256x256x4 bytes at 1x, or up to ~64 MiB at 4K packs.
static constexpr size_t TEXTURE_PAGE_CACHE_BUDGET_BYTES = 512u * 1024u * 1024u;

// Upper bound on the composite upscale factor. HD packs ship replacement
// images at 4x, but a 256x256 page composited at 4x is 4 MiB and games can
// reference hundreds of distinct page/palette pairs per frame: the working set
// then dwarfs any sane cache and every draw recomposites and re-uploads
// multi-megabyte textures. Capping at 2x keeps the images above the native
// page resolution while cutting composite memory and upload traffic 4x, which
// is what makes the page cache able to hold a game's live working set.
static constexpr float TEXPAGE_COMPOSITE_MAX_SCALE = 2.0f;

// VRAM page layout.
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

TextureReplacements::~TextureReplacements()
{
  StopTextureLoader();
  StopComposeWorker();
}

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
  StopTextureLoader();
  StopComposeWorker();
  m_texture_pending_hit = false;
  m_texture_cache.clear();
  m_texture_lru.clear();
  m_texture_lru_positions.clear();
  m_texture_cache_bytes = 0;
  m_vram_write_replacements.clear();
  for (auto& entries : m_texpage_replacements)
    entries.clear();
  for (auto& entries : m_texupload_replacements)
    entries.clear();
  m_texupload_replacement_count = 0;
  ClearVRAMWriteRecords();
  m_page_cache.clear();
  m_page_cache_bytes = 0;
  m_page_hash_cache.clear();
  m_palette_hash_cache.clear();
  m_rect_hash_cache.clear();
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
  for (auto& entries : m_texupload_replacements)
    entries.clear();
  m_texupload_replacement_count = 0;
  ClearVRAMWriteRecords();
  m_page_cache.clear();
  m_page_cache_bytes = 0;
  m_page_hash_cache.clear();
  m_palette_hash_cache.clear();
  m_rect_hash_cache.clear();
  m_texture_pending_hit = false;
  {
    std::lock_guard<std::mutex> lock(m_texture_loader_mutex);
    m_texture_loader_queue.clear();
    m_texture_loader_completed.clear();
  }
  m_texture_load_pending.clear();
  {
    // Jobs running against the old game's packs are dropped by the key lookup
    // or the serial comparison when they complete.
    std::lock_guard<std::mutex> lock(m_compose_mutex);
    m_compose_queue.clear();
    m_compose_completed.clear();
  }

  if (g_settings.texture_replacements.AnyReplacementsEnabled())
    FindTextures(GetSourceDirectory());

  if (g_settings.texture_replacements.preload_textures)
    PreloadTextures();

  PurgeUnreferencedTexturesFromCache();
}

void TextureReplacements::ClearVRAMWriteRecords()
{
  m_vram_writes.clear();
  m_vram_write_order.clear();
}

void TextureReplacements::RecordVRAMWrite(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
  // Nothing to match against if the pack has no texupload entries; skip the
  // bookkeeping entirely so packs that don't use them cost nothing.
  bool any_texupload = false;
  for (const auto& entries : m_texupload_replacements)
  {
    if (!entries.empty())
    {
      any_texupload = true;
      break;
    }
  }
  if (!any_texupload || !m_vram || width == 0 || height == 0)
    return;

  x %= VRAM_WIDTH;
  y %= VRAM_HEIGHT;
  if (x + width <= VRAM_WIDTH && y + height <= VRAM_HEIGHT)
  {
    AddVRAMWriteRecord(x, y, width, height);
    return;
  }

  // Wrapped uploads are split into per-row/per-column chunks so every record
  // describes a contiguous VRAM rectangle (the same shape the pack hashes).
  const uint32_t first_width = std::min(width, VRAM_WIDTH - x);
  const uint32_t first_height = std::min(height, VRAM_HEIGHT - y);
  const auto add = [this](uint32_t left, uint32_t top, uint32_t w, uint32_t h) {
    if (w > 0 && h > 0)
      AddVRAMWriteRecord(left, top, w, h);
  };
  add(x, y, first_width, first_height);
  add(0, y, width - first_width, first_height);
  add(x, 0, first_width, height - first_height);
  add(0, 0, width - first_width, height - first_height);
}

void TextureReplacements::AddVRAMWriteRecord(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
  const uint64_t key = (static_cast<uint64_t>(x) << 40) | (static_cast<uint64_t>(y) << 30) |
                       (static_cast<uint64_t>(width) << 15) | static_cast<uint64_t>(height);
  if (m_vram_writes.find(key) != m_vram_writes.end())
    return;

  if (m_vram_writes.size() >= MAX_VRAM_WRITE_RECORDS && !m_vram_write_order.empty())
  {
    const uint64_t oldest = m_vram_write_order.front();
    m_vram_write_order.pop_front();
    m_vram_writes.erase(oldest);
  }

  m_vram_writes.emplace(key, VRAMWriteRecord{x, y, width, height});
  m_vram_write_order.push_back(key);
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
    m_texture_cache_bytes += static_cast<size_t>(it.second->GetWidth()) * it.second->GetHeight() * 4;
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
      m_texture_cache_bytes -= static_cast<size_t>(it->second->GetWidth()) * it->second->GetHeight() * 4;
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

    // ST* variants share the bucket with their base mode (matching ignores the
    // semitransparency bit; it only affects how the replacement is composited).
    uint32_t entry_mode = name.texture_mode & 3u;
    if (entry_mode == static_cast<uint32_t>(GPUTextureMode::Reserved_Direct16Bit))
      entry_mode = static_cast<uint32_t>(GPUTextureMode::Direct16Bit);

    // texpage entries anchor to the composited page; texupload entries anchor
    // to the VRAM write rect they were dumped from and are matched against the
    // recorded uploads at draw time.
    auto& entries = is_texupload ? m_texupload_replacements[entry_mode] : m_texpage_replacements[entry_mode];
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
    if (is_texupload)
      m_texupload_replacement_count++;
  }

  size_t texpage_count = 0;
  for (const auto& entries : m_texpage_replacements)
    texpage_count += entries.size();

  Log_InfoPrintf("Found %zu replacement VRAM writes, %zu texture pages and %u texture uploads for '%s'",
                 m_vram_write_replacements.size(), texpage_count, m_texupload_replacement_count, m_game_id.c_str());
}

std::shared_ptr<TextureReplacementTexture> TextureReplacements::AcquireTexture(const std::string& filename)
{
  auto it = m_texture_cache.find(filename);
  if (it == m_texture_cache.end())
  {
    // Pick up anything the worker finished since the last lookup. Decoded
    // images enter the cache on this thread only.
    DrainLoadedTextures();
    it = m_texture_cache.find(filename);
  }

  if (it == m_texture_cache.end())
  {
    // Never decode on the emulation thread. The caller treats the texture as
    // missing for this frame and the page is recomposited once it arrives.
    QueueTextureLoad(filename);
    m_texture_pending_hit = true;
    return nullptr;
  }

  TouchTextureCacheEntry(filename);
  return it->second;
}

const TextureReplacementTexture* TextureReplacements::LoadTexture(const std::string& filename)
{
  const std::shared_ptr<TextureReplacementTexture> texture = AcquireTexture(filename);
  return texture ? texture.get() : nullptr;
}

void TextureReplacements::InsertDecodedTexture(std::string filename, TextureReplacementTexture image)
{
  const auto existing = m_texture_cache.find(filename);
  if (existing != m_texture_cache.end())
    return;

  const size_t image_bytes = static_cast<size_t>(image.GetWidth()) * image.GetHeight() * 4;
  EvictTexturesForBudget(image_bytes, filename);

  auto inserted =
    m_texture_cache.emplace(std::move(filename), std::make_shared<TextureReplacementTexture>(std::move(image))).first;
  m_texture_lru.push_back(inserted->first);
  m_texture_lru_positions[inserted->first] = std::prev(m_texture_lru.end());
  m_texture_cache_bytes += image_bytes;
}

void TextureReplacements::StartTextureLoader()
{
  if (m_texture_loader_thread.joinable())
    return;

  m_texture_loader_stop = false;
  m_texture_loader_thread = std::thread(&TextureReplacements::TextureLoaderEntry, this);
}

void TextureReplacements::StopTextureLoader()
{
  if (m_texture_loader_thread.joinable())
  {
    {
      std::lock_guard<std::mutex> lock(m_texture_loader_mutex);
      m_texture_loader_stop = true;
    }
    m_texture_loader_cv.notify_all();
    m_texture_loader_thread.join();
  }

  std::lock_guard<std::mutex> lock(m_texture_loader_mutex);
  m_texture_loader_queue.clear();
  m_texture_loader_completed.clear();
  m_texture_loader_stop = false;
  m_texture_load_pending.clear();
}

void TextureReplacements::QueueTextureLoad(const std::string& filename)
{
  if (m_texture_load_pending.size() >= MAX_TEXTURE_LOAD_REQUESTS)
    return;

  if (!m_texture_load_pending.insert(filename).second)
    return;

  StartTextureLoader();

  {
    std::lock_guard<std::mutex> lock(m_texture_loader_mutex);
    m_texture_loader_queue.push_back(filename);
  }
  m_texture_loader_cv.notify_one();
}

void TextureReplacements::TextureLoaderEntry(TextureReplacements* self)
{
  for (;;)
  {
    std::string filename;
    {
      std::unique_lock<std::mutex> lock(self->m_texture_loader_mutex);
      self->m_texture_loader_cv.wait(lock, [self] {
        return self->m_texture_loader_stop || !self->m_texture_loader_queue.empty();
      });

      if (self->m_texture_loader_queue.empty())
      {
        if (self->m_texture_loader_stop)
          return;
        continue;
      }

      filename = std::move(self->m_texture_loader_queue.front());
      self->m_texture_loader_queue.pop_front();
    }

    DecodedTexture decoded;
    decoded.filename = filename;
    if (!Common::LoadImageFromFile(&decoded.image, filename.c_str()))
    {
      // Cache the failure as an empty image so the page lookup does not keep
      // waiting for a file that will never decode.
      Log_ErrorPrintf("Failed to load '%s'", filename.c_str());
    }
    else
    {
      Log_InfoPrintf("Loaded '%s': %ux%u", filename.c_str(), decoded.image.GetWidth(), decoded.image.GetHeight());
    }

    {
      std::lock_guard<std::mutex> lock(self->m_texture_loader_mutex);
      self->m_texture_loader_completed.push_back(std::move(decoded));
    }
  }
}

void TextureReplacements::DrainLoadedTextures()
{
  std::deque<DecodedTexture> completed;
  {
    std::lock_guard<std::mutex> lock(m_texture_loader_mutex);
    if (m_texture_loader_completed.empty())
      return;
    completed.swap(m_texture_loader_completed);
  }

  for (DecodedTexture& decoded : completed)
  {
    m_texture_load_pending.erase(decoded.filename);
    InsertDecodedTexture(std::move(decoded.filename), std::move(decoded.image));
  }

  // Pages composited while these images were still loading need a rebuild.
  m_texture_load_generation.fetch_add(1, std::memory_order_relaxed);
}

bool TextureReplacements::BuildComposeJob(const PageCacheKey& key, uint64_t page_hash, uint64_t palette_hash,
                                          uint32_t mode_index, uint32_t page, uint32_t palette_x, uint32_t palette_y,
                                          const std::vector<ReplacementMatch>& matches, ComposeJob* job)
{
  job->key = key;
  job->page_hash = page_hash;
  job->palette_hash = palette_hash;
  job->max_texture_size = m_max_texture_size;
  job->resolution_scale = m_resolution_scale;
  job->textures_missing = false;

  const GPUTextureMode mode = static_cast<GPUTextureMode>(mode_index);
  const uint32_t page_row_words = VRAM_PAGE_WIDTH << mode_index;
  job->page_row_words = page_row_words;
  job->page_words.resize(static_cast<size_t>(TEXPAGE_NATIVE_HEIGHT) * page_row_words);
  const uint16_t* page_ptr = GetPagePointer(page);
  for (uint32_t y = 0; y < TEXPAGE_NATIVE_HEIGHT; y++)
  {
    std::memcpy(&job->page_words[static_cast<size_t>(y) * page_row_words],
                page_ptr + static_cast<size_t>(y) * VRAM_WIDTH,
                static_cast<size_t>(page_row_words) * sizeof(uint16_t));
  }

  if (TextureModeHasPalette(mode))
  {
    const uint16_t* palette_ptr = GetPalettePointer(palette_x, palette_y);
    const size_t remaining = (static_cast<size_t>(VRAM_WIDTH) * VRAM_HEIGHT) -
                             ((static_cast<size_t>(palette_y) * VRAM_WIDTH) + palette_x);
    const size_t palette_words = std::min<size_t>(GetPaletteWidth(mode), remaining);
    job->palette_words.assign(palette_ptr, palette_ptr + palette_words);
  }
  else
  {
    job->palette_words.clear();
  }

  job->matches.clear();
  job->matches.reserve(matches.size());
  for (const ReplacementMatch& match : matches)
  {
    const std::shared_ptr<TextureReplacementTexture> image = AcquireTexture(match.entry->filename);
    if (!image || image->GetWidth() == 0 || image->GetHeight() == 0)
    {
      job->textures_missing = true;
      continue;
    }

    ResolvedMatch resolved;
    resolved.image = image;
    resolved.semitransparent = match.entry->name.IsSemitransparent();
    resolved.scale_x = match.scale_x;
    resolved.scale_y = match.scale_y;
    resolved.dst_x = match.dst_x;
    resolved.dst_y = match.dst_y;
    resolved.dst_width = match.dst_width;
    resolved.dst_height = match.dst_height;
    resolved.src_x = match.src_x;
    resolved.src_y = match.src_y;
    resolved.src_width = match.src_width;
    resolved.src_height = match.src_height;
    job->matches.push_back(std::move(resolved));
  }

  return !job->matches.empty();
}

void TextureReplacements::StartComposeWorker()
{
  if (m_compose_thread.joinable())
    return;

  m_compose_stop = false;
  m_compose_thread = std::thread(&TextureReplacements::ComposeWorkerEntry, this);
}

void TextureReplacements::StopComposeWorker()
{
  if (m_compose_thread.joinable())
  {
    {
      std::lock_guard<std::mutex> lock(m_compose_mutex);
      m_compose_stop = true;
    }
    m_compose_cv.notify_all();
    m_compose_thread.join();
  }

  std::lock_guard<std::mutex> lock(m_compose_mutex);
  m_compose_queue.clear();
  m_compose_completed.clear();
  m_compose_stop = false;
}

bool TextureReplacements::TryQueueCompose(ComposeJob job)
{
  StartComposeWorker();

  {
    std::lock_guard<std::mutex> lock(m_compose_mutex);
    if (m_compose_queue.size() >= MAX_COMPOSE_QUEUE)
      return false;
    m_compose_queue.push_back(std::move(job));
  }
  m_compose_cv.notify_one();
  return true;
}

void TextureReplacements::ComposeWorkerEntry(TextureReplacements* self)
{
  for (;;)
  {
    ComposeJob job;
    {
      std::unique_lock<std::mutex> lock(self->m_compose_mutex);
      self->m_compose_cv.wait(lock, [self] {
        return self->m_compose_stop || !self->m_compose_queue.empty();
      });

      if (self->m_compose_queue.empty())
      {
        if (self->m_compose_stop)
          return;
        continue;
      }

      job = std::move(self->m_compose_queue.front());
      self->m_compose_queue.pop_front();
    }

    CompletedCompose completed;
    completed.key = job.key;
    completed.serial = job.serial;
    completed.page_hash = job.page_hash;
    completed.palette_hash = job.palette_hash;
    completed.textures_missing = job.textures_missing;

    std::vector<uint32_t> base_pixels;
    DecodeExpandedPage(base_pixels, static_cast<GPUTextureMode>(job.key.mode), job.page_words.data(),
                       job.page_row_words, job.palette_words.empty() ? nullptr : job.palette_words.data(),
                       job.palette_words.size());
    completed.image = ComposePageImage(job.matches, base_pixels, job.max_texture_size, job.resolution_scale,
                                       &completed.scale_x, &completed.scale_y);

    {
      std::lock_guard<std::mutex> lock(self->m_compose_mutex);
      self->m_compose_completed.push_back(std::move(completed));
    }
  }
}

void TextureReplacements::DrainComposedPages()
{
  std::deque<CompletedCompose> completed;
  {
    std::lock_guard<std::mutex> lock(m_compose_mutex);
    if (m_compose_completed.empty())
      return;
    completed.swap(m_compose_completed);
  }

  for (CompletedCompose& done : completed)
  {
    auto it = m_page_cache.find(done.key);
    if (it == m_page_cache.end())
      continue; // page evicted, or the game changed while the job was running

    CachedPage& cached = it->second;
    if (cached.in_flight_serial != done.serial)
      continue; // superseded by a newer rebuild request

    cached.in_flight_serial = 0;
    cached.pending_rebuild = false;
    cached.incomplete = done.textures_missing;
    cached.load_generation = m_texture_load_generation.load(std::memory_order_relaxed);

    const size_t old_size = cached.size_bytes;
    cached.page_hash = done.page_hash;
    cached.palette_hash = done.palette_hash;
    cached.size_bytes = 0;
    m_page_cache_bytes -= old_size;

    if (!done.image)
    {
      cached.replacement.image.reset();
      cached.replacement.id = 0;
      continue;
    }

    cached.replacement.image = std::move(done.image);
    if (cached.replacement.id == 0)
    {
      cached.replacement.id = m_next_replacement_id++;
      cached.replacement.revision = 1;
    }
    else
    {
      cached.replacement.revision++;
    }
    cached.replacement.vram_page_start_x = (done.key.page & 15u) * VRAM_PAGE_WIDTH;
    cached.replacement.vram_page_start_y = (done.key.page >> 4) * VRAM_PAGE_HEIGHT;
    cached.replacement.source_width = TEXPAGE_NATIVE_WIDTH;
    cached.replacement.source_height = TEXPAGE_NATIVE_HEIGHT;
    cached.replacement.scale_x = done.scale_x;
    cached.replacement.scale_y = done.scale_y;
    cached.size_bytes = static_cast<size_t>(cached.replacement.image->GetWidth()) *
                        static_cast<size_t>(cached.replacement.image->GetHeight()) * sizeof(uint32_t);
    m_page_cache_bytes += cached.size_bytes;
    EvictPageReplacementsForBudget(0);
  }
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

  const auto preload_one = [this](const std::string& filename) {
    if (m_texture_cache.find(filename) != m_texture_cache.end())
      return;

    Common::RGBA8Image image;
    if (!Common::LoadImageFromFile(&image, filename.c_str()))
    {
      Log_ErrorPrintf("Failed to load '%s'", filename.c_str());
      return;
    }

    Log_InfoPrintf("Loaded '%s': %ux%u", filename.c_str(), image.GetWidth(), image.GetHeight());
    InsertDecodedTexture(filename, std::move(image));
  };

  for (const auto& it : m_vram_write_replacements)
  {
    UPDATE_PROGRESS();

    preload_one(it.second);
    num_textures_loaded++;
  }

  for (const auto& entries : m_texpage_replacements)
  {
    for (const TexturePageReplacementEntry& entry : entries)
    {
      UPDATE_PROGRESS();

      preload_one(entry.filename);
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

// Parser for the texpage-* filename grammar. The token order differs
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
  hash_combine(seed, k.mode, k.palette_hash);
  return seed;
}

size_t TextureReplacements::RectHashKeyHash::operator()(const RectHashKey& k) const
{
  size_t seed = std::hash<uint32_t>{}(k.left);
  hash_combine(seed, k.top, k.width, k.height);
  return seed;
}

uint64_t TextureReplacements::GetCachedPageHash(uint32_t page, GPUTextureMode mode, uint64_t revision)
{
  const uint32_t key = (page << 3) | (static_cast<uint8_t>(mode) & 7u);
  const auto it = m_page_hash_cache.find(key);
  if (it != m_page_hash_cache.end() && it->second.revision == revision)
    return it->second.hash;

  const uint64_t hash = HashPage(page, mode);
  m_page_hash_cache.insert_or_assign(key, CachedHash{revision, hash});
  return hash;
}

uint64_t TextureReplacements::GetCachedPaletteHash(uint32_t palette_x, uint32_t palette_y, GPUTextureMode mode,
                                                   uint64_t revision)
{
  const uint32_t key = ((palette_y & 0x1FFu) << 13) | ((palette_x & 0x3FFu) << 3) |
                       static_cast<uint32_t>(static_cast<uint8_t>(mode) & 7u);
  const auto it = m_palette_hash_cache.find(key);
  if (it != m_palette_hash_cache.end() && it->second.revision == revision)
    return it->second.hash;

  const uint64_t hash = HashPalette(palette_x, palette_y, mode);
  m_palette_hash_cache.insert_or_assign(key, CachedHash{revision, hash});
  return hash;
}

uint64_t TextureReplacements::GetCachedRectHash(uint64_t page_revision, uint32_t left, uint32_t top, uint32_t width,
                                                 uint32_t height)
{
  const RectHashKey key = {left, top, width, height};
  const auto it = m_rect_hash_cache.find(key);
  if (it != m_rect_hash_cache.end() && it->second.revision == page_revision)
    return it->second.hash;

  if (m_rect_hash_cache.size() > 65536)
    m_rect_hash_cache.clear();

  const uint64_t hash = HashRect(left, top, width, height);
  m_rect_hash_cache.insert_or_assign(key, CachedHash{page_revision, hash});
  return hash;
}

void TextureReplacements::SetVRAM(const uint16_t* vram)
{
  if (m_vram == vram)
    return;

  m_vram = vram;
  m_page_cache.clear();
  m_page_cache_bytes = 0;
  m_page_hash_cache.clear();
  m_palette_hash_cache.clear();
  m_rect_hash_cache.clear();
}

void TextureReplacements::SetMaxTextureSize(uint32_t size)
{
  if (size == 0 || m_max_texture_size == size)
    return;

  m_max_texture_size = size;
  m_page_cache.clear();
  m_page_cache_bytes = 0;
}

void TextureReplacements::SetResolutionScale(uint32_t scale)
{
  scale = std::max<uint32_t>(1, scale);
  if (m_resolution_scale == scale)
    return;

  m_resolution_scale = scale;
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

  for (const auto& entries : m_texupload_replacements)
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
                                                  uint64_t page_hash, uint64_t full_palette_hash,
                                                  uint64_t page_revision)
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
      // coordinates as defined by the replacement matching rules.
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

      const uint64_t hash = GetCachedRectHash(page_revision, left, top, right - left, dst_height);
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
    match.src_x = 0;
    match.src_y = 0;
    match.src_width = image->GetWidth();
    match.src_height = image->GetHeight();
    matches.push_back(match);
  }
}

void TextureReplacements::FindTexuploadMatches(std::vector<ReplacementMatch>& matches, uint32_t page, GPUTextureMode mode,
                                               uint32_t palette_x, uint32_t palette_y, uint64_t full_palette_hash)
{
  if (m_vram_writes.empty() || m_texupload_replacements[static_cast<uint8_t>(mode) & 3u].empty())
    return;

  const auto& entries = m_texupload_replacements[static_cast<uint8_t>(mode) & 3u];
  const bool has_palette = TextureModeHasPalette(mode);
  const uint32_t page_start_x = (page & 15u) * VRAM_PAGE_WIDTH;
  const uint32_t page_start_y = (page >> 4) * VRAM_PAGE_HEIGHT;
  const uint32_t page_word_width = VRAM_PAGE_WIDTH << (static_cast<uint8_t>(mode) & 3u);
  const uint32_t shift = GetTextureModeShift(mode);

  for (const auto& [key, write] : m_vram_writes)
  {
    // Only writes that overlap the page can contribute pixels to it. The hash
    // covers the whole write rect, exactly like the dumped filename does.
    if (write.x + write.width <= page_start_x || write.x >= page_start_x + page_word_width ||
        write.y + write.height <= page_start_y || write.y >= page_start_y + VRAM_PAGE_HEIGHT)
    {
      continue;
    }

    if ((write.x + write.width) > VRAM_WIDTH || (write.y + write.height) > VRAM_HEIGHT)
      continue;

    const uint64_t write_revision =
      g_gpu ? g_gpu->GetVRAMRegionRevision(write.x, write.x + write.width, write.y, write.y + write.height) : 0;
    const uint64_t write_hash =
      GetCachedRectHash(write_revision, write.x, write.y, write.width, write.height);

    for (const TexturePageReplacementEntry& entry : entries)
    {
      const TexturePageReplacementName& name = entry.name;
      if (name.src_hash != write_hash)
        continue;

      if (has_palette && !IsMatchingReplacementPalette(full_palette_hash, mode, palette_x, palette_y, name))
        continue;

      const TextureReplacementTexture* image = LoadTexture(entry.filename);
      if (!image || image->GetWidth() == 0 || image->GetHeight() == 0)
        continue;

      // Place the replacement in page texel space. The write rect's top-left
      // maps to the name's (0,0); X offsets are in words -> texels.
      const int64_t dst_x = static_cast<int64_t>(name.offset_x) +
                            (static_cast<int64_t>(write.x - page_start_x) << shift);
      const int64_t dst_y = static_cast<int64_t>(name.offset_y) + static_cast<int64_t>(write.y - page_start_y);
      const int64_t dst_right = dst_x + name.width;
      const int64_t dst_bottom = dst_y + name.height;
      if (dst_right <= 0 || dst_bottom <= 0 || dst_x >= static_cast<int64_t>(TEXPAGE_NATIVE_WIDTH) ||
          dst_y >= static_cast<int64_t>(TEXPAGE_NATIVE_HEIGHT))
      {
        continue;
      }

      // Clip against the page and crop the source image to the same fraction,
      // so writes that start before the page (multi-page C16 uploads) still
      // line up correctly.
      const int64_t clamped_x = std::max<int64_t>(dst_x, 0);
      const int64_t clamped_y = std::max<int64_t>(dst_y, 0);
      const int64_t clamped_right = std::min<int64_t>(dst_right, TEXPAGE_NATIVE_WIDTH);
      const int64_t clamped_bottom = std::min<int64_t>(dst_bottom, TEXPAGE_NATIVE_HEIGHT);
      const float scale_x = static_cast<float>(image->GetWidth()) / static_cast<float>(std::max<uint32_t>(1, name.width));
      const float scale_y =
        static_cast<float>(image->GetHeight()) / static_cast<float>(std::max<uint32_t>(1, name.height));

      ReplacementMatch match;
      match.entry = &entry;
      match.scale_x = scale_x;
      match.scale_y = scale_y;
      match.dst_x = static_cast<uint32_t>(clamped_x);
      match.dst_y = static_cast<uint32_t>(clamped_y);
      match.dst_width = static_cast<uint32_t>(clamped_right - clamped_x);
      match.dst_height = static_cast<uint32_t>(clamped_bottom - clamped_y);
      match.src_x = static_cast<uint32_t>(std::max<float>(0.0f, (static_cast<float>(clamped_x - dst_x)) * scale_x));
      match.src_y = static_cast<uint32_t>(std::max<float>(0.0f, (static_cast<float>(clamped_y - dst_y)) * scale_y));
      match.src_width = std::min<uint32_t>(
        image->GetWidth() - match.src_x,
        static_cast<uint32_t>(std::max<float>(1.0f, static_cast<float>(clamped_right - clamped_x) * scale_x)));
      match.src_height = std::min<uint32_t>(
        image->GetHeight() - match.src_y,
        static_cast<uint32_t>(std::max<float>(1.0f, static_cast<float>(clamped_bottom - clamped_y) * scale_y)));
      if (match.dst_width == 0 || match.dst_height == 0 || match.src_width == 0 || match.src_height == 0)
        continue;

      matches.push_back(match);
    }
  }
}

void TextureReplacements::DecodePage(std::vector<uint32_t>& pixels, uint32_t page, GPUTextureMode mode,
                                     uint32_t palette_x, uint32_t palette_y) const
{
  const uint16_t* palette = TextureModeHasPalette(mode) ? GetPalettePointer(palette_x, palette_y) : nullptr;
  // Palette entries are read contiguously (a wrapped 8-bit palette
  // runs into the next VRAM row); clamp to the end of VRAM so a malformed
  // palette at the very last row can never read out of bounds.
  const size_t palette_words =
    TextureModeHasPalette(mode) ? ((static_cast<size_t>(VRAM_WIDTH) * VRAM_HEIGHT) -
                                   ((static_cast<size_t>(palette_y) * VRAM_WIDTH) + palette_x)) :
                                  0;
  DecodeExpandedPage(pixels, mode, GetPagePointer(page), VRAM_WIDTH, palette, palette_words);
}

void TextureReplacements::DecodeExpandedPage(std::vector<uint32_t>& pixels, GPUTextureMode mode, const uint16_t* page_ptr,
                                             size_t page_row_stride_words, const uint16_t* palette,
                                             size_t palette_words)
{
  pixels.resize(TEXPAGE_NATIVE_WIDTH * TEXPAGE_NATIVE_HEIGHT);
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
          if (index >= palette_words)
            index = 0;
          *(dest++) = VRAMRGBA5551ToRGBA8888(palette[index]);
        }

        page_ptr += page_row_stride_words;
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
          if (index >= palette_words)
            index = 0;
          *(dest++) = VRAMRGBA5551ToRGBA8888(palette[index]);
        }

        page_ptr += page_row_stride_words;
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

        page_ptr += page_row_stride_words;
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

static uint32_t SampleImageBilinear(const uint32_t* pixels, uint32_t width, uint32_t height, float u, float v,
                                    float* out_coverage = nullptr)
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

  if (out_coverage)
  {
    // Binary per-texel coverage: any non-zero texel counts as covered. This
    // mirrors the reference merge shader's alpha reconstruction and is what
    // keeps cut-out edges from averaging in the colour of fully transparent
    // texels.
    const float a00 = (p00 != 0) ? 1.0f : 0.0f;
    const float a10 = (p10 != 0) ? 1.0f : 0.0f;
    const float a01 = (p01 != 0) ? 1.0f : 0.0f;
    const float a11 = (p11 != 0) ? 1.0f : 0.0f;
    const float a0 = a00 + ((a10 - a00) * tx);
    const float a1 = a01 + ((a11 - a01) * tx);
    *out_coverage = std::clamp(a0 + ((a1 - a0) * ty), 0.0f, 1.0f);
  }

  return LerpRGBA8(LerpRGBA8(p00, p10, tx), LerpRGBA8(p01, p11, tx), ty);
}


std::shared_ptr<Common::RGBA8Image> TextureReplacements::ComposePage(const std::vector<ReplacementMatch>& matches,
                                                                     uint32_t page, GPUTextureMode mode,
                                                                     uint32_t palette_x, uint32_t palette_y,
                                                                     float* out_scale_x, float* out_scale_y)
{
  // Resolve the replacement images on this thread; the compose worker only
  // ever touches its own snapshot and reference-counted images.
  std::vector<ResolvedMatch> resolved;
  resolved.reserve(matches.size());
  for (const ReplacementMatch& match : matches)
  {
    const std::shared_ptr<TextureReplacementTexture> image = AcquireTexture(match.entry->filename);
    if (!image || image->GetWidth() == 0 || image->GetHeight() == 0)
      continue;

    ResolvedMatch out;
    out.image = image;
    out.semitransparent = match.entry->name.IsSemitransparent();
    out.scale_x = match.scale_x;
    out.scale_y = match.scale_y;
    out.dst_x = match.dst_x;
    out.dst_y = match.dst_y;
    out.dst_width = match.dst_width;
    out.dst_height = match.dst_height;
    out.src_x = match.src_x;
    out.src_y = match.src_y;
    out.src_width = match.src_width;
    out.src_height = match.src_height;
    resolved.push_back(std::move(out));
  }

  std::vector<uint32_t> base_pixels;
  DecodePage(base_pixels, page, mode, palette_x, palette_y);
  return ComposePageImage(resolved, base_pixels, m_max_texture_size, m_resolution_scale, out_scale_x, out_scale_y);
}

std::shared_ptr<Common::RGBA8Image> TextureReplacements::ComposePageImage(const std::vector<ResolvedMatch>& matches,
                                                                          const std::vector<uint32_t>& base_pixels,
                                                                          uint32_t max_texture_size,
                                                                          uint32_t resolution_scale,
                                                                          float* out_scale_x, float* out_scale_y)
{
  float max_scale_x = 1.0f;
  float max_scale_y = 1.0f;
  for (const ResolvedMatch& match : matches)
  {
    max_scale_x = std::max(max_scale_x, match.scale_x);
    max_scale_y = std::max(max_scale_y, match.scale_y);
  }

  // Clamp to the largest texture the renderer can bind (the device's max
  // texture size).
  const float max_possible_scale = static_cast<float>(max_texture_size) / static_cast<float>(TEXPAGE_NATIVE_WIDTH);
  max_scale_x = std::min(max_scale_x, max_possible_scale);
  max_scale_y = std::min(max_scale_y, max_possible_scale);

  // Cap the composite at the internal rendering resolution, and at the hard
  // memory/bandwidth bound above it.
  const float resolution_cap =
    std::min(static_cast<float>(std::max<uint32_t>(1, resolution_scale)), TEXPAGE_COMPOSITE_MAX_SCALE);
  max_scale_x = std::min(max_scale_x, resolution_cap);
  max_scale_y = std::min(max_scale_y, resolution_cap);
  if (!(max_scale_x > 0.0f) || !(max_scale_y > 0.0f))
    return nullptr;

  const uint32_t out_width =
    std::max<uint32_t>(1, static_cast<uint32_t>(std::ceil(static_cast<float>(TEXPAGE_NATIVE_WIDTH) * max_scale_x)));
  const uint32_t out_height =
    std::max<uint32_t>(1, static_cast<uint32_t>(std::ceil(static_cast<float>(TEXPAGE_NATIVE_HEIGHT) * max_scale_y)));

  auto image = std::make_shared<Common::RGBA8Image>();
  image->SetSize(out_width, out_height);
  uint32_t* out_pixels = image->GetPixels();

  // Upscale the decoded page with nearest filtering. Integer scale factors
  // (the common case: 2x/3x/4x/5x packs) expand one source row at a time and
  // duplicate it instead of running a divide per output pixel.

  if ((out_width % TEXPAGE_NATIVE_WIDTH) == 0 && (out_height % TEXPAGE_NATIVE_HEIGHT) == 0)
  {
    const uint32_t x_repeat = out_width / TEXPAGE_NATIVE_WIDTH;
    const uint32_t y_repeat = out_height / TEXPAGE_NATIVE_HEIGHT;
    std::vector<uint32_t> expanded_row(out_width);
    for (uint32_t src_y = 0; src_y < TEXPAGE_NATIVE_HEIGHT; src_y++)
    {
      const uint32_t* src_row = &base_pixels[static_cast<size_t>(src_y) * TEXPAGE_NATIVE_WIDTH];
      for (uint32_t x = 0; x < TEXPAGE_NATIVE_WIDTH; x++)
      {
        uint32_t* dst = &expanded_row[static_cast<size_t>(x) * x_repeat];
        for (uint32_t i = 0; i < x_repeat; i++)
          dst[i] = src_row[x];
      }

      uint32_t* dst_row = out_pixels + (static_cast<size_t>(src_y) * y_repeat * out_width);
      for (uint32_t i = 0; i < y_repeat; i++, dst_row += out_width)
        std::memcpy(dst_row, expanded_row.data(), sizeof(uint32_t) * out_width);
    }
  }
  else
  {
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
  }

  // Overlay each matching replacement image, scaled to its destination rect.
  for (const ResolvedMatch& match : matches)
  {
    const TextureReplacementTexture* tex = match.image.get();
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

    const bool semitransparent = match.semitransparent;
    const uint32_t src_texture_width = tex->GetWidth();
    const uint32_t src_texture_height = tex->GetHeight();
    const uint32_t* src_pixels = tex->GetPixels();
    // Sample only the requested sub-rect of the replacement image. Matching
    // rects normally cover the whole image; texupload matches that hang over
    // the page edge are cropped to the visible part.
    const uint32_t src_x0 = std::min<uint32_t>(match.src_x, src_texture_width - 1);
    const uint32_t src_y0 = std::min<uint32_t>(match.src_y, src_texture_height - 1);
    const uint32_t src_x1 = std::min<uint32_t>(src_x0 + std::max<uint32_t>(1, match.src_width), src_texture_width);
    const uint32_t src_y1 = std::min<uint32_t>(src_y0 + std::max<uint32_t>(1, match.src_height), src_texture_height);

    // Fast path: when the destination grid advances by an exact integer number
    // of source texels (1:1 copies and integer downscales alike), sampling
    // reduces to picking one texel. Packs whose images are integer multiples of
    // the source rectangles - the common case - hit this for every match, which
    // skips the per-pixel float sampling entirely.
    const uint32_t dst_width = dst_x1 - dst_x0;
    const uint32_t dst_height = dst_y1 - dst_y0;
    const uint32_t ratio_x = (dst_width != 0) ? (src_x1 - src_x0) / dst_width : 0;
    const uint32_t ratio_y = (dst_height != 0) ? (src_y1 - src_y0) / dst_height : 0;
    if (ratio_x != 0 && ratio_y != 0 && (ratio_x * dst_width) == (src_x1 - src_x0) &&
        (ratio_y * dst_height) == (src_y1 - src_y0))
    {
      for (uint32_t y = dst_y0; y < dst_y1; y++)
      {
        const uint32_t* src_row =
          src_pixels + (static_cast<size_t>(src_y0 + (y - dst_y0) * ratio_y) * src_texture_width) + src_x0;
        uint32_t* dst_row = out_pixels + (static_cast<size_t>(y) * out_width);
        if (semitransparent)
        {
          for (uint32_t x = dst_x0, src_x = 0; x < dst_x1; x++, src_x += ratio_x)
          {
            const uint32_t pixel = src_row[src_x];
            dst_row[x] = (pixel == 0) ?
                           0 :
                           ((pixel & 0x00FFFFFFu) | (((pixel >> 24) <= 242u) ? 0xFF000000u : 0x00000000u));
          }
        }
        else
        {
          for (uint32_t x = dst_x0, src_x = 0; x < dst_x1; x++, src_x += ratio_x)
          {
            const uint32_t pixel = src_row[src_x];
            dst_row[x] = (pixel != 0) ? ((pixel & 0x00FFFFFFu) | 0x02000000u) : 0u;
          }
        }
      }
      continue;
    }

    const float rcp_dst_width = 1.0f / static_cast<float>(dst_x1 - dst_x0);
    const float rcp_dst_height = 1.0f / static_cast<float>(dst_y1 - dst_y0);
    const float src_step_x = static_cast<float>(src_x1 - src_x0) * rcp_dst_width / static_cast<float>(src_texture_width);
    const float src_step_y =
      static_cast<float>(src_y1 - src_y0) * rcp_dst_height / static_cast<float>(src_texture_height);
    const float src_origin_x =
      (static_cast<float>(src_x0) + 0.5f * static_cast<float>(src_x1 - src_x0) * rcp_dst_width) /
      static_cast<float>(src_texture_width);
    const float src_origin_y =
      (static_cast<float>(src_y0) + 0.5f * static_cast<float>(src_y1 - src_y0) * rcp_dst_height) /
      static_cast<float>(src_texture_height);

    for (uint32_t y = dst_y0; y < dst_y1; y++)
    {
      const float v = src_origin_y + static_cast<float>(y - dst_y0) * src_step_y;
      uint32_t* dst_row = out_pixels + (static_cast<size_t>(y) * out_width);
      for (uint32_t x = dst_x0; x < dst_x1; x++)
      {
        const float u = src_origin_x + static_cast<float>(x - dst_x0) * src_step_x;

        if (semitransparent)
        {
          // Semitransparent replacements encode opacity in the image's alpha
          // channel and keep the straight bilinear result.
          const uint32_t pixel = SampleImageBilinear(src_pixels, src_texture_width, src_texture_height, u, v);

          // Anything which isn't fully opaque maps to the PSX STP bit
          // (bit 15). 0000h stays fully transparent. Mirrors the reference
          // merge shader's semitransparent path.
          if (pixel == 0)
            dst_row[x] = 0;
          else
            dst_row[x] = (pixel & 0x00FFFFFFu) | (((pixel >> 24) <= 242u) ? 0xFF000000u : 0x00000000u);
        }
        else
        {
          // Opaque replacements: reconstruct binary alpha coverage and
          // un-premultiply the bilinear colour by it, so partially covered
          // edge texels don't pick up the colour of fully transparent
          // neighbours. Sub-0.5 coverage becomes transparent, everything
          // else becomes an opaque texel. The low alpha value keeps the
          // texel from being transparency-culled while leaving bit 15
          // clear.
          float coverage = 1.0f;
          const uint32_t pixel =
            SampleImageBilinear(src_pixels, src_texture_width, src_texture_height, u, v, &coverage);

          if (coverage >= 0.5f)
          {
            // A single reciprocal is cheaper than three divides on the edge
            // texels that still take the generic sampling path.
            const float rcp_coverage = 1.0f / coverage;
            const auto unmultiply = [rcp_coverage](uint32_t channel) {
              return static_cast<uint32_t>(
                std::clamp(static_cast<float>(channel) * rcp_coverage, 0.0f, 255.0f));
            };
            const uint32_t rgb = unmultiply(pixel & 0xFFu) | (unmultiply((pixel >> 8) & 0xFFu) << 8) |
                                 (unmultiply((pixel >> 16) & 0xFFu) << 16);
            dst_row[x] = rgb | 0x02000000u;
          }
          else
          {
            dst_row[x] = 0;
          }
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

void TextureReplacements::BeginFrame()
{
  m_composes_remaining = MAX_COMPOSES_PER_FRAME;
}

const TexturePageReplacement* TextureReplacements::GetTexturePageReplacement(GPUTextureMode mode, uint32_t texture_page_x,
                                                                             uint32_t texture_page_y, uint32_t palette_x,
                                                                             uint32_t palette_y)
{
  // Install anything the compose worker finished since the last lookup.
  DrainComposedPages();

  if (!m_vram || m_game_id.empty() || texture_page_x >= VRAM_WIDTH || texture_page_y >= VRAM_HEIGHT ||
      palette_x >= VRAM_WIDTH || palette_y >= VRAM_HEIGHT)
  {
    return nullptr;
  }

  // Canonicalize the "reserved" direct16 mode; the runtime never uses it, but
  // the register can technically hold it. The raw_texture/ST bit does not
  // participate in matching (the replacement index ignores it), so it is
  // dropped here; the entry's own bit selects the alpha compositing path.
  uint32_t mode_bits = static_cast<uint8_t>(mode) & 3u;
  if (mode_bits == static_cast<uint8_t>(GPUTextureMode::Reserved_Direct16Bit))
    mode_bits = static_cast<uint8_t>(GPUTextureMode::Direct16Bit);

  const uint32_t mode_index = mode_bits;
  if (m_texpage_replacements[mode_index].empty() && m_texupload_replacements[mode_index].empty())
    return nullptr;

  const uint32_t page = ((texture_page_y / VRAM_PAGE_HEIGHT) * 16u) + (texture_page_x / VRAM_PAGE_WIDTH);
  const GPUTextureMode canonical_mode = static_cast<GPUTextureMode>(mode_index);
  const uint32_t source_word_width = VRAM_PAGE_WIDTH << mode_index;

  // Texpage hashes are row-contiguous and packs use those hashes.
  // A page which crosses the right VRAM edge has no compatible full-page hash,
  // so leave it on the native path instead of reading past the shadow buffer.
  if ((texture_page_x + source_word_width) > VRAM_WIDTH)
    return nullptr;

  // Hash memoization is keyed by the GPU's per-page content revisions, so a
  // page that has not been written since the last lookup is never re-hashed,
  // and the completed lookup below is cached purely by content hashes. This
  // keeps per-draw cost to a handful of map probes for static pages.
  const uint64_t page_revision =
    g_gpu ? g_gpu->GetVRAMRegionRevision(texture_page_x, texture_page_x + source_word_width, texture_page_y,
                                         texture_page_y + VRAM_PAGE_HEIGHT) :
            0;
  const uint32_t palette_width = TextureModeHasPalette(canonical_mode) ?
                                   std::min(GetPaletteWidth(canonical_mode), VRAM_WIDTH - palette_x) :
                                   0;
  const uint64_t palette_revision =
    (g_gpu && palette_width > 0) ?
      g_gpu->GetVRAMRegionRevision(palette_x, palette_x + palette_width, palette_y, palette_y + 1u) :
      0;
  const uint64_t page_hash = GetCachedPageHash(page, canonical_mode, page_revision);
  const uint64_t full_palette_hash = TextureModeHasPalette(canonical_mode) ?
                                       GetCachedPaletteHash(palette_x, palette_y, canonical_mode, palette_revision) :
                                       0;

  // One cache entry per page/mode/palette. Its id is stable for the lifetime
  // of the entry; a content change only bumps the revision, and the renderer
  // swaps the GPU texture at the frame boundary. This keeps the batch key
  // stable for animated pages, so changing content never re-flushes or
  // flickers between the native and replaced paths.
  const PageCacheKey key = {page, mode_index, full_palette_hash};
  auto it = m_page_cache.find(key);
  if (it != m_page_cache.end())
  {
    CachedPage& cached = it->second;
    cached.last_used = ++m_page_cache_used_counter;
    const uint64_t texture_generation = m_texture_load_generation.load(std::memory_order_relaxed);
    const bool waiting_for_textures = cached.incomplete && cached.load_generation != texture_generation;
    if (cached.page_hash == page_hash && cached.palette_hash == full_palette_hash && !waiting_for_textures &&
        !cached.pending_rebuild)
    {
      return (cached.replacement.id != 0) ? &cached.replacement : nullptr;
    }

    // A worker rebuild may already be producing the next image for this page.
    if (cached.in_flight_serial != 0)
      return (cached.replacement.id != 0) ? &cached.replacement : nullptr;

    // Content changed since the cached composition, or a replacement texture
    // that was still decoding on the worker has arrived; rebuild in place.
    m_texture_pending_hit = false;
    std::vector<ReplacementMatch> matches;
    FindTexturePageMatches(matches, page, canonical_mode, palette_x, palette_y, page_hash, full_palette_hash,
                           page_revision);
    FindTexuploadMatches(matches, page, canonical_mode, palette_x, palette_y, full_palette_hash);

    if (matches.empty())
    {
      const size_t old_size = cached.size_bytes;
      cached.page_hash = page_hash;
      cached.palette_hash = full_palette_hash;
      cached.size_bytes = 0;
      cached.pending_rebuild = false;
      cached.replacement.image.reset();
      cached.replacement.id = 0;
      cached.incomplete = false;
      cached.load_generation = texture_generation;
      m_page_cache_bytes -= old_size;
      return nullptr;
    }

    // Offload the composition to the worker. The frame thread only snapshots
    // the page and queues; the entry keeps serving its previous composite.
    ComposeJob job;
    const uint64_t compose_serial = m_next_compose_serial++;
    if (BuildComposeJob(key, page_hash, full_palette_hash, mode_index, page, palette_x, palette_y, matches, &job))
    {
      job.serial = compose_serial;
      if (TryQueueCompose(std::move(job)))
      {
        cached.in_flight_serial = compose_serial;
        cached.pending_rebuild = false;
        return (cached.replacement.id != 0) ? &cached.replacement : nullptr;
      }
    }
    else
    {
      // Every matched image is still decoding; retry once one arrives.
      cached.pending_rebuild = false;
      cached.incomplete = true;
      cached.load_generation = m_texture_load_generation.load(std::memory_order_relaxed);
      return (cached.replacement.id != 0) ? &cached.replacement : nullptr;
    }

    // The worker queue is saturated; fall back to an in-place compose, subject
    // to the per-frame budget so a burst cannot stall the frame.
    if (m_composes_remaining == 0)
    {
      cached.pending_rebuild = true;
      return (cached.replacement.id != 0) ? &cached.replacement : nullptr;
    }

    const size_t old_size = cached.size_bytes;
    cached.page_hash = page_hash;
    cached.palette_hash = full_palette_hash;
    cached.size_bytes = 0;
    cached.pending_rebuild = false;

    if (matches.empty())
    {
      cached.replacement.image.reset();
      cached.replacement.id = 0;
      cached.incomplete = false;
      cached.load_generation = texture_generation;
      m_page_cache_bytes -= old_size;
      return nullptr;
    }

    float scale_x = 1.0f;
    float scale_y = 1.0f;
    if (m_composes_remaining != 0)
      m_composes_remaining--;
    std::shared_ptr<Common::RGBA8Image> image =
      ComposePage(matches, page, canonical_mode, palette_x, palette_y, &scale_x, &scale_y);
    if (!image)
    {
      cached.replacement.image.reset();
      cached.replacement.id = 0;
      cached.incomplete = m_texture_pending_hit;
      cached.load_generation = m_texture_load_generation.load(std::memory_order_relaxed);
      m_page_cache_bytes -= old_size;
      return nullptr;
    }

    cached.incomplete = m_texture_pending_hit;
    cached.load_generation = m_texture_load_generation.load(std::memory_order_relaxed);
    cached.replacement.image = std::move(image);
    if (cached.replacement.id == 0)
    {
      cached.replacement.id = m_next_replacement_id++;
      cached.replacement.revision = 1;
    }
    else
    {
      cached.replacement.revision++;
    }
    cached.replacement.vram_page_start_x = (page & 15u) * VRAM_PAGE_WIDTH;
    cached.replacement.vram_page_start_y = (page >> 4) * VRAM_PAGE_HEIGHT;
    cached.replacement.source_width = TEXPAGE_NATIVE_WIDTH;
    cached.replacement.source_height = TEXPAGE_NATIVE_HEIGHT;
    cached.replacement.scale_x = scale_x;
    cached.replacement.scale_y = scale_y;
    cached.size_bytes = static_cast<size_t>(cached.replacement.image->GetWidth()) *
                        static_cast<size_t>(cached.replacement.image->GetHeight()) * sizeof(uint32_t);
    const size_t new_size = cached.size_bytes;
    m_page_cache_bytes -= old_size;

    EvictPageReplacementsForBudget(new_size);
    it = m_page_cache.find(key);
    if (it == m_page_cache.end())
      return nullptr;

    m_page_cache_bytes += new_size;
    return &it->second.replacement;
  }

  m_texture_pending_hit = false;
  std::vector<ReplacementMatch> matches;
  FindTexturePageMatches(matches, page, canonical_mode, palette_x, palette_y, page_hash, full_palette_hash,
                         page_revision);
  FindTexuploadMatches(matches, page, canonical_mode, palette_x, palette_y, full_palette_hash);

  CachedPage cached;
  cached.last_used = ++m_page_cache_used_counter;
  cached.replacement.id = 0;
  cached.page_hash = page_hash;
  cached.palette_hash = full_palette_hash;
  cached.incomplete = m_texture_pending_hit;
  cached.load_generation = m_texture_load_generation.load(std::memory_order_relaxed);

  if (!matches.empty())
  {
    ComposeJob job;
    const uint64_t compose_serial = m_next_compose_serial++;
    if (BuildComposeJob(key, page_hash, full_palette_hash, mode_index, page, palette_x, palette_y, matches, &job))
    {
      job.serial = compose_serial;
      if (TryQueueCompose(std::move(job)))
      {
        cached.in_flight_serial = compose_serial;
      }
      else if (m_composes_remaining != 0)
      {
        // Worker queue is saturated; compose here while the budget allows.
        m_composes_remaining--;
        float scale_x = 1.0f;
        float scale_y = 1.0f;
        std::shared_ptr<Common::RGBA8Image> image =
          ComposePage(matches, page, canonical_mode, palette_x, palette_y, &scale_x, &scale_y);
        if (image)
        {
          cached.replacement.image = std::move(image);
          cached.replacement.id = m_next_replacement_id++;
          cached.replacement.revision = 1;
          cached.replacement.vram_page_start_x = (page & 15u) * VRAM_PAGE_WIDTH;
          cached.replacement.vram_page_start_y = (page >> 4) * VRAM_PAGE_HEIGHT;
          // In expanded texel space a page is 256x256 for every mode.
          cached.replacement.source_width = TEXPAGE_NATIVE_WIDTH;
          cached.replacement.source_height = TEXPAGE_NATIVE_HEIGHT;
          cached.replacement.scale_x = scale_x;
          cached.replacement.scale_y = scale_y;
          cached.size_bytes = static_cast<size_t>(cached.replacement.image->GetWidth()) *
                              static_cast<size_t>(cached.replacement.image->GetHeight()) * sizeof(uint32_t);
        }
      }
      else
      {
        cached.pending_rebuild = true;
      }
    }
    else
    {
      // Every matched image is still decoding; rebuild once one arrives.
      cached.incomplete = true;
      cached.load_generation = m_texture_load_generation.load(std::memory_order_relaxed);
    }
  }

  EvictPageReplacementsForBudget(cached.size_bytes);
  it = m_page_cache.emplace(key, std::move(cached)).first;
  m_page_cache_bytes += it->second.size_bytes;
  return (it->second.replacement.id != 0) ? &it->second.replacement : nullptr;
}
