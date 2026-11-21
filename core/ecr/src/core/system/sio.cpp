#include "sio.h"
#include "common/state_wrapper.h"
#include "controller.h"
#include "host_interface.h"
#include "interrupt_controller.h"
#include "memory_card.h"

SIO g_sio;

SIO::SIO() = default;

SIO::~SIO() = default;

void SIO::Initialize()
{
  Reset();
}

void SIO::Shutdown() {}

void SIO::Reset()
{
  SoftReset();
}

bool SIO::DoState(StateWrapper& sw)
{
  sw.Do(&m_SIO_CTRL.bits);
  sw.Do(&m_SIO_STAT.bits);
  sw.Do(&m_SIO_MODE.bits);
  sw.Do(&m_SIO_BAUD);

  return !sw.HasError();
}

uint32_t SIO::ReadRegister(uint32_t offset)
{
  switch (offset)
  {
    case 0x00: // SIO_DATA
    {
      const uint8_t value = 0xFF;
      return (static_cast<uint32_t>(value) | (static_cast<uint32_t>(value) << 8) | (static_cast<uint32_t>(value) << 16) |
              (static_cast<uint32_t>(value) << 24));
    }

    case 0x04: // SIO_STAT
    {
      const uint32_t bits = m_SIO_STAT.bits;
      return bits;
    }

    case 0x08: // SIO_MODE
      return static_cast<uint32_t>(m_SIO_MODE.bits);

    case 0x0A: // SIO_CTRL
      return static_cast<uint32_t>(m_SIO_CTRL.bits);

    case 0x0E: // SIO_BAUD
      return static_cast<uint32_t>(m_SIO_BAUD);

    default:
      return UINT32_C(0xFFFFFFFF);
  }
}

void SIO::WriteRegister(uint32_t offset, uint32_t value)
{
  switch (offset)
  {
    case 0x0A: // SIO_CTRL
      m_SIO_CTRL.bits = static_cast<uint16_t>(value);
      if (m_SIO_CTRL.RESET)
        SoftReset();

      break;

    case 0x08: // SIO_MODE
      m_SIO_MODE.bits = static_cast<uint16_t>(value);
      break;

    case 0x0E:
      m_SIO_BAUD = static_cast<uint16_t>(value);
      break;

    case 0x00: // SIO_DATA
    default:
      break;
  }
}

void SIO::SoftReset()
{
  m_SIO_CTRL.bits = 0;
  m_SIO_STAT.bits = 0;
  m_SIO_STAT.DSRINPUTLEVEL = true;
  m_SIO_STAT.CTSINPUTLEVEL = true;
  m_SIO_STAT.TXDONE = true;
  m_SIO_STAT.TXRDY = true;
  m_SIO_MODE.bits = 0;
  m_SIO_BAUD = 0xDC;
}
