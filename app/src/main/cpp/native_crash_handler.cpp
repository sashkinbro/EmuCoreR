// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_crash_handler.h"

#include <android/log.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <unwind.h>

#include <atomic>
#include <exception>

namespace emucorer::crash {
namespace {

constexpr const char* kLogTag = "EmuCoreR-Crash";
constexpr int kMaxBreadcrumbs = 96;
constexpr int kMaxBreadcrumbLength = 128;
constexpr int kMaxFrames = 48;
constexpr size_t kReportCapacity = 16384;
constexpr size_t kAltStackSize = 64 * 1024;

char g_crash_log_path[256] = {};
char g_stage_log_path[256] = {};
char g_alt_stack[kAltStackSize] = {};

// Breadcrumb ring written by Mark() and dumped by the signal handler. Both a
// ring in memory (for the crash report) and an append-only file (for SIGKILL /
// OOM kills, where no handler runs at all).
char g_breadcrumbs[kMaxBreadcrumbs][kMaxBreadcrumbLength] = {};
std::atomic<unsigned> g_breadcrumb_head{0};
std::atomic<unsigned> g_breadcrumb_total{0};

struct sigaction g_previous_actions[NSIG];
volatile sig_atomic_t g_handling_crash = 0;
bool g_installed = false;

struct LineBuffer {
    char* data;
    size_t capacity;
    size_t used;

    void Append(const char* format, ...) __attribute__((format(printf, 2, 3))) {
        if (used + 1 >= capacity) return;
        va_list args;
        va_start(args, format);
        const int written = vsnprintf(data + used, capacity - used, format, args);
        va_end(args);
        if (written <= 0) return;
        const size_t room = capacity - used - 1;
        const size_t added = static_cast<size_t>(written);
        used += added < room ? added : room;
        data[used] = '\0';
    }
};

void FormatTimestamp(char* out, size_t size) {
    struct timespec now {};
    if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
        if (size > 0) out[0] = '\0';
        return;
    }
    struct tm broken_down {};
    localtime_r(&now.tv_sec, &broken_down);
    size_t used = strftime(out, size, "%Y-%m-%d %H:%M:%S", &broken_down);
    if (used == 0 || used >= size) return;
    snprintf(out + used, size - used, ".%03ld", static_cast<long>(now.tv_nsec / 1000000));
}

// open()/write() only: the whole path runs inside a fatal signal handler.
void AppendToFile(const char* path, const char* data, size_t length) {
    if (path[0] == '\0' || data == nullptr || length == 0) return;
    const int descriptor = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (descriptor < 0) return;
    size_t written = 0;
    while (written < length) {
        const ssize_t count = write(descriptor, data + written, length - written);
        if (count <= 0) break;
        written += static_cast<size_t>(count);
    }
    close(descriptor);
}

const char* BaseName(const char* path) {
    if (path == nullptr) return "?";
    const char* slash = strrchr(path, '/');
    return slash != nullptr ? slash + 1 : path;
}

const char* SignalName(int signal_number) {
    switch (signal_number) {
        case SIGSEGV: return "SIGSEGV";
        case SIGBUS: return "SIGBUS";
        case SIGILL: return "SIGILL";
        case SIGFPE: return "SIGFPE";
        case SIGABRT: return "SIGABRT";
        default: return "SIG?";
    }
}

struct BacktraceCollector {
    void** frames;
    int count;
    int capacity;
};

_Unwind_Reason_Code CollectFrame(struct _Unwind_Context* context, void* argument) {
    auto* collector = static_cast<BacktraceCollector*>(argument);
    const uintptr_t program_counter = static_cast<uintptr_t>(_Unwind_GetIP(context));
    if (program_counter == 0) return _URC_END_OF_STACK;
    if (collector->count >= collector->capacity) return _URC_END_OF_STACK;
    collector->frames[collector->count++] = reinterpret_cast<void*>(program_counter);
    return _URC_NO_REASON;
}

void AppendFrame(LineBuffer& buffer, int index, void* address) {
    Dl_info info {};
    if (dladdr(address, &info) != 0 && info.dli_fname != nullptr) {
        const auto base = reinterpret_cast<uintptr_t>(info.dli_fbase);
        const auto offset = reinterpret_cast<uintptr_t>(address) - base;
        buffer.Append("    #%02d pc %016llx  %s+0x%llx\n", index,
                      static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address)),
                      BaseName(info.dli_fname), static_cast<unsigned long long>(offset));
    } else {
        buffer.Append("    #%02d pc %016llx  <unknown>\n", index,
                      static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address)));
    }
}

void AppendBacktrace(LineBuffer& buffer) {
    void* frames[kMaxFrames] = {};
    BacktraceCollector collector {frames, 0, kMaxFrames};
    _Unwind_Backtrace(CollectFrame, &collector);
    buffer.Append("Backtrace (%d frames, unsymbolized offsets are relative to the module base):\n",
                  collector.count);
    for (int i = 0; i < collector.count; ++i) AppendFrame(buffer, i, frames[i]);
}

// ART owns SIGSEGV for its implicit null checks. Faults that belong to the
// runtime must be chained without a report, otherwise every Java null
// dereference would look like an emulator crash.
bool IsRuntimeManagedFault(int signal_number, const siginfo_t* info) {
    if (signal_number != SIGSEGV && signal_number != SIGBUS) return false;
    if (info == nullptr || info->si_code <= 0) return false;
    const struct sigaction& previous = g_previous_actions[signal_number];
    if ((previous.sa_flags & SA_SIGINFO) == 0) return false;
    Dl_info art {};
    if (dladdr(reinterpret_cast<const void*>(previous.sa_sigaction), &art) == 0) return false;
    const char* module = art.dli_fname;
    if (module == nullptr) return false;
    const bool in_runtime = strstr(module, "libart.so") != nullptr ||
                            strstr(module, "com.android.art") != nullptr ||
                            strstr(module, "libsigchain.so") != nullptr;
    if (!in_runtime) return false;
    // A fault address below one page is the runtime's null-check pattern.
    return reinterpret_cast<uintptr_t>(info->si_addr) < 4096;
}

void ChainToPreviousHandler(int signal_number, siginfo_t* info, void* context) {
    if (signal_number > 0 && signal_number < NSIG &&
        g_previous_actions[signal_number].sa_handler != nullptr) {
        const struct sigaction& previous = g_previous_actions[signal_number];
        if ((previous.sa_flags & SA_SIGINFO) != 0 && previous.sa_sigaction != nullptr) {
            previous.sa_sigaction(signal_number, info, context);
            return;
        }
        if (previous.sa_handler != SIG_DFL && previous.sa_handler != SIG_IGN) {
            previous.sa_handler(signal_number);
            return;
        }
    }
    // No usable previous handler: restore the default disposition and let the
    // kernel terminate the process so debuggerd still records a tombstone.
    signal(signal_number, SIG_DFL);
    raise(signal_number);
    _exit(128 + signal_number);
}

void WriteCrashReport(int signal_number, siginfo_t* info) {
    char report_storage[kReportCapacity];
    LineBuffer buffer {report_storage, sizeof(report_storage), 0};
    report_storage[0] = '\0';

    char timestamp[48] = {};
    FormatTimestamp(timestamp, sizeof(timestamp));

    char thread_name[32] = "?";
    pthread_getname_np(pthread_self(), thread_name, sizeof(thread_name));

    buffer.Append("[%s] CRASH-NATIVE: %s (%d) in process %d thread %d \"%s\"\n", timestamp,
                  SignalName(signal_number), signal_number, static_cast<int>(getpid()),
                  static_cast<int>(gettid()), thread_name);
    if (info != nullptr) {
        buffer.Append("  fault address: %p  code: %d\n", info->si_addr, info->si_code);
    }

    const unsigned total = g_breadcrumb_total.load(std::memory_order_relaxed);
    const unsigned first = total > static_cast<unsigned>(kMaxBreadcrumbs)
                               ? total - static_cast<unsigned>(kMaxBreadcrumbs)
                               : 0;
    buffer.Append("  startup stages: %u recorded, showing last %u\n", total,
                  total - first);
    for (unsigned index = first; index < total; ++index) {
        const char* breadcrumb = g_breadcrumbs[index % kMaxBreadcrumbs];
        buffer.Append("  > %s\n", breadcrumb);
    }

    AppendBacktrace(buffer);

    AppendToFile(g_crash_log_path, buffer.data, buffer.used);
    __android_log_write(ANDROID_LOG_FATAL, kLogTag, buffer.data);
}

void HandleFatalSignal(int signal_number, siginfo_t* info, void* context) {
    if (g_handling_crash != 0) {
        ChainToPreviousHandler(signal_number, info, context);
        return;
    }
    g_handling_crash = 1;
    if (!IsRuntimeManagedFault(signal_number, info)) {
        WriteCrashReport(signal_number, info);
    }
    ChainToPreviousHandler(signal_number, info, context);
    // The previous handler (or the default action) always terminates, but make
    // sure a handler that unexpectedly returns cannot resume the faulting
    // instruction.
    _exit(128 + signal_number);
}

void HandleTerminate() {
    if (g_handling_crash != 0) _exit(134);
    g_handling_crash = 1;
    char report_storage[kReportCapacity];
    LineBuffer buffer {report_storage, sizeof(report_storage), 0};
    report_storage[0] = '\0';
    char timestamp[48] = {};
    FormatTimestamp(timestamp, sizeof(timestamp));
    buffer.Append("[%s] CRASH-NATIVE: std::terminate() in process %d\n", timestamp,
                  static_cast<int>(getpid()));
    const unsigned total = g_breadcrumb_total.load(std::memory_order_relaxed);
    const unsigned first = total > static_cast<unsigned>(kMaxBreadcrumbs)
                               ? total - static_cast<unsigned>(kMaxBreadcrumbs)
                               : 0;
    for (unsigned index = first; index < total; ++index) {
        buffer.Append("  > %s\n", g_breadcrumbs[index % kMaxBreadcrumbs]);
    }
    AppendBacktrace(buffer);
    AppendToFile(g_crash_log_path, buffer.data, buffer.used);
    __android_log_write(ANDROID_LOG_FATAL, kLogTag, buffer.data);
    abort();
}

}  // namespace

void Install(const char* log_directory) {
    if (g_installed) return;
    if (log_directory == nullptr || log_directory[0] == '\0') return;
    snprintf(g_crash_log_path, sizeof(g_crash_log_path), "%s/crash.log", log_directory);
    snprintf(g_stage_log_path, sizeof(g_stage_log_path), "%s/last-session.log", log_directory);

    stack_t alternative {};
    alternative.ss_sp = g_alt_stack;
    alternative.ss_size = sizeof(g_alt_stack);
    alternative.ss_flags = 0;
    sigaltstack(&alternative, nullptr);

    const int handled[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
    for (int signal_number : handled) {
        struct sigaction action {};
        action.sa_sigaction = HandleFatalSignal;
        sigemptyset(&action.sa_mask);
        sigaddset(&action.sa_mask, signal_number);
        action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        if (sigaction(signal_number, &action, &g_previous_actions[signal_number]) != 0) {
            memset(&g_previous_actions[signal_number], 0, sizeof(struct sigaction));
        }
    }
    std::set_terminate(HandleTerminate);
    g_installed = true;
    Mark("native crash handler installed");
}

void Mark(const char* format, ...) {
    if (!g_installed || format == nullptr) return;
    char message[kMaxBreadcrumbLength];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    for (char& character : message) {
        if (character == '\n' || character == '\r') character = ' ';
    }

    const unsigned slot = g_breadcrumb_head.fetch_add(1, std::memory_order_relaxed);
    char* cell = g_breadcrumbs[slot % kMaxBreadcrumbs];
    strncpy(cell, message, kMaxBreadcrumbLength - 1);
    cell[kMaxBreadcrumbLength - 1] = '\0';
    g_breadcrumb_total.fetch_add(1, std::memory_order_relaxed);

    char timestamp[48] = {};
    FormatTimestamp(timestamp, sizeof(timestamp));
    char line[kMaxBreadcrumbLength + 64];
    const int length = snprintf(line, sizeof(line), "[%s] %s\n", timestamp, message);
    if (length > 0) {
        AppendToFile(g_stage_log_path, line,
                     static_cast<size_t>(length) < sizeof(line) ? static_cast<size_t>(length)
                                                                : sizeof(line) - 1);
    }
}

}  // namespace emucorer::crash
