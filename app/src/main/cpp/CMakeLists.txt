# SPDX-FileCopyrightText: 2026 SBRO
# SPDX-License-Identifier: LicenseRef-EmuCoreR-Proprietary
cmake_minimum_required(VERSION 3.22)
project(EmuCoreRAndroid LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Discord Social SDK is an optional local build input used by the isolated
# :discord helper process. When absent, emucorer_discord is not built and the
# integration reports unavailable.
if(NOT DISCORD_SDK_DIR AND DEFINED ENV{DISCORD_SDK_DIR})
    set(DISCORD_SDK_DIR "$ENV{DISCORD_SDK_DIR}")
endif()
set(DISCORD_SDK_SO "${DISCORD_SDK_DIR}/${ANDROID_ABI}/libdiscord_partner_sdk.so")
set(DISCORD_SDK_INCLUDE "${DISCORD_SDK_DIR}/include")
if(DISCORD_SDK_DIR AND EXISTS "${DISCORD_SDK_SO}" AND EXISTS "${DISCORD_SDK_INCLUDE}/discordpp.h")
    add_library(discord_partner_sdk SHARED IMPORTED)
    set_target_properties(discord_partner_sdk PROPERTIES
        IMPORTED_LOCATION "${DISCORD_SDK_SO}"
        INTERFACE_INCLUDE_DIRECTORIES "${DISCORD_SDK_INCLUDE}")
    add_library(emucorer_discord SHARED discord_bridge.cpp)
    target_link_libraries(emucorer_discord PRIVATE discord_partner_sdk log)
    set_target_properties(emucorer_discord PROPERTIES OUTPUT_NAME "emucorer_discord")
    message(STATUS "Discord Social SDK enabled: ${DISCORD_SDK_SO}")
else()
    message(STATUS "Discord Social SDK unavailable; Discord integration will report unavailable")
endif()

# -----------------------------------------------------------------------------
# SwanStation libretro core (vendored, in-tree dependencies).
# -----------------------------------------------------------------------------
set(SWANSTATION_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../third_party/swanstation")
set(SWANSTATION_BIN_DIR "${CMAKE_BINARY_DIR}/swanstation")
add_subdirectory("${SWANSTATION_DIR}" "${SWANSTATION_BIN_DIR}")

# libchdr ships a benchmark executable that is not part of the core and does
# not link the libretro VFS. Android's externalNativeBuild builds the `all`
# target, so exclude it and, defensively, satisfy its filestream_* symbols.
if(TARGET chdr-benchmark)
    set_target_properties(chdr-benchmark PROPERTIES EXCLUDE_FROM_ALL TRUE)
    if(TARGET libretro-common)
        target_link_libraries(chdr-benchmark PRIVATE libretro-common)
    endif()
endif()
if(TARGET chdr-fuzz)
    set_target_properties(chdr-fuzz PROPERTIES EXCLUDE_FROM_ALL TRUE)
    if(TARGET libretro-common)
        target_link_libraries(chdr-fuzz PRIVATE libretro-common)
    endif()
endif()

# JNI bridge that drives the core through the libretro API.
add_library(emucorer_jni SHARED
    native_bridge.cpp
)

target_include_directories(emucorer_jni PRIVATE
    "${SWANSTATION_DIR}/dep/libretro-common/include"
    "${SWANSTATION_DIR}/src"
)

target_link_libraries(emucorer_jni PRIVATE
    swanstation_libretro
    android
    log
    aaudio
    EGL
    GLESv3
)

target_compile_options(emucorer_jni PRIVATE -Wall -Wextra)

set_target_properties(emucorer_jni PROPERTIES OUTPUT_NAME "emucorer_jni")
