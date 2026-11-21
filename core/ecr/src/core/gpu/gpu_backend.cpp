#include "gpu_backend.h"
#include "common/align.h"
#include "common/state_wrapper.h"
#include "common/timer.h"
#include "settings.h"

GPUBackend::GPUBackend() = default;

GPUBackend::~GPUBackend() = default;

bool GPUBackend::Initialize(bool force_thread)
{
  if (force_thread || g_settings.gpu_use_thread)
    StartGPUThread();

  return true;
}

void GPUBackend::Reset(bool clear_vram)
{
  Sync(true);
  m_drawing_area = {};
}

void GPUBackend::UpdateSettings()
{
  Sync(true);

  if (m_use_gpu_thread != g_settings.gpu_use_thread)
  {
    if (!g_settings.gpu_use_thread)
      StopGPUThread();
    else
      StartGPUThread();
  }
}

void GPUBackend::Shutdown()
{
  StopGPUThread();
}

GPUBackendFillVRAMCommand* GPUBackend::NewFillVRAMCommand()
{
  return static_cast<GPUBackendFillVRAMCommand*>(
    AllocateCommand(GPUBackendCommandType::FillVRAM, sizeof(GPUBackendFillVRAMCommand)));
}

GPUBackendUpdateVRAMCommand* GPUBackend::NewUpdateVRAMCommand(uint32_t num_words)
{
  const uint32_t size = sizeof(GPUBackendUpdateVRAMCommand) + (num_words * sizeof(uint16_t));
  GPUBackendUpdateVRAMCommand* cmd =
    static_cast<GPUBackendUpdateVRAMCommand*>(AllocateCommand(GPUBackendCommandType::UpdateVRAM, size));
  return cmd;
}

GPUBackendCopyVRAMCommand* GPUBackend::NewCopyVRAMCommand()
{
  return static_cast<GPUBackendCopyVRAMCommand*>(
    AllocateCommand(GPUBackendCommandType::CopyVRAM, sizeof(GPUBackendCopyVRAMCommand)));
}

GPUBackendSetDrawingAreaCommand* GPUBackend::NewSetDrawingAreaCommand()
{
  return static_cast<GPUBackendSetDrawingAreaCommand*>(
    AllocateCommand(GPUBackendCommandType::SetDrawingArea, sizeof(GPUBackendSetDrawingAreaCommand)));
}

GPUBackendDrawPolygonCommand* GPUBackend::NewDrawPolygonCommand(uint32_t num_vertices)
{
  const uint32_t size = sizeof(GPUBackendDrawPolygonCommand) + (num_vertices * sizeof(GPUBackendDrawPolygonCommand::Vertex));
  GPUBackendDrawPolygonCommand* cmd =
    static_cast<GPUBackendDrawPolygonCommand*>(AllocateCommand(GPUBackendCommandType::DrawPolygon, size));
  cmd->num_vertices = static_cast<uint16_t>(num_vertices);
  return cmd;
}

GPUBackendDrawRectangleCommand* GPUBackend::NewDrawRectangleCommand()
{
  return static_cast<GPUBackendDrawRectangleCommand*>(
    AllocateCommand(GPUBackendCommandType::DrawRectangle, sizeof(GPUBackendDrawRectangleCommand)));
}

GPUBackendDrawLineCommand* GPUBackend::NewDrawLineCommand(uint32_t num_vertices)
{
  const uint32_t size = sizeof(GPUBackendDrawLineCommand) + (num_vertices * sizeof(GPUBackendDrawLineCommand::Vertex));
  GPUBackendDrawLineCommand* cmd =
    static_cast<GPUBackendDrawLineCommand*>(AllocateCommand(GPUBackendCommandType::DrawLine, size));
  cmd->num_vertices = static_cast<uint16_t>(num_vertices);
  return cmd;
}

void* GPUBackend::AllocateCommand(GPUBackendCommandType command, uint32_t size)
{
  // Ensure size is a multiple of 4 so we don't end up with an unaligned command.
  size = Common::AlignUpPow2(size, 4);

  for (;;)
  {
    uint32_t read_ptr = m_command_fifo_read_ptr.load(std::memory_order_acquire);
    uint32_t write_ptr = m_command_fifo_write_ptr.load(std::memory_order_relaxed);
    if (read_ptr > write_ptr)
    {
      uint32_t available_size = read_ptr - write_ptr;
      while (available_size < (size + sizeof(GPUBackendCommandType)))
      {
        WakeGPUThread();
        read_ptr = m_command_fifo_read_ptr.load(std::memory_order_acquire);
        available_size = (read_ptr > write_ptr) ? (read_ptr - write_ptr) : (COMMAND_QUEUE_SIZE - write_ptr);
      }
    }
    else
    {
      const uint32_t available_size = COMMAND_QUEUE_SIZE - write_ptr;
      if ((size + sizeof(GPUBackendCommand)) > available_size)
      {
        // allocate a dummy command to wrap the buffer around
        GPUBackendCommand* dummy_cmd = reinterpret_cast<GPUBackendCommand*>(&m_command_fifo_data[write_ptr]);
        dummy_cmd->type = GPUBackendCommandType::Wraparound;
        dummy_cmd->size = available_size;
        dummy_cmd->params.bits = 0;
        m_command_fifo_write_ptr.store(0, std::memory_order_seq_cst);
        continue;
      }
    }

    GPUBackendCommand* cmd = reinterpret_cast<GPUBackendCommand*>(&m_command_fifo_data[write_ptr]);
    cmd->type = command;
    cmd->size = size;
    return cmd;
  }
}

uint32_t GPUBackend::GetPendingCommandSize() const
{
  const uint32_t read_ptr = m_command_fifo_read_ptr.load(std::memory_order_seq_cst);
  const uint32_t write_ptr = m_command_fifo_write_ptr.load(std::memory_order_seq_cst);
  return (write_ptr >= read_ptr) ? (write_ptr - read_ptr) : (COMMAND_QUEUE_SIZE - read_ptr + write_ptr);
}

void GPUBackend::PushCommand(GPUBackendCommand* cmd)
{
  if (!m_use_gpu_thread)
  {
    // single-thread mode
    if (cmd->type != GPUBackendCommandType::Sync)
      HandleCommand(cmd);
  }
  else
  {
    const uint32_t new_write_ptr = m_command_fifo_write_ptr.load(std::memory_order_relaxed) + cmd->size;
    m_command_fifo_write_ptr.store(new_write_ptr, std::memory_order_seq_cst);
    if (m_gpu_thread_sleeping.load(std::memory_order_seq_cst) && GetPendingCommandSize() >= THRESHOLD_TO_WAKE_GPU)
      WakeGPUThread();
  }
}

void GPUBackend::WakeGPUThread()
{
  if (!m_gpu_thread_sleeping.load(std::memory_order_seq_cst))
    return;

  std::unique_lock<std::mutex> lock(m_sync_mutex);
  if (!m_gpu_thread_sleeping.load(std::memory_order_relaxed))
    return;

  m_wake_gpu_thread_cv.notify_one();
}

void GPUBackend::StartGPUThread()
{
  m_gpu_loop_done.store(false);
  m_use_gpu_thread = true;
  m_gpu_thread = std::thread(&GPUBackend::RunGPULoop, this);
}

void GPUBackend::StopGPUThread()
{
  if (!m_use_gpu_thread)
    return;

  m_gpu_loop_done.store(true);
  WakeGPUThread();
  m_gpu_thread.join();
  m_use_gpu_thread = false;
}

void GPUBackend::Sync(bool allow_sleep)
{
  if (!m_use_gpu_thread)
    return;

  GPUBackendSyncCommand* cmd =
    static_cast<GPUBackendSyncCommand*>(AllocateCommand(GPUBackendCommandType::Sync, sizeof(GPUBackendSyncCommand)));
  cmd->allow_sleep = allow_sleep;
  PushCommand(cmd);
  WakeGPUThread();

  m_sync_event.Wait();
  m_sync_event.Reset();
}

void GPUBackend::RunGPULoop()
{
  // 1ms spin window before the GPU thread sleeps, expressed in the timer's
  // native integer units (handles the Windows QPC tick scale via the same
  // conversion). Comparing Values directly avoids a per-iteration trip
  // through floating-point nanoseconds.
  const Common::Timer::Value spin_time = Common::Timer::ConvertSecondsToValue(0.001);
  Common::Timer::Value last_command_time = 0;
  uint32_t spin_iters = 0;

  for (;;)
  {
    uint32_t write_ptr = m_command_fifo_write_ptr.load(std::memory_order_acquire);
    uint32_t read_ptr = m_command_fifo_read_ptr.load(std::memory_order_relaxed);
    if (read_ptr == write_ptr)
    {
      if ((spin_iters++ & 63u) != 0u)
      {
#if defined(__aarch64__)
        asm volatile("yield" ::: "memory");
#endif
        continue;
      }

      const Common::Timer::Value current_time = Common::Timer::GetValue();
      if ((current_time - last_command_time) < spin_time)
        continue;

      std::unique_lock<std::mutex> lock(m_sync_mutex);
      m_gpu_thread_sleeping.store(true, std::memory_order_seq_cst);
      m_wake_gpu_thread_cv.wait(lock, [this]() { return m_gpu_loop_done.load() || GetPendingCommandSize() > 0; });
      m_gpu_thread_sleeping.store(false, std::memory_order_relaxed);
      spin_iters = 0;

      if (!m_gpu_loop_done.load())
        continue;
      break;
    }

    if (write_ptr < read_ptr)
      write_ptr = COMMAND_QUEUE_SIZE;

    bool allow_sleep = false;
    while (read_ptr < write_ptr)
    {
      const GPUBackendCommand* cmd = reinterpret_cast<const GPUBackendCommand*>(&m_command_fifo_data[read_ptr]);
      read_ptr += cmd->size;

      switch (cmd->type)
      {
        case GPUBackendCommandType::Wraparound:
        {
          write_ptr = m_command_fifo_write_ptr.load(std::memory_order_acquire);
          read_ptr = 0;
        }
        break;

        case GPUBackendCommandType::Sync:
        {
          m_sync_event.Signal();
          allow_sleep = static_cast<const GPUBackendSyncCommand*>(cmd)->allow_sleep;
        }
        break;

        default:
          HandleCommand(cmd);
          break;
      }
    }

    spin_iters = 0;
    last_command_time = allow_sleep ? 0 : Common::Timer::GetValue();
    m_command_fifo_read_ptr.store(read_ptr, std::memory_order_release);
  }
}

void GPUBackend::HandleCommand(const GPUBackendCommand* cmd)
{
  switch (cmd->type)
  {
    case GPUBackendCommandType::FillVRAM:
    {
      FlushRender();
      const GPUBackendFillVRAMCommand* ccmd = static_cast<const GPUBackendFillVRAMCommand*>(cmd);
      FillVRAM(static_cast<uint32_t>(ccmd->x), static_cast<uint32_t>(ccmd->y), static_cast<uint32_t>(ccmd->width), static_cast<uint32_t>(ccmd->height),
               ccmd->color, ccmd->params);
    }
    break;

    case GPUBackendCommandType::UpdateVRAM:
    {
      FlushRender();
      const GPUBackendUpdateVRAMCommand* ccmd = static_cast<const GPUBackendUpdateVRAMCommand*>(cmd);
      UpdateVRAM(static_cast<uint32_t>(ccmd->x), static_cast<uint32_t>(ccmd->y), static_cast<uint32_t>(ccmd->width), static_cast<uint32_t>(ccmd->height),
                 ccmd->data, ccmd->params);
    }
    break;

    case GPUBackendCommandType::CopyVRAM:
    {
      FlushRender();
      const GPUBackendCopyVRAMCommand* ccmd = static_cast<const GPUBackendCopyVRAMCommand*>(cmd);
      CopyVRAM(static_cast<uint32_t>(ccmd->src_x), static_cast<uint32_t>(ccmd->src_y), static_cast<uint32_t>(ccmd->dst_x),
               static_cast<uint32_t>(ccmd->dst_y), static_cast<uint32_t>(ccmd->width), static_cast<uint32_t>(ccmd->height), ccmd->params);
    }
    break;

    case GPUBackendCommandType::SetDrawingArea:
    {
      FlushRender();
      m_drawing_area = static_cast<const GPUBackendSetDrawingAreaCommand*>(cmd)->new_area;
    }
    break;

    case GPUBackendCommandType::DrawPolygon:
    {
      DrawPolygon(static_cast<const GPUBackendDrawPolygonCommand*>(cmd));
    }
    break;

    case GPUBackendCommandType::DrawRectangle:
    {
      DrawRectangle(static_cast<const GPUBackendDrawRectangleCommand*>(cmd));
    }
    break;

    case GPUBackendCommandType::DrawLine:
    {
      DrawLine(static_cast<const GPUBackendDrawLineCommand*>(cmd));
    }
    break;

    default:
      break;
  }
}
