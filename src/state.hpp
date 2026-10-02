#pragma once

// Authoritative coordinator state. Every mutation of this state is the
// application of a journal record: the live path appends a record and applies
// it, and recovery replays the same records through the same function. The
// state digest recorded in each commit is therefore a fixed point that a
// replay must reproduce exactly, and a mismatch is reported as corruption
// rather than ignored.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "codec.hpp"
#include "drc/engine.hpp"
#include "drc/journal.hpp"

namespace drc::internal {

inline constexpr std::uint16_t kStateFormatVersion = 1;

struct CoordinatorState {
    Epoch epoch;
    Limits limits{};
    RecoveryPolicy policy{};
    Sequence last_sequence;
    Digest chain;
    std::uint64_t record_count = 0;
    std::uint64_t records_since_snapshot = 0;

    std::map<FailureDomainId, FailureDomainRecord> domains;
    std::map<SiteId, SiteRecord> sites;
    std::map<ObligationId, ProtectedObligation> obligations;
    std::map<SiteId, DestinationCapability> capabilities;
    std::map<SiteId, SiteReadinessEvidence> readiness;
    FederationState federation;
    std::map<DisasterEventId, DisasterEvent> events;
    std::map<DisasterEventId, AssessmentEvidence> assessments;
    std::map<RecoveryPlanId, RecoveryPlan> plans;
    std::map<DisasterEventId, RecoveryPlanId> active_plan;
    std::map<EffectRequestId, EffectRequest> outstanding;
    std::map<EffectReceiptId, EffectReceipt> receipts;
    std::map<EffectRequestId, EffectReceiptId> receipt_by_request;
    std::map<EffectDomain, EndpointDescriptor> endpoints;
    std::vector<RejectedEvidence> rejected_evidence;
    journal::SnapshotRefPayload last_snapshot;
    bool has_snapshot = false;

    // Identity watermarks. They only ever move forward, so replay reproduces
    // them exactly even when the object that advanced them has been retired.
    std::uint64_t next_failure_domain_id = 1;
    std::uint64_t next_site_id = 1;
    std::uint64_t next_obligation_id = 1;
    std::uint64_t next_event_id = 1;
    std::uint64_t next_plan_id = 1;
    std::uint64_t next_step_id = 1;
    std::uint64_t next_request_id = 1;
    std::uint64_t next_receipt_id = 1;
    std::uint64_t next_endpoint_id = 1;
    std::map<DisasterEventId, std::uint32_t> plan_generation_count;
    std::map<DisasterEventId, std::uint32_t> plan_count;
};

// Canonical encoding of the authoritative content. Journal bookkeeping (epoch,
// sequence, chain) is deliberately excluded: it changes on every restart and
// is not authority.
[[nodiscard]] std::vector<std::uint8_t> encode_state(const CoordinatorState& state);
[[nodiscard]] Result<CoordinatorState> decode_state(std::span<const std::uint8_t> bytes);
[[nodiscard]] Digest state_digest(const CoordinatorState& state);

// Applies one durable record. Returns the sequence number of the record for
// reporting, and fills notices with anything an observer should learn about.
[[nodiscard]] Status apply_record(CoordinatorState& state,
                                  const journal::Record& record,
                                  std::vector<TransitionNotice>& notices);

[[nodiscard]] RecoveryPlan* find_plan_of_step(CoordinatorState& state, RecoveryStepId step);

}  // namespace drc::internal
