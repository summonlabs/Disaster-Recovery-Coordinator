#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "drc/error.hpp"
#include "drc/id.hpp"
#include "drc/time.hpp"

namespace drc {

// Recovery class: the priority ordering that protected obligations retain for
// the whole life of a disaster event. Lower value == more protected.
enum class RecoveryClass : std::uint32_t {
    SafetyCritical = 0,
    Protected = 1,
    Essential = 2,
    Standard = 3,
    Deferrable = 4,
};

inline constexpr std::uint32_t kRecoveryClassCount = 5;

[[nodiscard]] std::string_view to_string(RecoveryClass value) noexcept;
[[nodiscard]] Result<RecoveryClass> parse_recovery_class(std::string_view text);
[[nodiscard]] bool is_protected_class(RecoveryClass value) noexcept;

// A site is an authoritative object owned by this boundary only as an identity
// and a failure-domain membership. Everything else about a site is somebody
// else's truth, consumed as evidence.
struct SiteRecord {
    SiteId id;
    std::string name;
    FailureDomainId domain;
    std::uint64_t capacity_units = 0;
};

struct FailureDomainRecord {
    FailureDomainId id;
    std::string name;
};

// An obligation whose continuity survives a site loss. The coordinator owns the
// obligation's recovery state; it does not own the workload, the placement, or
// the reservation that satisfies it.
struct ProtectedObligation {
    ObligationId id;
    std::string name;
    RecoveryClass recovery_class = RecoveryClass::Standard;
    SiteId home_site;
    FailureDomainId home_domain;
    std::uint64_t required_capacity_units = 0;
    // Obligations this one depends on. A dependency must be restored before the
    // dependent obligation is restored.
    std::vector<ObligationId> depends_on;
};

// Bounds. Every one of these is enforced; none of them is advisory.
struct Limits {
    std::uint32_t max_sites = 4096;
    std::uint32_t max_obligations = 16384;
    std::uint32_t max_events = 256;
    std::uint32_t max_steps_per_plan = 8192;
    std::uint32_t max_plans_per_event = 64;
    std::uint32_t max_dependencies_per_obligation = 64;
    std::uint32_t max_readiness_checks = 64;
    std::uint32_t max_endpoints = 16;
    std::uint32_t max_outstanding_requests = 4096;
    std::uint32_t max_pending_receipts = 8192;
    std::uint32_t max_receipts_retained = 65536;
    std::uint64_t max_journal_bytes = 64ull * 1024ull * 1024ull;
    std::uint32_t compaction_record_threshold = 8192;
    std::uint32_t max_worker_threads = 32;
};

struct RecoveryPolicy {
    Generation generation{1};
    // Evidence older than this window is not fresh.
    UnixNanos evidence_freshness_window = 300 * kNanosPerSecond;
    std::uint32_t max_step_attempts = 3;
    std::uint32_t max_in_flight_steps = 8;
    // Planning refuses to produce a plan when a protected obligation has no
    // destination with fresh capability evidence, instead of inventing one.
    bool require_protected_placement = true;
    // Readiness checks a site must pass before return to service. Empty means
    // "any passing readiness observation with no failing check".
    std::vector<std::string> required_readiness_checks;
    // Contradictory readiness/capability evidence for the same generation moves
    // the event to Conflicted instead of picking a winner.
    bool conflict_on_equal_generation_evidence = true;
    // When true, a destination domain is only considered reachable if an
    // explicit federation observation says so. The default treats "no
    // observation" as "no known partition" and records that choice here rather
    // than hiding it.
    bool require_federation_evidence = false;
};

[[nodiscard]] bool operator==(const RecoveryPolicy& a, const RecoveryPolicy& b) noexcept;

// Well-formed UTF-8 test: rejects overlong encodings, surrogate halves, and
// code points beyond U+10FFFF. Text that arrives from outside this boundary is
// checked with this before it is stored, encoded, or used in a path.
[[nodiscard]] bool is_valid_utf8(std::string_view value) noexcept;

// Text accepted for durable metadata: bounded, no control characters, and
// well-formed UTF-8.
[[nodiscard]] bool is_valid_text(std::string_view value, bool allow_empty) noexcept;

// Checked capacity arithmetic. These return Overflow rather than wrapping.
[[nodiscard]] Result<std::uint64_t> checked_add_capacity(std::uint64_t a, std::uint64_t b);
[[nodiscard]] Result<std::uint64_t> checked_sub_capacity(std::uint64_t a, std::uint64_t b);

}  // namespace drc
