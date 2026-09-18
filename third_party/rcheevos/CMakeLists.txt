# SPDX-FileCopyrightText: 2026 SBRO
# SPDX-License-Identifier: LicenseRef-EmuCoreR-Proprietary
#
# Vendored RetroAchievements client library (rcheevos 12.5.0).
# Only the client + runtime + API + hashing sources are compiled; the
# RAIntegration (Windows) and libretro-core helper files are not used by the
# Android frontend.
cmake_minimum_required(VERSION 3.22)
project(rcheevos C)

add_library(rcheevos STATIC
    src/rc_client.c
    src/rc_compat.c
    src/rc_libretro.c
    src/rc_util.c
    src/rc_version.c
    src/rapi/rc_api_common.c
    src/rapi/rc_api_editor.c
    src/rapi/rc_api_info.c
    src/rapi/rc_api_runtime.c
    src/rapi/rc_api_user.c
    src/rcheevos/alloc.c
    src/rcheevos/condition.c
    src/rcheevos/condset.c
    src/rcheevos/consoleinfo.c
    src/rcheevos/format.c
    src/rcheevos/lboard.c
    src/rcheevos/memref.c
    src/rcheevos/operand.c
    src/rcheevos/rc_validate.c
    src/rcheevos/richpresence.c
    src/rcheevos/runtime.c
    src/rcheevos/runtime_progress.c
    src/rcheevos/trigger.c
    src/rcheevos/value.c
    src/rhash/aes.c
    src/rhash/cdreader.c
    src/rhash/hash.c
    src/rhash/hash_disc.c
    src/rhash/hash_encrypted.c
    src/rhash/hash_rom.c
    src/rhash/hash_zip.c
    src/rhash/md5.c
)

target_include_directories(rcheevos PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

# Hash/identify support: lets rc_client identify a game straight from the
# disc image path instead of requiring the frontend to hash it first.
target_compile_definitions(rcheevos PUBLIC RC_CLIENT_SUPPORTS_HASH)
