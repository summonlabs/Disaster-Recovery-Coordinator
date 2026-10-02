#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "drc/error.hpp"
#include "drc/id.hpp"
#include "drc/model.hpp"
#include "drc/time.hpp"

namespace drc {

// Evidence is consumed truth owned by a neighbouring boundary. The coordinator
// records it, fences it by generation and epoch, and never edits the source.

// Destination capability: what a neighbouring capacity/placement authority says
// a site can accept right now, and for which recovery classes.
struct DestinationCapability {
    SiteId site;
    Generation generation;
    std::uint64_t available_capacity_units = 0;
    std::uint32_t supported_classes_mask = 0;
    UnixNanos observed_at = 0;
    Epoch observation_epoch;
    std::string source;
};

[[nodiscard]] bool capability_supports_class(const DestinationCapability& capability, RecoveryClass cls) noexcept;

struct ReadinessCheck {
    std::string name;
    bool passed = false;
};

// Readiness to carry service again. Return to service requires a readiness
// observation that is fresh under the active policy.
struct SiteReadinessEvidence {
    SiteId site;
    Generation generation;
    UnixNanos observed_at = 0;
    Epoch observation_epoch;
    std::string source;
    std::vector<ReadinessCheck> checks;
};

[[nodiscard]] bool readiness_passes_checks(const SiteReadinessEvidence& evidence,
                                           const std::vector<std::string>& required_checks);

enum class SiteAvailability : std::uint32_t {
    Unknown = 0,
    Available,
    Degraded,
    Unavailable,
};

[[nodiscard]] std::string_view to_string(SiteAvailability value) noexcept;

struct SiteAssessment {
    SiteId site;
    SiteAvailability availability = SiteAvailability::Unknown;
    // False when the reporting authority had no telemetry for this site. A gap
    // in telemetry is recorded, never promoted into a declaration.
    bool telemetry_present = false;
    std::uint32_t failed_checks = 0;
    std::string note;
};

struct AssessmentEvidence {
    DisasterEventId event;
    Generation event_generation;
    Generation generation;
    UnixNanos observed_at = 0;
    Epoch observation_epoch;
    std::string source;
    std::vector<SiteAssessment> sites;
};

// Federation state: whether this coordinator can currently reach the domains it
// must coordinate. When the state is unknown, remote effects are deferred
// rather than assumed reachable. When the federation is partitioned, only work
// whose authority stays inside the reachable set may be dispatched.
struct FederationState {
    Generation generation;
    UnixNanos observed_at = 0;
    Epoch observation_epoch;
    std::string source;
    // False means "no observation", which is not the same as "not partitioned".
    bool known = false;
    bool partitioned = false;
    std::vector<FailureDomainId> unreachable_domains;
};

[[nodiscard]] bool federation_reaches(const FederationState& state, FailureDomainId domain) noexcept;

// Evidence bookkeeping: the coordinator keeps the newest generation per object
// and rejects stale or contradictory observations instead of merging them.
enum class EvidenceKind : std::uint32_t {
    Capability = 0,
    Readiness,
    Assessment,
    Federation,
};

[[nodiscard]] std::string_view to_string(EvidenceKind kind) noexcept;

struct RejectedEvidence {
    EvidenceKind kind = EvidenceKind::Capability;
    SiteId site;
    Generation offered_generation;
    Generation retained_generation;
    ErrorCode reason = ErrorCode::Ok;
    UnixNanos observed_at = 0;
    std::string detail;
};

}  // namespace drc
