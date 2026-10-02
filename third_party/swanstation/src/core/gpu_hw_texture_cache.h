// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "gpu_types.h"

#include <cstdint>
#include <cstring>

// CPU-side texture page cache with content-hash validation, rectangle-based
// invalidation, and GPU draw-rect tracking.
//
//   1. CPU decode: pages are decoded on the CPU from m_vram_shadow into RGBA8
//      buffers, then uploaded via staging. No GPU decode passes, no render
//      pass breaks.
//
//   2. Rectangle invalidation: AddWrittenRectangle (CPU writes) and
//      AddDrawnRectangle (GPU draws) invalidate only entries whose
//      texture/palette rectangles actually intersect the written area.
//      Per-VRAM-page source masks (32 pages of 64x256 words) provide
//      O(entries-per-page) lookup instead of scanning all MAX_ENTRIES.
//      A content hash revalidation in Lookup avoids re-decoding entries
//      whose intersecting write did not actually change their content.
//
//   3. Draw-rect tracking: each VRAM page keeps up to
//      MAX_DRAW_RECTS_PER_PAGE precise draw rectangles plus their union.
//      Once the budget is exceeded the union is used for the remaining
//      tests. Pages with active draw rects overlapping a source are NOT
//      sampled from the cache (render-to-texture correctness). A CPU write
//      removes the draw rects it fully covers (write-wins semantics);
//      partially covered rects are kept so GPU-produced pixels never leak
//      into a decoded page.
//
//   4. Content-hash keying: HashCacheKey{texture_hash, palette_hash, mode}
//      identifies entry contents. Entries are recycled lazily with an LRU
//      fallback and compacted once per frame by age.
//
// All coordinates are native (1x) VRAM texel space. The texture window is
// applied at sample time and is deliberately not part of the key.

class GPUTexturePageCache
{
public:
  // Decoded page textures are always 256x256 texels.
  static constexpr uint32_t PAGE_TEXELS = 256;

  // Upper bound on live cache entries. Each slot owns two persistent
  // 256x256 RGBA8 textures (512 KiB) in the backend once allocated.
  static constexpr uint32_t MAX_ENTRIES = 256;

  // Returned when no slot can be handed out.
  static constexpr uint32_t NO_SLOT = UINT32_MAX;

  // Native VRAM dimensions in 16-bit words.
  static constexpr uint32_t VRAM_WIDTH = 1024;
  static constexpr uint32_t VRAM_HEIGHT = 512;

  // VRAM is divided into 32 pages of 64x256 words for per-page source
  // tracking and draw-rect tracking.
  static constexpr uint32_t VRAM_PAGE_WIDTH = 64;
  static constexpr uint32_t VRAM_PAGE_HEIGHT = 256;
  static constexpr uint32_t NUM_VRAM_PAGES = 32; // 16 cols x 2 rows

  // Maximum age (in frames) before an unreferenced entry is compacted.
  static constexpr uint32_t COMPACT_AGE_FRAMES = 600;

  // Maximum precise draw rects tracked per VRAM page.
  static constexpr uint32_t MAX_DRAW_RECTS_PER_PAGE = 4;

  // Native VRAM rectangle (left-inclusive, right/bottom-exclusive).
  struct Rect
  {
    uint16_t left, top, right, bottom;

    bool Valid() const { return left < right && top < bottom; }

    bool Intersects(const Rect& other) const
    {
      return left < other.right && other.left < right &&
             top < other.bottom && other.top < bottom;
    }

    bool Intersects(uint16_t r_left, uint16_t r_top, uint16_t r_right, uint16_t r_bottom) const
    {
      return left < r_right && r_left < right &&
             top < r_bottom && r_top < bottom;
    }
  };

  // Source key: identifies a (page, palette, mode) combination.
  struct SourceKey
  {
    uint16_t page_x = 0;
    uint16_t page_y = 0;
    uint16_t palette_x = 0;
    uint16_t palette_y = 0;
    GPUTextureMode mode = GPUTextureMode::Direct16Bit;

    bool operator==(const SourceKey& rhs) const
    {
      return page_x == rhs.page_x && page_y == rhs.page_y &&
             palette_x == rhs.palette_x && palette_y == rhs.palette_y &&
             mode == rhs.mode;
    }
    bool operator!=(const SourceKey& rhs) const { return !(*this == rhs); }
  };

  // Content-hash key: identifies an entry by its actual VRAM content.
  struct HashCacheKey
  {
    uint64_t texture_hash = 0;
    uint64_t palette_hash = 0;
    GPUTextureMode mode = GPUTextureMode::Direct16Bit;

    bool operator==(const HashCacheKey& rhs) const
    {
      return texture_hash == rhs.texture_hash &&
             palette_hash == rhs.palette_hash &&
             mode == rhs.mode;
    }
    bool operator!=(const HashCacheKey& rhs) const { return !(*this == rhs); }
  };

  struct LookupResult
  {
    uint32_t slot;
    bool valid;           // true if the slot contains correctly decoded data
    bool newly_allocated; // true the first time a slot is handed out
    bool needs_upload;    // true when the CPU decoded data needs staging upload
  };

  GPUTexturePageCache();

  // ---- Core lookup/invalidation API ----

  // Looks up or allocates a slot for the given source key. The caller must
  // decode the page (CPU-side) and upload via staging when !valid.
  // vram_ptr points to the full 1024x512 VRAM shadow.
  LookupResult Lookup(const SourceKey& key, const uint16_t* vram_ptr,
                      uint64_t frame_number);

  // Marks a slot as containing correctly decoded contents.
  void MarkDecoded(uint32_t slot);

  // Invalidates all entries (VRAM reset, context reset, resolution change).
  void InvalidateAll();

  // ---- Rectangle-based invalidation ----

  // Called on CPU VRAM writes (UpdateVRAM, FillVRAM, CopyVRAM).
  // Invalidates entries whose texture/palette rects intersect the write.
  // shadow_is_authoritative is false for check_mask writes and GPU-driven
  // copies: their result depends on GPU state the CPU shadow does not track,
  // so the written rectangle is treated like a draw and stays excluded from
  // the decoded cache.
  void AddWrittenRectangle(uint32_t left, uint32_t top, uint32_t right, uint32_t bottom,
                           bool shadow_is_authoritative);

  // Called on GPU draws (from IncludeVRAMDirtyRectangle).
  // Invalidates entries whose texture/palette rects intersect the draw,
  // and records the draw rect for the affected VRAM pages.
  void AddDrawnRectangle(uint32_t left, uint32_t top, uint32_t right, uint32_t bottom);

  // ---- Draw-rect tracking ----

  // Returns true if any VRAM page that the given source key depends on
  // has active GPU draw rects. Such pages must NOT be sampled from the
  // cache (render-to-texture correctness).
  bool AreSourcePagesDrawn(const SourceKey& key) const;

  // ---- Compaction ----

  // Called once per frame. Evicts entries older than COMPACT_AGE_FRAMES.
  void Compact(uint64_t current_frame);

  // ---- Static helpers ----

  // True when the texture mode can be represented by a decoded page.
  static bool IsCacheableTextureMode(GPUTextureMode mode);

  // Conservative native VRAM rectangle covered by a page's source texels.
  // Wrapped pages (8-bit / 16-bit pages whose base is near the VRAM edge)
  // are expanded across the full row so rectangle invalidation can never
  // miss a write to the wrapped tail.
  static Rect GetPageSourceRect(const SourceKey& key);

  // Conservative native VRAM rectangle covered by a page's palette.
  static Rect GetPagePaletteRect(const SourceKey& key);

  // Hashing helpers. These hash exactly the words DecodePage reads,
  // including wrap-around at the VRAM edges.
  static uint64_t HashPage(const SourceKey& key, const uint16_t* vram_ptr);
  static uint64_t HashPalette(const SourceKey& key, const uint16_t* vram_ptr);

  // CPU-side page decode: decodes a 256x256 page from VRAM shadow into
  // an RGBA8 output buffer. The caller owns the output buffer (must be
  // at least PAGE_TEXELS * PAGE_TEXELS * 4 bytes).
  static void DecodePage(const SourceKey& key, const uint16_t* vram_ptr,
                         uint32_t* rgba_out);

  // Returns the entry's source key for a given slot (for the backend to
  // know which page the slot represents).
  const SourceKey& GetEntryKey(uint32_t slot) const { return m_entries[slot].key; }

  // Returns the number of currently allocated entries.
  uint32_t GetAllocatedCount() const { return m_allocated_count; }

private:
  struct Entry
  {
    SourceKey key;
    HashCacheKey hash_key;
    Rect texture_rect;
    Rect palette_rect;
    uint64_t last_used_frame;
    uint64_t last_use; // monotonic use counter for LRU
    bool valid;
    bool decoded; // true once a decode completed for hash_key
    bool allocated;

    // Bitmask of which VRAM pages this entry is registered in. An entry can
    // span multiple VRAM pages; it appears in each page's source mask.
    uint32_t page_membership;
  };

  // Per-VRAM-page source tracking. With MAX_ENTRIES=256 each page stores a
  // 256-bit mask of entries whose texture/palette rects overlap it.
  struct PageTracker
  {
    uint64_t entry_mask[4];
  };

  // Draw-rect tracking per VRAM page. Up to MAX_DRAW_RECTS_PER_PAGE precise
  // rects are kept; once saturated only the union is maintained.
  struct DrawTracker
  {
    uint8_t count;
    bool saturated;
    Rect rects[MAX_DRAW_RECTS_PER_PAGE];
    Rect total;
  };

  // FNV-1a hash over a wrapped word region (width/height in 16-bit words).
  static uint64_t HashVRAMRegionWrapped(const uint16_t* vram_ptr, uint32_t x,
                                        uint32_t y, uint32_t word_width,
                                        uint32_t word_height);

  // Convert native VRAM 16-bit pixel to the RGBA8 encoding used by the
  // backend's VRAM texture (5-bit channels expanded, alpha carries STP).
  static uint32_t VRAM16ToRGBA8(uint16_t pixel);

  // Register/unregister entry in per-page trackers.
  void RegisterEntryInPages(uint32_t slot);
  void UnregisterEntryFromPages(uint32_t slot);

  // Compute VRAM page bitmask for a rect.
  static uint32_t GetPageMask(uint16_t left, uint16_t top, uint16_t right, uint16_t bottom);

  // Invalidates entries whose rects intersect the argument. Returns the
  // number of entries flipped from valid to invalid.
  uint32_t InvalidateEntriesInRect(uint16_t left, uint16_t top, uint16_t right, uint16_t bottom);

  // In-bounds implementations of the public rectangle notifications. The
  // public entry points split wrap-around transfers into up to four
  // in-bounds rectangles before calling these.
  void AddWrittenRectangleInternal(uint16_t left, uint16_t top, uint16_t right, uint16_t bottom,
                                   bool shadow_is_authoritative);
  void AddDrawnRectangleInternal(uint16_t left, uint16_t top, uint16_t right, uint16_t bottom);

  Entry m_entries[MAX_ENTRIES];
  PageTracker m_page_trackers[NUM_VRAM_PAGES];
  DrawTracker m_draw_trackers[NUM_VRAM_PAGES];
  uint64_t m_use_counter;
  uint32_t m_allocated_count;
};
