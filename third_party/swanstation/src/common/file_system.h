#pragma once
#include "types.h"
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <streams/file_stream.h>

#include "string.h"

class ByteStream;

#ifdef _WIN32
#define FS_OSPATH_SEPARATOR_CHARACTER '\\'
#define FS_OSPATH_SEPARATOR_STR "\\"
#else
#define FS_OSPATH_SEPARATOR_CHARACTER '/'
#define FS_OSPATH_SEPARATOR_STR "/"
#endif

inline constexpr uint32_t FILESYSTEM_FILE_ATTRIBUTE_DIRECTORY = 1;

inline constexpr uint32_t FILESYSTEM_FIND_RECURSIVE = (1 << 0), FILESYSTEM_FIND_RELATIVE_PATHS = (1 << 1),
                     FILESYSTEM_FIND_HIDDEN_FILES = (1 << 2),
                     FILESYSTEM_FIND_FILES = (1 << 4);

struct FILESYSTEM_FIND_DATA
{
  std::string FileName;
  uint64_t Size;
};

namespace FileSystem {

using FindResultsArray = std::vector<FILESYSTEM_FIND_DATA>;

// builds a path relative to the specified file
std::string BuildRelativePath(const std::string_view& filename, const std::string_view& new_filename);

/// Returns true if the specified path is an absolute path (C:\Path on Windows or /path on Unix).
bool IsAbsolutePath(const std::string_view& path);

/// Removes the extension of a filename.
std::string_view StripExtension(const std::string_view& path);

/// Replaces the extension of a filename with another.
std::string ReplaceExtension(const std::string_view& path, const std::string_view& new_extension);

/// Returns the display name of a filename. Usually this is the same as the path, except on Android
/// where it resolves a content URI to its name.
std::string GetDisplayNameFromPath(const std::string_view& path);

/// Returns the file title (less the extension and path) from a filename.
std::string_view GetFileTitleFromPath(const std::string_view& path);

// search for files
bool FindFiles(const char* Path, const char* Pattern, uint32_t Flags, FindResultsArray* pResults);

// open files
std::unique_ptr<ByteStream> OpenFile(const char* FileName, uint32_t Flags);

std::optional<std::vector<uint8_t>> ReadBinaryFile(const char* filename);
bool WriteBinaryFile(const char* filename, const void* data, size_t data_length);

RFILE *OpenRFile(const char* filename, const char* mode);

/// Read-only open that asks the VFS for a memory mapping
/// (RETRO_VFS_FILE_ACCESS_HINT_FREQUENT_ACCESS).  The hint is exactly
/// that: on platforms without mapping support, for frontend-backed
/// (URI) handles, or when the map fails (e.g. >SIZE_MAX on 32-bit),
/// the handle silently degrades to a plain descriptor and all the
/// usual read/seek calls keep working.  Use GetMappedView() to find
/// out which one you got.
RFILE* OpenMappableRFile(const char* filename);

/// Borrowed read-only view of the whole file when the handle is
/// memory-mapped, else nullptr with *size untouched.  Owned by the
/// handle; valid until rfclose().  Callers must keep their ordinary
/// read path - nullptr is a normal answer, not an error.
const uint8_t* GetMappedView(RFILE* fp, int64_t* size);

int64_t FSeek64(RFILE* fp, int64_t offset, int whence);
int64_t FTell64(RFILE* fp);
std::optional<std::string> ReadFileToString(RFILE* fp);
std::optional<std::vector<uint8_t>> ReadBinaryFile(RFILE* fp);

}; // namespace FileSystem

#ifdef __cplusplus
extern "C" {
#endif

char *rfgets(char *buffer, int maxCount, RFILE* stream);
int rfeof(RFILE* stream);
RFILE* rfopen(const char *path, const char *mode);
int rfclose(RFILE* stream);
int64_t rftell(RFILE* stream);
int64_t rfseek(RFILE* stream, int64_t offset, int origin);
int64_t rfwrite(void const* buffer,
   size_t elem_size, size_t elem_count, RFILE* stream);
int64_t rfread(void* buffer,
   size_t elem_size, size_t elem_count, RFILE* stream);
int rfgetc(RFILE* stream);

#ifdef __cplusplus
}
#endif
