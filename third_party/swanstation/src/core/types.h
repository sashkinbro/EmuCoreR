#pragma once
#include "common/types.h"

// Physical memory addresses are 32-bits wide
using PhysicalMemoryAddress = uint32_t;
using VirtualMemoryAddress = uint32_t;

enum class MemoryAccessType : uint32_t
{
  Read,
  Write
};
enum class MemoryAccessSize : uint32_t
{
  Byte,
  HalfWord,
  Word
};

using TickCount = int32_t;

enum class ConsoleRegion
{
  Auto,
  NTSC_J,
  NTSC_U,
  PAL,
  Count
};

enum class DiscRegion : uint8_t
{
  NTSC_J, // SCEI
  NTSC_U, // SCEA
  PAL,    // SCEE
  Other,
  Count
};

enum class CPUExecutionMode : uint8_t
{
  Interpreter,
  CachedInterpreter,
  Recompiler,
  Count
};

enum class PGXPMode : uint8_t
{
  Disabled,
  Memory,
  CPU
};

enum class GPURenderer : uint8_t
{
#ifdef _WIN32
  HardwareD3D11,
  HardwareD3D12,
#endif
  HardwareVulkan,
  HardwareOpenGL,
  Software,
  Count
};

enum class GPUTextureFilter : uint8_t
{
  Nearest,
  Bilinear,
  BilinearBinAlpha,
  JINC2,
  JINC2BinAlpha,
  xBR,
  xBRBinAlpha,
  Count
};

enum class GPUDownsampleMode : uint8_t
{
  Disabled,
  Box,
  Adaptive,
  Count
};

// Controls when batch fragment shaders / PSOs are built. There are
// 144 batch fragment shader permutations on D3D11 / OpenGL and up to
// 2160 PSO permutations on Vulkan, but most games only ever dispatch a
// small subset of them at runtime. The historical behaviour was to
// compile the entire matrix at GPU init - which is what 'Enabled' does
// - and that's where the multi-second 'libretro looks hung' stall on
// texture-filter changes comes from.
//
//   - Disabled: no precompile at GPU init. Each batch shader / PSO is
//     compiled on the main thread the first time the game actually
//     dispatches a draw using that combination, then cached. The user
//     trades the single multi-second stall for a series of small
//     first-use hitches spread across early gameplay. Lowest startup
//     latency; useful on low-end hardware and for users who don't use
//     the heavier filters (JINC2/xBR/Bilinear).
//   - Enabled: the historical behaviour. CompileShaders walks the full
//     matrix synchronously at GPU init. RetroArch is blocked the whole
//     time. After that there are no per-draw hitches at all. Useful
//     for benchmarking and for users who want a single up-front cost.
//   - Lazy: compile on a background thread while gameplay starts
//     immediately. Each combo the game actually needs is faulted in
//     on the main thread if the background thread hasn't reached it
//     yet (worst case: same hitch as Disabled for that one combo),
//     otherwise the entry is just picked up. After the background
//     thread finishes, behaviour is identical to Enabled.
//     This is the default.
enum class GPUShaderPrecompileMode : uint8_t
{
  Disabled,
  Enabled,
  Lazy,
  Count
};

enum class DisplayCropMode : uint8_t
{
  None,
  Overscan,
  Borders,
  Count
};

enum class DisplayAspectRatio : uint8_t
{
  Auto,
  MatchWindow,
  Custom,
  R4_3,
  R16_9,
  R19_9,
  R20_9,
  PAR1_1,
  Native,
  Count
};

enum class ControllerType
{
  None,
  DigitalController,
  AnalogController,
  AnalogJoystick,
  NamcoGunCon,
  PlayStationMouse,
  NeGcon,
  NeGconRumble,
  Count
};

enum class MemoryCardType
{
  None,
  Shared,
  PerGame,
  PerGameTitle,
  PerGameFileTitle,
  NonPersistent,
  Libretro,
  Count
};

enum class MultitapMode
{
  Disabled,
  Port1Only,
  Port2Only,
  BothPorts,
  Count
};

inline constexpr uint32_t NUM_CONTROLLER_AND_CARD_PORTS = 8, NUM_MULTITAPS = 2;

enum class CPUFastmemMode
{
  Disabled,
  MMap,
  LUT,
  Count
};

inline constexpr size_t HOST_PAGE_SIZE = 4096, HOST_PAGE_OFFSET_MASK = HOST_PAGE_SIZE - 1;
