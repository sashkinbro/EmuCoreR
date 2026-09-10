#pragma once
#include "common/event.h"
#include "common/heap_array.h"
#include "gpu_types.h"
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324) // warning C4324: 'GPUBackend': structure was padded due to alignment specifier
#endif

class GPUBackend
{
public:
  GPUBackend();
  virtual ~GPUBackend();

  ALWAYS_INLINE uint16_t* GetVRAM() const { return m_vram_ptr; }

  virtual bool Initialize(bool force_thread);
  virtual void UpdateSettings();
  virtual void Reset(bool clear_vram);
  virtual void Shutdown();

  GPUBackendFillVRAMCommand* NewFillVRAMCommand();
  GPUBackendUpdateVRAMCommand* NewUpdateVRAMCommand(uint32_t num_words);
  GPUBackendCopyVRAMCommand* NewCopyVRAMCommand();
  GPUBackendSetDrawingAreaCommand* NewSetDrawingAreaCommand();
  GPUBackendDrawPolygonCommand* NewDrawPolygonCommand(uint32_t num_vertices);
  GPUBackendDrawRectangleCommand* NewDrawRectangleCommand();
  GPUBackendDrawLineCommand* NewDrawLineCommand(uint32_t num_vertices);

  void PushCommand(GPUBackendCommand* cmd);
  void Sync(bool allow_sleep);

protected:
  virtual void FillVRAM(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t color, GPUBackendCommandParameters params) = 0;
  virtual void UpdateVRAM(uint32_t x, uint32_t y, uint32_t width, uint32_t height, const void* data,
                          GPUBackendCommandParameters params) = 0;
  virtual void CopyVRAM(uint32_t src_x, uint32_t src_y, uint32_t dst_x, uint32_t dst_y, uint32_t width, uint32_t height,
                        GPUBackendCommandParameters params) = 0;
  virtual void DrawPolygon(const GPUBackendDrawPolygonCommand* cmd) = 0;
  virtual void DrawRectangle(const GPUBackendDrawRectangleCommand* cmd) = 0;
  virtual void DrawLine(const GPUBackendDrawLineCommand* cmd) = 0;
  virtual void FlushRender() = 0;

  void HandleCommand(const GPUBackendCommand* cmd);

  uint16_t* m_vram_ptr = nullptr;

  Common::Rectangle<uint32_t> m_drawing_area{};

  Common::Event m_sync_event;
  std::atomic_bool m_gpu_thread_sleeping{false};
  std::atomic_bool m_gpu_loop_done{false};
  std::thread m_gpu_thread;
  bool m_use_gpu_thread = false;

  std::mutex m_sync_mutex;
  std::condition_variable m_wake_gpu_thread_cv;

  static constexpr uint32_t COMMAND_QUEUE_SIZE = 4 * 1024 * 1024, THRESHOLD_TO_WAKE_GPU = 256;

  HeapArray<uint8_t, COMMAND_QUEUE_SIZE> m_command_fifo_data;
  alignas(64) std::atomic<uint32_t> m_command_fifo_read_ptr{0};
  alignas(64) std::atomic<uint32_t> m_command_fifo_write_ptr{0};

private:
  /// Thread entry point for the GPU worker. Drains the command FIFO until
  /// m_gpu_loop_done is set.
  void RunGPULoop();

  void* AllocateCommand(GPUBackendCommandType command, uint32_t size);
  uint32_t GetPendingCommandSize() const;
  void WakeGPUThread();
  void StartGPUThread();
  void StopGPUThread();
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif
