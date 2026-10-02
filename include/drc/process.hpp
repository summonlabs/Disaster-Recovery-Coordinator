#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "drc/error.hpp"
#include "drc/time.hpp"

namespace drc::process {

// Framed pipe protocol used between the coordinator process and the independent
// participant processes that stand in for neighbouring boundaries: a 32-bit
// little-endian length followed by that many payload bytes. The length is
// validated against max_frame_bytes before any allocation.
inline constexpr std::uint32_t kMaxFrameBytes = 4u * 1024u * 1024u;

struct SpawnOptions {
    std::vector<std::string> command;
    std::string working_directory;
};

// POSIX note. Writing to a participant that has already exited raises SIGPIPE,
// whose default action terminates the process; a participant that dies
// mid-exchange is a transport outcome, not the end of the coordinator. The
// first spawn therefore replaces the *default* disposition of SIGPIPE with
// "ignore" once, so the write reports EPIPE and becomes an unknown result.
// A process that installed its own SIGPIPE handler keeps it untouched.

class Child {
public:
    Child() = default;
    Child(Child&&) noexcept;
    Child& operator=(Child&&) noexcept;
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
    ~Child();

    [[nodiscard]] static Result<std::unique_ptr<Child>> spawn(const SpawnOptions& options);

    [[nodiscard]] Status write(std::span<const std::uint8_t> bytes);
    [[nodiscard]] Status write_frame(std::span<const std::uint8_t> payload,
                                     std::uint32_t max_frame_bytes = kMaxFrameBytes);
    [[nodiscard]] Status close_stdin();
    // Returns an empty vector at end of stream. With budget_nanos == 0 it
    // blocks until data arrives, the peer closes, or the peer is terminated.
    // With a positive budget it returns NotReady once the budget is spent;
    // partial frames are reported so the caller knows the stream is no longer
    // trustworthy.
    [[nodiscard]] Result<std::vector<std::uint8_t>> read_frame(
        std::uint32_t max_frame_bytes = kMaxFrameBytes,
        UnixNanos budget_nanos = 0);

    [[nodiscard]] Status terminate();
    [[nodiscard]] Result<int> wait();
    [[nodiscard]] bool running() const;
    [[nodiscard]] std::uint64_t pid() const noexcept { return pid_; }
    [[nodiscard]] Result<std::string> read_stderr(std::uint32_t max_bytes);

private:
    void close_handles();

    void* stdin_write_ = nullptr;
    void* stdout_read_ = nullptr;
    void* stderr_read_ = nullptr;
    void* process_ = nullptr;
    void* job_ = nullptr;
    std::uint64_t pid_ = 0;
    bool exited_ = false;
    int exit_code_ = 0;
};

}  // namespace drc::process
