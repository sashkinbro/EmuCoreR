#pragma once
#include "gpu_backend.h"
#include <algorithm>
#include <array>
#include <tuple>

class GPU_SW_Backend final : public GPUBackend
{
public:
  GPU_SW_Backend();
  ~GPU_SW_Backend() override;

  bool Initialize(bool force_thread) override;
  void Reset(bool clear_vram) override;

  ALWAYS_INLINE_RELEASE uint16_t GetPixel(const uint32_t x, const uint32_t y) const { return m_vram[VRAM_WIDTH * y + x]; }
  ALWAYS_INLINE_RELEASE void SetPixel(const uint32_t x, const uint32_t y, const uint16_t value) { m_vram[VRAM_WIDTH * y + x] = value; }

  // this is actually (31 * 255) >> 4) == 494, but to simplify addressing we use the next power of two (512)
  static constexpr uint32_t DITHER_LUT_SIZE = 512;
  using DitherLUT = std::array<std::array<std::array<uint8_t, 512>, DITHER_MATRIX_SIZE>, DITHER_MATRIX_SIZE>;
  static constexpr DitherLUT ComputeDitherLUT();

protected:
  union VRAMPixel
  {
    uint16_t bits;

    BitField<uint16_t, uint8_t, 0, 5> r;
    BitField<uint16_t, uint8_t, 5, 5> g;
    BitField<uint16_t, uint8_t, 10, 5> b;
    BitField<uint16_t, bool, 15, 1> c;

    void Set(uint8_t r_, uint8_t g_, uint8_t b_, bool c_ = false)
    {
      bits = (static_cast<uint16_t>(r_)) | (static_cast<uint16_t>(g_) << 5) | (static_cast<uint16_t>(b_) << 10) | (static_cast<uint16_t>(c_) << 15);
    }

    void SetRGB24(uint32_t rgb24, bool c_ = false)
    {
      bits = static_cast<uint16_t>(((rgb24 >> 3) & 0x1F) | (((rgb24 >> 11) & 0x1F) << 5) | (((rgb24 >> 19) & 0x1F) << 10)) |
             (static_cast<uint16_t>(c_) << 15);
    }

    void SetRGB24(uint8_t r8, uint8_t g8, uint8_t b8, bool c_ = false)
    {
      bits = (static_cast<uint16_t>(r8 >> 3)) | (static_cast<uint16_t>(g8 >> 3) << 5) | (static_cast<uint16_t>(b8 >> 3) << 10) |
             (static_cast<uint16_t>(c_) << 15);
    }
  };

  static constexpr std::tuple<uint8_t, uint8_t> UnpackTexcoord(uint16_t texcoord)
  {
    return std::make_tuple(static_cast<uint8_t>(texcoord), static_cast<uint8_t>(texcoord >> 8));
  }

  static constexpr std::tuple<uint8_t, uint8_t, uint8_t> UnpackColorRGB24(uint32_t rgb24)
  {
    return std::make_tuple(static_cast<uint8_t>(rgb24), static_cast<uint8_t>(rgb24 >> 8), static_cast<uint8_t>(rgb24 >> 16));
  }

  void FillVRAM(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t color, GPUBackendCommandParameters params) override;
  void UpdateVRAM(uint32_t x, uint32_t y, uint32_t width, uint32_t height, const void* data, GPUBackendCommandParameters params) override;
  void CopyVRAM(uint32_t src_x, uint32_t src_y, uint32_t dst_x, uint32_t dst_y, uint32_t width, uint32_t height,
                GPUBackendCommandParameters params) override;

  void DrawPolygon(const GPUBackendDrawPolygonCommand* cmd) override;
  void DrawLine(const GPUBackendDrawLineCommand* cmd) override;
  void DrawRectangle(const GPUBackendDrawRectangleCommand* cmd) override;
  void FlushRender() override;

  //////////////////////////////////////////////////////////////////////////
  // Rasterization
  //////////////////////////////////////////////////////////////////////////
  template<bool texture_enable, bool raw_texture_enable, bool transparency_enable, bool dithering_enable>
  void ShadePixel(const GPUBackendDrawCommand* cmd, uint32_t x, uint32_t y, uint8_t color_r, uint8_t color_g, uint8_t color_b, uint8_t texcoord_x,
                  uint8_t texcoord_y);

  template<bool texture_enable, bool raw_texture_enable, bool transparency_enable>
  void DrawRectangle(const GPUBackendDrawRectangleCommand* cmd);

  using DrawRectangleFunction = void (GPU_SW_Backend::*)(const GPUBackendDrawRectangleCommand* cmd);
  DrawRectangleFunction GetDrawRectangleFunction(bool texture_enable, bool raw_texture_enable,
                                                 bool transparency_enable);

  //////////////////////////////////////////////////////////////////////////
  // Polygon and line rasterization ported from Mednafen
  //////////////////////////////////////////////////////////////////////////
  struct i_deltas
  {
    uint32_t du_dx, dv_dx;
    uint32_t dr_dx, dg_dx, db_dx;

    uint32_t du_dy, dv_dy;
    uint32_t dr_dy, dg_dy, db_dy;
  };

  struct i_group
  {
    uint32_t u, v;
    uint32_t r, g, b;
  };

  template<bool shading_enable, bool texture_enable>
  bool CalcIDeltas(i_deltas& idl, const GPUBackendDrawPolygonCommand::Vertex* A,
                   const GPUBackendDrawPolygonCommand::Vertex* B, const GPUBackendDrawPolygonCommand::Vertex* C);

  template<bool shading_enable, bool texture_enable>
  void AddIDeltas_DX(i_group& ig, const i_deltas& idl, uint32_t count = 1);

  template<bool shading_enable, bool texture_enable>
  void AddIDeltas_DY(i_group& ig, const i_deltas& idl, uint32_t count = 1);

  template<bool shading_enable, bool texture_enable, bool raw_texture_enable, bool transparency_enable,
           bool dithering_enable>
  void DrawSpan(const GPUBackendDrawPolygonCommand* cmd, int32_t y, int32_t x_start, int32_t x_bound, i_group ig,
                const i_deltas& idl);

  template<bool shading_enable, bool texture_enable, bool raw_texture_enable, bool transparency_enable,
           bool dithering_enable>
  void DrawTriangle(const GPUBackendDrawPolygonCommand* cmd, const GPUBackendDrawPolygonCommand::Vertex* v0,
                    const GPUBackendDrawPolygonCommand::Vertex* v1, const GPUBackendDrawPolygonCommand::Vertex* v2);

  using DrawTriangleFunction = void (GPU_SW_Backend::*)(const GPUBackendDrawPolygonCommand* cmd,
                                                        const GPUBackendDrawPolygonCommand::Vertex* v0,
                                                        const GPUBackendDrawPolygonCommand::Vertex* v1,
                                                        const GPUBackendDrawPolygonCommand::Vertex* v2);
  DrawTriangleFunction GetDrawTriangleFunction(bool shading_enable, bool texture_enable, bool raw_texture_enable,
                                               bool transparency_enable, bool dithering_enable);

  template<bool shading_enable, bool transparency_enable, bool dithering_enable>
  void DrawLine(const GPUBackendDrawLineCommand* cmd, const GPUBackendDrawLineCommand::Vertex* p0,
                const GPUBackendDrawLineCommand::Vertex* p1);

  using DrawLineFunction = void (GPU_SW_Backend::*)(const GPUBackendDrawLineCommand* cmd,
                                                    const GPUBackendDrawLineCommand::Vertex* p0,
                                                    const GPUBackendDrawLineCommand::Vertex* p1);
  DrawLineFunction GetDrawLineFunction(bool shading_enable, bool transparency_enable, bool dithering_enable);

  std::array<uint16_t, VRAM_WIDTH * VRAM_HEIGHT> m_vram;
};
