#include "archive_disc.h"
#include <archive.h>
#include <archive_entry.h>
#include <android/log.h>
#include <unistd.h>
#include <sys/stat.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace emucorer::archive_disc {
namespace {
constexpr const char* kPrefix = "archive://";
constexpr size_t kMaxEntries = 5000;
constexpr int64_t kBlockSize = 1024 * 1024;
constexpr size_t kMaxBlocks = 32;
struct MountState {
    int fd = -1;
    std::map<std::string, int64_t> entries;
    struct Block { std::vector<uint8_t> data; uint64_t used; };
    std::map<std::pair<std::string, int64_t>, Block> blocks;
    std::mutex cache_mutex;
    uint64_t cache_clock = 0;
    ~MountState() { if (fd >= 0) close(fd); }
};
struct Source { std::shared_ptr<MountState> mount; int64_t offset = 0; std::array<char, 65536> buffer{}; };
struct Reader {
    Source source;
    struct archive* archive = nullptr;
    ~Reader() { if (archive) archive_read_free(archive); }
};
struct FileHandle {
    std::shared_ptr<MountState> mount;
    std::string path, entry;
    int64_t position = 0, size = 0, decoded = 0;
    std::unique_ptr<Reader> reader;
};
std::mutex g_mutex;
std::map<std::string, std::shared_ptr<MountState>> g_mounts;
std::atomic<uint64_t> g_next{1};

la_ssize_t ReadSource(struct archive*, void* data, const void** output) {
    auto* s = static_cast<Source*>(data);
    const ssize_t count = pread(s->mount->fd, s->buffer.data(), s->buffer.size(), s->offset);
    if (count < 0) return -1;
    s->offset += count; *output = s->buffer.data(); return count;
}
la_int64_t SkipSource(struct archive*, void* data, la_int64_t count) {
    auto* s = static_cast<Source*>(data);
    if (count < 0) return 0;
    struct stat st{};
    if (fstat(s->mount->fd, &st) != 0) return 0;
    const int64_t n = std::min<int64_t>(count, std::max<int64_t>(0, st.st_size - s->offset));
    s->offset += n; return n;
}
la_int64_t SeekSource(struct archive*, void* data, la_int64_t offset, int whence) {
    auto* s = static_cast<Source*>(data); struct stat st{};
    if (fstat(s->mount->fd, &st) != 0) return -1;
    const int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? s->offset : whence == SEEK_END ? st.st_size : -1;
    if (base < 0 || offset < -base || (offset > 0 && base > INT64_MAX - offset)) return -1;
    s->offset = base + offset; return s->offset;
}
int OpenSource(struct archive*, void*) { return ARCHIVE_OK; }
int CloseSource(struct archive*, void*) { return ARCHIVE_OK; }
std::unique_ptr<Reader> NewReader(const std::shared_ptr<MountState>& mount) {
    auto r = std::make_unique<Reader>(); r->source.mount = mount;
    r->archive = archive_read_new(); if (!r->archive) return {};
    archive_read_support_format_zip(r->archive);
    archive_read_support_format_7zip(r->archive);
    archive_read_support_format_rar(r->archive);
    archive_read_support_format_rar5(r->archive);
    archive_read_set_seek_callback(r->archive, SeekSource);
    if (archive_read_open2(r->archive, &r->source, OpenSource, ReadSource, SkipSource, CloseSource) != ARCHIVE_OK) return {};
    return r;
}
std::string Normalize(const char* name) {
    if (!name) return {};
    std::string value(name); std::replace(value.begin(), value.end(), '\\', '/');
    while (value.rfind("./", 0) == 0) value.erase(0, 2);
    if (value.empty() || value[0] == '/' || value.find("://") != std::string::npos) return {};
    size_t start = 0;
    while (start <= value.size()) {
        const size_t end = value.find('/', start);
        const auto part = value.substr(start, end == std::string::npos ? end : end - start);
        if (part == ".." || part.empty()) return {};
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return value;
}
std::string Lower(std::string s) { for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }
std::string Extension(const std::string& s) { auto p = s.find_last_of('.'); return p == std::string::npos ? "" : Lower(s.substr(p)); }
const char* EntryName(struct archive_entry* entry) {
    const char* name = archive_entry_pathname_utf8(entry);
    return name ? name : archive_entry_pathname(entry);
}
bool PositionReader(FileHandle& file) {
    if (file.reader && file.position >= file.decoded) return true;
    file.reader = NewReader(file.mount); file.decoded = 0;
    if (!file.reader) return false;
    struct archive_entry* entry = nullptr;
    while (archive_read_next_header(file.reader->archive, &entry) == ARCHIVE_OK) {
        if (Normalize(EntryName(entry)) == file.entry) return true;
    }
    file.reader.reset(); return false;
}
bool Resolve(const std::string& path, std::shared_ptr<MountState>& mount, std::string& entry) {
    if (path.rfind(kPrefix, 0) != 0) return false;
    const size_t begin = std::strlen(kPrefix), slash = path.find('/', begin);
    if (slash == std::string::npos) return false;
    const std::string id = path.substr(begin, slash - begin);
    entry = Normalize(path.c_str() + slash + 1); if (entry.empty()) return false;
    std::lock_guard lock(g_mutex);
    auto it = g_mounts.find(id); if (it == g_mounts.end()) return false;
    mount = it->second; return mount->entries.count(entry) != 0;
}
const char* GetPath(retro_vfs_file_handle* h) { return reinterpret_cast<FileHandle*>(h)->path.c_str(); }
retro_vfs_file_handle* Open(const char* path, unsigned mode, unsigned) {
    if (!path || mode != RETRO_VFS_FILE_ACCESS_READ) return nullptr;
    std::shared_ptr<MountState> mount; std::string entry;
    if (!Resolve(path, mount, entry)) return nullptr;
    auto f = std::make_unique<FileHandle>(); f->mount = mount; f->path = path; f->entry = entry; f->size = mount->entries.at(entry);
    return reinterpret_cast<retro_vfs_file_handle*>(f.release());
}
int Close(retro_vfs_file_handle* h) { delete reinterpret_cast<FileHandle*>(h); return 0; }
int64_t Size(retro_vfs_file_handle* h) { return reinterpret_cast<FileHandle*>(h)->size; }
int64_t Tell(retro_vfs_file_handle* h) { return reinterpret_cast<FileHandle*>(h)->position; }
int64_t Seek(retro_vfs_file_handle* h, int64_t offset, int origin) {
    auto& f = *reinterpret_cast<FileHandle*>(h);
    int64_t base = origin == RETRO_VFS_SEEK_POSITION_START ? 0 : origin == RETRO_VFS_SEEK_POSITION_CURRENT ? f.position : origin == RETRO_VFS_SEEK_POSITION_END ? f.size : -1;
    if (base < 0 || offset < -base || (offset > 0 && base > INT64_MAX - offset)) return -1;
    f.position = base + offset; return f.position;
}
int64_t Read(retro_vfs_file_handle* h, void* output, uint64_t length) {
    auto& f = *reinterpret_cast<FileHandle*>(h);
    if (f.position >= f.size) return 0;
    const uint64_t wanted = std::min<uint64_t>(length, f.size - f.position);
    auto* dest = static_cast<uint8_t*>(output);
    uint64_t copied = 0;
    // Keep a bounded, shared cache of decompressed blocks in memory only.
    // A seek never materializes a complete image on storage.
    std::lock_guard lock(f.mount->cache_mutex);
    while (copied < wanted) {
        const int64_t block = f.position / kBlockSize;
        const int64_t block_start = block * kBlockSize;
        const auto key = std::make_pair(f.entry, block);
        auto it = f.mount->blocks.find(key);
        if (it == f.mount->blocks.end()) {
            const int64_t requested_pos = f.position;
            f.position = block_start;
            if (!PositionReader(f)) { __android_log_print(ANDROID_LOG_ERROR, "EmuCoreR-Archive", "cannot open entry %s", f.entry.c_str()); f.position = requested_pos; return copied ? copied : -1; }
            std::array<char, 65536> discard{};
            while (f.decoded < block_start) {
                const size_t nbytes = static_cast<size_t>(std::min<int64_t>(discard.size(), block_start - f.decoded));
                const la_ssize_t n = archive_read_data(f.reader->archive, discard.data(), nbytes);
                if (n <= 0) { __android_log_print(ANDROID_LOG_ERROR, "EmuCoreR-Archive", "read %s offset=%lld n=%lld error=%s", f.entry.c_str(), static_cast<long long>(f.decoded), static_cast<long long>(n), archive_error_string(f.reader->archive) ? archive_error_string(f.reader->archive) : "none"); f.position = requested_pos; return copied ? copied : -1; }
                f.decoded += n;
            }
            MountState::Block decoded;
            const size_t size = static_cast<size_t>(std::min<int64_t>(kBlockSize, f.size - block_start));
            decoded.data.resize(size);
            size_t filled = 0;
            while (filled < size) {
                const la_ssize_t n = archive_read_data(f.reader->archive, decoded.data.data() + filled, size - filled);
                if (n <= 0) { __android_log_print(ANDROID_LOG_ERROR, "EmuCoreR-Archive", "read %s offset=%lld n=%lld error=%s", f.entry.c_str(), static_cast<long long>(f.decoded), static_cast<long long>(n), archive_error_string(f.reader->archive) ? archive_error_string(f.reader->archive) : "none"); f.position = requested_pos; return copied ? copied : -1; }
                filled += static_cast<size_t>(n); f.decoded += n;
            }
            decoded.used = ++f.mount->cache_clock;
            if (f.mount->blocks.size() >= kMaxBlocks) {
                auto oldest = std::min_element(f.mount->blocks.begin(), f.mount->blocks.end(),
                    [](const auto& a, const auto& b) { return a.second.used < b.second.used; });
                f.mount->blocks.erase(oldest);
            }
            it = f.mount->blocks.emplace(key, std::move(decoded)).first;
            f.position = requested_pos;
        }
        it->second.used = ++f.mount->cache_clock;
        const size_t in_block = static_cast<size_t>(f.position - block_start);
        const size_t n = static_cast<size_t>(std::min<uint64_t>(wanted - copied, it->second.data.size() - in_block));
        if (!n) break;
        std::memcpy(dest + copied, it->second.data.data() + in_block, n);
        copied += n; f.position += n;
    }
    return static_cast<int64_t>(copied);
}
int64_t Write(retro_vfs_file_handle*, const void*, uint64_t) { return -1; }
int Flush(retro_vfs_file_handle*) { return 0; }
int Remove(const char*) { return -1; }
int Rename(const char*, const char*) { return -1; }
int Stat(const char* path, int32_t* size) {
    std::shared_ptr<MountState> mount; std::string entry;
    if (!path || !Resolve(path, mount, entry)) return 0;
    if (size) *size = static_cast<int32_t>(std::min<int64_t>(mount->entries.at(entry), INT32_MAX));
    return RETRO_VFS_STAT_IS_VALID;
}
retro_vfs_interface g_interface = {GetPath, Open, Close, Size, Tell, Seek, Read, Write, Flush, Remove, Rename,
                                  nullptr, Stat, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
}
std::string Mount(int fd) {
    auto mount = std::make_shared<MountState>(); mount->fd = dup(fd);
    if (mount->fd < 0) return {};
    auto reader = NewReader(mount); if (!reader) return {};
    std::vector<std::string> cues, images;
    struct archive_entry* e = nullptr;
    int status = ARCHIVE_OK; size_t count = 0;
    while ((status = archive_read_next_header(reader->archive, &e)) == ARCHIVE_OK) {
        if (++count > kMaxEntries || archive_entry_is_encrypted(e) > 0) return {};
        if (archive_entry_filetype(e) != AE_IFREG) continue;
        const std::string name = Normalize(EntryName(e)); const int64_t size = archive_entry_size(e);
        if (name.empty() || size < 0 || !mount->entries.emplace(name, size).second) return {};
        const std::string ext = Extension(name);
        if (ext == ".cue") cues.push_back(name);
        else if (ext == ".bin" || ext == ".iso" || ext == ".img") images.push_back(name);
    }
    if (status != ARCHIVE_EOF) return {};
    const std::string selected = cues.size() == 1 ? cues[0] : cues.empty() && images.size() == 1 ? images[0] : "";
    if (selected.empty()) return {};
    const std::string id = std::to_string(g_next.fetch_add(1));
    { std::lock_guard lock(g_mutex); g_mounts[id] = mount; }
    return std::string(kPrefix) + id + "/" + selected;
}
void Unmount(const std::string& path) {
    if (path.rfind(kPrefix, 0) != 0) return;
    const auto begin = std::strlen(kPrefix), slash = path.find('/', begin);
    if (slash == std::string::npos) return;
    std::lock_guard lock(g_mutex); g_mounts.erase(path.substr(begin, slash - begin));
}
const retro_vfs_interface* Interface() { return &g_interface; }
}
