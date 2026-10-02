#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace drc {

// Stable machine-readable failure classes. Codes are part of the public API:
// callers branch on them, so their meanings must not drift and new codes are
// appended rather than reordered.
enum class ErrorCode : std::uint32_t {
    Ok = 0,
    // Input was structurally or semantically invalid.
    Invalid,
    // A referenced object is not registered.
    NotFound,
    // A unique identity or unique effect was submitted twice.
    Duplicate,
    // Checked arithmetic rejected the operation.
    Overflow,
    // A value is outside the accepted range.
    OutOfRange,
    // Recognised, but this boundary does not implement it.
    Unsupported,
    // The answer is genuinely unknown (no observation exists).
    Unknown,
    // Evidence is contradictory or insufficient to decide.
    Indeterminate,
    // A generation, epoch, or timestamp is older than the operation requires.
    Stale,
    // Two authoritative claims disagree.
    Conflict,
    // The caller has no delegated authority for this effect.
    Unauthorized,
    // A dependency of the requested action has not completed.
    DependencyUnmet,
    // Federation partition does not permit the effect right now.
    Partitioned,
    // Recovery policy forbids the action.
    PolicyViolation,
    // A return-to-service or failback gate is not satisfied.
    GateUnmet,
    // A bounded resource limit was reached.
    Exhausted,
    // Another operation is in flight for the same object.
    Busy,
    // The durable store is held by another process.
    Locked,
    // The operation was cancelled; results must not be published as success.
    Cancelled,
    // Preconditions are not yet satisfied but may become satisfied later.
    NotReady,
    // Interior corruption: bytes that are not a recoverable tail are damaged.
    Corrupt,
    // An incomplete trailing record: recoverable by truncation to the last
    // committed boundary.
    TornTail,
    // The durable format version is not supported by this build.
    FormatUnsupported,
    // An operating-system or filesystem operation failed.
    Io,
    // The runtime is shutting down and cannot accept new authority.
    Shutdown,
};

[[nodiscard]] std::string_view to_string(ErrorCode code) noexcept;

[[nodiscard]] constexpr bool is_ok(ErrorCode code) noexcept {
    return code == ErrorCode::Ok;
}

// True when the code means "the stored bytes are damaged". These are the codes
// recovery must never paper over.
[[nodiscard]] constexpr bool is_corruption(ErrorCode code) noexcept {
    return code == ErrorCode::Corrupt || code == ErrorCode::TornTail;
}

// A failure value. Empty Status means success.
class Status {
public:
    Status() = default;
    Status(ErrorCode code, std::string message) : code_(code), message_(std::move(message)) {}

    [[nodiscard]] bool ok() const noexcept { return code_ == ErrorCode::Ok; }
    [[nodiscard]] ErrorCode code() const noexcept { return code_; }
    [[nodiscard]] const std::string& message() const noexcept { return message_; }
    [[nodiscard]] std::string to_string() const;

    // A Status is truthy when it is *not* ok, so explicit checks read the same
    // as Result. Callers must still write the check.
    [[nodiscard]] explicit operator bool() const noexcept { return !ok(); }

private:
    ErrorCode code_ = ErrorCode::Ok;
    std::string message_;
};

[[nodiscard]] inline Status ok_status() { return Status{}; }

[[nodiscard]] inline Status make_error(ErrorCode code, std::string message) {
    return Status{code, std::move(message)};
}

// Result<T> is either a value or a Status. There are no exceptions in the
// public API, and no partially valid results: a failed Result carries no value.
template <class T>
class Result {
public:
    Result(T value) : storage_(std::in_place_index<0>, std::move(value)) {}
    Result(Status status) : storage_(std::in_place_index<1>, std::move(status)) {}

    [[nodiscard]] bool ok() const noexcept { return storage_.index() == 0; }
    [[nodiscard]] explicit operator bool() const noexcept { return ok(); }
    [[nodiscard]] ErrorCode code() const noexcept {
        return ok() ? ErrorCode::Ok : std::get<1>(storage_).code();
    }

    // Precondition: ok(). Callers check ok() first; there is no throwing path
    // and no silent default, because both would hide defects.
    [[nodiscard]] const T& value() const& { return std::get<0>(storage_); }
    [[nodiscard]] T& value() & { return std::get<0>(storage_); }
    [[nodiscard]] T&& value() && { return std::get<0>(std::move(storage_)); }
    [[nodiscard]] const T& operator*() const& { return std::get<0>(storage_); }
    [[nodiscard]] T& operator*() & { return std::get<0>(storage_); }
    [[nodiscard]] const T* operator->() const { return &std::get<0>(storage_); }
    [[nodiscard]] T* operator->() { return &std::get<0>(storage_); }

    [[nodiscard]] const Status& status() const& { return std::get<1>(storage_); }

private:
    std::variant<T, Status> storage_;
};

template <>
class Result<void> {
public:
    Result() = default;
    Result(Status status) : status_(std::move(status)) {}

    [[nodiscard]] bool ok() const noexcept { return status_.ok(); }
    [[nodiscard]] explicit operator bool() const noexcept { return ok(); }
    [[nodiscard]] ErrorCode code() const noexcept { return status_.code(); }
    [[nodiscard]] const Status& status() const noexcept { return status_; }

private:
    Status status_;
};

[[nodiscard]] inline Result<void> ok_result() { return Result<void>{}; }

}  // namespace drc
