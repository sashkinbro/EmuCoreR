// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pre-baked Vulkan source for the hardware texture page decode pass.
//
// Renders one 256x256 PS1 texture page into an RGBA8 page texture so batch
// draws can sample it with ordinary hardware filtering instead of decoding
// palette / packing formats per fragment out of the VRAM atlas.
//
// The fragment coordinates are page-local texel coordinates (0..255). The
// push constants carry the native VRAM page and palette origins; the
// resolution scale converts them to coordinates in the scaled VRAM read
// texture that binding 0 samples.
//
// Variants (selected by -D):
//   PAGE_PALETTE_4_BIT : 4-bit paletted page, 16-entry CLUT
//   PAGE_PALETTE_8_BIT : 8-bit paletted page, 256-entry CLUT
//   PAGE_DIRECT_16_BIT : 16-bit direct page
//
// The texture window is applied when the page texture is sampled, so the
// decoded page itself stays window-independent - that keeps the cache key to
// (page, palette, format) only.

#version 450 core

layout(push_constant) uniform PushConstants {
  uvec2 u_page_origin;     // native VRAM coordinates of the page origin
  uvec2 u_palette_origin;  // native VRAM coordinates of the palette origin
  uint  u_resolution_scale;
  uint  u_pad0;
} pc;

layout(set = 0, binding = 0) uniform sampler2D samp0;
layout(location = 0) out vec4 o_col0;

uint RGBA8ToRGBA5551(vec4 v)
{
  uint r = uint(roundEven(v.r * 31.0));
  uint g = uint(roundEven(v.g * 31.0));
  uint b = uint(roundEven(v.b * 31.0));
  uint a = (v.a != 0.0) ? 1u : 0u;
  return r | (g << 5) | (b << 10) | (a << 15);
}

vec4 LoadVRAMTexel(uvec2 icoord)
{
  return texelFetch(samp0, ivec2(icoord), 0);
}

void main()
{
  const uint scale = pc.u_resolution_scale;
  const uvec2 texel = uvec2(gl_FragCoord.xy);

#if defined(PAGE_DIRECT_16_BIT)
  const uvec2 icoord = uvec2((pc.u_page_origin.x + texel.x) * scale,
                             (pc.u_page_origin.y + texel.y) * scale);
  o_col0 = LoadVRAMTexel(icoord);
#else
  uvec2 index_coord = texel;
#if defined(PAGE_PALETTE_4_BIT)
  index_coord.x /= 4u;
#elif defined(PAGE_PALETTE_8_BIT)
  index_coord.x /= 2u;
#endif

  const uvec2 packed_icoord = uvec2((pc.u_page_origin.x + index_coord.x) * scale,
                                    (pc.u_page_origin.y + index_coord.y) * scale);
  const uint vram_value = RGBA8ToRGBA5551(LoadVRAMTexel(packed_icoord));

  uint palette_index;
#if defined(PAGE_PALETTE_4_BIT)
  const uint subpixel = texel.x & 3u;
  palette_index = (vram_value >> (subpixel * 4u)) & 0x0Fu;
#elif defined(PAGE_PALETTE_8_BIT)
  const uint subpixel = texel.x & 1u;
  palette_index = (vram_value >> (subpixel * 8u)) & 0xFFu;
#else
  palette_index = 0u;
#endif

  const uvec2 palette_icoord = uvec2((pc.u_palette_origin.x + palette_index) * scale,
                                     pc.u_palette_origin.y * scale);
  o_col0 = LoadVRAMTexel(palette_icoord);
#endif
}
