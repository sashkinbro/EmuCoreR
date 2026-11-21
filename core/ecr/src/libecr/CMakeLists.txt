add_library(libecr SHARED
  libecr_audio_stream.cpp
  libecr_audio_stream.h
  libecr_game_settings.cpp
  libecr_game_settings.h
  libecr_host_display.cpp
  libecr_host_display.h
  libecr_host_interface.cpp
  libecr_settings_interface.cpp
  libecr_settings_interface.h
)

target_link_libraries(libecr PRIVATE core common glad vulkan-loader libecr-common)

# no lib prefix
set_target_properties(libecr PROPERTIES PREFIX "")

# drop in the build directory
set_target_properties(libecr PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set_target_properties(libecr PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# work around .sln issue of hardcoded config directories
if(WIN32)
  set_target_properties(libecr PROPERTIES RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}")
  set_target_properties(libecr PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}")
endif()

# for android, suffix _android
if(ANDROID)
  set_target_properties(libecr PROPERTIES OUTPUT_NAME "libecr_android")
endif()
