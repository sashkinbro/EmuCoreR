#pragma once
#include "common/hash_combine.h"
#include "common/image.h"
#include "types.h"
#include <string>
#include <unordered_map>
#include <vector>

struct TextureReplacementHash
{
  uint64_t low;
  uint64_t high;

  std::string ToString() const;
  bool ParseString(const std::string_view& sv);

  bool operator==(const TextureReplacementHash& rhs) const { return low == rhs.low && high == rhs.high; }
};

namespace std {
template<>
struct hash<TextureReplacementHash>
{
  size_t operator()(const TextureReplacementHash& h) const
  {
    size_t hash_hash = std::hash<uint64_t>{}(h.low);
    hash_combine(hash_hash, h.high);
    return hash_hash;
  }
};
} // namespace std

using TextureReplacementTexture = Common::RGBA8Image;

class TextureReplacements
{
public:
  enum class ReplacmentType
  {
    VRAMWrite
  };

  TextureReplacements();
  ~TextureReplacements();

  void SetGameID(std::string game_id);

  void Reload();

  const TextureReplacementTexture* GetVRAMWriteReplacement(uint32_t width, uint32_t height, const void* pixels);

  void Shutdown();

private:
  using VRAMWriteReplacementMap = std::unordered_map<TextureReplacementHash, std::string>;
  using TextureCache = std::unordered_map<std::string, TextureReplacementTexture>;

  static bool ParseReplacementFilename(const std::string& filename, TextureReplacementHash* replacement_hash,
                                       ReplacmentType* replacement_type);

  std::string GetSourceDirectory() const;

  TextureReplacementHash GetVRAMWriteHash(uint32_t width, uint32_t height, const void* pixels) const;

  void FindTextures(const std::string& dir);

  const TextureReplacementTexture* LoadTexture(const std::string& filename);
  void PreloadTextures();
  void PurgeUnreferencedTexturesFromCache();

  std::string m_game_id;

  TextureCache m_texture_cache;

  VRAMWriteReplacementMap m_vram_write_replacements;
};

extern TextureReplacements g_texture_replacements;