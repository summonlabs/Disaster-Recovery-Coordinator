#include "drc/time.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>

namespace drc {

Clock::~Clock() = default;

UnixNanos SystemClock::now_nanos() const {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now);
    return static_cast<UnixNanos>(nanos.count());
}

void ManualClock::advance(UnixNanos delta) {
    // Saturating, never wrapping: a wrapped clock would silently reorder
    // freshness decisions.
    if (delta > 0 && now_ > INT64_MAX - delta) {
        now_ = INT64_MAX;
        return;
    }
    if (delta < 0 && now_ < INT64_MIN - delta) {
        now_ = INT64_MIN;
        return;
    }
    now_ += delta;
}

void ManualClock::set(UnixNanos value) {
    now_ = value;
}

std::string_view to_string(Freshness freshness) noexcept {
    switch (freshness) {
        case Freshness::Unknown: return "unknown";
        case Freshness::Fresh: return "fresh";
        case Freshness::Expired: return "expired";
        case Freshness::FutureDated: return "future_dated";
        case Freshness::ForeignEpoch: return "foreign_epoch";
    }
    return "unknown";
}

bool is_fresh(Freshness freshness) noexcept {
    return freshness == Freshness::Fresh;
}

Freshness evaluate_freshness(UnixNanos now,
                             UnixNanos observed_at,
                             std::uint64_t observation_epoch,
                             std::uint64_t current_epoch,
                             UnixNanos window_nanos) {
    if (observed_at == 0 || observation_epoch == 0) {
        return Freshness::Unknown;
    }
    if (observation_epoch != current_epoch) {
        return Freshness::ForeignEpoch;
    }
    if (observed_at > now) {
        return Freshness::FutureDated;
    }
    if (window_nanos < 0) {
        return Freshness::Expired;
    }
    const UnixNanos age = now - observed_at;
    if (age > window_nanos) {
        return Freshness::Expired;
    }
    return Freshness::Fresh;
}

std::string format_timestamp_utc(UnixNanos value) {
    const UnixNanos seconds = value / kNanosPerSecond;
    UnixNanos fraction = value % kNanosPerSecond;
    if (fraction < 0) {
        fraction += kNanosPerSecond;
    }
    const std::time_t raw = static_cast<std::time_t>(seconds);
    std::tm parts{};
#if defined(_WIN32)
    if (gmtime_s(&parts, &raw) != 0) {
        return "invalid";
    }
#else
    if (gmtime_r(&raw, &parts) == nullptr) {
        return "invalid";
    }
#endif
    char buffer[40] = {};
    const int written = std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d.%09lldZ",
                                      parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday,
                                      parts.tm_hour, parts.tm_min, parts.tm_sec,
                                      static_cast<long long>(fraction));
    if (written <= 0) {
        return "invalid";
    }
    return std::string{buffer, static_cast<std::size_t>(written)};
}

Result<UnixNanos> parse_timestamp_utc(std::string_view text) {
    if (text.size() < 20 || text.size() > 30) {
        return Status{ErrorCode::Invalid, "timestamp length out of range"};
    }
    if (text.back() != 'Z') {
        return Status{ErrorCode::Invalid, "timestamp must end with Z"};
    }
    const std::string copy{text.substr(0, text.size() - 1)};
    std::tm parts{};
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    long long nanos = 0;
    const int matched = std::sscanf(copy.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d.%9lld", &year, &month, &day,
                                    &hour, &minute, &second, &nanos);
    if (matched != 7) {
        return Status{ErrorCode::Invalid, "timestamp is not ISO-8601 UTC with nanoseconds"};
    }
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60) {
        return Status{ErrorCode::Invalid, "timestamp field out of range"};
    }
    if (nanos < 0 || nanos > 999999999) {
        return Status{ErrorCode::Invalid, "timestamp nanoseconds out of range"};
    }
    parts.tm_year = year - 1900;
    parts.tm_mon = month - 1;
    parts.tm_mday = day;
    parts.tm_hour = hour;
    parts.tm_min = minute;
    parts.tm_sec = second;
#if defined(_WIN32)
    const std::time_t seconds = _mkgmtime(&parts);
#else
    const std::time_t seconds = timegm(&parts);
#endif
    if (seconds == static_cast<std::time_t>(-1)) {
        return Status{ErrorCode::Invalid, "timestamp is not representable"};
    }
    const UnixNanos base = static_cast<UnixNanos>(seconds) * kNanosPerSecond;
    if (base > INT64_MAX - static_cast<UnixNanos>(nanos)) {
        return Status{ErrorCode::Overflow, "timestamp overflows nanoseconds"};
    }
    return base + static_cast<UnixNanos>(nanos);
}

Result<UnixNanos> checked_add_nanos(UnixNanos base, UnixNanos delta) {
    if (delta > 0 && base > INT64_MAX - delta) {
        return Status{ErrorCode::Overflow, "nanosecond addition overflows"};
    }
    if (delta < 0 && base < INT64_MIN - delta) {
        return Status{ErrorCode::Overflow, "nanosecond addition underflows"};
    }
    return base + delta;
}

}  // namespace drc
