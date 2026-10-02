#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "drc/error.hpp"

namespace drc {

// Nanoseconds since the Unix epoch, in UTC. Signed so that a differences and
// pre-epoch observations can be represented and checked.
using UnixNanos = std::int64_t;

inline constexpr UnixNanos kNanosPerSecond = 1000000000;
inline constexpr UnixNanos kNanosPerMillisecond = 1000000;

// The clock is injected so that the deterministic core never reads the wall
// clock directly and tests can drive time explicitly.
class Clock {
public:
    Clock() = default;
    Clock(const Clock&) = delete;
    Clock& operator=(const Clock&) = delete;
    virtual ~Clock();

    [[nodiscard]] virtual UnixNanos now_nanos() const = 0;
};

class SystemClock final : public Clock {
public:
    [[nodiscard]] UnixNanos now_nanos() const override;
};

// A clock the caller advances. Deterministic tests use this.
class ManualClock final : public Clock {
public:
    explicit ManualClock(UnixNanos start = 1767225600LL * kNanosPerSecond) : now_(start) {}

    [[nodiscard]] UnixNanos now_nanos() const override { return now_; }
    void advance(UnixNanos delta);
    void set(UnixNanos value);

private:
    UnixNanos now_;
};

// Result of comparing an observation against a freshness window. "Unknown"
// means no observation was ever recorded; it is not the same as "Expired".
enum class Freshness : std::uint32_t {
    Unknown = 0,
    Fresh,
    Expired,
    FutureDated,
    ForeignEpoch,
};

[[nodiscard]] std::string_view to_string(Freshness freshness) noexcept;

// Freshness is a function of the observation time, the coordinator epoch that
// observed it, the current epoch, and the policy window. Evidence recovered
// from a journal written by an earlier incarnation therefore comes back
// ForeignEpoch until it is observed again.
[[nodiscard]] Freshness evaluate_freshness(UnixNanos now,
                                           UnixNanos observed_at,
                                           std::uint64_t observation_epoch,
                                           std::uint64_t current_epoch,
                                           UnixNanos window_nanos);

[[nodiscard]] bool is_fresh(Freshness freshness) noexcept;

// ISO-8601 UTC with nanosecond precision, e.g. 2026-01-01T00:00:00.000000000Z.
[[nodiscard]] std::string format_timestamp_utc(UnixNanos value);
[[nodiscard]] Result<UnixNanos> parse_timestamp_utc(std::string_view text);

// Checked addition for durations and deadlines.
[[nodiscard]] Result<UnixNanos> checked_add_nanos(UnixNanos base, UnixNanos delta);

}  // namespace drc
