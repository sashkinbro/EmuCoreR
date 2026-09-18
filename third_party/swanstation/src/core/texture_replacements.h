#pragma once
#include "common/hash_combine.h"
#include "common/image.h"
#include "gpu_types.h"
#include "types.h"
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
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
// 256x256 for every mode (P4/P8/C16), matching the texture cache layout;
// the image is that page upscaled by the replacement's scale.
struct TexturePageReplacement
{
  std::shared_ptr<Common::RGBA8Image> image;
  uint64_t id = 0;                    // stable per page/mode/palette; id == 0 when there is no match
  uint64_t revision = 0;              // bumped whenever the composited image changes
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

  const TextureReplacementTexture* GetVRAMWriteReplacement(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                                                           const void* pixels);

  // A replacement texture that was still decoding when its VRAM write was
  // processed. The renderer re-blits these at the frame boundary, so a static
  // upload that only happens once still gets its replacement.
  struct VRAMWriteReplacementResult
  {
    std::shared_ptr<TextureReplacementTexture> texture;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
  };
  void CollectReadyVRAMWriteReplacements(std::vector<VRAMWriteReplacementResult>* out);

  // Texture page replacement lookup. `mode` may carry GPUTextureMode::RawTextureBit
  // (the ST* variants); page/palette coordinates are in VRAM words.
  // Returns a pointer owned by the internal cache, valid until the next lookup.
  const TexturePageReplacement* GetTexturePageReplacement(GPUTextureMode mode, uint32_t texture_page_x,
                                                          uint32_t texture_page_y, uint32_t palette_x,
                                                          uint32_t palette_y);

  /// Records a VRAM write (an upload from the CPU) so texupload-* replacements
  /// can be matched against it. Coordinates may wrap past the VRAM edges.
  void RecordVRAMWrite(uint32_t x, uint32_t y, uint32_t width, uint32_t height);

  /// True if any texpage-* or texupload-* replacements were loaded for the
  /// current game.
  bool HasTexturePageReplacements() const;

  // The CPU-side VRAM shadow the manager hashes/composites from.
  void SetVRAM(const uint16_t* vram);

  // Largest texture the renderer can bind; caps the composited page size.
  void SetMaxTextureSize(uint32_t size);

  // Internal rendering resolution. The composited page is capped to this
  // scale: detail beyond the visible resolution is wasted work (and memory),
  // and animated pages recompose frequently in some games.
  void SetResolutionScale(uint32_t scale);

  void Shutdown();

  // Called by the renderer at each frame boundary. Resets the per-frame
  // recomposition budget so animated pages spread their rebuilds over several
  // frames instead of stalling one.
  void BeginFrame();

private:
  using VRAMWriteReplacementMap = std::unordered_map<TextureReplacementHash, std::string>;
  // Reference-counted so a compose job on the worker keeps its images alive
  // even if the LRU evicts them from the cache meanwhile.
  using TextureCache = std::unordered_map<std::string, std::shared_ptr<TextureReplacementTexture>>;
  using TextureLruList = std::list<std::string>;
  using TextureLruPositions = std::unordered_map<std::string, TextureLruList::iterator>;

  // Parsed contents of a texpage-* filename.
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
    // Sub-rect of the replacement image to sample, in image pixels. Matches
    // that hang over the page edge (texupload writes can start outside it) are
    // clipped against the page and sample only the visible part.
    uint32_t src_x;
    uint32_t src_y;
    uint32_t src_width;
    uint32_t src_height;
  };

  // ReplacementMatch with the replacement image resolved to a reference-counted
  // pointer, so the compositor can run on a worker thread without racing the
  // texture cache LRU (evicting an image must not free it mid-compose).
  struct ResolvedMatch
  {
    std::shared_ptr<TextureReplacementTexture> image;
    bool semitransparent = false;
    float scale_x = 1.0f;
    float scale_y = 1.0f;
    uint32_t dst_x = 0;
    uint32_t dst_y = 0;
    uint32_t dst_width = 0;
    uint32_t dst_height = 0;
    uint32_t src_x = 0;
    uint32_t src_y = 0;
    uint32_t src_width = 0;
    uint32_t src_height = 0;
  };

  struct PageCacheKey
  {
    uint32_t page;
    uint32_t mode;
    // Palette CONTENT hash, not position: the composite's decoded page only
    // depends on the palette values, so identical palettes stored at different
    // VRAM addresses must share one entry. Games scatter CLUTs all over VRAM,
    // and keying on position turned every draw into a cache miss.
    uint64_t palette_hash;

    bool operator==(const PageCacheKey& rhs) const
    {
      return page == rhs.page && mode == rhs.mode && palette_hash == rhs.palette_hash;
    }
  };

  struct PageCacheKeyHash
  {
    size_t operator()(const PageCacheKey& k) const;
  };

  struct CachedPage
  {
    TexturePageReplacement replacement; // id stays stable; id == 0 when there is no match
    uint64_t page_hash = 0;
    uint64_t palette_hash = 0;
    size_t size_bytes = 0;
    uint64_t last_used = 0;
    // Set when the page was composited while one of its replacement images was
    // still decoding on the worker thread. The page is recomposited once the
    // texture generation advances past load_generation.
    bool incomplete = false;
    uint64_t load_generation = 0;
    // Set when a rebuild was deferred because the per-frame recomposition
    // budget ran out. The page keeps serving its previous composite until the
    // next frame picks the rebuild up again.
    bool pending_rebuild = false;
    // Serial of the compose job currently producing this entry's next image
    // (0 = none). Completions with a stale serial are discarded.
    uint64_t in_flight_serial = 0;
  };

  // A page recomposition queued for the compose worker. Everything the worker
  // needs is snapshotted: the page and palette words (the live VRAM shadow
  // keeps changing on the emulation thread) and the resolved replacement
  // images (reference-counted, so LRU eviction cannot free them mid-compose).
  struct ComposeJob
  {
    PageCacheKey key;
    uint64_t serial = 0;
    uint64_t page_hash = 0;
    uint64_t palette_hash = 0;
    uint32_t page_row_words = 0;
    uint32_t max_texture_size = 2048;
    uint32_t resolution_scale = 1;
    std::vector<uint16_t> page_words;
    std::vector<uint16_t> palette_words;
    std::vector<ResolvedMatch> matches;
    bool textures_missing = false;
  };

  struct CompletedCompose
  {
    PageCacheKey key;
    uint64_t serial = 0;
    uint64_t page_hash = 0;
    uint64_t palette_hash = 0;
    float scale_x = 1.0f;
    float scale_y = 1.0f;
    std::shared_ptr<Common::RGBA8Image> image;
    bool textures_missing = false;
  };

  void StartComposeWorker();
  void StopComposeWorker();
  bool TryQueueCompose(ComposeJob job);
  void DrainComposedPages();
  static void ComposeWorkerEntry(TextureReplacements* self);
  bool BuildComposeJob(const PageCacheKey& key, uint64_t page_hash, uint64_t palette_hash, uint32_t mode_index,
                       uint32_t page, uint32_t palette_x, uint32_t palette_y,
                       const std::vector<ReplacementMatch>& matches, ComposeJob* job);

  uint64_t GetCachedPageHash(uint32_t page, GPUTextureMode mode, uint64_t revision);
  uint64_t GetCachedPaletteHash(uint32_t palette_x, uint32_t palette_y, GPUTextureMode mode, uint64_t revision);
  uint64_t GetCachedRectHash(uint64_t page_revision, uint32_t left, uint32_t top, uint32_t width, uint32_t height);

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

  std::shared_ptr<TextureReplacementTexture> AcquireTexture(const std::string& filename);
  const TextureReplacementTexture* LoadTexture(const std::string& filename);
  void PreloadTextures();
  void PurgeUnreferencedTexturesFromCache();

  // Keeps the decoded texture cache inside a fixed memory budget by evicting
  // the least recently used entries when a new image is inserted.
  void EvictTexturesForBudget(size_t incoming_bytes, const std::string& keep_filename);
  void TouchTextureCacheEntry(const std::string& filename);
  void ResetTextureCacheOrdering();

  // Replacement images are decoded on a worker thread. The emulation thread
  // never touches the filesystem: a cache miss queues a request and reports the
  // texture as unavailable for that frame, and the page it belongs to is
  // recomposited once the decode lands.
  void StartTextureLoader();
  void StopTextureLoader();
  void QueueTextureLoad(const std::string& filename);
  void DrainLoadedTextures();
  void InsertDecodedTexture(std::string filename, TextureReplacementTexture image);
  static void TextureLoaderEntry(TextureReplacements* self);

  // VRAM writes whose replacement was still decoding when they were seen.
  struct PendingVRAMWriteReplacement
  {
    TextureReplacementHash hash;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
  };

  void QueuePendingVRAMWriteReplacement(const TextureReplacementHash& hash, uint32_t x, uint32_t y, uint32_t width,
                                        uint32_t height);
  void RemovePendingVRAMWriteReplacement(uint32_t x, uint32_t y, uint32_t width, uint32_t height);

  static constexpr size_t MAX_PENDING_VRAM_WRITE_REPLACEMENTS = 256;
  std::vector<PendingVRAMWriteReplacement> m_pending_vram_write_replacements;

  struct DecodedTexture
  {
    std::string filename;
    TextureReplacementTexture image;
  };

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
                              uint64_t full_palette_hash, uint64_t page_revision);
  void FindTexuploadMatches(std::vector<ReplacementMatch>& matches, uint32_t page, GPUTextureMode mode,
                            uint32_t palette_x, uint32_t palette_y, uint64_t full_palette_hash);

  void DecodePage(std::vector<uint32_t>& pixels, uint32_t page, GPUTextureMode mode, uint32_t palette_x,
                  uint32_t palette_y) const;
  // Shared by the synchronous path and the compose worker; reads the page from
  // a caller-provided (possibly packed) snapshot instead of the live shadow.
  static void DecodeExpandedPage(std::vector<uint32_t>& pixels, GPUTextureMode mode, const uint16_t* page_ptr,
                                 size_t page_row_stride_words, const uint16_t* palette, size_t palette_words);

  std::shared_ptr<Common::RGBA8Image> ComposePage(const std::vector<ReplacementMatch>& matches, uint32_t page,
                                                  GPUTextureMode mode, uint32_t palette_x, uint32_t palette_y,
                                                  float* scale_x, float* scale_y);
  static std::shared_ptr<Common::RGBA8Image> ComposePageImage(const std::vector<ResolvedMatch>& matches,
                                                              const std::vector<uint32_t>& base_pixels,
                                                              uint32_t max_texture_size, uint32_t resolution_scale,
                                                              float* scale_x, float* scale_y);

  void EvictPageReplacementsForBudget(size_t incoming_bytes);

  std::string m_game_id;

  TextureCache m_texture_cache;
  TextureLruList m_texture_lru;
  TextureLruPositions m_texture_lru_positions;
  size_t m_texture_cache_bytes = 0;

  // Decode worker. Queue and completion list are guarded by the mutex; the
  // cache itself is only mutated on the emulation thread.
  std::thread m_texture_loader_thread;
  std::mutex m_texture_loader_mutex;
  std::condition_variable m_texture_loader_cv;
  bool m_texture_loader_stop = false;
  std::deque<std::string> m_texture_loader_queue;
  std::deque<DecodedTexture> m_texture_loader_completed;
  std::unordered_set<std::string> m_texture_load_pending;
  std::atomic<uint64_t> m_texture_load_generation{0};
  bool m_texture_pending_hit = false;

  static constexpr size_t MAX_TEXTURE_LOAD_REQUESTS = 192;

  // Compose worker. Keeping page composition off the emulation thread means a
  // burst of animated pages never delays a frame; the frame only snapshots the
  // page (a few tens of KB) and enqueues a job.
  std::thread m_compose_thread;
  std::mutex m_compose_mutex;
  std::condition_variable m_compose_cv;
  bool m_compose_stop = false;
  std::deque<ComposeJob> m_compose_queue;
  std::deque<CompletedCompose> m_compose_completed;
  uint64_t m_next_compose_serial = 1;

  static constexpr size_t MAX_COMPOSE_QUEUE = 32;

  VRAMWriteReplacementMap m_vram_write_replacements;

  // [base mode 0..2] - the ST* (semi-transparent) variants share the
  // bucket with their base mode; the ST bit only affects compositing.
  std::array<std::vector<TexturePageReplacementEntry>, 3> m_texpage_replacements;
  // texupload-* entries, same bucket layout. These anchor to a VRAM write rect
  // instead of the page, so they are matched against the recorded uploads.
  std::array<std::vector<TexturePageReplacementEntry>, 3> m_texupload_replacements;
  uint32_t m_texupload_replacement_count = 0;

  // Uploads seen on the CPU side, keyed by their VRAM rect. Games re-upload
  // the same texture constantly, so identical rects fold into one record and
  // the content hash is recomputed from the VRAM shadow at match time.
  struct VRAMWriteRecord
  {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
  };

  static constexpr size_t MAX_VRAM_WRITE_RECORDS = 256;
  std::unordered_map<uint64_t, VRAMWriteRecord> m_vram_writes;
  std::deque<uint64_t> m_vram_write_order;
  void AddVRAMWriteRecord(uint32_t x, uint32_t y, uint32_t width, uint32_t height);
  void ClearVRAMWriteRecords();

  const uint16_t* m_vram = nullptr;
  uint32_t m_max_texture_size = 2048;
  uint32_t m_resolution_scale = 1;

  // Cap the number of page recompositions per frame. Composing a page costs a
  // few hundred microseconds of emulation-thread time, and a scene change can
  // mark dozens of animated pages dirty at once; without a cap those rebuilds
  // stack up into one long frame (a visible drop with an audible audio hitch).
  static constexpr uint32_t MAX_COMPOSES_PER_FRAME = 4;

  std::unordered_map<PageCacheKey, CachedPage, PageCacheKeyHash> m_page_cache;
  size_t m_page_cache_bytes = 0;
  uint64_t m_page_cache_used_counter = 0;
  uint64_t m_next_replacement_id = 1;
  uint32_t m_composes_remaining = MAX_COMPOSES_PER_FRAME;

  struct CachedHash
  {
    uint64_t revision;
    uint64_t hash;
  };

  // One entry per logical source. The revision signature covers every physical
  // 64x256 page in that source, including wrapped P8/C16 pages and CLUTs.
  std::unordered_map<uint32_t, CachedHash> m_page_hash_cache;
  std::unordered_map<uint32_t, CachedHash> m_palette_hash_cache;

  // Partial-rectangle hashes for texpage matching, memoized by the page
  // content revision so unchanged pages never re-hash their sub-rectangles.
  struct RectHashKey
  {
    uint32_t left;
    uint32_t top;
    uint32_t width;
    uint32_t height;

    bool operator==(const RectHashKey& rhs) const
    {
      return left == rhs.left && top == rhs.top && width == rhs.width && height == rhs.height;
    }
  };

  struct RectHashKeyHash
  {
    size_t operator()(const RectHashKey& k) const;
  };

  std::unordered_map<RectHashKey, CachedHash, RectHashKeyHash> m_rect_hash_cache;
};

extern TextureReplacements g_texture_replacements;

// EmuCoreR Android frontend hook: overrides the base directory replacement
// textures are loaded from. An empty path restores the default location.
void SetTextureReplacementsPathOverride(std::string path);
