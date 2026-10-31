// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * Native crash reporting for the emulator process.
 *
 * The Kotlin uncaught-exception handler can only see Java exceptions. A
 * SIGSEGV/SIGABRT/abort raised inside the libretro core, the GPU driver or
 * libvulkan kills the process without ever touching that handler, which is
 * why affected devices produced an empty logs/ folder. This module installs
 * fatal-signal handlers plus a C++ terminate hook, keeps a breadcrumb trail of
 * the startup stages that were reached, and appends a symbolized backtrace to
 * <log dir>/crash.log before the process is allowed to die.
 */
namespace emucorer::crash {

/** Installs the handlers once. Safe to call on every app start. */
void Install(const char* log_directory);

/** Records a breadcrumb. Signal-safe and cheap; keep it to launch stages. */
void Mark(const char* format, ...) __attribute__((format(printf, 1, 2)));

}  // namespace emucorer::crash
