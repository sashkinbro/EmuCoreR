#pragma once
#include "common/hash_combine.h"
#include "common/image.h"
#include "gpu_types.h"
#include "types.h"
#include <array>
#include <list>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct TextureReplacementHash
{
  uint64_t low;
  uint64_t high;

  std::string ToString() const;
  bool ParseString(const std::string_view& sv);

  bool operator==(const TextureReplacementHash& rhs) const { return low == rhs.low && high == rhs.high; }
};

namespace std {
template<>
struct hash<TextureReplacementHash>
{
  size_t operator()(const TextureReplacementHash& h) const
  {
    size_t hash_hash = std::hash<uint64_t>{}(h.low);
    hash_combine(hash_hash, h.high);
    return hash_hash;
  }
};
} // namespace std

using TextureReplacementTexture = Common::RGBA8Image;

// A composited texture page (all matching texpage replacements rasterized into
// a single RGBA8 image) plus the information the renderer needs to sample it
// with normalized coordinates. In "expanded" texel space a full page is always
// 256x256 for every mode (P4/P8/C16), matching DuckStation's texture cache;
// the image is that page upscaled by the replacement's scale.
struct TexturePageReplacement
{
  std::shared_ptr<Common::RGBA8Image> image;
  uint64_t id = 0;                    // unique; changes whenever the page is recomposited
  uint32_t vram_page_start_x = 0;     // page origin in VRAM word coordinates
  uint32_t vram_page_start_y = 0;
  uint32_t source_width = 0;          // source hash region size in VRAM words (64/128/256)
  uint32_t source_height = 0;
  float scale_x = 1.0f;
  float scale_y = 1.0f;
};

class TextureReplacements
{
public:
  enum class ReplacmentType
  {
    VRAMWrite,
    TextureFromVRAMWrite,
    TextureFromPage
  };

  TextureReplacements();
  ~TextureReplacements();

  void SetGameID(std::string game_id);

  void Reload();

  const TextureReplacementTexture* GetVRAMWriteReplacement(uint32_t width, uint32_t height, const void* pixels);

  // Texture page replacement lookup. `mode` may carry GPUTextureMode::RawTextureBit
  // (DuckStation's ST* variants); page/palette coordinates are in VRAM words.
  // Returns a pointer owned by the internal cache, valid until the next lookup.
  const TexturePageReplacement* GetTexturePageReplacement(GPUTextureMode mode, uint32_t texture_page_x,
                                                          uint32_t texture_page_y, uint32_t palette_x,
                                                          uint32_t palette_y);

  /// True if any texpage-* replacements were loaded for the current game.
  bool HasTexturePageReplacements() const;

  // The CPU-side VRAM shadow the manager hashes/composites from.
  void SetVRAM(const uint16_t* vram);

  // Largest texture the renderer can bind; caps the composited page size.
  void SetMaxTextureSize(uint32_t size);

  void Shutdown();

private:
  using VRAMWriteReplacementMap = std::unordered_map<TextureReplacementHash, std::string>;
  using TextureCache = std::unordered_map<std::string, TextureReplacementTexture>;
  using TextureLruList = std::list<std::string>;
  using TextureLruPositions = std::unordered_map<std::string, TextureLruList::iterator>;

  // Parsed contents of a texpage-* filename. Mirrors DuckStation's
  // TextureReplacementName.
  struct TexturePageReplacementName
  {
    uint64_t src_hash = 0;
    uint64_t pal_hash = 0;
    uint16_t src_width = 0;
    uint16_t src_height = 0;
    uint8_t texture_mode = 0; // 0..7: P4/P8/C16/C16/STP4/STP8/STC16/STC16
    uint16_t offset_x = 0;
    uint16_t offset_y = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    uint8_t pal_min = 0;
    uint8_t pal_max = 0;

    GPUTextureMode GetTextureMode() const { return static_cast<GPUTextureMode>(texture_mode & 3u); }
    bool IsSemitransparent() const { return (texture_mode >= 4); }
    bool HasPalette() const { return (GetTextureMode() < GPUTextureMode::Direct16Bit); }
    bool Parse(const std::string_view file_title);
    bool operator==(const TexturePageReplacementName& rhs) const;
  };

  struct TexturePageReplacementEntry
  {
    TexturePageReplacementName name;
    std::string filename;
  };

  struct ReplacementMatch
  {
    const TexturePageReplacementEntry* entry;
    float scale_x;
    float scale_y;
    uint32_t dst_x; // destination rect in expanded page texel space
    uint32_t dst_y;
    uint32_t dst_width;
    uint32_t dst_height;
  };

  struct PageCacheKey
  {
    uint32_t page;
    uint32_t mode;
    uint32_t palette_x;
    uint32_t palette_y;

    bool operator==(const PageCacheKey& rhs) const
    {
      return page == rhs.page && mode == rhs.mode && palette_x == rhs.palette_x && palette_y == rhs.palette_y;
    }
  };

  struct PageCacheKeyHash
  {
    size_t operator()(const PageCacheKey& k) const;
  };

  struct CachedPage
  {
    TexturePageReplacement replacement; // id == 0 when there was no match
    uint64_t vram_generation = 0;       // generation the hashes were computed at
    uint64_t match_signature = 0;       // identity of the matched entry set
    size_t size_bytes = 0;
    uint64_t last_used = 0;
  };

  static bool ParseReplacementFilename(const std::string& filename, TextureReplacementHash* replacement_hash,
                                       ReplacmentType* replacement_type);
  static bool IsSupportedExtension(const std::string_view extension);

  static bool TextureModeHasPalette(GPUTextureMode mode) { return (static_cast<uint8_t>(mode) & 3u) < 2u; }
  static uint32_t GetPaletteWidth(GPUTextureMode mode)
  {
    return ((static_cast<uint8_t>(mode) & 3u) == static_cast<uint8_t>(GPUTextureMode::Palette4Bit)) ? 16u : 256u;
  }
  static uint32_t GetTextureModeShift(GPUTextureMode mode)
  {
    // Number of texels packed into one VRAM word, as a log2 shift.
    const uint8_t base = static_cast<uint8_t>(mode) & 3u;
    return (base == static_cast<uint8_t>(GPUTextureMode::Palette4Bit)) ? 2u :
           (base == static_cast<uint8_t>(GPUTextureMode::Palette8Bit)) ? 1u :
                                                                          0u;
  }

  std::string GetSourceDirectory() const;

  TextureReplacementHash GetVRAMWriteHash(uint32_t width, uint32_t height, const void* pixels) const;

  void FindTextures(const std::string& dir);

  const TextureReplacementTexture* LoadTexture(const std::string& filename);
  void PreloadTextures();
  void PurgeUnreferencedTexturesFromCache();

  // Keeps the decoded texture cache inside a fixed memory budget by evicting
  // the least recently used entries when a new image is inserted.
  void EvictTexturesForBudget(size_t incoming_bytes, const std::string& keep_filename);
  void TouchTextureCacheEntry(const std::string& filename);
  void ResetTextureCacheOrdering();

  // Texture page replacement hashing/compositing.
  const uint16_t* GetPagePointer(uint32_t page) const;
  const uint16_t* GetPalettePointer(uint32_t palette_x, uint32_t palette_y) const;
  uint64_t HashPage(uint32_t page, GPUTextureMode mode) const;
  uint64_t HashPalette(uint32_t palette_x, uint32_t palette_y, GPUTextureMode mode) const;
  uint64_t HashPartialPalette(uint32_t palette_x, uint32_t palette_y, uint32_t min, uint32_t max) const;
  uint64_t HashRect(uint32_t left, uint32_t top, uint32_t width, uint32_t height) const;

  bool IsMatchingReplacementPalette(uint64_t full_palette_hash, GPUTextureMode mode, uint32_t palette_x,
                                    uint32_t palette_y, const TexturePageReplacementName& name) const;
  void FindTexturePageMatches(std::vector<ReplacementMatch>& matches, uint32_t page, GPUTextureMode mode,
                              uint32_t palette_x, uint32_t palette_y, uint64_t page_hash,
                              uint64_t full_palette_hash);

  void DecodePage(std::vector<uint32_t>& pixels, uint32_t page, GPUTextureMode mode, uint32_t palette_x,
                  uint32_t palette_y) const;
  std::shared_ptr<Common::RGBA8Image> ComposePage(const std::vector<ReplacementMatch>& matches, uint32_t page,
                                                  GPUTextureMode mode, uint32_t palette_x, uint32_t palette_y,
                                                  float* scale_x, float* scale_y);

  void EvictPageReplacementsForBudget(size_t incoming_bytes);

  std::string m_game_id;

  TextureCache m_texture_cache;
  TextureLruList m_texture_lru;
  TextureLruPositions m_texture_lru_positions;
  size_t m_texture_cache_bytes = 0;

  VRAMWriteReplacementMap m_vram_write_replacements;

  // [base mode 0..2] - DuckStation's ST* (semi-transparent) variants share the
  // bucket with their base mode; the ST bit only affects compositing.
  std::array<std::vector<TexturePageReplacementEntry>, 3> m_texpage_replacements;
  uint32_t m_texupload_replacement_count = 0;

  const uint16_t* m_vram = nullptr;
  uint32_t m_max_texture_size = 2048;

  std::unordered_map<PageCacheKey, CachedPage, PageCacheKeyHash> m_page_cache;
  size_t m_page_cache_bytes = 0;
  uint64_t m_page_cache_used_counter = 0;
  uint64_t m_next_replacement_id = 1;
};

extern TextureReplacements g_texture_replacements;

// EmuCoreR Android frontend hook: overrides the base directory replacement
// textures are loaded from. An empty path restores the default location.
void SetTextureReplacementsPathOverride(std::string path);
