// Portable best-effort current-thread priority adjustment.
//
// Used by the GPU backend shader-compile workers to lower their
// scheduling priority to "below normal" so the runloop / CPU
// emulation / audio threads stay responsive on CPU-contended
// systems. On a high-core-count machine with spare cycles the
// effect is barely observable: the worker still runs flat-out
// when nothing else competes for the core. On a 4-core or lower
// machine, the lowered worker yields to default-priority threads
// when they need a core, so the runloop keeps its frame budget.
//
// Failure is silent. Priority adjustment is best-effort - if the
// platform refuses (insufficient privileges, unsupported scheduler,
// etc.) the worker continues at default priority and the only cost
// is a small amount of contention on low-end hardware.
//
// All variants here operate on the CALLING thread, so this helper
// must be invoked from inside the worker function itself (at the
// top of the entry-point routine), not from the spawner.
//
// Platform notes:
//
//   Windows: SetThreadPriority. Reliable, per-thread.
//
//   Linux: setpriority(PRIO_PROCESS, 0, ...) actually operates per
//     thread under NPTL despite the function name. This is a
//     long-standing Linux/NPTL deviation from POSIX (documented in
//     the setpriority(2) man page). We rely on that explicitly -
//     using pthread_setschedparam with SCHED_OTHER on Linux is a
//     no-op because SCHED_OTHER only accepts priority 0.
//
//   macOS / *BSD: pthread_setschedparam under SCHED_OTHER. Honours
//     a sched_priority within a narrow band; we move halfway from
//     the current value toward the minimum. Going to absolute min
//     has historically risked priority-inversion deadlocks on
//     mutex-heavy workloads.
//
//   Other Unices (Solaris, Haiku, etc.): the macOS/*BSD path is a
//     reasonable best-effort default. If it returns an error the
//     caller just gets default priority - no incorrect behaviour.
//
// NB: We deliberately do NOT use the POSIX-portable setpriority()
// with PRIO_PROCESS on non-Linux platforms, because POSIX defines
// it as a per-process attribute and applying it would lower the
// priority of the runloop and audio threads too - the exact
// opposite of what we want.

#pragma once

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#elif defined(__linux__)

#include <sys/resource.h>
#include <sys/time.h>
#include <unistd.h>

#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || \
  defined(__DragonFly__) || defined(__unix__)

#include <pthread.h>
#include <sched.h>

#endif

namespace ThreadPriority {

// Lower the priority of the calling thread to "below normal".
//
// Returns true on success, false on failure. Failure is non-fatal;
// the caller should generally ignore the return value unless
// logging is desired. Either way the thread keeps running, just at
// whatever priority the platform gave it.
inline bool LowerCurrentThreadPriority()
{
#if defined(_WIN32)
  return SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL) != 0;
#elif defined(__linux__)
  // setpriority(PRIO_PROCESS, 0, +5) on Linux/NPTL targets the
  // calling THREAD, not the whole process - this is documented and
  // relied upon by many applications. +5 nice is the conventional
  // offset for "background work that should yield to interactive
  // threads but still make forward progress".
  //
  // RLIMIT_NICE caps how high (numerically) we can go without
  // privileges; +5 is far below the default 20 ceiling and works
  // for every reasonable launching shell.
  return setpriority(PRIO_PROCESS, 0, 5) == 0;
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || \
  defined(__DragonFly__) || defined(__unix__)
  // pthread_setschedparam under SCHED_OTHER. The range of valid
  // priorities is platform-specific; pick a value halfway between
  // the current priority and the minimum. Halfway rather than
  // absolute-min because going to min has been observed to cause
  // priority-inversion deadlocks when the worker holds a mutex a
  // higher-priority thread is waiting on.
  sched_param sp = {};
  int policy = 0;
  if (pthread_getschedparam(pthread_self(), &policy, &sp) != 0)
    return false;
  const int low = sched_get_priority_min(policy);
  if (low < 0)
    return false;
  const int current = sp.sched_priority;
  const int target = (low + current) / 2;
  if (target == current)
    return false; // already at min, nothing to do
  sp.sched_priority = target;
  return pthread_setschedparam(pthread_self(), policy, &sp) == 0;
#else
  // Unknown platform - silently no-op rather than break the build.
  // The caller still gets default-priority scheduling.
  return false;
#endif
}

} // namespace ThreadPriority
