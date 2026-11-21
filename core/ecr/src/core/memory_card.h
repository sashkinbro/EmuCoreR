#pragma once
#include "common/bitfield.h"
#include "controller.h"
#include "memory_card_image.h"
#include <array>
#include <memory>
#include <string>
#include <string_view>

class TimingEvent;

class MemoryCard final
{
public:
  MemoryCard();
  ~MemoryCard();

  static std::string SanitizeGameTitleForFileName(const std::string_view& name);

  static std::unique_ptr<MemoryCard> Create();
  static std::unique_ptr<MemoryCard> Open(std::string_view filename);

  const MemoryCardImage::DataArray& GetData() const { return m_data; }
  MemoryCardImage::DataArray& GetData() { return m_data; }
  const std::string& GetFilename() const { return m_filename; }
  void SetFilename(std::string filename) { m_filename = std::move(filename); }

  void Reset();
  bool DoState(StateWrapper& sw);

  void ResetTransferState();
  bool Transfer(const uint8_t data_in, uint8_t* data_out);

  void Format();

private:
  // save in three seconds, that should be long enough for everything to finish writing
  static constexpr uint32_t SAVE_DELAY_IN_SECONDS = 5;

  union FLAG
  {
    uint8_t bits;

    BitField<uint8_t, bool, 3, 1> no_write_yet;
    BitField<uint8_t, bool, 2, 1> write_error;
  };

  enum class State : uint8_t
  {
    Idle,
    Command,

    ReadCardID1,
    ReadCardID2,
    ReadAddressMSB,
    ReadAddressLSB,
    ReadACK1,
    ReadACK2,
    ReadConfirmAddressMSB,
    ReadConfirmAddressLSB,
    ReadData,
    ReadChecksum,
    ReadEnd,

    WriteCardID1,
    WriteCardID2,
    WriteAddressMSB,
    WriteAddressLSB,
    WriteData,
    WriteChecksum,
    WriteACK1,
    WriteACK2,
    WriteEnd,
  };

  static TickCount GetSaveDelayInTicks();

  bool LoadFromFile();
  bool SaveIfChanged(bool display_osd_message);
  void QueueFileSave();

  std::unique_ptr<TimingEvent> m_save_event;

  State m_state = State::Idle;
  FLAG m_FLAG = {};
  uint16_t m_address = 0;
  uint8_t m_sector_offset = 0;
  uint8_t m_checksum = 0;
  uint8_t m_last_byte = 0;
  bool m_changed = false;

  MemoryCardImage::DataArray m_data{};

  std::string m_filename;
};
