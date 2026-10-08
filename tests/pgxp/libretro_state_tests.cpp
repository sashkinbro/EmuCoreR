// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: GPL-3.0-or-later
#include <libretro.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <unistd.h>
#include <vector>

static const char* root;
static const char* cpu_mode;
static bool large_ram;
static retro_savestate_context context = RETRO_SAVESTATE_CONTEXT_NORMAL;
static int checks = 0, failures = 0;

static void Check(bool condition, const char* name)
{
  std::printf("%s %s CPU=%s RAM=%s context=%u\n", condition ? "PASS" : "FAIL", name,
              cpu_mode, large_ram ? "8MB" : "2MB", unsigned(context));
  checks++;
  failures += !condition;
}

static bool Environment(unsigned command, void* data)
{
  switch (command)
  {
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
      *static_cast<const char**>(data) = root;
      return true;
    case RETRO_ENVIRONMENT_GET_SAVESTATE_CONTEXT:
      *static_cast<retro_savestate_context*>(data) = context;
      return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE:
    {
      auto* variable = static_cast<retro_variable*>(data);
      const char* value = nullptr;
      const std::string key(variable->key);
      if (key == "swanstation_GPU_Renderer") value = "Software";
      else if (key == "swanstation_CPU_ExecutionMode") value = cpu_mode;
      else if (key == "swanstation_Console_Enable8MBRAM") value = large_ram ? "true" : "false";
      else if (key == "swanstation_Console_Region") value = "NTSC-U";
      else if (key == "swanstation_BIOS_PathNTSCU") value = "probe.bin";
      else if (key == "swanstation_MemoryCards_Card1Type" || key == "swanstation_MemoryCards_Card2Type") value = "None";
      else if (key == "swanstation_BIOS_PatchFastBoot" || key == "swanstation_Main_ApplyGameSettings") value = "false";
      else if (key == "swanstation_Main_SaveStateCompression") value = "None";
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
  if (argc != 2)
    return 2;
  void* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (!library)
  {
    std::fprintf(stderr, "%s\n", dlerror());
    return 2;
  }
#define API(name) const auto name = reinterpret_cast<decltype(&::name)>(dlsym(library, #name)); if (!name) return 2
  API(retro_set_environment);
  API(retro_set_video_refresh);
  API(retro_set_audio_sample);
  API(retro_set_audio_sample_batch);
  API(retro_set_input_poll);
  API(retro_set_input_state);
  API(retro_init);
  API(retro_deinit);
  API(retro_load_game);
  API(retro_unload_game);
  API(retro_serialize_size);
  API(retro_serialize);
  API(retro_unserialize);
  API(retro_get_memory_data);
  API(retro_get_memory_size);
#undef API
  char directory[] = "/data/local/tmp/emucorer-pgxp-state-XXXXXX";
  root = mkdtemp(directory);
  if (!root)
    return 2;
  const std::string bios_path = std::string(root) + "/probe.bin";
  std::vector<uint32_t> bios(512 * 1024 / sizeof(uint32_t));
  bios[0] = 0x1000ffff; // Owned stationary BIOS: branch to itself, NOP delay slot.
  FILE* file = std::fopen(bios_path.c_str(), "wb");
  if (!file)
    return 2;
  const bool written = std::fwrite(bios.data(), sizeof(uint32_t), bios.size(), file) == bios.size();
  std::fclose(file);
  if (!written)
    return 2;

  for (const char* mode : {"Interpreter", "Recompiler"})
  {
    cpu_mode = mode;
    for (bool ram : {false, true})
    {
      large_ram = ram;
      context = RETRO_SAVESTATE_CONTEXT_NORMAL;
      retro_set_environment(Environment);
      retro_set_video_refresh([](const void*, unsigned, unsigned, size_t) {});
      retro_set_audio_sample([](int16_t, int16_t) {});
      retro_set_audio_sample_batch([](const int16_t*, size_t count) { return count; });
      retro_set_input_poll([]() {});
      retro_set_input_state([](unsigned, unsigned, unsigned, unsigned) { return int16_t(0); });
      retro_init();
      const bool loaded = retro_load_game(nullptr);
      Check(loaded, "boot the owned BIOS without game data");
      if (!loaded)
      {
        retro_deinit();
        continue;
      }
      auto* memory = static_cast<uint32_t*>(retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM));
      Check(memory && retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM) == (ram ? 8u : 2u) * 1024u * 1024u,
            "configured RAM is available");
      if (memory)
      {
        memory[0x1000 / 4] = 0x11223344;
        std::vector<uint8_t> state(retro_serialize_size());
        const bool saved = retro_serialize(state.data(), state.size());
        Check(saved, "serialize a complete frontend state");
        if (saved)
        {
          for (retro_savestate_context mode : {RETRO_SAVESTATE_CONTEXT_NORMAL,
                RETRO_SAVESTATE_CONTEXT_RUNAHEAD_SAME_INSTANCE, RETRO_SAVESTATE_CONTEXT_RUNAHEAD_SAME_BINARY})
          {
            context = mode;
            memory[0x1000 / 4] = 0xaabbccdd;
            Check(retro_unserialize(state.data(), state.size()), "load the full state in the requested context");
            memory = static_cast<uint32_t*>(retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM));
            Check(memory && memory[0x1000 / 4] == 0x11223344, "rollback restores the RAM payload");
          }
        }
      }
      retro_unload_game();
      retro_deinit();
    }
  }
  std::remove(bios_path.c_str());
  rmdir(root);
  dlclose(library);
  std::printf("Libretro states: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
