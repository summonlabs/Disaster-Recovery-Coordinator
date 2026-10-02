#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "drc/error.hpp"

namespace drc::fileio {

// Durable filesystem primitives. Everything here is explicitly flushed; no
// operation claims durability without a flush at the stated boundary.
class File {
public:
    File() = default;
    File(File&&) noexcept;
    File& operator=(File&&) noexcept;
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    ~File();

    // mode: "rb", "wb", "ab", "r+b"
    [[nodiscard]] static Result<File> open(const std::string& path, const char* mode);
    [[nodiscard]] static Result<File> open_exclusive_new(const std::string& path);

    [[nodiscard]] Status write(std::span<const std::uint8_t> bytes);
    [[nodiscard]] Status write_text(std::string_view text);
    [[nodiscard]] Result<std::size_t> read_some(std::span<std::uint8_t> buffer);
    [[nodiscard]] Result<std::vector<std::uint8_t>> read_all(std::uint64_t max_bytes);
    [[nodiscard]] Status flush_data();
    [[nodiscard]] Status seek_start();
    [[nodiscard]] Status truncate(std::uint64_t size);
    [[nodiscard]] Result<std::uint64_t> size() const;
    [[nodiscard]] Status close();
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] const std::string& path() const noexcept { return path_; }

private:
    void* handle_ = nullptr;
    std::string path_;
};

[[nodiscard]] Status ensure_directory(const std::string& path);
[[nodiscard]] Status sync_directory(const std::string& path);
[[nodiscard]] Result<bool> exists(const std::string& path);
[[nodiscard]] Result<std::uint64_t> file_size(const std::string& path);
[[nodiscard]] Result<std::vector<std::uint8_t>> read_file(const std::string& path,
                                                          std::uint64_t max_bytes);
[[nodiscard]] Status write_file_durable(const std::string& path,
                                        std::span<const std::uint8_t> bytes);
// Flush, verify by re-reading, then atomically replace. The destination either
// holds its previous contents or the complete new contents.
[[nodiscard]] Status atomic_replace(const std::string& temp_path, const std::string& final_path);
[[nodiscard]] Status remove_file(const std::string& path);
// Removes a file or a whole directory tree. On Windows the tree walk uses the
// extended-length prefix, because the standard filesystem library is not
// reliable on paths beyond MAX_PATH and can fail to terminate there.
[[nodiscard]] Status remove_tree(const std::string& path);

// Untrusted metadata never reaches the filesystem as a path: names are checked
// for traversal, separators, drive-relative forms, alternate data streams,
// reserved device names, and control characters.
[[nodiscard]] Status validate_leaf_name(std::string_view name);
[[nodiscard]] std::string join(const std::string& directory, const std::string& leaf);

// Windows paths beyond MAX_PATH need the extended-length prefix; POSIX needs
// nothing. Callers pass ordinary paths and this handles the platform.
[[nodiscard]] std::string to_platform_path(const std::string& utf8_path);

}  // namespace drc::fileio
