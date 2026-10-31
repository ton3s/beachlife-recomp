#include "local_files.h"
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif
#include "game_path.h"
#include "platform/path.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace beachlife::data
{
namespace
{
#ifdef _WIN32
using Native = HANDLE;
const Native invalid = INVALID_HANDLE_VALUE;
void close_native(Native value)
{
    CloseHandle(value);
}
#else
using Native = int;
constexpr Native invalid = -1;
void close_native(Native value)
{
    ::close(value);
}
#endif

struct Handle
{
    Native value = invalid;
    explicit Handle(Native handle)
        : value(handle)
    {
        if (value == invalid)
        {
            throw std::runtime_error(
                "Cannot open prepared content; missing, linked or inaccessible path.");
        }
    }
    ~Handle()
    {
        if (value != invalid)
        {
            close_native(value);
        }
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept
        : value(std::exchange(other.value, invalid))
    {
    }
};

class LocalFile final : public SourceFile
{
  public:
    LocalFile(Handle file, std::uint64_t expected)
        : file_(std::move(file))
        , size_(expected)
    {
#ifdef _WIN32
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(file_.value, &info) ||
            (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
            info.nNumberOfLinks != 1 || GetFileType(file_.value) != FILE_TYPE_DISK ||
            ((std::uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow) != size_)
#else
        struct stat info{};
        if (fstat(file_.value, &info) || !S_ISREG(info.st_mode) || info.st_nlink != 1 ||
            info.st_size < 0 || static_cast<std::uint64_t>(info.st_size) != size_)
#endif
            throw std::runtime_error("Prepared file has changed size or is not a regular file.");
    }
    std::uint64_t size() const override
    {
        return size_;
    }
    std::uint64_t tell() const override
    {
        return position_;
    }
    void seek(std::uint64_t position) override
    {
        if (position > size_)
        {
            throw std::out_of_range("Seek is outside the prepared file.");
        }
        position_ = position;
    }
    std::size_t read(std::span<unsigned char> output) override
    {
        const auto count = static_cast<std::size_t>(
            std::min<std::uint64_t>({output.size(), size_ - position_, 1024 * 1024}));
        if (!count)
        {
            return 0;
        }
        std::size_t done = 0;
        while (done < count)
        {
#ifdef _WIN32
            LARGE_INTEGER offset{};
            offset.QuadPart = static_cast<LONGLONG>(position_ + done);
            DWORD received = 0;
            if (!SetFilePointerEx(file_.value, offset, nullptr, FILE_BEGIN) ||
                !ReadFile(file_.value,
                    output.data() + done,
                    static_cast<DWORD>(count - done),
                    &received,
                    nullptr) ||
                !received)
            {
                throw std::runtime_error("Prepared file is truncated or unreadable.");
            }
#else
            const auto received = pread(file_.value,
                output.data() + done,
                count - done,
                static_cast<off_t>(position_ + done));
            if (received < 0 && errno == EINTR)
            {
                continue;
            }
            if (received <= 0)
            {
                throw std::runtime_error("Prepared file is truncated or unreadable.");
            }
#endif
            done += static_cast<std::size_t>(received);
        }
        position_ += done;
        return done;
    }

  private:
    Handle file_;
    std::uint64_t size_, position_ = 0;
};

#ifdef _WIN32
Handle open_windows(const std::filesystem::path& path, bool directory)
{
    Handle handle(CreateFileW(path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
        nullptr));
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle.value, &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        bool(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != directory)
    {
        throw std::runtime_error("Prepared content contains a link or unexpected file type.");
    }
    return handle;
}
#endif
} // namespace

void require_real_directory(const std::filesystem::path& path)
{
    const auto absolute = std::filesystem::absolute(path).lexically_normal();
    auto current = absolute.root_path();
    for (const auto& part : absolute.relative_path())
    {
        current /= part;
#ifdef _WIN32
        const auto flags = GetFileAttributesW(current.c_str());
        if (flags == INVALID_FILE_ATTRIBUTES || !(flags & FILE_ATTRIBUTE_DIRECTORY) ||
            (flags & FILE_ATTRIBUTE_REPARSE_POINT))
#else
        const auto status = std::filesystem::symlink_status(current);
        if (!std::filesystem::is_directory(status) || std::filesystem::is_symlink(status))
#endif
            throw std::runtime_error("Content directory is missing, linked or not a directory: " +
                                     platform::utf8(current));
    }
}

std::filesystem::path canonical_content_root(const std::filesystem::path& path)
{
    // System ancestors may legitimately be aliases (e.g. macOS /var -> /private/var).
    // The selected root itself and every path subsequently opened below it may not.
#ifdef _WIN32
    const auto flags = GetFileAttributesW(path.c_str());
    if (flags == INVALID_FILE_ATTRIBUTES || !(flags & FILE_ATTRIBUTE_DIRECTORY) ||
        (flags & FILE_ATTRIBUTE_REPARSE_POINT))
#else
    const auto status = std::filesystem::symlink_status(path);
    if (!std::filesystem::is_directory(status) || std::filesystem::is_symlink(status))
#endif
        throw std::runtime_error("Content root is missing, linked or not a directory.");
    auto result = std::filesystem::canonical(path);
    require_real_directory(result);
    return result;
}

std::unique_ptr<SourceFile> open_local_file(
    const std::filesystem::path& root, std::string_view canonical_path, std::uint64_t expected_size)
{
    const auto logical = normalize_game_path(canonical_path);
    if (logical.display != canonical_path ||
        expected_size > std::uint64_t(std::numeric_limits<std::int64_t>::max()))
    {
        throw std::runtime_error("Invalid prepared file record.");
    }
    const auto full =
        std::filesystem::absolute(root).lexically_normal() / platform::utf8_path(logical.display);
    // Pin every parent while opening. POSIX resolves relative to directory
    // descriptors; Windows parent handles deny deletion/rename and reparse points.
    std::vector<Handle> parents;
#ifdef _WIN32
    auto path = full.root_path();
    parents.push_back(open_windows(path, true));
    for (const auto& part : full.relative_path().parent_path())
    {
        path /= part;
        parents.push_back(open_windows(path, true));
    }
    auto file = open_windows(full, false);
#else
#if defined(TARGET_OS_IOS) && TARGET_OS_IOS
    // The iOS sandbox denies opening the folders above the app's container, so the
    // walk starts at the content root, which canonical_content_root already checked.
    parents.emplace_back(::open(std::filesystem::absolute(root).lexically_normal().c_str(),
        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    const auto below_root = platform::utf8_path(logical.display).parent_path();
#else
    parents.emplace_back(::open(full.root_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    const auto below_root = full.relative_path().parent_path();
#endif
    for (const auto& part : below_root)
    {
        parents.emplace_back(openat(
            parents.back().value, part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    }
    Handle file(openat(parents.back().value,
        full.filename().c_str(),
        O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
#endif
    return std::make_unique<LocalFile>(std::move(file), expected_size);
}
} // namespace beachlife::data
