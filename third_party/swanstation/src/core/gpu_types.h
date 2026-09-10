#pragma once
#include "common/bitfield.h"
#include "common/rectangle.h"
#include "types.h"
#include <array>

inline constexpr uint32_t VRAM_WIDTH = 1024, VRAM_HEIGHT = 512, VRAM_SIZE = VRAM_WIDTH * VRAM_HEIGHT * sizeof(uint16_t),
                     VRAM_WIDTH_MASK = VRAM_WIDTH - 1, VRAM_HEIGHT_MASK = VRAM_HEIGHT - 1, TEXTURE_PAGE_WIDTH = 256,
                     TEXTURE_PAGE_HEIGHT = 256,

                     // In interlaced modes, we can exceed the 512 height of VRAM, up to 576 in PAL games.
  GPU_MAX_DISPLAY_WIDTH = 720, GPU_MAX_DISPLAY_HEIGHT = 576,

                     DITHER_MATRIX_SIZE = 4;

inline constexpr int32_t MAX_PRIMITIVE_WIDTH = 1024, MAX_PRIMITIVE_HEIGHT = 512;

enum class GPUPrimitive : uint8_t
{
  Reserved = 0,
  Polygon = 1,
  Line = 2,
  Rectangle = 3
};

enum class GPUDrawRectangleSize : uint8_t
{
  Variable = 0,
  R1x1 = 1,
  R8x8 = 2,
  R16x16 = 3
};

enum class GPUTextureMode : uint8_t
{
  Palette4Bit = 0,
  Palette8Bit = 1,
  Direct16Bit = 2,
  Reserved_Direct16Bit = 3,

  // Not register values.
  RawTextureBit = 4,
  RawPalette4Bit = RawTextureBit | Palette4Bit,
  RawPalette8Bit = RawTextureBit | Palette8Bit,
  RawDirect16Bit = RawTextureBit | Direct16Bit,
  Reserved_RawDirect16Bit = RawTextureBit | Reserved_Direct16Bit,

  Disabled = 8 // Not a register value
};

IMPLEMENT_ENUM_CLASS_BITWISE_OPERATORS(GPUTextureMode);

enum class GPUTransparencyMode : uint8_t
{
  HalfBackgroundPlusHalfForeground = 0,
  BackgroundPlusForeground = 1,
  BackgroundMinusForeground = 2,
  BackgroundPlusQuarterForeground = 3,

  Disabled = 4 // Not a register value
};

enum class GPUInterlacedDisplayMode : uint8_t
{
  None,
  InterleavedFields,
  SeparateFields
};

union GPURenderCommand
{
  uint32_t bits;

  BitField<uint32_t, uint32_t, 0, 24> color_for_first_vertex;
  BitField<uint32_t, bool, 24, 1> raw_texture_enable; // not valid for lines
  BitField<uint32_t, bool, 25, 1> transparency_enable;
  BitField<uint32_t, bool, 26, 1> texture_enable;
  BitField<uint32_t, GPUDrawRectangleSize, 27, 2> rectangle_size; // only for rectangles
  BitField<uint32_t, bool, 27, 1> quad_polygon;                   // only for polygons
  BitField<uint32_t, bool, 27, 1> polyline;                       // only for lines
  BitField<uint32_t, bool, 28, 1> shading_enable;                 // 0 - flat, 1 = gouroud
  BitField<uint32_t, GPUPrimitive, 29, 21> primitive;

  /// Returns true if texturing should be enabled. Depends on the primitive type.
  ALWAYS_INLINE bool IsTexturingEnabled() const { return (primitive != GPUPrimitive::Line) ? texture_enable : false; }

  /// Returns true if dithering should be enabled. Depends on the primitive type.
  ALWAYS_INLINE bool IsDitheringEnabled() const
  {
    switch (primitive)
    {
      case GPUPrimitive::Polygon:
        return shading_enable || (texture_enable && !raw_texture_enable);

      case GPUPrimitive::Line:
        return true;

      case GPUPrimitive::Rectangle:
      default:
        return false;
    }
  }
};

ALWAYS_INLINE static constexpr uint32_t VRAMRGBA5551ToRGBA8888(uint32_t color)
{
  // Helper/format conversion functions - constants from https://stackoverflow.com/a/9069480
#define E5TO8(color) ((((color) * 527u) + 23u) >> 6)

  const uint32_t r = E5TO8(color & 31u);
  const uint32_t g = E5TO8((color >> 5) & 31u);
  const uint32_t b = E5TO8((color >> 10) & 31u);
  const uint32_t a = ((color >> 15) != 0) ? 255 : 0;
  return static_cast<uint32_t>(r) | (static_cast<uint32_t>(g) << 8) | (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(a) << 24);

#undef E5TO8
}

ALWAYS_INLINE static constexpr uint16_t VRAMRGBA8888ToRGBA5551(uint32_t color)
{
  const uint32_t r = (color & 0xFFu) >> 3;
  const uint32_t g = ((color >> 8) & 0xFFu) >> 3;
  const uint32_t b = ((color >> 16) & 0xFFu) >> 3;
  const uint32_t a = ((color >> 24) & 0x01u);
  return static_cast<uint16_t>(r | (g << 5) | (b << 10) | (a << 15));
}

union GPUVertexPosition
{
  uint32_t bits;

  BitField<uint32_t, int32_t, 0, 11> x;
  BitField<uint32_t, int32_t, 16, 11> y;
};

// Sprites/rectangles should be clipped to 12 bits before drawing.
static constexpr int32_t TruncateGPUVertexPosition(int32_t x)
{
  // Sign-extend low 11 bits to int32_t. Shift in unsigned space to avoid UB
  // on negative left shift (UB in C++<20; well-defined in C++20+).
  return static_cast<int32_t>(static_cast<uint32_t>(x) << 21) >> 21;
}

// bits in GP0(E1h) or texpage part of polygon
union GPUDrawModeReg
{
  static constexpr uint16_t MASK = 0b1111111111111;
  static constexpr uint16_t TEXTURE_PAGE_MASK = UINT16_C(0b0000000000011111);

  // Polygon texpage commands only affect bits 0-8, 11
  static constexpr uint16_t POLYGON_TEXPAGE_MASK = 0b0000100111111111;

  // Bits 0..5 are returned in the GPU status register, latched at E1h/polygon draw time.
  static constexpr uint32_t GPUSTAT_MASK = 0b11111111111;

  uint16_t bits;

  BitField<uint16_t, uint8_t, 0, 4> texture_page_x_base;
  BitField<uint16_t, uint8_t, 4, 1> texture_page_y_base;
  BitField<uint16_t, GPUTransparencyMode, 5, 2> transparency_mode;
  BitField<uint16_t, GPUTextureMode, 7, 2> texture_mode;
  BitField<uint16_t, bool, 9, 1> dither_enable;
  BitField<uint16_t, bool, 10, 1> draw_to_displayed_field;
  BitField<uint16_t, bool, 11, 1> texture_disable;
  BitField<uint16_t, bool, 12, 1> texture_x_flip;
  BitField<uint16_t, bool, 13, 1> texture_y_flip;

  ALWAYS_INLINE uint16_t GetTexturePageBaseX() const { return static_cast<uint16_t>(texture_page_x_base.GetValue()) * 64; }
  ALWAYS_INLINE uint16_t GetTexturePageBaseY() const { return static_cast<uint16_t>(texture_page_y_base.GetValue()) * 256; }

  /// Returns true if the texture mode requires a palette.
  ALWAYS_INLINE bool IsUsingPalette() const { return (bits & (2 << 7)) == 0; }

  /// Returns a rectangle comprising the texture page area.
  ALWAYS_INLINE_RELEASE Common::Rectangle<uint32_t> GetTexturePageRectangle() const
  {
    static constexpr std::array<uint32_t, 4> texture_page_widths = {
      {TEXTURE_PAGE_WIDTH / 4, TEXTURE_PAGE_WIDTH / 2, TEXTURE_PAGE_WIDTH, TEXTURE_PAGE_WIDTH}};
    return Common::Rectangle<uint32_t>::FromExtents(GetTexturePageBaseX(), GetTexturePageBaseY(),
                                               texture_page_widths[static_cast<uint8_t>(texture_mode.GetValue())],
                                               TEXTURE_PAGE_HEIGHT);
  }
};

union GPUTexturePaletteReg
{
  static constexpr uint16_t MASK = UINT16_C(0b0111111111111111);

  uint16_t bits;

  BitField<uint16_t, uint16_t, 0, 6> x;
  BitField<uint16_t, uint16_t, 6, 9> y;

  ALWAYS_INLINE uint32_t GetXBase() const { return static_cast<uint32_t>(x) * 16u; }
  ALWAYS_INLINE uint32_t GetYBase() const { return static_cast<uint32_t>(y); }
};

struct GPUTextureWindow
{
  uint8_t and_x;
  uint8_t and_y;
  uint8_t or_x;
  uint8_t or_y;
};

// 4x4 dither matrix.
static constexpr int32_t DITHER_MATRIX[DITHER_MATRIX_SIZE][DITHER_MATRIX_SIZE] = {{-4, +0, -3, +1},  // row 0
                                                                              {+2, -2, +3, -1},  // row 1
                                                                              {-3, +1, -4, +0},  // row 2
                                                                              {+3, -1, +2, -2}}; // row 3

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4200) // warning C4200: nonstandard extension used: zero-sized array in struct/union
#endif

enum class GPUBackendCommandType : uint8_t
{
  Wraparound,
  Sync,
  FillVRAM,
  UpdateVRAM,
  CopyVRAM,
  SetDrawingArea,
  DrawPolygon,
  DrawRectangle,
  DrawLine
};

union GPUBackendCommandParameters
{
  uint8_t bits;

  BitField<uint8_t, bool, 0, 1> interlaced_rendering;

  /// Returns 0 if the currently-displayed field is on an even line in VRAM, otherwise 1.
  BitField<uint8_t, uint8_t, 1, 1> active_line_lsb;

  BitField<uint8_t, bool, 2, 1> set_mask_while_drawing;
  BitField<uint8_t, bool, 3, 1> check_mask_before_draw;

  ALWAYS_INLINE bool IsMaskingEnabled() const { return (bits & 12u) != 0u; }

  // During transfer/render operations, if ((dst_pixel & mask_and) == 0) { pixel = src_pixel | mask_or }
  uint16_t GetMaskAND() const
  {
    // return check_mask_before_draw ? 0x8000 : 0x0000;
    return static_cast<uint16_t>((bits << 12) & 0x8000);
  }
  uint16_t GetMaskOR() const
  {
    // return set_mask_while_drawing ? 0x8000 : 0x0000;
    return static_cast<uint16_t>((bits << 13) & 0x8000);
  }
};

struct GPUBackendCommand
{
  uint32_t size;
  GPUBackendCommandType type;
  GPUBackendCommandParameters params;
};

struct GPUBackendSyncCommand : public GPUBackendCommand
{
  bool allow_sleep;
};

struct GPUBackendFillVRAMCommand : public GPUBackendCommand
{
  uint16_t x;
  uint16_t y;
  uint16_t width;
  uint16_t height;
  uint32_t color;
};

struct GPUBackendUpdateVRAMCommand : public GPUBackendCommand
{
  uint16_t x;
  uint16_t y;
  uint16_t width;
  uint16_t height;
  uint16_t data[0];
};

struct GPUBackendCopyVRAMCommand : public GPUBackendCommand
{
  uint16_t src_x;
  uint16_t src_y;
  uint16_t dst_x;
  uint16_t dst_y;
  uint16_t width;
  uint16_t height;
};

struct GPUBackendSetDrawingAreaCommand : public GPUBackendCommand
{
  Common::Rectangle<uint32_t> new_area;
};

struct GPUBackendDrawCommand : public GPUBackendCommand
{
  GPUDrawModeReg draw_mode;
  GPURenderCommand rc;
  GPUTexturePaletteReg palette;
  GPUTextureWindow window;

  ALWAYS_INLINE bool IsDitheringEnabled() const { return rc.IsDitheringEnabled() && draw_mode.dither_enable; }
};

struct GPUBackendDrawPolygonCommand : public GPUBackendDrawCommand
{
  uint16_t num_vertices;

  struct Vertex
  {
    int32_t x, y;
    union
    {
      struct
      {
        uint8_t r, g, b, a;
      };
      uint32_t color;
    };
    union
    {
      struct
      {
        uint8_t u, v;
      };
      uint16_t texcoord;
    };
  };

  Vertex vertices[0];
};

struct GPUBackendDrawRectangleCommand : public GPUBackendDrawCommand
{
  int32_t x, y;
  uint16_t width, height;
  uint16_t texcoord;
  uint32_t color;
};

struct GPUBackendDrawLineCommand : public GPUBackendDrawCommand
{
  uint16_t num_vertices;

  struct Vertex
  {
    int32_t x, y;
    union
    {
      struct
      {
        uint8_t r, g, b, a;
      };
      uint32_t color;
    };

    ALWAYS_INLINE void Set(int32_t x_, int32_t y_, uint32_t color_)
    {
      x = x_;
      y = y_;
      color = color_;
    }
  };

  Vertex vertices[0];
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif
