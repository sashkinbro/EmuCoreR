#pragma once
#include "types.h"
#include "file_system.h"
#include "cd_image.h"
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace Common {
class Error;
}

namespace CueParser {

using TrackMode = CDImage::TrackMode;
using MSF = CDImage::Position;

inline constexpr int32_t MIN_TRACK_NUMBER = 1, MAX_TRACK_NUMBER = 99, MIN_INDEX_NUMBER = 0, MAX_INDEX_NUMBER = 99;

enum class TrackFlag : uint32_t
{
  PreEmphasis = (1 << 0),
  CopyPermitted = (1 << 1),
  FourChannelAudio = (1 << 2),
  SerialCopyManagement = (1 << 3),
};

struct Track
{
  uint32_t number;
  uint32_t flags;
  std::string file;
  std::vector<std::pair<uint32_t, MSF>> indices;
  TrackMode mode;
  MSF start;
  std::optional<MSF> length;
  std::optional<MSF> zero_pregap;

  const MSF* GetIndex(uint32_t n) const;

  ALWAYS_INLINE bool HasFlag(TrackFlag flag) const { return (flags & static_cast<uint32_t>(flag)) != 0; }
  ALWAYS_INLINE void SetFlag(TrackFlag flag) { flags |= static_cast<uint32_t>(flag); }
};

class File
{
public:
  File();
  ~File();

  const Track* GetTrack(uint32_t n) const;

  bool Parse(RFILE* fp, Common::Error* error);

private:
  Track* GetMutableTrack(uint32_t n);

  void SetError(uint32_t line_number, Common::Error* error, const char* format, ...);

  static std::string_view GetToken(const char*& line);
  static std::optional<MSF> GetMSF(const std::string_view& token);

  bool ParseLine(const char* line, uint32_t line_number, Common::Error* error);

  bool HandleFileCommand(const char* line, uint32_t line_number, Common::Error* error);
  bool HandleTrackCommand(const char* line, uint32_t line_number, Common::Error* error);
  bool HandleIndexCommand(const char* line, uint32_t line_number, Common::Error* error);
  bool HandlePregapCommand(const char* line, uint32_t line_number, Common::Error* error);
  bool HandleFlagCommand(const char* line, uint32_t line_number, Common::Error* error);

  bool CompleteLastTrack(uint32_t line_number, Common::Error* error);
  bool SetTrackLengths(uint32_t line_number, Common::Error* error);

  std::vector<Track> m_tracks;
  std::optional<std::string> m_current_file;
  std::optional<Track> m_current_track;
};

} // namespace CueParser
