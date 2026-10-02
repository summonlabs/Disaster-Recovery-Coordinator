#pragma once

#include <cstdint>
#include <string>

#include "drc/error.hpp"
#include "drc/id.hpp"

namespace drc::lockfile {

// One coordinator per journal directory. Two processes replaying and appending
// the same journal would each believe they hold authority, so the lock is part
// of the authority model rather than an optimisation.
struct Contents {
    std::uint64_t pid = 0;
    std::string owner;
    Epoch epoch;
    std::int64_t acquired_at = 0;
};

class Lock {
public:
    Lock() = default;
    Lock(Lock&&) noexcept;
    Lock& operator=(Lock&&) noexcept;
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
    ~Lock();

    // allow_takeover: when the recorded owner process no longer exists, take
    // the lock over instead of failing. A live owner is never displaced, and
    // the caller learns which happened through takeover().
    struct Options {
        std::string path;
        std::string owner;
        Epoch epoch;
        std::int64_t now_unix_seconds = 0;
        bool allow_takeover = true;
    };

    [[nodiscard]] static Result<Lock> acquire(const Options& options);

    [[nodiscard]] bool takeover() const noexcept { return takeover_; }
    [[nodiscard]] const Contents& previous() const noexcept { return previous_; }
    [[nodiscard]] const std::string& path() const noexcept { return path_; }
    [[nodiscard]] Status release();

private:
    std::string path_;
    bool held_ = false;
    bool takeover_ = false;
    Contents previous_{};
};

[[nodiscard]] Result<Contents> read(const std::string& path);
[[nodiscard]] Result<bool> is_process_alive(std::uint64_t pid);
[[nodiscard]] std::uint64_t current_process_id();

}  // namespace drc::lockfile
