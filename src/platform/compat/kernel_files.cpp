#include "kernel_files.h"
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif
#include "data/game_path.h"
#include "io.h"
#include "native_kernel.h"
#include "platform/path.h"
#include "runtime/retail_files.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <system_error>

namespace beachlife::compat::kernel
{
namespace fs = std::filesystem;
namespace retail = runtime::retail_files;
namespace host = beachlife::compat::native;

namespace
{
struct File
{
    std::FILE* user = nullptr;
    retail::File* retail = nullptr;
    bool writable = false;
    bool readable = true;

    ~File()
    {
        if (user)
        {
            std::fclose(user);
        }
        if (retail)
        {
            retail::close(retail);
        }
    }
};

// exit() flushes open game streams after static destructors have run (the
// game's fatal path calls exit), so the table lives for the whole process.
std::mutex& files_mutex = *new std::mutex;
std::map<int, std::unique_ptr<File>>& files = *new std::map<int, std::unique_ptr<File>>;
int next_file = 0x10000;

std::string fold(std::string text)
{
    for (char& ch : text)
    {
        if (ch >= 'A' && ch <= 'Z')
        {
            ch = static_cast<char>(ch + ('a' - 'A'));
        }
    }
    return text;
}

int install_file(std::unique_ptr<File> file)
{
    if (next_file == INT_MAX)
    {
        errno = EMFILE;
        last_error = ERROR_TOO_MANY_OPEN_FILES;
        return -1;
    }
    const int token = next_file;
    files.emplace(token, std::move(file));
    ++next_file;
    return token;
}

std::unique_ptr<File> open_read(const char* path)
{
    const auto logical = logical_path(path);
    const auto user = user_path(path);
    auto file = std::make_unique<File>();
    std::error_code error;
    const auto status = fs::symlink_status(user, error);
    if (error && error != std::errc::no_such_file_or_directory)
    {
        last_error = file_error(error);
        return nullptr;
    }
    if (fs::exists(status))
    {
        if (!fs::is_regular_file(status))
        {
            last_error = ERROR_ACCESS_DENIED;
            return nullptr;
        }
        file->user = host::open_file(user, "rb");
        if (!file->user)
        {
            last_error = file_error(errno);
            return nullptr;
        }
    }
    else
    {
        file->retail = retail::open(logical.c_str(), "rb");
        if (!file->retail)
        {
            errno = ENOENT;
            last_error = ERROR_FILE_NOT_FOUND;
            return nullptr;
        }
    }
    return file;
}

std::size_t read_bytes(File& file, void* buffer, std::size_t bytes)
{
    if (file.user)
    {
        const auto received = std::fread(buffer, 1, bytes, file.user);
        if (std::ferror(file.user))
        {
            throw std::runtime_error("User file read failed");
        }
        return received;
    }
    return retail::read(buffer, 1, bytes, file.retail);
}

std::optional<uint64_t> length(File& file)
{
    if (file.user)
    {
        const auto position = host::tell(file.user);
        if (position < 0 || host::seek(file.user, 0, SEEK_END) != 0)
        {
            return std::nullopt;
        }
        const auto size = host::tell(file.user);
        if (host::seek(file.user, position, SEEK_SET) != 0 || size < 0)
        {
            return std::nullopt;
        }
        return static_cast<uint64_t>(size);
    }
    retail::Position position, size;
    if (retail::position(file.retail, &position) != 0 ||
        retail::seek(file.retail, 0, retail::SeekOrigin::end) != 0 ||
        retail::position(file.retail, &size) != 0)
    {
        return std::nullopt;
    }
    if (position > INT32_MAX)
    {
        unsupported("OpenFile", "VFS cursor restoration beyond the signed 32-bit seek range");
    }
    if (retail::seek(file.retail, static_cast<int32_t>(position), retail::SeekOrigin::begin) != 0)
    {
        return std::nullopt;
    }
    return size;
}
} // namespace

DWORD file_error(int error)
{
    switch (error)
    {
    case 0:
        return ERROR_SUCCESS;
    case ENOENT:
        return ERROR_FILE_NOT_FOUND;
    case ENOTDIR:
        return ERROR_PATH_NOT_FOUND;
    case EACCES:
    case EPERM:
    case EROFS:
        return ERROR_ACCESS_DENIED;
    case EEXIST:
        return ERROR_ALREADY_EXISTS;
    case ENOTEMPTY:
        return ERROR_DIR_NOT_EMPTY;
    case EMFILE:
    case ENFILE:
        return ERROR_TOO_MANY_OPEN_FILES;
    case ENOMEM:
        return ERROR_NOT_ENOUGH_MEMORY;
    case EBADF:
        return ERROR_INVALID_HANDLE;
    case EINVAL:
        return ERROR_INVALID_PARAMETER;
    default:
        return ERROR_GEN_FAILURE;
    }
}

DWORD file_error(const std::error_code& error)
{
#ifdef _WIN32
    if (error.category() == std::system_category())
    {
        return static_cast<DWORD>(error.value());
    }
#endif
    return file_error(error.value());
}

fs::path user_root()
{
    // Checks and automation use their own profile so they never change the
    // player's options, progress or saves.
    if (const char* override = SDL_getenv("BEACHLIFE_BOOT_USER_DIR"))
    {
        // "~/" names the home folder: on iOS its absolute path changes with each install.
        auto folder = beachlife::platform::utf8_path(override);
        const char* home = SDL_getenv("HOME");
        if (home && std::string_view(override).starts_with("~/"))
        {
            folder = beachlife::platform::utf8_path(home) / (override + 2);
        }
        fs::create_directories(folder);
        return fs::weakly_canonical(folder);
    }
    std::unique_ptr<char, decltype(&SDL_free)> path(
        SDL_GetPrefPath("BeachLife", "BeachLife"), SDL_free);
    if (!path)
    {
        throw std::runtime_error("SDL_GetPrefPath failed");
    }
    return fs::weakly_canonical(beachlife::platform::utf8_path(path.get()));
}

std::string logical_path(const char* path)
{
    if (!path)
    {
        throw std::invalid_argument("Null retail path");
    }
    std::string text(path);
    std::replace(text.begin(), text.end(), '\\', '/');
    if (beachlife::platform::utf8_path(text).is_absolute())
    {
        auto root = beachlife::platform::utf8(user_root());
        std::replace(root.begin(), root.end(), '\\', '/');
        if (fold(text) == fold(root) || fold(text) == fold(root) + '/')
        {
            return ".";
        }
        if (!fold(text).starts_with(fold(root) + '/'))
        {
            unsupported("file path", "absolute paths outside the per-user folder");
        }
        text.erase(0, root.size() + 1);
    }
    if (text == "." || text == "./" || text.empty())
    {
        if (text.empty())
        {
            throw std::invalid_argument("Empty retail path");
        }
        return ".";
    }
    return data::normalize_game_path(text).key;
}

#if defined(TARGET_OS_IOS) && TARGET_OS_IOS
// The Files app shows only Documents, so saves live there and can be copied off the
// device. Earlier builds kept them in the private folder: move those across once.
fs::path saves_root()
{
    const char* home = SDL_getenv("HOME");
    if (!home || SDL_getenv("BEACHLIFE_BOOT_USER_DIR"))
    {
        return user_root();
    }
    const auto documents = beachlife::platform::utf8_path(home) / "Documents";
    std::error_code error;
    const auto old_saves = user_root() / "savegames";
    if (fs::is_directory(old_saves, error) && !fs::exists(documents / "savegames", error))
    {
        fs::create_directories(documents, error);
        fs::rename(old_saves, documents / "savegames", error);
    }
    return documents;
}
#endif

fs::path user_path(const char* path)
{
    const auto logical = logical_path(path);
    auto current = user_root();
    if (logical == ".")
    {
        return current;
    }
#if defined(TARGET_OS_IOS) && TARGET_OS_IOS
    if (logical == "savegames" || logical.starts_with("savegames/"))
    {
        current = saves_root();
    }
#endif
    // Resolve existing components using Windows ASCII case rules. New names
    // use the normalized spelling so subsequent writes cannot fork by case.
    for (const auto& part : beachlife::platform::utf8_path(logical))
    {
        const auto wanted = beachlife::platform::utf8(part);
        std::optional<fs::path> found;
        std::error_code error;
        if (fs::is_directory(current, error))
        {
            for (const auto& entry : fs::directory_iterator(current))
            {
                if (fold(beachlife::platform::utf8(entry.path().filename())) != wanted)
                {
                    continue;
                }
                if (found)
                {
                    unsupported("file path", "ambiguous case-insensitive user filename");
                }
                if (entry.is_symlink())
                {
                    unsupported("file path", "symbolic links in the per-user folder");
                }
                found = entry.path();
            }
        }
        else if (error && error != std::errc::no_such_file_or_directory)
        {
            throw fs::filesystem_error("Cannot inspect user folder", current, error);
        }
        current = found ? *found : current / part;
    }
    return current;
}

std::optional<std::vector<unsigned char>> read_file(const char* path)
{
    auto file = open_read(path);
    if (!file)
    {
        return std::nullopt;
    }
    const auto size = length(*file);
    if (!size || *size > SIZE_MAX)
    {
        throw std::runtime_error("Cannot determine retail file length");
    }
    std::vector<unsigned char> bytes(static_cast<std::size_t>(*size));
    std::size_t done = 0;
    while (done < bytes.size())
    {
        const auto received = read_bytes(
            *file, bytes.data() + done, std::min<std::size_t>(bytes.size() - done, 256 * 1024));
        if (!received)
        {
            throw std::runtime_error("Incomplete retail file read");
        }
        done += received;
    }
    return bytes;
}

bool write_file(const char* path, std::string_view bytes)
{
    const auto destination = user_path(path);
    // INI writes may create their parent folders. CreateDirectory itself
    // retains the Windows single-directory contract.
    fs::create_directories(destination.parent_path());
    auto temporary = destination;
    const int descriptor = host::temporary_file(temporary);
    if (descriptor < 0)
    {
        last_error = file_error(errno);
        return false;
    }
    std::unique_ptr<std::FILE, decltype(&std::fclose)> file(
        host::descriptor_stream(descriptor, "wb"), std::fclose);
    if (!file)
    {
        host::close_descriptor(descriptor);
        fs::remove(temporary);
        last_error = ERROR_WRITE_FAULT;
        return false;
    }
    const bool written = std::fwrite(bytes.data(), 1, bytes.size(), file.get()) == bytes.size() &&
                         std::fflush(file.get()) == 0;
    const bool closed = std::fclose(file.release()) == 0;
    if (!written || !closed || !host::replace_file(temporary, destination))
    {
        fs::remove(temporary);
        last_error = ERROR_WRITE_FAULT;
        return false;
    }
    return true;
}

std::optional<bool> close_file_handle(HANDLE handle)
{
    const auto value = reinterpret_cast<intptr_t>(handle);
    if (value < 0x10000 || value > INT_MAX)
    {
        return std::nullopt;
    }
    std::lock_guard lock(files_mutex);
    const auto found = files.find(static_cast<int>(value));
    if (found == files.end())
    {
        return std::nullopt;
    }
    auto& file = *found->second;
    bool result = true;
    if (file.user)
    {
        result = std::fclose(file.user) == 0;
        file.user = nullptr;
    }
    if (file.retail)
    {
        result = retail::close(file.retail) == 0;
        file.retail = nullptr;
    }
    files.erase(found);
    if (!result)
    {
        last_error = ERROR_GEN_FAILURE;
    }
    return result;
}

std::optional<uint64_t> seek_file(int descriptor, int64_t offset, int origin)
{
    std::lock_guard lock(files_mutex);
    const auto found = files.find(descriptor);
    if (found == files.end())
    {
        errno = EBADF;
        last_error = ERROR_INVALID_HANDLE;
        return std::nullopt;
    }
    if (origin < 0 || origin > 2)
    {
        errno = EINVAL;
        last_error = ERROR_INVALID_PARAMETER;
        return std::nullopt;
    }
    auto& file = *found->second;
    uint64_t position;
    if (file.user)
    {
        if (host::seek(file.user, offset, origin) != 0)
        {
            last_error = file_error(errno);
            return std::nullopt;
        }
        const auto value = host::tell(file.user);
        if (value < 0)
        {
            return std::nullopt;
        }
        position = static_cast<uint64_t>(value);
    }
    else if (offset < INT32_MIN || offset > INT32_MAX)
    {
        unsupported("fseek", "VFS seek offset exceeds the signed 32-bit range");
    }
    else if (retail::seek(file.retail,
                 static_cast<int32_t>(offset),
                 static_cast<retail::SeekOrigin>(origin)) != 0 ||
             retail::position(file.retail, &position) != 0)
    {
        errno = EINVAL;
        last_error = ERROR_READ_FAULT;
        return std::nullopt;
    }
    return position;
}

int mapping_descriptor(HANDLE handle, bool writable, uint64_t& size)
{
    std::lock_guard lock(files_mutex);
    const auto token = reinterpret_cast<intptr_t>(handle);
    const auto found =
        token >= 0x10000 && token <= INT_MAX ? files.find(static_cast<int>(token)) : files.end();
    if (found == files.end())
    {
        last_error = ERROR_INVALID_HANDLE;
        return -1;
    }
    auto& file = *found->second;
    if (!file.readable || (writable && !file.writable))
    {
        last_error = ERROR_ACCESS_DENIED;
        return -1;
    }
    const auto actual = length(file);
    if (!actual)
    {
        last_error = ERROR_READ_FAULT;
        return -1;
    }
    if (!size)
    {
        size = *actual;
    }
    if (!size || size > INT64_MAX)
    {
        last_error = ERROR_INVALID_PARAMETER;
        return -1;
    }
    if (size > *actual && !file.writable)
    {
        last_error = ERROR_ACCESS_DENIED;
        return -1;
    }
    if (file.user)
    {
        if (std::fflush(file.user) != 0 ||
            (size > *actual &&
                host::resize_descriptor(host::file_descriptor(file.user), size) != 0))
        {
            last_error = file_error(errno);
            return -1;
        }
        const int descriptor = host::duplicate_descriptor(host::file_descriptor(file.user));
        if (descriptor < 0)
        {
            last_error = file_error(errno);
        }
        return descriptor;
    }
    std::unique_ptr<std::FILE, decltype(&std::fclose)> backing(std::tmpfile(), std::fclose);
    if (!backing)
    {
        last_error = file_error(errno);
        return -1;
    }
    retail::Position position;
    if (retail::position(file.retail, &position) != 0)
    {
        last_error = ERROR_READ_FAULT;
        return -1;
    }
    if (position > INT32_MAX)
    {
        unsupported("CreateFileMappingA", "VFS cursor exceeds the signed 32-bit seek range");
    }
    if (retail::seek(file.retail, 0, retail::SeekOrigin::begin) != 0)
    {
        last_error = ERROR_READ_FAULT;
        return -1;
    }
    std::array<unsigned char, 256 * 1024> buffer;
    uint64_t done = 0;
    bool good = true;
    while (done < size)
    {
        const auto count = static_cast<std::size_t>(std::min<uint64_t>(buffer.size(), size - done));
        const auto received = read_bytes(file, buffer.data(), count);
        if (!received || std::fwrite(buffer.data(), 1, received, backing.get()) != received)
        {
            good = false;
            break;
        }
        done += received;
    }
    if (retail::seek(file.retail, static_cast<int32_t>(position), retail::SeekOrigin::begin) != 0 ||
        !good || std::fflush(backing.get()) != 0)
    {
        last_error = ERROR_READ_FAULT;
        return -1;
    }
    const int descriptor = host::duplicate_descriptor(host::file_descriptor(backing.get()));
    if (descriptor < 0)
    {
        last_error = file_error(errno);
    }
    return descriptor;
}

#ifdef _WIN32
FILE* stdio_stream(int descriptor)
{
    std::unique_ptr<File> file;
    {
        std::lock_guard lock(files_mutex);
        const auto found = files.find(descriptor);
        if (found == files.end())
        {
            errno = EBADF;
            return nullptr;
        }
        file = std::move(found->second);
        files.erase(found);
    }
    if (file->user)
    {
        return std::exchange(file->user, nullptr);
    }
    std::unique_ptr<FILE, decltype(&std::fclose)> stream(std::tmpfile(), std::fclose);
    if (!stream)
    {
        return nullptr;
    }
    std::array<unsigned char, 256 * 1024> buffer;
    for (;;)
    {
        const auto count = read_bytes(*file, buffer.data(), buffer.size());
        if (!count)
        {
            break;
        }
        if (std::fwrite(buffer.data(), 1, count, stream.get()) != count)
        {
            return nullptr;
        }
    }
    if (host::seek(stream.get(), 0, SEEK_SET) != 0)
    {
        return nullptr;
    }
    // Match fopen("rb"): the snapshot's construction needed write access,
    // but the FILE exposed to retail must reject writes and retain EOF rules.
    const int backing = host::duplicate_descriptor(host::file_descriptor(stream.get()));
    if (backing < 0)
    {
        return nullptr;
    }
    FILE* result = host::descriptor_stream(backing, "rb");
    if (!result)
    {
        host::close_descriptor(backing);
    }
    return result;
}
#endif

} // namespace beachlife::compat::kernel

using namespace beachlife::compat::kernel;

extern "C"
{
    DWORD WINAPI GetCurrentDirectoryA(DWORD size, LPSTR buffer)
    {
        return guard(
            [&]() -> DWORD
            {
                const auto root = beachlife::platform::utf8(user_root());
                if (!size || root.size() + 1 > size)
                {
                    return static_cast<DWORD>(root.size() + 1);
                }
                if (!buffer)
                {
                    last_error = ERROR_INVALID_PARAMETER;
                    return 0;
                }
                std::memcpy(buffer, root.c_str(), root.size() + 1);
                return static_cast<DWORD>(root.size());
            },
            DWORD(0));
    }

    BOOL WINAPI CreateDirectoryA(LPCSTR path, LPSECURITY_ATTRIBUTES security)
    {
        return guard(
            [&]() -> BOOL
            {
                if (security && (security->lpSecurityDescriptor || security->bInheritHandle))
                {
                    beachlife::compat::unsupported(
                        "CreateDirectoryA", "custom security or inheritance");
                }
                std::error_code error;
                if (std::filesystem::create_directory(user_path(path), error))
                {
                    return TRUE;
                }
                last_error = error ? file_error(error) : ERROR_ALREADY_EXISTS;
                return FALSE;
            },
            FALSE);
    }

    BOOL WINAPI DeleteFileA(LPCSTR path)
    {
        return guard(
            [&]() -> BOOL
            {
                const auto user = user_path(path);
                std::error_code error;
                if (std::filesystem::is_directory(user, error))
                {
                    last_error = ERROR_ACCESS_DENIED;
                    return FALSE;
                }
                error.clear();
                if (std::filesystem::remove(user, error))
                {
                    return TRUE;
                }
                if (error)
                {
                    last_error = file_error(error);
                }
                else
                {
                    auto retail = open_read(path);
                    last_error = retail ? ERROR_ACCESS_DENIED : ERROR_FILE_NOT_FOUND;
                }
                return FALSE;
            },
            FALSE);
    }

    DWORD WINAPI GetFileAttributesA(LPCSTR path)
    {
        return guard(
            [&]() -> DWORD
            {
                std::error_code error;
                const auto status = std::filesystem::status(user_path(path), error);
                if (std::filesystem::exists(status))
                {
                    if (std::filesystem::is_directory(status))
                    {
                        return FILE_ATTRIBUTE_DIRECTORY;
                    }
                    if (!std::filesystem::is_regular_file(status))
                    {
                        beachlife::compat::unsupported(
                            "GetFileAttributesA", "nonregular user file");
                    }
                    return FILE_ATTRIBUTE_ARCHIVE |
                           ((status.permissions() & std::filesystem::perms::owner_write) ==
                                       std::filesystem::perms::none
                                   ? FILE_ATTRIBUTE_READONLY
                                   : 0);
                }
                if (error && error != std::errc::no_such_file_or_directory)
                {
                    last_error = file_error(error);
                    return INVALID_FILE_ATTRIBUTES;
                }
                const auto logical = logical_path(path);
                if (logical == "photos" || logical.starts_with("photos/") ||
                    logical == "savegames" || logical.starts_with("savegames/"))
                {
                    last_error = ERROR_FILE_NOT_FOUND;
                    return INVALID_FILE_ATTRIBUTES;
                }
                if (const auto attributes = manifest_attributes(logical))
                {
                    if (*attributes == INVALID_FILE_ATTRIBUTES)
                    {
                        last_error = ERROR_FILE_NOT_FOUND;
                    }
                    return *attributes;
                }
                if (open_read(path))
                {
                    return FILE_ATTRIBUTE_READONLY;
                }
                beachlife::compat::unsupported("GetFileAttributesA",
                    "bind compat::DirectorySession to the prepared VFS manifest");
            },
            INVALID_FILE_ATTRIBUTES);
    }

    HFILE WINAPI OpenFile(LPCSTR path, LPOFSTRUCT info, UINT style)
    {
        return guard(
            [&]() -> HFILE
            {
                if (!info)
                {
                    last_error = ERROR_INVALID_PARAMETER;
                    return HFILE_ERROR;
                }
                if (style & ~(OF_CREATE | OF_EXIST | OF_DELETE | 0x0043u))
                {
                    beachlife::compat::unsupported(
                        "OpenFile", "legacy parse, reopen or prompt flags");
                }
                std::memset(info, 0, sizeof(*info));
                info->cBytes = sizeof(*info);
                const auto logical = logical_path(path);
                if (logical.size() >= sizeof(info->szPathName))
                {
                    last_error = ERROR_INVALID_NAME;
                    info->nErrCode = static_cast<WORD>(last_error);
                    return HFILE_ERROR;
                }
                std::memcpy(info->szPathName, logical.c_str(), logical.size() + 1);
                info->fFixedDisk = TRUE;
                if (style & OF_DELETE)
                {
                    if (DeleteFileA(path))
                    {
                        return 1;
                    }
                    info->nErrCode = static_cast<WORD>(last_error);
                    return HFILE_ERROR;
                }
                if ((style & 0x0070) != 0 && (style & 0x0070) != OF_SHARE_DENY_NONE)
                {
                    beachlife::compat::unsupported("OpenFile", "exclusive or deny sharing modes");
                }
                const auto access = style & 3;
                int flags = access == OF_READ    ? _O_RDONLY
                            : access == OF_WRITE ? _O_WRONLY
                                                 : _O_RDWR;
                flags |= _O_BINARY;
                if (style & OF_CREATE)
                {
                    flags |= _O_CREAT | _O_TRUNC;
                }
                const int result = _open(path, flags);
                if (result < 0)
                {
                    info->nErrCode = static_cast<WORD>(last_error);
                }
                else if (style & OF_EXIST)
                {
                    _close(result);
                }
                return result;
            },
            HFILE_ERROR);
    }

    int _open(const char* path, int flags, ...)
    {
        return guard(
            [&]() -> int
            {
                if (flags &
                    ~(_O_WRONLY | _O_RDWR | _O_APPEND | _O_CREAT | _O_TRUNC | _O_EXCL | _O_BINARY))
                {
                    beachlife::compat::unsupported("_open", "text mode or descriptor flags");
                }
                std::lock_guard lock(files_mutex);
                if ((flags & 3) == _O_RDONLY && !(flags & (_O_CREAT | _O_TRUNC | _O_APPEND)))
                {
                    auto file = open_read(path);
                    return file ? install_file(std::move(file)) : -1;
                }
                if ((flags & 3) == 3)
                {
                    errno = EINVAL;
                    last_error = ERROR_INVALID_PARAMETER;
                    return -1;
                }
                auto file = std::make_unique<File>();
                file->writable = (flags & 3) != _O_RDONLY;
                file->readable = (flags & 3) != _O_WRONLY;
                int host_flags = file->readable ? (file->writable ? O_RDWR : O_RDONLY) : O_WRONLY;
                if (flags & _O_CREAT)
                {
                    host_flags |= O_CREAT;
                }
                if (flags & _O_TRUNC)
                {
                    host_flags |= O_TRUNC;
                }
                if (flags & _O_EXCL)
                {
                    host_flags |= O_EXCL;
                }
                if (flags & _O_APPEND)
                {
                    host_flags |= O_APPEND;
                }
                const int descriptor = host::open_descriptor(user_path(path), host_flags);
                if (descriptor < 0)
                {
                    last_error = file_error(errno);
                    return -1;
                }
                file->user = host::descriptor_stream(
                    descriptor, file->readable ? (file->writable ? "r+b" : "rb") : "wb");
                if (!file->user)
                {
                    host::close_descriptor(descriptor);
                    last_error = file_error(errno);
                    return -1;
                }
                if (std::setvbuf(file->user, nullptr, _IONBF, 0) != 0)
                {
                    last_error = ERROR_GEN_FAILURE;
                    return -1;
                }
                return install_file(std::move(file));
            },
            -1);
    }

    int _read(int descriptor, void* buffer, unsigned int bytes)
    {
        return guard(
            [&]() -> int
            {
                std::lock_guard lock(files_mutex);
                const auto found = files.find(descriptor);
                if (found == files.end() || !found->second->readable)
                {
                    errno = EBADF;
                    last_error = ERROR_INVALID_HANDLE;
                    return -1;
                }
                if (bytes > INT_MAX || (bytes && !buffer))
                {
                    errno = EINVAL;
                    last_error = ERROR_INVALID_PARAMETER;
                    return -1;
                }
                return static_cast<int>(read_bytes(*found->second, buffer, bytes));
            },
            -1);
    }

    int _write(int descriptor, const void* buffer, unsigned int bytes)
    {
        return guard(
            [&]() -> int
            {
                std::lock_guard lock(files_mutex);
                const auto found = files.find(descriptor);
                if (found == files.end() || !found->second->writable)
                {
                    errno = EBADF;
                    last_error = ERROR_INVALID_HANDLE;
                    return -1;
                }
                if (bytes > INT_MAX || (bytes && !buffer))
                {
                    errno = EINVAL;
                    last_error = ERROR_INVALID_PARAMETER;
                    return -1;
                }
                const auto written = std::fwrite(buffer, 1, bytes, found->second->user);
                if (std::ferror(found->second->user))
                {
                    last_error = ERROR_WRITE_FAULT;
                    return -1;
                }
                return static_cast<int>(written);
            },
            -1);
    }

    int32_t _lseek(int descriptor, int32_t offset, int origin)
    {
        return guard(
            [&]() -> int32_t
            {
                const auto position = seek_file(descriptor, offset, origin);
                if (!position)
                {
                    return -1;
                }
                if (*position > INT32_MAX)
                {
                    errno = EOVERFLOW;
                    last_error = ERROR_INVALID_PARAMETER;
                    return -1;
                }
                return static_cast<int32_t>(*position);
            },
            int32_t(-1));
    }

    int _close(int descriptor)
    {
        return guard(
            [&]()
            {
                const auto result =
                    close_file_handle(reinterpret_cast<HANDLE>(static_cast<intptr_t>(descriptor)));
                if (!result)
                {
                    errno = EBADF;
                    last_error = ERROR_INVALID_HANDLE;
                    return -1;
                }
                return *result ? 0 : -1;
            },
            -1);
    }
}

// For the game's autosave: replaces one save only after its replacement is complete.
extern "C" int beachlife_compat_replace_user_file(const char* from, const char* to)
{
    return host::replace_file(user_path(from), user_path(to)) ? 1 : 0;
}
