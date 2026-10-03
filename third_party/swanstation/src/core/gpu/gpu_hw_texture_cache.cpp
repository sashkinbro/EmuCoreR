// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gpu_hw_texture_cache.h"
#include "common/state_wrapper.h"

#include <algorithm>
#include <cstring>
#include <limits>

// FNV-1a constants.
static constexpr uint64_t FNV_OFFSET_BASIS = UINT64_C(14695981039346656037);
static constexpr uint64_t FNV_PRIME = UINT64_C(1099511628211);

// VK_FORMAT_A1R5G5B5_UNORM_PACK16 stores R in bits 14..10 and B in bits 4..0,
// while PS1 5551 words store B in bits 14..10 and R in bits 4..0. Swap the two
// colour fields so a raw PS1 word sampled through the hardware format yields
// the intended colour; the STP/mask bit in bit 15 stays where the shader's
// alpha test expects it.
static ALWAYS_INLINE uint16_t SwapRedBlue5551(uint16_t word)
{
  return static_cast<uint16_t>((word & 0x8000u) | ((word & 0x001Fu) << 10) | (word & 0x03E0u) |
                               ((word >> 10) & 0x001Fu));
}

GPUTexturePageCache::GPUTexturePageCache() : m_use_counter(0), m_allocated_count(0)
{
  for (Entry& entry : m_entries)
  {
    entry.last_used_frame = 0;
    entry.last_use = 0;
    entry.valid = false;
    entry.hash_valid = false;
    entry.decoded = false;
    entry.allocated = false;
    entry.page_membership = 0;
  }
  for (PageTracker& pt : m_page_trackers)
    std::memset(pt.entry_mask, 0, sizeof(pt.entry_mask));
  for (DrawTracker& dt : m_draw_trackers)
  {
    dt.count = 0;
    dt.saturated = false;
    dt.total = {};
  }
}

bool GPUTexturePageCache::IsCacheableTextureMode(GPUTextureMode mode)
{
  switch (mode)
  {
    case GPUTextureMode::Palette4Bit:
    case GPUTextureMode::Palette8Bit:
    case GPUTextureMode::Direct16Bit:
      return true;
    default:
      return false;
  }
}

GPUTexturePageCache::Rect GPUTexturePageCache::GetPageSourceRect(const SourceKey& key)
{
  const uint32_t word_width = (key.mode == GPUTextureMode::Palette4Bit) ? (PAGE_TEXELS / 4u)
                              : (key.mode == GPUTextureMode::Palette8Bit) ? (PAGE_TEXELS / 2u)
                                                                          : PAGE_TEXELS;
  Rect r;
  if (static_cast<uint32_t>(key.page_x) + word_width > VRAM_WIDTH)
  {
    r.left = 0;
    r.right = static_cast<uint16_t>(VRAM_WIDTH);
  }
  else
  {
    r.left = key.page_x;
    r.right = static_cast<uint16_t>(key.page_x + word_width);
  }

  if (static_cast<uint32_t>(key.page_y) + PAGE_TEXELS > VRAM_HEIGHT)
  {
    r.top = 0;
    r.bottom = static_cast<uint16_t>(VRAM_HEIGHT);
  }
  else
  {
    r.top = key.page_y;
    r.bottom = static_cast<uint16_t>(key.page_y + PAGE_TEXELS);
  }
  return r;
}

GPUTexturePageCache::Rect GPUTexturePageCache::GetPagePaletteRect(const SourceKey& key)
{
  Rect r;
  uint32_t palette_width;
  if (key.mode == GPUTextureMode::Palette4Bit)
    palette_width = 16;
  else if (key.mode == GPUTextureMode::Palette8Bit)
    palette_width = 256;
  else
  {
    r.left = 0;
    r.top = 0;
    r.right = 0;
    r.bottom = 0;
    return r;
  }

  if (static_cast<uint32_t>(key.palette_x) + palette_width > VRAM_WIDTH)
  {
    r.left = 0;
    r.right = static_cast<uint16_t>(VRAM_WIDTH);
  }
  else
  {
    r.left = key.palette_x;
    r.right = static_cast<uint16_t>(key.palette_x + palette_width);
  }

  r.top = std::min<uint16_t>(key.palette_y, static_cast<uint16_t>(VRAM_HEIGHT - 1u));
  r.bottom = static_cast<uint16_t>(r.top + 1u);
  return r;
}

uint32_t GPUTexturePageCache::GetPageMask(uint16_t left, uint16_t top, uint16_t right, uint16_t bottom)
{
  if (left >= right || top >= bottom)
    return 0;

  uint32_t mask = 0;
  const uint32_t page_x0 = static_cast<uint32_t>(left) / VRAM_PAGE_WIDTH;
  const uint32_t page_x1 = (static_cast<uint32_t>(right) - 1u) / VRAM_PAGE_WIDTH;
  const uint32_t page_y0 = static_cast<uint32_t>(top) / VRAM_PAGE_HEIGHT;
  const uint32_t page_y1 = (static_cast<uint32_t>(bottom) - 1u) / VRAM_PAGE_HEIGHT;
  for (uint32_t py = page_y0; py <= page_y1 && py < 2u; py++)
  {
    for (uint32_t px = page_x0; px <= page_x1 && px < 16u; px++)
      mask |= (1u << (py * 16u + px));
  }
  return mask;
}

void GPUTexturePageCache::RegisterEntryInPages(uint32_t slot)
{
  Entry& entry = m_entries[slot];
  uint32_t mask = GetPageMask(entry.texture_rect.left, entry.texture_rect.top,
                              entry.texture_rect.right, entry.texture_rect.bottom);
  if (entry.palette_rect.Valid())
  {
    mask |= GetPageMask(entry.palette_rect.left, entry.palette_rect.top,
                        entry.palette_rect.right, entry.palette_rect.bottom);
  }
  entry.page_membership = mask;

  const uint32_t word_idx = slot / 64u;
  const uint64_t bit = UINT64_C(1) << (slot % 64u);
  uint32_t pages = mask;
  while (pages != 0)
  {
    const uint32_t page = static_cast<uint32_t>(__builtin_ctz(pages));
    pages &= ~(1u << page);
    m_page_trackers[page].entry_mask[word_idx] |= bit;
  }
}

void GPUTexturePageCache::UnregisterEntryFromPages(uint32_t slot)
{
  Entry& entry = m_entries[slot];
  const uint32_t word_idx = slot / 64u;
  const uint64_t bit = UINT64_C(1) << (slot % 64u);
  uint32_t pages = entry.page_membership;
  while (pages != 0)
  {
    const uint32_t page = static_cast<uint32_t>(__builtin_ctz(pages));
    pages &= ~(1u << page);
    m_page_trackers[page].entry_mask[word_idx] &= ~bit;
  }
  entry.page_membership = 0;
}

uint64_t GPUTexturePageCache::HashVRAMRegionWrapped(const uint16_t* vram_ptr, uint32_t x,
                                                    uint32_t y, uint32_t word_width,
                                                    uint32_t word_height)
{
  uint64_t hash = FNV_OFFSET_BASIS;
  for (uint32_t row = 0; row < word_height; row++)
  {
    const uint16_t* src = &vram_ptr[((y + row) % VRAM_HEIGHT) * VRAM_WIDTH];
    for (uint32_t col = 0; col < word_width; col++)
    {
      hash ^= static_cast<uint64_t>(src[(x + col) % VRAM_WIDTH]);
      hash *= FNV_PRIME;
    }
  }
  return hash;
}

uint64_t GPUTexturePageCache::HashPage(const SourceKey& key, const uint16_t* vram_ptr)
{
  const uint32_t word_width = (key.mode == GPUTextureMode::Palette4Bit) ? (PAGE_TEXELS / 4u)
                              : (key.mode == GPUTextureMode::Palette8Bit) ? (PAGE_TEXELS / 2u)
                                                                          : PAGE_TEXELS;
  return HashVRAMRegionWrapped(vram_ptr, key.page_x, key.page_y, word_width, PAGE_TEXELS);
}

uint64_t GPUTexturePageCache::HashPalette(const SourceKey& key, const uint16_t* vram_ptr)
{
  uint32_t palette_width;
  if (key.mode == GPUTextureMode::Palette4Bit)
    palette_width = 16;
  else if (key.mode == GPUTextureMode::Palette8Bit)
    palette_width = 256;
  else
    return 0;

  return HashVRAMRegionWrapped(vram_ptr, key.palette_x, key.palette_y, palette_width, 1);
}

uint32_t GPUTexturePageCache::VRAM16ToRGBA8(uint16_t pixel)
{
  const uint32_t r5 = pixel & 0x1Fu;
  const uint32_t g5 = (pixel >> 5) & 0x1Fu;
  const uint32_t b5 = (pixel >> 10) & 0x1Fu;
  const uint32_t stp = (pixel >> 15) & 0x1u;

  // 5-bit to 8-bit expansion matching the shader's roundEven(v * 31).
  const uint32_t r8 = (r5 << 3) | (r5 >> 2);
  const uint32_t g8 = (g5 << 3) | (g5 >> 2);
  const uint32_t b8 = (b5 << 3) | (b5 >> 2);

  // Alpha carries the STP bit exactly as the backend's VRAM texture does:
  // 0x0000 decodes to fully transparent (the shader discards all-zero
  // texels), every other value has RGB or alpha set and survives.
  const uint32_t a8 = (pixel != 0 && stp) ? 0xFFu : 0u;

  return r8 | (g8 << 8) | (b8 << 16) | (a8 << 24);
}

void GPUTexturePageCache::DecodePage(const SourceKey& key, const uint16_t* vram_ptr,
                                     uint32_t* rgba_out)
{
  const uint32_t base_x = key.page_x;
  const uint32_t base_y = key.page_y;

  if (key.mode == GPUTextureMode::Direct16Bit)
  {
    for (uint32_t y = 0; y < PAGE_TEXELS; y++)
    {
      const uint32_t vram_y = (base_y + y) % VRAM_HEIGHT;
      const uint16_t* src_row = &vram_ptr[vram_y * VRAM_WIDTH];
      uint32_t* dst_row = &rgba_out[y * PAGE_TEXELS];
      for (uint32_t x = 0; x < PAGE_TEXELS; x++)
        dst_row[x] = VRAM16ToRGBA8(src_row[(base_x + x) % VRAM_WIDTH]);
    }
  }
  else if (key.mode == GPUTextureMode::Palette8Bit)
  {
    const uint32_t pal_y = key.palette_y % VRAM_HEIGHT;
    const uint16_t* pal_row = &vram_ptr[pal_y * VRAM_WIDTH];

    uint32_t palette_rgba[256];
    for (uint32_t i = 0; i < 256u; i++)
      palette_rgba[i] = VRAM16ToRGBA8(pal_row[(key.palette_x + i) % VRAM_WIDTH]);

    for (uint32_t y = 0; y < PAGE_TEXELS; y++)
    {
      const uint32_t vram_y = (base_y + y) % VRAM_HEIGHT;
      const uint16_t* src_row = &vram_ptr[vram_y * VRAM_WIDTH];
      uint32_t* dst_row = &rgba_out[y * PAGE_TEXELS];
      for (uint32_t x = 0; x < PAGE_TEXELS; x += 2u)
      {
        const uint16_t packed = src_row[(base_x + x / 2u) % VRAM_WIDTH];
        dst_row[x + 0] = palette_rgba[packed & 0xFFu];
        dst_row[x + 1] = palette_rgba[(packed >> 8) & 0xFFu];
      }
    }
  }
  else // Palette4Bit
  {
    const uint32_t pal_y = key.palette_y % VRAM_HEIGHT;
    const uint16_t* pal_row = &vram_ptr[pal_y * VRAM_WIDTH];

    uint32_t palette_rgba[16];
    for (uint32_t i = 0; i < 16u; i++)
      palette_rgba[i] = VRAM16ToRGBA8(pal_row[(key.palette_x + i) % VRAM_WIDTH]);

    for (uint32_t y = 0; y < PAGE_TEXELS; y++)
    {
      const uint32_t vram_y = (base_y + y) % VRAM_HEIGHT;
      const uint16_t* src_row = &vram_ptr[vram_y * VRAM_WIDTH];
      uint32_t* dst_row = &rgba_out[y * PAGE_TEXELS];
      for (uint32_t x = 0; x < PAGE_TEXELS; x += 4u)
      {
        const uint16_t packed = src_row[(base_x + x / 4u) % VRAM_WIDTH];
        dst_row[x + 0] = palette_rgba[(packed >> 0) & 0x0Fu];
        dst_row[x + 1] = palette_rgba[(packed >> 4) & 0x0Fu];
        dst_row[x + 2] = palette_rgba[(packed >> 8) & 0x0Fu];
        dst_row[x + 3] = palette_rgba[(packed >> 12) & 0x0Fu];
      }
    }
  }
}

void GPUTexturePageCache::DecodePage16(const SourceKey& key, const uint16_t* vram_ptr, uint16_t* out)
{
  const uint32_t base_x = key.page_x;
  const uint32_t base_y = key.page_y;

  if (key.mode == GPUTextureMode::Direct16Bit)
  {
    for (uint32_t y = 0; y < PAGE_TEXELS; y++)
    {
      const uint16_t* src_row = &vram_ptr[((base_y + y) % VRAM_HEIGHT) * VRAM_WIDTH];
      uint16_t* dst_row = &out[y * PAGE_TEXELS];
      for (uint32_t x = 0; x < PAGE_TEXELS; x++)
        dst_row[x] = SwapRedBlue5551(src_row[(base_x + x) % VRAM_WIDTH]);
    }
  }
  else if (key.mode == GPUTextureMode::Palette8Bit)
  {
    const uint16_t* pal_row = &vram_ptr[(key.palette_y % VRAM_HEIGHT) * VRAM_WIDTH];
    uint16_t palette[256];
    for (uint32_t i = 0; i < 256u; i++)
      palette[i] = SwapRedBlue5551(pal_row[(key.palette_x + i) % VRAM_WIDTH]);

    for (uint32_t y = 0; y < PAGE_TEXELS; y++)
    {
      const uint16_t* src_row = &vram_ptr[((base_y + y) % VRAM_HEIGHT) * VRAM_WIDTH];
      uint16_t* dst_row = &out[y * PAGE_TEXELS];
      for (uint32_t x = 0; x < PAGE_TEXELS; x += 2u)
      {
        const uint16_t packed = src_row[(base_x + x / 2u) % VRAM_WIDTH];
        dst_row[x + 0] = palette[packed & 0xFFu];
        dst_row[x + 1] = palette[(packed >> 8) & 0xFFu];
      }
    }
  }
  else // Palette4Bit
  {
    const uint16_t* pal_row = &vram_ptr[(key.palette_y % VRAM_HEIGHT) * VRAM_WIDTH];
    uint16_t palette[16];
    for (uint32_t i = 0; i < 16u; i++)
      palette[i] = SwapRedBlue5551(pal_row[(key.palette_x + i) % VRAM_WIDTH]);

    for (uint32_t y = 0; y < PAGE_TEXELS; y++)
    {
      const uint16_t* src_row = &vram_ptr[((base_y + y) % VRAM_HEIGHT) * VRAM_WIDTH];
      uint16_t* dst_row = &out[y * PAGE_TEXELS];
      for (uint32_t x = 0; x < PAGE_TEXELS; x += 4u)
      {
        const uint16_t packed = src_row[(base_x + x / 4u) % VRAM_WIDTH];
        dst_row[x + 0] = palette[(packed >> 0) & 0x0Fu];
        dst_row[x + 1] = palette[(packed >> 4) & 0x0Fu];
        dst_row[x + 2] = palette[(packed >> 8) & 0x0Fu];
        dst_row[x + 3] = palette[(packed >> 12) & 0x0Fu];
      }
    }
  }
}

GPUTexturePageCache::LookupResult GPUTexturePageCache::Lookup(const SourceKey& key,
                                                              const uint16_t* vram_ptr,
                                                              uint64_t frame_number)
{
  m_use_counter++;

  if (AreSourcePagesDrawn(key))
    return LookupResult{NO_SLOT, false, false, false};

  uint32_t free_slot = NO_SLOT;
  uint32_t lru_slot = NO_SLOT;
  uint64_t lru_use = std::numeric_limits<uint64_t>::max();

  for (uint32_t i = 0; i < MAX_ENTRIES; i++)
  {
    Entry& entry = m_entries[i];
    if (entry.allocated && entry.key == key)
    {
      entry.last_use = m_use_counter;
      entry.last_used_frame = frame_number;

      if (entry.valid)
        return LookupResult{i, true, false, false};

      // The entry was invalidated by a write/draw that may not have changed
      // its actual content. Content-hash revalidation avoids a re-decode.
      if (!entry.hash_valid)
      {
        entry.pending_hash_key = HashCacheKey{HashPage(key, vram_ptr), HashPalette(key, vram_ptr), key.mode};
        entry.hash_valid = true;
      }
      if (entry.decoded && entry.pending_hash_key == entry.hash_key)
      {
        entry.valid = true;
        return LookupResult{i, true, false, false};
      }

      return LookupResult{i, false, false, true};
    }

    if (!entry.allocated)
    {
      if (free_slot == NO_SLOT)
        free_slot = i;
      continue;
    }

    // LRU candidate: never evict entries used in the current or previous frame.
    if (entry.last_used_frame + 1u < frame_number && entry.last_use < lru_use)
    {
      lru_use = entry.last_use;
      lru_slot = i;
    }
  }

  const bool newly_allocated = (free_slot != NO_SLOT);
  const uint32_t slot = newly_allocated ? free_slot : lru_slot;
  if (slot == NO_SLOT)
    return LookupResult{NO_SLOT, false, false, false};

  Entry& entry = m_entries[slot];
  if (entry.allocated)
    UnregisterEntryFromPages(slot);
  else
    m_allocated_count++;

  entry.key = key;
  entry.pending_hash_key = HashCacheKey{HashPage(key, vram_ptr), HashPalette(key, vram_ptr), key.mode};
  entry.hash_valid = true;
  entry.texture_rect = GetPageSourceRect(key);
  entry.palette_rect = GetPagePaletteRect(key);
  entry.last_use = m_use_counter;
  entry.last_used_frame = frame_number;
  entry.valid = false;
  entry.decoded = false;
  entry.allocated = true;

  RegisterEntryInPages(slot);

  return LookupResult{slot, false, newly_allocated, true};
}

void GPUTexturePageCache::MarkDecoded(uint32_t slot)
{
  if (slot >= MAX_ENTRIES || !m_entries[slot].allocated || !m_entries[slot].hash_valid)
    return;
  m_entries[slot].hash_key = m_entries[slot].pending_hash_key;
  m_entries[slot].valid = true;
  m_entries[slot].decoded = true;
}

void GPUTexturePageCache::InvalidateAll()
{
  for (uint32_t i = 0; i < MAX_ENTRIES; i++)
  {
    m_entries[i].valid = false;
    m_entries[i].hash_valid = false;
    m_entries[i].decoded = false;
    if (m_entries[i].allocated)
    {
      UnregisterEntryFromPages(i);
      m_entries[i].allocated = false;
    }
  }
  m_allocated_count = 0;

  InvalidateContents(true);
}

void GPUTexturePageCache::InvalidateContents(bool clear_draw_rects)
{
  for (Entry& entry : m_entries)
  {
    entry.valid = false;
    entry.hash_valid = false;
  }
  if (!clear_draw_rects)
    return;
  for (DrawTracker& dt : m_draw_trackers)
  {
    dt.count = 0;
    dt.saturated = false;
    dt.total = {};
  }
}

void GPUTexturePageCache::DoState(StateWrapper& sw)
{
  if (sw.IsReading())
    InvalidateContents(true);
  for (DrawTracker& dt : m_draw_trackers)
  {
    sw.Do(&dt.count);
    sw.Do(&dt.saturated);
    sw.DoBytes(&dt.total, sizeof(dt.total));
    dt.count = std::min<uint8_t>(dt.count, MAX_DRAW_RECTS_PER_PAGE);
    sw.DoBytes(dt.rects, dt.count * sizeof(Rect));
  }
}

uint32_t GPUTexturePageCache::InvalidateEntriesInRect(uint16_t left, uint16_t top,
                                                      uint16_t right, uint16_t bottom)
{
  uint32_t invalidated = 0;
  uint32_t pages = GetPageMask(left, top, right, bottom);
  while (pages != 0)
  {
    const uint32_t page = static_cast<uint32_t>(__builtin_ctz(pages));
    pages &= ~(1u << page);

    const PageTracker& pt = m_page_trackers[page];
    for (uint32_t w = 0; w < 4u; w++)
    {
      uint64_t bits = pt.entry_mask[w];
      while (bits != 0)
      {
        const uint32_t bit_idx = static_cast<uint32_t>(__builtin_ctzll(bits));
        bits &= ~(UINT64_C(1) << bit_idx);
        const uint32_t slot = w * 64u + bit_idx;
        Entry& entry = m_entries[slot];
        if (!entry.allocated || (!entry.valid && !entry.hash_valid))
          continue;

        if (entry.texture_rect.Intersects(left, top, right, bottom) ||
            (entry.palette_rect.Valid() && entry.palette_rect.Intersects(left, top, right, bottom)))
        {
          invalidated += entry.valid;
          entry.valid = false;
          entry.hash_valid = false;
        }
      }
    }
  }

  return invalidated;
}

void GPUTexturePageCache::ClearDrawRectsInRect(uint16_t left, uint16_t top, uint16_t right,
                                               uint16_t bottom)
{
  const auto contained = [left, top, right, bottom](const Rect& r) {
    return r.left >= left && r.top >= top && r.right <= right && r.bottom <= bottom;
  };

  uint32_t pages = GetPageMask(left, top, right, bottom);
  while (pages != 0)
  {
    const uint32_t page = static_cast<uint32_t>(__builtin_ctz(pages));
    pages &= ~(1u << page);

    DrawTracker& dt = m_draw_trackers[page];
    if (dt.count == 0 && !dt.saturated)
      continue;

    if (dt.saturated)
    {
      // The precise rects were collapsed into the union; it can only be
      // cleared when the write covers every draw recorded for this page.
      if (contained(dt.total))
      {
        dt.count = 0;
        dt.saturated = false;
        dt.total = {};
      }
      continue;
    }

    uint32_t write_index = 0;
    for (uint32_t i = 0; i < dt.count; i++)
    {
      if (!contained(dt.rects[i]))
        dt.rects[write_index++] = dt.rects[i];
    }
    dt.count = static_cast<uint8_t>(write_index);
    if (dt.count == 0)
      dt.total = {};
  }
}

void GPUTexturePageCache::AddWrittenRectangle(uint32_t left, uint32_t top,
                                              uint32_t right, uint32_t bottom,
                                              bool shadow_is_authoritative)
{
  if (left >= right || top >= bottom)
    return;

  // Split a possibly wrap-around transfer into up to four in-bounds rects so
  // every touched VRAM page is invalidated.
  const uint32_t width = std::min(right - left, VRAM_WIDTH);
  const uint32_t height = std::min(bottom - top, VRAM_HEIGHT);
  const uint32_t first_x = left % VRAM_WIDTH;
  const uint32_t first_y = top % VRAM_HEIGHT;
  const uint32_t first_width = std::min(width, VRAM_WIDTH - first_x);
  const uint32_t first_height = std::min(height, VRAM_HEIGHT - first_y);
  const uint32_t wrap_width = width - first_width;
  const uint32_t wrap_height = height - first_height;

  const uint16_t x0 = static_cast<uint16_t>(first_x);
  const uint16_t y0 = static_cast<uint16_t>(first_y);
  const uint16_t x1 = static_cast<uint16_t>(first_x + first_width);
  const uint16_t y1 = static_cast<uint16_t>(first_y + first_height);
  const uint16_t wx = static_cast<uint16_t>(wrap_width);
  const uint16_t wy = static_cast<uint16_t>(wrap_height);

  AddWrittenRectangleInternal(x0, y0, x1, y1, shadow_is_authoritative);
  if (wx > 0)
    AddWrittenRectangleInternal(0, y0, wx, y1, shadow_is_authoritative);
  if (wy > 0)
  {
    AddWrittenRectangleInternal(x0, 0, x1, wy, shadow_is_authoritative);
    if (wx > 0)
      AddWrittenRectangleInternal(0, 0, wx, wy, shadow_is_authoritative);
  }
}

void GPUTexturePageCache::AddWrittenRectangleInternal(uint16_t left, uint16_t top,
                                                      uint16_t right, uint16_t bottom,
                                                      bool shadow_is_authoritative)
{
  if (left >= right || top >= bottom)
    return;

  if (shadow_is_authoritative)
  {
    // The CPU shadow reproduces this write exactly, so overlapping entries are
    // simply re-decoded on their next use. Draw rects fully covered by the
    // write are no longer needed: the write wins over the earlier GPU draw for
    // that area in both the GPU buffer (queue order) and the shadow the cache
    // decodes from. Partially covered rects stay so GPU-produced pixels never
    // leak into a decoded page.
    ClearDrawRectsInRect(left, top, right, bottom);
    InvalidateEntriesInRect(left, top, right, bottom);
  }
  else
  {
    // check_mask writes and GPU-driven copies resolve against GPU state the
    // CPU shadow does not track, so their result is not reproducible from the
    // shadow. Treat the written rectangle like a draw: intersecting entries
    // are invalidated and the region stays out of the decoded cache.
    AddDrawnRectangleInternal(left, top, right, bottom);
  }
}

void GPUTexturePageCache::AddDrawnRectangle(uint32_t left, uint32_t top,
                                            uint32_t right, uint32_t bottom)
{
  if (left >= right || top >= bottom)
    return;

  const uint32_t width = std::min(right - left, VRAM_WIDTH);
  const uint32_t height = std::min(bottom - top, VRAM_HEIGHT);
  const uint32_t first_x = left % VRAM_WIDTH;
  const uint32_t first_y = top % VRAM_HEIGHT;
  const uint32_t first_width = std::min(width, VRAM_WIDTH - first_x);
  const uint32_t first_height = std::min(height, VRAM_HEIGHT - first_y);
  const uint32_t wrap_width = width - first_width;
  const uint32_t wrap_height = height - first_height;

  const uint16_t x0 = static_cast<uint16_t>(first_x);
  const uint16_t y0 = static_cast<uint16_t>(first_y);
  const uint16_t x1 = static_cast<uint16_t>(first_x + first_width);
  const uint16_t y1 = static_cast<uint16_t>(first_y + first_height);
  const uint16_t wx = static_cast<uint16_t>(wrap_width);
  const uint16_t wy = static_cast<uint16_t>(wrap_height);

  AddDrawnRectangleInternal(x0, y0, x1, y1);
  if (wx > 0)
    AddDrawnRectangleInternal(0, y0, wx, y1);
  if (wy > 0)
  {
    AddDrawnRectangleInternal(x0, 0, x1, wy);
    if (wx > 0)
      AddDrawnRectangleInternal(0, 0, wx, wy);
  }
}

void GPUTexturePageCache::AddDrawnRectangleInternal(uint16_t rleft, uint16_t rtop,
                                                    uint16_t rright, uint16_t rbottom)
{
  if (rleft >= rright || rtop >= rbottom)
    return;

  uint32_t pages = GetPageMask(rleft, rtop, rright, rbottom);
  while (pages != 0)
  {
    const uint32_t page = static_cast<uint32_t>(__builtin_ctz(pages));
    pages &= ~(1u << page);

    DrawTracker& dt = m_draw_trackers[page];
    if (!dt.saturated)
    {
      bool merged = false;
      for (uint32_t i = 0; i < dt.count; i++)
      {
        if (dt.rects[i].Intersects(rleft, rtop, rright, rbottom))
        {
          dt.rects[i].left = std::min(dt.rects[i].left, rleft);
          dt.rects[i].top = std::min(dt.rects[i].top, rtop);
          dt.rects[i].right = std::max(dt.rects[i].right, rright);
          dt.rects[i].bottom = std::max(dt.rects[i].bottom, rbottom);
          merged = true;
          break;
        }
      }

      if (!merged)
      {
        if (dt.count < MAX_DRAW_RECTS_PER_PAGE)
          dt.rects[dt.count++] = Rect{rleft, rtop, rright, rbottom};
        else
          dt.saturated = true;
      }
    }

    if (!dt.total.Valid())
      dt.total = Rect{rleft, rtop, rright, rbottom};
    else
    {
      dt.total.left = std::min(dt.total.left, rleft);
      dt.total.top = std::min(dt.total.top, rtop);
      dt.total.right = std::max(dt.total.right, rright);
      dt.total.bottom = std::max(dt.total.bottom, rbottom);
    }
  }

  InvalidateEntriesInRect(rleft, rtop, rright, rbottom);
}

bool GPUTexturePageCache::AreSourcePagesDrawn(const SourceKey& key) const
{
  const Rect texture_rect = GetPageSourceRect(key);
  const Rect palette_rect = GetPagePaletteRect(key);

  uint32_t pages = GetPageMask(texture_rect.left, texture_rect.top, texture_rect.right, texture_rect.bottom);
  if (palette_rect.Valid())
  {
    pages |= GetPageMask(palette_rect.left, palette_rect.top, palette_rect.right, palette_rect.bottom);
  }

  while (pages != 0)
  {
    const uint32_t page = static_cast<uint32_t>(__builtin_ctz(pages));
    pages &= ~(1u << page);

    const DrawTracker& dt = m_draw_trackers[page];
    if (dt.count == 0 && !dt.saturated)
      continue;

    if (dt.saturated)
    {
      if (texture_rect.Intersects(dt.total) ||
          (palette_rect.Valid() && palette_rect.Intersects(dt.total)))
        return true;
      continue;
    }

    for (uint32_t i = 0; i < dt.count; i++)
    {
      if (texture_rect.Intersects(dt.rects[i]) ||
          (palette_rect.Valid() && palette_rect.Intersects(dt.rects[i])))
        return true;
    }
  }

  return false;
}

void GPUTexturePageCache::Compact(uint64_t current_frame)
{
  for (uint32_t i = 0; i < MAX_ENTRIES; i++)
  {
    Entry& entry = m_entries[i];
    if (!entry.allocated)
      continue;

    if (current_frame > entry.last_used_frame &&
        (current_frame - entry.last_used_frame) > COMPACT_AGE_FRAMES)
    {
      UnregisterEntryFromPages(i);
      entry.allocated = false;
      entry.valid = false;
      entry.decoded = false;
      m_allocated_count--;
    }
  }
}
