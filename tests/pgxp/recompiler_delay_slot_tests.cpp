// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: GPL-3.0-or-later
#include <libretro.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <dlfcn.h>
#include <string>
#include <unistd.h>
#include <vector>

static const char* root;
static const char* cpu_mode;
static bool Environment(unsigned command, void* data)
{
  switch (command)
  {
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
      *static_cast<const char**>(data) = root;
      return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE:
    {
      auto* variable = static_cast<retro_variable*>(data);
      const std::string key(variable->key);
      const char* value = nullptr;
      if (key == "swanstation_GPU_Renderer") value = "Software";
      else if (key == "swanstation_CPU_ExecutionMode") value = cpu_mode;
      else if (key == "swanstation_CPU_RecompilerBlockLinking") value = "true";
      else if (key == "swanstation_Console_Region") value = "NTSC-U";
      else if (key == "swanstation_BIOS_PathNTSCU") value = "probe.bin";
      else if (key == "swanstation_MemoryCards_Card1Type" || key == "swanstation_MemoryCards_Card2Type") value = "None";
      else if (key == "swanstation_BIOS_PatchFastBoot" || key == "swanstation_Main_ApplyGameSettings") value = "false";
      variable->value = value;
      return value != nullptr;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
      *static_cast<bool*>(data) = false;
      return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
      *static_cast<bool*>(data) = true;
      return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
      return true;
    default:
      return false;
  }
}

int main(int argc, char** argv)
{
  if (argc != 4) return 2;
  const bool readable_delay_slot = std::strcmp(argv[3], "readable") == 0;
  cpu_mode = argv[2];
  void* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (!library) { std::fprintf(stderr, "%s\n", dlerror()); return 2; }
#define API(name) const auto name = reinterpret_cast<decltype(&::name)>(dlsym(library, #name)); if (!name) return 2
  API(retro_set_environment); API(retro_set_video_refresh);
  API(retro_set_audio_sample); API(retro_set_audio_sample_batch);
  API(retro_set_input_poll); API(retro_set_input_state);
  API(retro_init); API(retro_deinit); API(retro_load_game);
  API(retro_unload_game); API(retro_run); API(retro_get_memory_data);
#undef API
  char directory[] = "/data/local/tmp/emucorer-delay-slot-XXXXXX";
  root = mkdtemp(directory);
  if (!root) return 2;
  const std::string bios_path = std::string(root) + "/probe.bin";
  std::vector<uint32_t> bios(512 * 1024 / sizeof(uint32_t));
  bios[0] = 0x3c088000; // Record that the owned BIOS actually executed.
  bios[1] = 0x3c091234;
  bios[2] = 0x35295678;
  bios[3] = 0xad091000;
  bios[4] = 0x0bf1ffff; // j 0xbfc7fffc: last readable BIOS instruction.
  bios.back() = 0x0bf00060; // j 0xbfc00180: its delay slot is outside BIOS.
  if (readable_delay_slot)
  {
    bios[4] = 0x0bf1fffe; // Same branch, with room for a readable NOP delay slot.
    bios[bios.size() - 2] = 0x0bf00060;
    bios.back() = 0;
  }
  // Keep a stationary guest exception handler in the BIOS as well.
  bios[0x180 / 4] = 0x3c088000; // lui t0, 0x8000
  bios[0x184 / 4] = 0x3c091234; // lui t1, 0x1234
  bios[0x188 / 4] = 0x35295678; // ori t1, t1, 0x5678
  bios[0x18c / 4] = 0xad091004; // sw t1, 0x1004(t0)
  bios[0x190 / 4] = 0x1000ffff; // stationary handler loop with NOP delay slot.
  FILE* file = std::fopen(bios_path.c_str(), "wb");
  if (!file) return 2;
  const bool written = std::fwrite(bios.data(), sizeof(uint32_t), bios.size(), file) == bios.size();
  std::fclose(file);
  if (!written) return 2;
  retro_set_environment(Environment);
  retro_set_video_refresh([](const void*, unsigned, unsigned, size_t) {});
  retro_set_audio_sample([](int16_t, int16_t) {});
  retro_set_audio_sample_batch([](const int16_t*, size_t count) { return count; });
  retro_set_input_poll([]() {});
  retro_set_input_state([](unsigned, unsigned, unsigned, unsigned) { return int16_t(0); });
  retro_init();
  const bool loaded = retro_load_game(nullptr);
  bool passed = false;
  if (loaded)
  {
    for (unsigned frame = 0; frame < 3; frame++) retro_run();
    const auto* memory = static_cast<const uint32_t*>(retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM));
    passed = memory && memory[0x1000 / 4] == 0x12345678;
    if (readable_delay_slot) passed = passed && memory[0x1004 / 4] == 0x12345678;
    retro_unload_game();
  }
  retro_deinit();
  std::remove(bios_path.c_str());
  rmdir(root);
  dlclose(library);
  std::printf("%s CPU=%s delay-slot=%s: completes frames and preserves guest results\n",
              passed ? "PASS" : "FAIL", cpu_mode, argv[3]);
  return passed ? 0 : 1;
}
