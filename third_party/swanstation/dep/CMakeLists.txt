add_subdirectory(glad)
add_subdirectory(stb)
add_subdirectory(libchdr EXCLUDE_FROM_ALL)
add_subdirectory(xxhash)
add_subdirectory(vulkan-loader)
add_subdirectory(libretro-common)
add_subdirectory(openbios)

if(${CPU_ARCH} STREQUAL "aarch32" OR ${CPU_ARCH} STREQUAL "aarch64")
  add_subdirectory(vixl)
endif()

# Configure libchdr to use libretro's file IO.
target_compile_definitions(chdr-static PUBLIC USE_LIBRETRO_VFS)
target_include_directories(chdr-static PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/libretro-common/include")

# Make it easier to include miniz's header.
target_include_directories(miniz INTERFACE "libchdr/deps/miniz-3.1.1")
