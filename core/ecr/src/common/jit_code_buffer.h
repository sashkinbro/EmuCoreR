#pragma once
#include "types.h"

class JitCodeBuffer
{
public:
  JitCodeBuffer();
  ~JitCodeBuffer();

  bool IsValid() const { return (m_code_ptr != nullptr); }

  bool Allocate(uint32_t size = 64 * 1024 * 1024, uint32_t far_code_size = 0);
  bool Initialize(void* buffer, uint32_t size, uint32_t far_code_size = 0, uint32_t guard_size = 0);
  void Destroy();
  void Reset();

  ALWAYS_INLINE uint8_t* GetCodePointer() const { return m_code_ptr; }
  ALWAYS_INLINE uint32_t GetTotalSize() const { return m_total_size; }

  ALWAYS_INLINE uint8_t* GetFreeCodePointer() const { return m_free_code_ptr; }
  ALWAYS_INLINE uint32_t GetFreeCodeSpace() const { return static_cast<uint32_t>(m_code_size - m_code_used); }
  void ReserveCode(uint32_t size);
  void CommitCode(uint32_t length);

  ALWAYS_INLINE uint8_t* GetFreeFarCodePointer() const { return m_free_far_code_ptr; }
  ALWAYS_INLINE uint32_t GetFreeFarCodeSpace() const { return static_cast<uint32_t>(m_far_code_size - m_far_code_used); }
  void CommitFarCode(uint32_t length);

  /// Adjusts the free code pointer to the specified alignment, padding with bytes.
  /// Assumes alignment is a power-of-two.
  void Align(uint32_t alignment, uint8_t padding_value);

  /// Flushes the instruction cache on the host for the specified range.
  static void FlushInstructionCache(void* address, uint32_t size);

  /// For Apple Silicon - Toggles write protection on the JIT space.
#if defined(__APPLE__) && defined(__aarch64__)
  static void WriteProtect(bool enabled);
#else
  ALWAYS_INLINE static void WriteProtect(bool enabled) {}
#endif

private:
  uint8_t* m_code_ptr = nullptr;
  uint8_t* m_free_code_ptr = nullptr;
  uint32_t m_code_size = 0;
  uint32_t m_code_reserve_size = 0;
  uint32_t m_code_used = 0;

  uint8_t* m_far_code_ptr = nullptr;
  uint8_t* m_free_far_code_ptr = nullptr;
  uint32_t m_far_code_size = 0;
  uint32_t m_far_code_used = 0;

  uint32_t m_total_size = 0;
  uint32_t m_guard_size = 0;
  uint32_t m_old_protection = 0;
  bool m_owns_buffer = false;
};
