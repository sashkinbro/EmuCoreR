#pragma once
#include "common/bitfield.h"
#include "common/fifo_queue.h"
#include "types.h"
#include <array>
#include <memory>

class StateWrapper;

class Controller;
class MemoryCard;

class SIO
{
public:
  SIO();
  ~SIO();

  void Initialize();
  void Shutdown();
  void Reset();
  bool DoState(StateWrapper& sw);

  uint32_t ReadRegister(uint32_t offset);
  void WriteRegister(uint32_t offset, uint32_t value);

private:
  union SIO_CTRL
  {
    uint16_t bits;

    BitField<uint16_t, bool, 0, 1> TXEN;
    BitField<uint16_t, bool, 1, 1> DTROUTPUT;
    BitField<uint16_t, bool, 2, 1> RXEN;
    BitField<uint16_t, bool, 3, 1> TXOUTPUT;
    BitField<uint16_t, bool, 4, 1> ACK;
    BitField<uint16_t, bool, 5, 1> RTSOUTPUT;
    BitField<uint16_t, bool, 6, 1> RESET;
    BitField<uint16_t, uint8_t, 8, 2> RXIMODE;
    BitField<uint16_t, bool, 10, 1> TXINTEN;
    BitField<uint16_t, bool, 11, 1> RXINTEN;
    BitField<uint16_t, bool, 12, 1> ACKINTEN;
  };

  union SIO_STAT
  {
    uint32_t bits;

    BitField<uint32_t, bool, 0, 1> TXRDY;
    BitField<uint32_t, bool, 1, 1> RXFIFONEMPTY;
    BitField<uint32_t, bool, 2, 1> TXDONE;
    BitField<uint32_t, bool, 3, 1> RXPARITY;
    BitField<uint32_t, bool, 4, 1> RXFIFOOVERRUN;
    BitField<uint32_t, bool, 5, 1> RXBADSTOPBIT;
    BitField<uint32_t, bool, 6, 1> RXINPUTLEVEL;
    BitField<uint32_t, bool, 7, 1> DSRINPUTLEVEL;
    BitField<uint32_t, bool, 8, 1> CTSINPUTLEVEL;
    BitField<uint32_t, bool, 9, 1> INTR;
    BitField<uint32_t, uint32_t, 11, 15> TMR;
  };

  union SIO_MODE
  {
    uint16_t bits;

    BitField<uint16_t, uint8_t, 0, 2> reload_factor;
    BitField<uint16_t, uint8_t, 2, 2> character_length;
    BitField<uint16_t, bool, 4, 1> parity_enable;
    BitField<uint16_t, uint8_t, 5, 1> parity_type;
    BitField<uint16_t, uint8_t, 6, 2> stop_bit_length;
  };

  void SoftReset();

  SIO_CTRL m_SIO_CTRL = {};
  SIO_STAT m_SIO_STAT = {};
  SIO_MODE m_SIO_MODE = {};
  uint16_t m_SIO_BAUD = 0;
};

extern SIO g_sio;