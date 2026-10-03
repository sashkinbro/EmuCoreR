#include "gpu_hw_texture_cache.h"
#include "common/state_wrapper.h"
#include "common/byte_stream.h"
#include <cstdio>
#include <vector>

int main()
{
  int failures = 0;
  auto check = [&](bool ok, const char* name) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    failures += !ok;
  };
  std::vector<uint16_t> vram(1024 * 512, 0x001f);
  GPUTexturePageCache cache;
  GPUTexturePageCache::SourceKey key{};
  key.page_x = 512;
  key.mode = GPUTextureMode::Direct16Bit;
  auto initial = cache.Lookup(key, vram.data(), 10);
  check(!initial.valid && initial.needs_upload, "cold lookup requests upload");
  cache.MarkDecoded(initial.slot);
  check(cache.Lookup(key, vram.data(), 10).valid, "completed upload is a hit");
  vram[512] = 0x03e0;
  cache.AddWrittenRectangle(512, 0, 513, 1, true);
  auto changed = cache.Lookup(key, vram.data(), 10);
  check(!changed.valid && changed.needs_upload, "changed content requests upload");
  // Simulate the real backend taking its budget/busy-buffer fallback: no
  // MarkDecoded call, and no GPU upload for the changed content.
  auto deferred = cache.Lookup(key, vram.data(), 10);
  check(!deferred.valid && deferred.needs_upload, "deferred upload must stay invalid on repeated lookup");
  vram[512] = 0x7c00;
  cache.AddWrittenRectangle(512, 0, 513, 1, true);
  check(!cache.Lookup(key, vram.data(), 11).valid, "second write during deferred upload stays invalid");
  vram[512] = 0x001f;
  cache.AddWrittenRectangle(512, 0, 513, 1, true);
  check(cache.Lookup(key, vram.data(), 11).valid, "reverting a deferred write reuses the actual uploaded image");
  vram[512] = 0x03e0;
  cache.AddWrittenRectangle(512, 0, 513, 1, true);
  cache.Lookup(key, vram.data(), 11);
  cache.MarkDecoded(deferred.slot);
  cache.AddWrittenRectangle(512, 0, 513, 1, true);
  check(cache.Lookup(key, vram.data(), 11).valid, "unchanged write revalidates uploaded content");
  cache.AddDrawnRectangle(512, 0, 513, 1);
  check(cache.Lookup(key, vram.data(), 12).slot == GPUTexturePageCache::NO_SLOT,
        "GPU-produced source excluded");
  cache.AddWrittenRectangle(512, 0, 513, 1, false);
  check(cache.Lookup(key, vram.data(), 12).slot == GPUTexturePageCache::NO_SLOT,
        "masked/non-authoritative write remains excluded");
  cache.AddWrittenRectangle(512, 0, 513, 1, true);
  check(cache.Lookup(key, vram.data(), 12).slot != GPUTexturePageCache::NO_SLOT,
        "authoritative overwrite clears draw exclusion");

  cache.AddDrawnRectangle(512, 0, 513, 2);
  cache.InvalidateContents();
  check(cache.AreSourcePagesDrawn(key), "filter transition preserves GPU-source exclusions");
  cache.AddWrittenRectangle(512, 0, 513, 2, false);
  check(cache.AreSourcePagesDrawn(key), "interlaced fill does not clear untouched GPU rows");
  std::vector<uint8_t> state(4096);
  auto stream = ByteStream_CreateMemoryStream(state.data(), static_cast<uint32_t>(state.size()));
  StateWrapper writer(stream.get(), StateWrapper::Mode::Write, 0);
  cache.DoState(writer);
  check(!writer.HasError(), "write cache provenance snapshot");
  cache.AddWrittenRectangle(512, 0, 513, 2, true);
  check(!cache.AreSourcePagesDrawn(key), "future overwrite changes draw provenance");
  stream->SeekAbsolute(0);
  StateWrapper reader(stream.get(), StateWrapper::Mode::Read, 0);
  cache.DoState(reader);
  check(!reader.HasError() && cache.AreSourcePagesDrawn(key), "rollback restores GPU-source exclusions");
  cache.AddWrittenRectangle(512, 0, 513, 2, true);
  check(cache.Lookup(key, vram.data(), 12).valid, "rollback retains unchanged uploaded textures for hash revalidation");

  GPUTexturePageCache palette_cache;
  GPUTexturePageCache::SourceKey pkey{};
  pkey.mode = GPUTextureMode::Palette4Bit;
  pkey.page_x = 960;
  pkey.palette_x = 1008;
  pkey.palette_y = 400;
  auto p = palette_cache.Lookup(pkey, vram.data(), 20);
  palette_cache.MarkDecoded(p.slot);
  vram[400 * 1024 + 1008] ^= 1;
  palette_cache.AddWrittenRectangle(1008, 400, 1009, 401, true);
  check(!palette_cache.Lookup(pkey, vram.data(), 20).valid,
        "palette write invalidates decoded P4 page");
  check(!palette_cache.Lookup(pkey, vram.data(), 21).valid,
        "deferred palette upload must stay invalid across frames");

  GPUTexturePageCache wrapped_cache;
  GPUTexturePageCache::SourceKey wkey{};
  wkey.page_x = 960;
  wkey.page_y = 256;
  wkey.mode = GPUTextureMode::Direct16Bit;
  auto w = wrapped_cache.Lookup(wkey, vram.data(), 30);
  wrapped_cache.MarkDecoded(w.slot);
  vram[256 * 1024] ^= 1;
  wrapped_cache.AddWrittenRectangle(1023, 256, 1025, 257, true);
  check(!wrapped_cache.Lookup(wkey, vram.data(), 31).valid,
        "wrapped transfer invalidates wrapped source tail");

  std::vector<uint32_t> rgba(256 * 256);
  std::vector<uint16_t> packed(256 * 256);
  for (unsigned y = 0; y < 256; ++y)
    for (unsigned x = 0; x < 256; ++x)
      vram[y * 1024 + x] = static_cast<uint16_t>(y * 256 + x);
  GPUTexturePageCache::SourceKey direct{};
  direct.mode = GPUTextureMode::Direct16Bit;
  GPUTexturePageCache::DecodePage(direct, vram.data(), rgba.data());
  GPUTexturePageCache::DecodePage16(direct, vram.data(), packed.data());
  bool colours_ok = true;
  for (unsigned word = 0; word < 65536; ++word)
  {
    unsigned r = word & 31, g = (word >> 5) & 31, b = (word >> 10) & 31;
    unsigned expected = ((r << 3) | (r >> 2)) | (((g << 3) | (g >> 2)) << 8) |
                        (((b << 3) | (b >> 2)) << 16) | ((word & 0x8000) ? 0xff000000u : 0u);
    unsigned expected16 = (word & 0x8000) | (r << 10) | (g << 5) | b;
    colours_ok &= rgba[word] == expected && packed[word] == expected16;
  }
  check(colours_ok, "all 65536 direct colours preserve RGB and STP in both decode formats");
  std::printf("TOTAL failures=%d\n", failures);
  return failures != 0;
}
