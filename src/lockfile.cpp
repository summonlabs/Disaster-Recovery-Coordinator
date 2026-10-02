#include "drc/lockfile.hpp"

#include <string>
#include <vector>

#include "drc/fileio.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <cerrno>
#include <unistd.h>
#endif

namespace drc::lockfile {
namespace {

constexpr const char* kMagicLine = "drc-lock 1";

[[nodiscard]] std::string serialize(const Contents& contents) {
    std::string out;
    out.append(kMagicLine).append("\n");
    out.append("pid=").append(std::to_string(contents.pid)).append("\n");
    out.append("owner=").append(contents.owner).append("\n");
    out.append("epoch=").append(contents.epoch.to_string()).append("\n");
    out.append("acquired_at=").append(std::to_string(contents.acquired_at)).append("\n");
    return out;
}

[[nodiscard]] Result<Contents> parse(std::string_view text) {
    Contents contents;
    bool have_pid = false;
    bool have_owner = false;
    bool have_epoch = false;
    bool have_time = false;
    std::size_t offset = 0;
    bool first = true;
    while (offset < text.size()) {
        const std::size_t end = text.find('\n', offset);
        const std::string_view line =
            text.substr(offset, end == std::string_view::npos ? text.size() - offset : end - offset);
        offset = end == std::string_view::npos ? text.size() : end + 1;
        if (line.empty()) {
            continue;
        }
        if (first) {
            first = false;
            if (line != kMagicLine) {
                return Status{ErrorCode::Invalid, "lock file does not carry the expected header"};
            }
            continue;
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos) {
            return Status{ErrorCode::Invalid, "lock file line is not key=value"};
        }
        const std::string_view key = line.substr(0, equals);
        const std::string_view value = line.substr(equals + 1);
        if (key == "pid") {
            Result<Sequence> unused = Sequence::parse(value);
            if (!unused.ok()) {
                return Status{ErrorCode::Invalid, "lock pid is not a decimal integer"};
            }
            contents.pid = unused.value().value();
            have_pid = true;
        } else if (key == "owner") {
            if (value.size() > 256) {
                return Status{ErrorCode::OutOfRange, "lock owner exceeds the accepted length"};
            }
            contents.owner = std::string{value};
            have_owner = true;
        } else if (key == "epoch") {
            Result<Epoch> epoch = Epoch::parse(value);
            if (!epoch.ok()) {
                return epoch.status();
            }
            contents.epoch = epoch.value();
            have_epoch = true;
        } else if (key == "acquired_at") {
            Result<Sequence> parsed = Sequence::parse(value);
            if (!parsed.ok()) {
                return Status{ErrorCode::Invalid, "lock timestamp is not a decimal integer"};
            }
            contents.acquired_at = static_cast<std::int64_t>(parsed.value().value());
            have_time = true;
        } else {
            return Status{ErrorCode::Invalid, "lock file has an unknown key"};
        }
    }
    if (!have_pid || !have_owner || !have_epoch || !have_time) {
        return Status{ErrorCode::Invalid, "lock file is missing a required key"};
    }
    if (contents.pid == 0) {
        return Status{ErrorCode::Invalid, "lock pid is zero"};
    }
    return contents;
}

}  // namespace

std::uint64_t current_process_id() {
#if defined(_WIN32)
    return static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

Result<bool> is_process_alive(std::uint64_t pid) {
    if (pid == 0) {
        return false;
    }
    if (pid == current_process_id()) {
        return true;
    }
#if defined(_WIN32)
    const HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                      static_cast<DWORD>(pid));
    if (handle == nullptr) {
        return false;
    }
    DWORD code = 0;
    const bool alive = GetExitCodeProcess(handle, &code) != 0 && code == STILL_ACTIVE;
    (void)CloseHandle(handle);
    return alive;
#else
    if (::kill(static_cast<pid_t>(pid), 0) == 0) {
        return true;
    }
    if (errno == EPERM) {
        // The process exists but belongs to another user.
        return true;
    }
    return false;
#endif
}

Result<Contents> read(const std::string& path) {
    Result<std::vector<std::uint8_t>> bytes = fileio::read_file(path, 4096);
    if (!bytes.ok()) {
        return bytes.status();
    }
    const std::string_view text{reinterpret_cast<const char*>(bytes.value().data()),
                                bytes.value().size()};
    return parse(text);
}

Lock::Lock(Lock&& other) noexcept
    : path_(std::move(other.path_)), held_(other.held_), takeover_(other.takeover_),
      previous_(other.previous_) {
    other.held_ = false;
}

Lock& Lock::operator=(Lock&& other) noexcept {
    if (this != &other) {
        (void)release();
        path_ = std::move(other.path_);
        held_ = other.held_;
        takeover_ = other.takeover_;
        previous_ = other.previous_;
        other.held_ = false;
    }
    return *this;
}

Lock::~Lock() {
    (void)release();
}

Result<Lock> Lock::acquire(const Options& options) {
    const Status name_check = fileio::validate_leaf_name(
        options.path.substr(options.path.find_last_of("/\\") == std::string::npos
                                ? 0
                                : options.path.find_last_of("/\\") + 1));
    if (!name_check.ok()) {
        return name_check;
    }
    Contents mine;
    mine.pid = current_process_id();
    mine.owner = options.owner;
    mine.epoch = options.epoch;
    mine.acquired_at = options.now_unix_seconds;

    Lock lock;
    lock.path_ = options.path;
    Contents previous;

    for (int attempt = 0; attempt < 3; ++attempt) {
        Result<fileio::File> created = fileio::File::open_exclusive_new(options.path);
        if (created.ok()) {
            const Status written = created.value().write_text(serialize(mine));
            if (!written.ok()) {
                return written;
            }
            const Status flushed = created.value().flush_data();
            if (!flushed.ok()) {
                return flushed;
            }
            const Status closed = created.value().close();
            if (!closed.ok()) {
                return closed;
            }
            lock.held_ = true;
            lock.previous_ = previous;
            lock.takeover_ = previous.pid != 0;
            return lock;
        }
        if (created.status().code() != ErrorCode::Duplicate) {
            return created.status();
        }
        Result<Contents> existing = read(options.path);
        if (!existing.ok()) {
            // An unreadable lock is never taken over silently. A human decides.
            return Status{ErrorCode::Locked,
                          "lock file exists but is not a valid DRC lock; refusing to take it over"};
        }
        previous = existing.value();
        Result<bool> alive = is_process_alive(previous.pid);
        if (!alive.ok()) {
            return alive.status();
        }
        if (alive.value()) {
            std::string message = "journal is locked by pid ";
            message.append(std::to_string(previous.pid));
            message.append(" since ");
            message.append(std::to_string(previous.acquired_at));
            return Status{ErrorCode::Locked, std::move(message)};
        }
        if (!options.allow_takeover) {
            std::string message = "journal lock is held by a process that is gone (pid ";
            message.append(std::to_string(previous.pid));
            message.append("); takeover is disabled");
            return Status{ErrorCode::Locked, std::move(message)};
        }
        // Exactly one process can win the rename of the stale lock, so two
        // coordinators cannot both conclude that they may take over.
        std::string quarantine = options.path;
        quarantine.append(".stale.");
        quarantine.append(std::to_string(mine.pid));
        const Status renamed = fileio::atomic_replace(options.path, quarantine);
        if (!renamed.ok()) {
            continue;  // somebody else moved it first
        }
        (void)fileio::remove_file(quarantine);
    }
    return Status{ErrorCode::Locked, "lock acquisition did not converge"};
}

Status Lock::release() {
    if (!held_) {
        return ok_status();
    }
    held_ = false;
    return fileio::remove_file(path_);
}

}  // namespace drc::lockfile
