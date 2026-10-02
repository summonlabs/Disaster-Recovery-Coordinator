#pragma once

#include <charconv>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "drc/error.hpp"

namespace drc {

// Identifiers are opaque 64-bit values. Zero is reserved for "no identity", so
// a default-constructed identifier can never name an authoritative object.
template <class Tag>
class Id {
public:
    using value_type = std::uint64_t;

    constexpr Id() noexcept = default;
    constexpr explicit Id(std::uint64_t value) noexcept : value_(value) {}

    [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
    [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }

    [[nodiscard]] std::string to_string() const { return std::to_string(value_); }

    [[nodiscard]] static Result<Id> parse(std::string_view text) {
        if (text.empty() || text.size() > 20) {
            return Status{ErrorCode::Invalid, "identifier length out of range"};
        }
        std::uint64_t value = 0;
        const char* first = text.data();
        const char* last = text.data() + text.size();
        const std::from_chars_result r = std::from_chars(first, last, value);
        if (r.ec != std::errc{} || r.ptr != last) {
            return Status{ErrorCode::Invalid, "identifier is not a decimal integer"};
        }
        if (value == 0) {
            return Status{ErrorCode::Invalid, "identifier 0 is the reserved no-identity value"};
        }
        return Id{value};
    }

    friend constexpr bool operator==(const Id& a, const Id& b) noexcept {
        return a.value_ == b.value_;
    }
    friend constexpr bool operator!=(const Id& a, const Id& b) noexcept {
        return a.value_ != b.value_;
    }
    friend constexpr bool operator<(const Id& a, const Id& b) noexcept {
        return a.value_ < b.value_;
    }
    friend constexpr bool operator>(const Id& a, const Id& b) noexcept {
        return a.value_ > b.value_;
    }
    friend constexpr bool operator<=(const Id& a, const Id& b) noexcept {
        return a.value_ <= b.value_;
    }
    friend constexpr bool operator>=(const Id& a, const Id& b) noexcept {
        return a.value_ >= b.value_;
    }

private:
    std::uint64_t value_ = 0;
};

// Monotone counters. Increment is checked: a counter never wraps silently.
template <class Tag>
class Counter {
public:
    constexpr Counter() noexcept = default;
    constexpr explicit Counter(std::uint64_t value) noexcept : value_(value) {}

    [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
    [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }

    [[nodiscard]] Result<Counter> next() const {
        if (value_ == UINT64_MAX) {
            return Status{ErrorCode::Overflow, "counter exhausted at 2^64-1"};
        }
        return Counter{value_ + 1};
    }

    [[nodiscard]] std::string to_string() const { return std::to_string(value_); }

    [[nodiscard]] static Result<Counter> parse(std::string_view text) {
        if (text.empty() || text.size() > 20) {
            return Status{ErrorCode::Invalid, "counter length out of range"};
        }
        std::uint64_t value = 0;
        const char* first = text.data();
        const char* last = text.data() + text.size();
        const std::from_chars_result r = std::from_chars(first, last, value);
        if (r.ec != std::errc{} || r.ptr != last) {
            return Status{ErrorCode::Invalid, "counter is not a decimal integer"};
        }
        if (value == 0) {
            return Status{ErrorCode::Invalid, "counter 0 is not a valid generation or epoch"};
        }
        return Counter{value};
    }

    friend constexpr bool operator==(const Counter& a, const Counter& b) noexcept {
        return a.value_ == b.value_;
    }
    friend constexpr bool operator!=(const Counter& a, const Counter& b) noexcept {
        return a.value_ != b.value_;
    }
    friend constexpr bool operator<(const Counter& a, const Counter& b) noexcept {
        return a.value_ < b.value_;
    }
    friend constexpr bool operator>(const Counter& a, const Counter& b) noexcept {
        return a.value_ > b.value_;
    }
    friend constexpr bool operator<=(const Counter& a, const Counter& b) noexcept {
        return a.value_ <= b.value_;
    }
    friend constexpr bool operator>=(const Counter& a, const Counter& b) noexcept {
        return a.value_ >= b.value_;
    }

private:
    std::uint64_t value_ = 0;
};

struct SiteTag;
struct FailureDomainTag;
struct ObligationTag;
struct DisasterEventTag;
struct RecoveryPlanTag;
struct RecoveryStepTag;
struct EffectRequestTag;
struct EffectReceiptTag;
struct EndpointTag;
struct PlacementRefTag;
struct ReservationRefTag;
struct GenerationTag;
struct EpochTag;
struct SequenceTag;

using SiteId = Id<SiteTag>;
using FailureDomainId = Id<FailureDomainTag>;
using ObligationId = Id<ObligationTag>;
using DisasterEventId = Id<DisasterEventTag>;
using RecoveryPlanId = Id<RecoveryPlanTag>;
using RecoveryStepId = Id<RecoveryStepTag>;
using EffectRequestId = Id<EffectRequestTag>;
using EffectReceiptId = Id<EffectReceiptTag>;
using EndpointId = Id<EndpointTag>;
using PlacementRef = Id<PlacementRefTag>;
using ReservationRef = Id<ReservationRefTag>;

// Generation: a fencing value for any authority that can go stale. Epoch: the
// coordinator incarnation, bumped on every successful journal open, used to
// fence asynchronous completions and evidence from an earlier incarnation.
using Generation = Counter<GenerationTag>;
using Epoch = Counter<EpochTag>;
using Sequence = Counter<SequenceTag>;

}  // namespace drc

namespace std {

template <class Tag>
struct hash<drc::Id<Tag>> {
    [[nodiscard]] std::size_t operator()(const drc::Id<Tag>& id) const noexcept {
        return std::hash<std::uint64_t>{}(id.value());
    }
};

template <class Tag>
struct hash<drc::Counter<Tag>> {
    [[nodiscard]] std::size_t operator()(const drc::Counter<Tag>& counter) const noexcept {
        return std::hash<std::uint64_t>{}(counter.value());
    }
};

}  // namespace std
