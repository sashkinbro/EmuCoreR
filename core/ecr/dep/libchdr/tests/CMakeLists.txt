# EmuCoreR: keep the benchmark out of the default `all` target. Android's
# externalNativeBuild builds `all`, and chdr-benchmark deliberately does not
# link the libretro VFS that chdr-static depends on.
add_executable(chdr-benchmark EXCLUDE_FROM_ALL benchmark.c)
target_link_libraries(chdr-benchmark PRIVATE chdr-static)

# fuzzing
if(BUILD_FUZZER)
  add_executable(chdr-fuzz fuzz.c)
  target_link_options(chdr-fuzz PRIVATE "-fsanitize=address,fuzzer")
  target_link_libraries(chdr-fuzz PRIVATE chdr-static)
  add_custom_target(fuzz
    COMMAND "$<TARGET_FILE:chdr-fuzz>" "-max_len=131072"
    DEPENDS chdr-fuzz)
endif()
