#include "drc/fileio.hpp"

#include <algorithm>

#include "drc/model.hpp"
#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <filesystem>
#include <system_error>
#endif

namespace drc::fileio {
namespace {

constexpr std::uint64_t kMaxPathChars = 30000;

[[nodiscard]] bool is_separator(char c) noexcept {
    return c == '/' || c == '\\';
}

[[nodiscard]] std::string lower_ascii(std::string_view value) {
    std::string out{value};
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

// Windows reserves these device names with or without an extension, in any
// directory. Creating them silently misbehaves, so they are rejected as names.
[[nodiscard]] bool is_reserved_device_name(std::string_view name) {
    const std::size_t dot = name.find('.');
    const std::string stem = lower_ascii(dot == std::string_view::npos ? name : name.substr(0, dot));
    if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul") {
        return true;
    }
    if (stem.size() == 4 && (stem.compare(0, 3, "com") == 0 || stem.compare(0, 3, "lpt") == 0)) {
        return stem[3] >= '1' && stem[3] <= '9';
    }
    return false;
}

[[nodiscard]] bool has_control_characters(std::string_view value) {
    for (const char raw : value) {
        const auto byte = static_cast<unsigned char>(raw);
        if (byte < 0x20u || byte == 0x7Fu) {
            return true;
        }
    }
    return false;
}

#if defined(_WIN32)
[[nodiscard]] Result<std::wstring> to_wide(const std::string& utf8_path,
                                          bool allow_long_prefix,
                                          bool force_prefix = false) {
    if (utf8_path.empty()) {
        return Status{ErrorCode::Invalid, "empty path"};
    }
    if (utf8_path.size() > kMaxPathChars) {
        return Status{ErrorCode::OutOfRange, "path exceeds the supported length"};
    }
    std::wstring input;
    input.resize(utf8_path.size());
    const int converted = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8_path.data(),
                                              static_cast<int>(utf8_path.size()), input.data(),
                                              static_cast<int>(input.size()));
    if (converted <= 0) {
        return Status{ErrorCode::Invalid, "path is not valid UTF-8"};
    }
    input.resize(static_cast<std::size_t>(converted));
    if (allow_long_prefix && (force_prefix || input.size() >= 248)) {
        const bool absolute = (input.size() >= 2 && input[1] == L':') ||
                              (input.size() >= 2 && input[0] == L'\\' && input[1] == L'\\');
        if (absolute) {
            for (wchar_t& c : input) {
                if (c == L'/') {
                    c = L'\\';
                }
            }
            input.insert(0, L"\\\\?\\");
        }
    }
    return input;
}

[[nodiscard]] std::string last_error_message(const char* what) {
    const DWORD code = GetLastError();
    std::string message{what};
    message.append(" failed with Windows error ");
    message.append(std::to_string(static_cast<unsigned long>(code)));
    return message;
}

[[nodiscard]] Result<std::wstring> to_wide_checked(const std::string& utf8_path) {
    return to_wide(utf8_path, true);
}
#else
[[nodiscard]] Result<std::string> to_native(const std::string& utf8_path) {
    if (utf8_path.empty()) {
        return Status{ErrorCode::Invalid, "empty path"};
    }
    if (utf8_path.size() > kMaxPathChars) {
        return Status{ErrorCode::OutOfRange, "path exceeds the supported length"};
    }
    return utf8_path;
}

[[nodiscard]] std::string last_error_message(const char* what) {
    std::string message{what};
    message.append(" failed with errno ");
    message.append(std::to_string(errno));
    return message;
}
#endif

#if !defined(_WIN32)
// POSIX file descriptors are small integers and zero is a valid descriptor, so
// the stored handle is the descriptor plus one. Without this, a process whose
// standard input is closed would treat descriptor 0 as "not open".
[[nodiscard]] inline void* pack_fd(int fd) {
    return reinterpret_cast<void*>(static_cast<std::intptr_t>(fd) + 1);
}

[[nodiscard]] inline int unpack_fd(const void* handle) {
    return static_cast<int>(reinterpret_cast<std::intptr_t>(handle) - 1);
}
#endif

}  // namespace

File::File(File&& other) noexcept : handle_(other.handle_), path_(std::move(other.path_)) {
    other.handle_ = nullptr;
}

File& File::operator=(File&& other) noexcept {
    if (this != &other) {
        (void)close();
        handle_ = other.handle_;
        path_ = std::move(other.path_);
        other.handle_ = nullptr;
    }
    return *this;
}

File::~File() {
    (void)close();
}

bool File::valid() const noexcept {
    return handle_ != nullptr;
}

#if defined(_WIN32)

Result<File> File::open(const std::string& path, const char* mode) {
    Result<std::wstring> wide = to_wide_checked(path);
    if (!wide.ok()) {
        return wide.status();
    }
    DWORD access = 0;
    DWORD creation = 0;
    // A reader must be able to inspect a file that a live coordinator holds
    // open for writing, so a reader's share mode permits the writer's access.
    // Writers keep sharing to readers only, so a second writer still fails.
    DWORD sharing = FILE_SHARE_READ;
    const std::string_view m{mode};
    if (m == "rb") {
        access = GENERIC_READ;
        creation = OPEN_EXISTING;
        sharing = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    } else if (m == "wb") {
        access = GENERIC_WRITE;
        creation = CREATE_ALWAYS;
    } else if (m == "ab") {
        access = GENERIC_WRITE | GENERIC_READ;
        creation = OPEN_ALWAYS;
    } else if (m == "r+b") {
        access = GENERIC_WRITE | GENERIC_READ;
        creation = OPEN_EXISTING;
        sharing = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    } else {
        return Status{ErrorCode::Invalid, "unsupported file mode"};
    }
    const HANDLE handle = CreateFileW(wide.value().c_str(), access, sharing, nullptr,
                                      creation, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        std::string message = last_error_message("CreateFileW");
        message.append(" for '");
        message.append(path);
        message.append("'");
        return Status{ErrorCode::Io, std::move(message)};
    }
    File file;
    file.handle_ = handle;
    file.path_ = path;
    if (creation == OPEN_ALWAYS) {
        LARGE_INTEGER zero{};
        zero.QuadPart = 0;
        (void)SetFilePointerEx(handle, zero, nullptr, FILE_END);
    }
    return file;
}

Result<File> File::open_exclusive_new(const std::string& path) {
    Result<std::wstring> wide = to_wide_checked(path);
    if (!wide.ok()) {
        return wide.status();
    }
    const HANDLE handle = CreateFileW(wide.value().c_str(), GENERIC_WRITE | GENERIC_READ,
                                      FILE_SHARE_READ, nullptr, CREATE_NEW,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS) {
            return Status{ErrorCode::Duplicate, "file already exists: " + path};
        }
        std::string message = last_error_message("CreateFileW(CREATE_NEW)");
        message.append(" for '");
        message.append(path);
        message.append("'");
        return Status{ErrorCode::Io, std::move(message)};
    }
    File file;
    file.handle_ = handle;
    file.path_ = path;
    return file;
}

Status File::write(std::span<const std::uint8_t> bytes) {
    if (handle_ == nullptr) {
        return Status{ErrorCode::Invalid, "file is not open"};
    }
    std::size_t written = 0;
    while (written < bytes.size()) {
        const std::size_t chunk = std::min<std::size_t>(bytes.size() - written, 1u << 20);
        DWORD done = 0;
        if (WriteFile(handle_, bytes.data() + written, static_cast<DWORD>(chunk), &done, nullptr) ==
            0) {
            return Status{ErrorCode::Io, last_error_message("WriteFile")};
        }
        if (done == 0) {
            return Status{ErrorCode::Io, "WriteFile wrote zero bytes"};
        }
        written += done;
    }
    return ok_status();
}

Status File::write_text(std::string_view text) {
    return write(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

Result<std::size_t> File::read_some(std::span<std::uint8_t> buffer) {
    if (handle_ == nullptr) {
        return Status{ErrorCode::Invalid, "file is not open"};
    }
    if (buffer.empty()) {
        return std::size_t{0};
    }
    DWORD done = 0;
    if (ReadFile(handle_, buffer.data(), static_cast<DWORD>(buffer.size()), &done, nullptr) == 0) {
        return Status{ErrorCode::Io, last_error_message("ReadFile")};
    }
    return static_cast<std::size_t>(done);
}

Status File::flush_data() {
    if (handle_ == nullptr) {
        return Status{ErrorCode::Invalid, "file is not open"};
    }
    if (FlushFileBuffers(handle_) == 0) {
        return Status{ErrorCode::Io, last_error_message("FlushFileBuffers")};
    }
    return ok_status();
}

Status File::seek_start() {
    if (handle_ == nullptr) {
        return Status{ErrorCode::Invalid, "file is not open"};
    }
    LARGE_INTEGER zero{};
    zero.QuadPart = 0;
    if (SetFilePointerEx(handle_, zero, nullptr, FILE_BEGIN) == 0) {
        return Status{ErrorCode::Io, last_error_message("SetFilePointerEx")};
    }
    return ok_status();
}

Status File::truncate(std::uint64_t size) {
    if (handle_ == nullptr) {
        return Status{ErrorCode::Invalid, "file is not open"};
    }
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(size);
    if (SetFilePointerEx(handle_, position, nullptr, FILE_BEGIN) == 0) {
        return Status{ErrorCode::Io, last_error_message("SetFilePointerEx(truncate)")};
    }
    if (SetEndOfFile(handle_) == 0) {
        return Status{ErrorCode::Io, last_error_message("SetEndOfFile")};
    }
    return ok_status();
}

Result<std::uint64_t> File::size() const {
    if (handle_ == nullptr) {
        return Status{ErrorCode::Invalid, "file is not open"};
    }
    LARGE_INTEGER value{};
    if (GetFileSizeEx(handle_, &value) == 0) {
        return Status{ErrorCode::Io, last_error_message("GetFileSizeEx")};
    }
    return static_cast<std::uint64_t>(value.QuadPart);
}

Status File::close() {
    if (handle_ == nullptr) {
        return ok_status();
    }
    const HANDLE handle = handle_;
    handle_ = nullptr;
    if (CloseHandle(handle) == 0) {
        return Status{ErrorCode::Io, last_error_message("CloseHandle")};
    }
    return ok_status();
}

#else  // POSIX

Result<File> File::open(const std::string& path, const char* mode) {
    Result<std::string> native = to_native(path);
    if (!native.ok()) {
        return native.status();
    }
    int flags = 0;
    const std::string_view m{mode};
    if (m == "rb") {
        flags = O_RDONLY;
    } else if (m == "wb") {
        flags = O_WRONLY | O_CREAT | O_TRUNC;
    } else if (m == "ab") {
        flags = O_RDWR | O_CREAT;
    } else if (m == "r+b") {
        flags = O_RDWR;
    } else {
        return Status{ErrorCode::Invalid, "unsupported file mode"};
    }
    const int fd = ::open(native.value().c_str(), flags, 0644);
    if (fd < 0) {
        std::string message = last_error_message("open");
        message.append(" for '");
        message.append(path);
        message.append("'");
        return Status{ErrorCode::Io, std::move(message)};
    }
    if (m == "ab") {
        (void)::lseek(fd, 0, SEEK_END);
    }
    File file;
    file.handle_ = pack_fd(fd);
    file.path_ = path;
    return file;
}

Result<File> File::open_exclusive_new(const std::string& path) {
    Result<std::string> native = to_native(path);
    if (!native.ok()) {
        return native.status();
    }
    const int fd = ::open(native.value().c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        if (errno == EEXIST) {
            return Status{ErrorCode::Duplicate, "file already exists: " + path};
        }
        std::string message = last_error_message("open(O_EXCL)");
        message.append(" for '");
        message.append(path);
        message.append("'");
        return Status{ErrorCode::Io, std::move(message)};
    }
    File file;
    file.handle_ = pack_fd(fd);
    file.path_ = path;
    return file;
}

Status File::write(std::span<const std::uint8_t> bytes) {
    if (handle_ == nullptr) {
        return Status{ErrorCode::Invalid, "file is not open"};
    }
    const int fd = unpack_fd(handle_);
    std::size_t written = 0;
    while (written < bytes.size()) {
        const ssize_t done = ::write(fd, bytes.data() + written, bytes.size() - written);
        if (done < 0) {
            if (errno == EINTR) {
                continue;
            }
            return Status{ErrorCode::Io, last_error_message("write")};
        }
        written += static_cast<std::size_t>(done);
    }
    return ok_status();
}

Status File::write_text(std::string_view text) {
    return write(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

Result<std::size_t> File::read_some(std::span<std::uint8_t> buffer) {
    if (handle_ == nullptr) {
        return Status{ErrorCode::Invalid, "file is not open"};
    }
    if (buffer.empty()) {
        return std::size_t{0};
    }
    const int fd = unpack_fd(handle_);
    for (;;) {
        const ssize_t done = ::read(fd, buffer.data(), buffer.size());
        if (done < 0) {
            if (errno == EINTR) {
                continue;
            }
            return Status{ErrorCode::Io, last_error_message("read")};
        }
        return static_cast<std::size_t>(done);
    }
}

Status File::flush_data() {
    if (handle_ == nullptr) {
        return Status{ErrorCode::Invalid, "file is not open"};
    }
    const int fd = unpack_fd(handle_);
    if (::fsync(fd) != 0) {
        return Status{ErrorCode::Io, last_error_message("fsync")};
    }
    return ok_status();
}

Status File::seek_start() {
    if (handle_ == nullptr) {
        return Status{ErrorCode::Invalid, "file is not open"};
    }
    const int fd = unpack_fd(handle_);
    if (::lseek(fd, 0, SEEK_SET) < 0) {
        return Status{ErrorCode::Io, last_error_message("lseek")};
    }
    return ok_status();
}

Status File::truncate(std::uint64_t size) {
    if (handle_ == nullptr) {
        return Status{ErrorCode::Invalid, "file is not open"};
    }
    const int fd = unpack_fd(handle_);
    if (::ftruncate(fd, static_cast<off_t>(size)) != 0) {
        return Status{ErrorCode::Io, last_error_message("ftruncate")};
    }
    return ok_status();
}

Result<std::uint64_t> File::size() const {
    if (handle_ == nullptr) {
        return Status{ErrorCode::Invalid, "file is not open"};
    }
    const int fd = unpack_fd(handle_);
    struct stat info {};
    if (::fstat(fd, &info) != 0) {
        return Status{ErrorCode::Io, last_error_message("fstat")};
    }
    return static_cast<std::uint64_t>(info.st_size);
}

Status File::close() {
    if (handle_ == nullptr) {
        return ok_status();
    }
    const int fd = unpack_fd(handle_);
    handle_ = nullptr;
    if (::close(fd) != 0) {
        return Status{ErrorCode::Io, last_error_message("close")};
    }
    return ok_status();
}

#endif

Result<std::vector<std::uint8_t>> File::read_all(std::uint64_t max_bytes) {
    Result<std::uint64_t> total = size();
    if (!total.ok()) {
        return total.status();
    }
    if (total.value() > max_bytes) {
        return Status{ErrorCode::Exhausted, "file exceeds the accepted maximum size"};
    }
    const Status rewound = seek_start();
    if (!rewound.ok()) {
        return rewound;
    }
    std::vector<std::uint8_t> out;
    out.resize(static_cast<std::size_t>(total.value()));
    std::size_t offset = 0;
    while (offset < out.size()) {
        Result<std::size_t> done =
            read_some(std::span<std::uint8_t>(out.data() + offset, out.size() - offset));
        if (!done.ok()) {
            return done.status();
        }
        if (done.value() == 0) {
            break;
        }
        offset += done.value();
    }
    out.resize(offset);
    return out;
}

#if defined(_WIN32)
namespace {

constexpr std::uint32_t kMaxDirectoryDepth = 128;

[[nodiscard]] Status windows_error(const char* call, DWORD code, const std::string& path) {
    std::string message{call};
    message.append(" failed with Windows error ");
    message.append(std::to_string(static_cast<unsigned long>(code)));
    message.append(" for '");
    message.append(path);
    message.append("'");
    return Status{ErrorCode::Io, std::move(message)};
}

[[nodiscard]] Status create_directory_error(DWORD code, const std::string& path) {
    std::string message = "CreateDirectoryW failed with Windows error ";
    message.append(std::to_string(static_cast<unsigned long>(code)));
    message.append(" for directory '");
    message.append(path);
    message.append("'");
    return Status{ErrorCode::Io, std::move(message)};
}

[[nodiscard]] bool is_existing_directory(const std::wstring& wide) {
    const DWORD attributes = GetFileAttributesW(wide.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// Creates one directory and, when its parent is missing, its parent first.
[[nodiscard]] Status create_directory_chain(const std::wstring& wide,
                                            const std::string& original,
                                            std::uint32_t depth) {
    if (depth > kMaxDirectoryDepth) {
        return Status{ErrorCode::OutOfRange, "directory nesting exceeds the supported depth"};
    }
    if (CreateDirectoryW(wide.c_str(), nullptr) != 0) {
        return ok_status();
    }
    const DWORD code = GetLastError();
    if (code == ERROR_ALREADY_EXISTS || code == ERROR_ACCESS_DENIED) {
        // Access denied here means "this component already exists" (a drive
        // root, for example) far more often than "this component is forbidden".
        if (is_existing_directory(wide)) {
            return ok_status();
        }
        if (code == ERROR_ALREADY_EXISTS) {
            return ok_status();
        }
    }
    if (code != ERROR_PATH_NOT_FOUND) {
        return create_directory_error(code, original);
    }
    const std::size_t separator = wide.find_last_of(L"\\/");
    if (separator == std::wstring::npos) {
        return create_directory_error(code, original);
    }
    std::wstring parent = wide.substr(0, separator);
    if (parent.size() < 3 || parent.back() == L':') {
        // Nothing above this is creatable: the caller named a path under a root
        // that does not exist.
        return create_directory_error(code, original);
    }
    const Status created = create_directory_chain(parent, original, depth + 1);
    if (!created.ok()) {
        return created;
    }
    if (CreateDirectoryW(wide.c_str(), nullptr) != 0) {
        return ok_status();
    }
    const DWORD retry = GetLastError();
    if (retry == ERROR_ALREADY_EXISTS || is_existing_directory(wide)) {
        return ok_status();
    }
    return create_directory_error(retry, original);
}

}  // namespace
#endif

Status ensure_directory(const std::string& path) {
    if (path.empty()) {
        return Status{ErrorCode::Invalid, "empty directory path"};
    }
#if defined(_WIN32)
    Result<std::wstring> wide = to_wide_checked(path);
    if (!wide.ok()) {
        return wide.status();
    }
    // CreateDirectoryW does not create intermediate components, and walking the
    // path as text is fragile once the extended-length prefix is involved
    // ("\\\\?\\C:" and "\\\\?" are not directories). So the parent is created first,
    // recursively, with an explicit depth bound and a check for the cases where
    // the component already exists — including a drive root, which reports
    // access denied rather than "already exists".
    return create_directory_chain(wide.value(), path, 0);
#else
    std::string partial;
    std::size_t index = 0;
    while (index <= path.size()) {
        partial.push_back(index < path.size() ? path[index] : '/');
        const bool at_end = index >= path.size();
        if (at_end || path[index] == '/') {
            std::string trimmed = partial;
            while (trimmed.size() > 1 && trimmed.back() == '/') {
                trimmed.pop_back();
            }
            if (!trimmed.empty() && trimmed != "/") {
                if (::mkdir(trimmed.c_str(), 0755) != 0 && errno != EEXIST) {
                    return Status{ErrorCode::Io, last_error_message("mkdir")};
                }
            }
        }
        ++index;
    }
    return ok_status();
#endif
}

Status sync_directory(const std::string& path) {
#if defined(_WIN32)
    // NTFS orders directory metadata behind the write-through rename used by
    // atomic_replace, and has no supported directory flush. Nothing to do here;
    // the durability boundary is documented as the rename, not this call.
    (void)path;
    return ok_status();
#else
    Result<std::string> native = to_native(path);
    if (!native.ok()) {
        return native.status();
    }
    const int fd = ::open(native.value().c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        return Status{ErrorCode::Io, last_error_message("open(directory)")};
    }
    const int result = ::fsync(fd);
    const int saved = errno;
    (void)::close(fd);
    if (result != 0) {
        errno = saved;
        return Status{ErrorCode::Io, last_error_message("fsync(directory)")};
    }
    return ok_status();
#endif
}

Result<bool> exists(const std::string& path) {
#if defined(_WIN32)
    Result<std::wstring> wide = to_wide_checked(path);
    if (!wide.ok()) {
        return wide.status();
    }
    const DWORD attributes = GetFileAttributesW(wide.value().c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            return false;
        }
        return Status{ErrorCode::Io, last_error_message("GetFileAttributesW")};
    }
    return true;
#else
    Result<std::string> native = to_native(path);
    if (!native.ok()) {
        return native.status();
    }
    struct stat info {};
    if (::stat(native.value().c_str(), &info) != 0) {
        if (errno == ENOENT || errno == ENOTDIR) {
            return false;
        }
        return Status{ErrorCode::Io, last_error_message("stat")};
    }
    return true;
#endif
}

Result<std::uint64_t> file_size(const std::string& path) {
    Result<File> file = File::open(path, "rb");
    if (!file.ok()) {
        return file.status();
    }
    return file.value().size();
}

Result<std::vector<std::uint8_t>> read_file(const std::string& path, std::uint64_t max_bytes) {
    Result<File> file = File::open(path, "rb");
    if (!file.ok()) {
        return file.status();
    }
    return file.value().read_all(max_bytes);
}

Status write_file_durable(const std::string& path, std::span<const std::uint8_t> bytes) {
    Result<File> file = File::open(path, "wb");
    if (!file.ok()) {
        return file.status();
    }
    const Status written = file.value().write(bytes);
    if (!written.ok()) {
        return written;
    }
    const Status flushed = file.value().flush_data();
    if (!flushed.ok()) {
        return flushed;
    }
    return file.value().close();
}

Status atomic_replace(const std::string& temp_path, const std::string& final_path) {
#if defined(_WIN32)
    Result<std::wstring> from = to_wide_checked(temp_path);
    if (!from.ok()) {
        return from.status();
    }
    Result<std::wstring> to = to_wide_checked(final_path);
    if (!to.ok()) {
        return to.status();
    }
    if (MoveFileExW(from.value().c_str(), to.value().c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        return Status{ErrorCode::Io, last_error_message("MoveFileExW")};
    }
    return ok_status();
#else
    Result<std::string> from = to_native(temp_path);
    if (!from.ok()) {
        return from.status();
    }
    Result<std::string> to = to_native(final_path);
    if (!to.ok()) {
        return to.status();
    }
    if (::rename(from.value().c_str(), to.value().c_str()) != 0) {
        return Status{ErrorCode::Io, last_error_message("rename")};
    }
    return ok_status();
#endif
}

#if defined(_WIN32)
namespace {

[[nodiscard]] Status remove_tree_wide(const std::wstring& directory) {
    std::wstring pattern = directory;
    if (!pattern.empty() && pattern.back() != L'\\' && pattern.back() != L'/') {
        pattern.push_back(L'\\');
    }
    pattern.push_back(L'*');

    WIN32_FIND_DATAW found{};
    const HANDLE handle = FindFirstFileW(pattern.c_str(), &found);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND) {
            return windows_error("FindFirstFileW", code, "a directory being removed");
        }
    } else {
        Status first = ok_status();
        do {
            const std::wstring name = found.cFileName;
            if (name == L"." || name == L"..") {
                continue;
            }
            std::wstring child = directory;
            child.push_back(L'\\');
            child.append(name);
            if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                const Status status = remove_tree_wide(child);
                if (!status.ok() && first.ok()) {
                    first = status;
                }
                continue;
            }
            (void)SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL);
            if (DeleteFileW(child.c_str()) == 0 && first.ok()) {
                const DWORD code = GetLastError();
                if (code != ERROR_FILE_NOT_FOUND) {
                    first = windows_error("DeleteFileW", code, "a file being removed");
                }
            }
        } while (FindNextFileW(handle, &found) != 0);
        (void)FindClose(handle);
        if (!first.ok()) {
            return first;
        }
    }
    if (RemoveDirectoryW(directory.c_str()) == 0) {
        const DWORD code = GetLastError();
        if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND) {
            return windows_error("RemoveDirectoryW", code, "a directory being removed");
        }
    }
    return ok_status();
}

}  // namespace
#endif

Status remove_tree(const std::string& path) {
    if (path.empty()) {
        return Status{ErrorCode::Invalid, "empty path"};
    }
#if defined(_WIN32)
    // The extended-length form is applied unconditionally for absolute paths:
    // a tree walk crosses MAX_PATH on the way down, and the prefix lifts the
    // limit for every level, not only the first.
    Result<std::wstring> wide = to_wide(path, true, true);
    if (!wide.ok()) {
        return wide.status();
    }
    return remove_tree_wide(wide.value());
#else
    std::error_code error;
    std::filesystem::remove_all(path, error);
    if (error) {
        std::string message = "remove_all failed: ";
        message.append(error.message());
        message.append(" for '");
        message.append(path);
        message.append("'");
        return Status{ErrorCode::Io, std::move(message)};
    }
    return ok_status();
#endif
}

Status remove_file(const std::string& path) {
#if defined(_WIN32)
    Result<std::wstring> wide = to_wide_checked(path);
    if (!wide.ok()) {
        return wide.status();
    }
    if (DeleteFileW(wide.value().c_str()) == 0) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND) {
            return ok_status();
        }
        return Status{ErrorCode::Io, last_error_message("DeleteFileW")};
    }
    return ok_status();
#else
    Result<std::string> native = to_native(path);
    if (!native.ok()) {
        return native.status();
    }
    if (::unlink(native.value().c_str()) != 0 && errno != ENOENT) {
        return Status{ErrorCode::Io, last_error_message("unlink")};
    }
    return ok_status();
#endif
}

Status validate_leaf_name(std::string_view name) {
    if (name.empty()) {
        return Status{ErrorCode::Invalid, "name is empty"};
    }
    if (name.size() > 128) {
        return Status{ErrorCode::OutOfRange, "name exceeds the supported length"};
    }
    if (name == "." || name == "..") {
        return Status{ErrorCode::Invalid, "relative traversal name is not accepted"};
    }
    if (has_control_characters(name)) {
        return Status{ErrorCode::Invalid, "name contains a control character"};
    }
    if (!drc::is_valid_utf8(name)) {
        return Status{ErrorCode::Invalid, "name is not well-formed UTF-8"};
    }
    for (const char c : name) {
        if (is_separator(c)) {
            return Status{ErrorCode::Invalid, "name contains a path separator"};
        }
        switch (c) {
            case ':':
            case '*':
            case '?':
            case '"':
            case '<':
            case '>':
            case '|':
            case '\\':
                return Status{ErrorCode::Invalid, "name contains a reserved character"};
            default:
                break;
        }
    }
    if (name.back() == '.' || name.back() == ' ') {
        return Status{ErrorCode::Invalid, "name ends with a dot or space"};
    }
    if (is_reserved_device_name(name)) {
        return Status{ErrorCode::Invalid, "name is a reserved device name"};
    }
    return ok_status();
}

std::string join(const std::string& directory, const std::string& leaf) {
    if (directory.empty()) {
        return leaf;
    }
    if (is_separator(directory.back())) {
        return directory + leaf;
    }
    return directory + "/" + leaf;
}

std::string to_platform_path(const std::string& utf8_path) {
    return utf8_path;
}

}  // namespace drc::fileio
