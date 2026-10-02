add_library(core
    input/analog_controller.cpp
    analog_controller.h
    input/analog_joystick.cpp
    analog_joystick.h
    system/bios.cpp
    bios.h
    system/bus.cpp
    bus.h
    cdrom/cdrom.cpp
    cdrom.h
    cdrom/cdrom_async_reader.cpp
    cdrom_async_reader.h
    system/cheats.cpp
    cheats.h
    input/controller.cpp
    controller.h
    cpu/cpu_code_cache.cpp
    cpu_code_cache.h
    cpu/cpu_core.cpp
    cpu_core.h
    cpu_core_private.h
    cpu/cpu_types.cpp
    cpu_types.h
    input/digital_controller.cpp
    digital_controller.h
    system/dma.cpp
    dma.h
    gpu/gpu.cpp
    gpu.h
    gpu/gpu_backend.cpp
    gpu_backend.h
    gpu/gpu_commands.cpp
    gpu/gpu_hw.cpp
    gpu_hw.h
    gpu/gpu_hw_opengl.cpp
    gpu_hw_opengl.h
    gpu/gpu_hw_shadergen.cpp
    gpu_hw_shadergen.h
    gpu/gpu_hw_texture_cache.cpp
    gpu_hw_texture_cache.h
    gpu/gpu_hw_vulkan.cpp
    gpu_hw_vulkan.h
    gpu/gpu_sw.cpp
    gpu_sw.h
    gpu/gpu_sw_backend.cpp
    gpu_sw_backend.h
    gpu_types.h
    cpu/gte.cpp
    gte.h
    gte_types.h
    system/host_display.cpp
    host_display.h
    system/host_interface.cpp
    host_interface.h
    system/host_interface_progress_callback.cpp
    host_interface_progress_callback.h
    system/interrupt_controller.cpp
    interrupt_controller.h
    cdrom/libcrypt_game_codes.cpp
    libcrypt_game_codes.h
    cdrom/mdec.cpp
    mdec.h
    system/memory_card.cpp
    memory_card.h
    system/memory_card_image.cpp
    memory_card_image.h
    input/multitap.cpp
    multitap.h
    input/namco_guncon.cpp
    namco_guncon.h
    input/negcon.cpp
    negcon.h
    input/negcon_rumble.cpp
    negcon_rumble.h
    input/pad.cpp
    pad.h
    cpu/pgxp.cpp
    pgxp.h
    input/playstation_mouse.cpp
    playstation_mouse.h
    cdrom/psf_loader.cpp
    psf_loader.h
    system/resources.cpp
    resources.h
    save_state_version.h
    system/settings.cpp
    settings.h
    shader_cache_version.h
    gpu/shadergen.cpp
    shadergen.h
    system/sio.cpp
    sio.h
    spu/spu.cpp
    spu.h
    system/system.cpp
    system.h
    gpu/texture_replacements.cpp
    texture_replacements.h
    system/timers.cpp
    timers.h
    system/timing_event.cpp
    timing_event.h
    types.h
)

set(RECOMPILER_SRCS
    cpu/cpu_recompiler_code_generator.cpp
    cpu_recompiler_code_generator.h
    cpu/cpu_recompiler_code_generator_generic.cpp
    cpu/cpu_recompiler_register_cache.cpp
    cpu_recompiler_register_cache.h
    cpu_recompiler_thunks.h
    cpu_recompiler_types.h
)

target_include_directories(core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_include_directories(core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(core PUBLIC Threads::Threads common miniz libretro-common vulkan-loader)
target_link_libraries(core PRIVATE glad openbios stb xxhash)

if(WIN32)
  target_sources(core PRIVATE
    gpu/gpu_hw_d3d11.cpp
    gpu_hw_d3d11.h
  )
  if(USE_D3D12)
    target_sources(core PRIVATE
      gpu/gpu_hw_d3d12.cpp
      gpu_hw_d3d12.h
    )
  endif()
  target_link_libraries(core PRIVATE winmm.lib)
endif()

if(${CPU_ARCH} STREQUAL "x64")
  target_include_directories(core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../../dep/xbyak/xbyak")
  target_compile_definitions(core PUBLIC "WITH_RECOMPILER=1" "WITH_MMAP_FASTMEM=1")
  target_sources(core PRIVATE ${RECOMPILER_SRCS}
    cpu/cpu_recompiler_code_generator_x64.cpp
  )
  message("Building x64 recompiler")
elseif(${CPU_ARCH} STREQUAL "aarch32")
  target_compile_definitions(core PUBLIC "WITH_RECOMPILER=1")
  target_sources(core PRIVATE ${RECOMPILER_SRCS}
    cpu/cpu_recompiler_code_generator_aarch32.cpp
  )
  target_link_libraries(core PUBLIC vixl)
  message("Building AArch32 recompiler")
elseif(${CPU_ARCH} STREQUAL "aarch64")
  target_compile_definitions(core PUBLIC "WITH_RECOMPILER=1" "WITH_MMAP_FASTMEM=1")
  target_sources(core PRIVATE ${RECOMPILER_SRCS}
    cpu/cpu_recompiler_code_generator_aarch64.cpp
  )
  target_link_libraries(core PUBLIC vixl)
  message("Building AArch64 recompiler")
else()
  message("Not building recompiler")
endif()
