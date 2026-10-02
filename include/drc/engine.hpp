#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "drc/digest.hpp"
#include "drc/effect.hpp"
#include "drc/error.hpp"
#include "drc/evidence.hpp"
#include "drc/id.hpp"
#include "drc/journal.hpp"
#include "drc/model.hpp"
#include "drc/plan.hpp"
#include "drc/ports.hpp"
#include "drc/time.hpp"

namespace drc {

// Lifecycle of a disaster event. The order is authoritative: a transition may
// only move forward, and every move is journaled with its reason.
enum class EventPhase : std::uint32_t {
    Declared = 0,
    Assessed,
    PlanReady,
    Evacuating,
    FailingOver,
    Stabilized,
    Restoring,
    Validating,
    Returning,
    Closed,
    Blocked,
    Conflicted,
};

inline constexpr std::uint32_t kEventPhaseCount = 12;

[[nodiscard]] std::string_view to_string(EventPhase phase) noexcept;
[[nodiscard]] Result<EventPhase> parse_event_phase(std::string_view text);
[[nodiscard]] bool is_terminal_phase(EventPhase phase) noexcept;

enum class EventDisposition : std::uint32_t {
    Active = 0,
    Superseded,
    Closed,
};

[[nodiscard]] std::string_view to_string(EventDisposition disposition) noexcept;

struct DisasterEvent {
    DisasterEventId id;
    Generation generation;
    EventPhase phase = EventPhase::Declared;
    EventPhase phase_before_interruption = EventPhase::Declared;
    EventDisposition disposition = EventDisposition::Active;
    std::uint32_t severity = 0;
    std::string declared_by;
    std::string reason;
    UnixNanos declared_at = 0;
    Epoch declared_epoch;
    UnixNanos updated_at = 0;
    Epoch updated_epoch;
    std::vector<SiteId> affected_sites;
    std::vector<FailureDomainId> affected_domains;
    std::vector<ObligationId> obligations_in_scope;
    DisasterEventId supersedes;
    std::vector<DisasterEventId> conflicts_with;
    // Sites whose evidence is contradictory. A newer observation for one of
    // these sites is what clears the conflict, so the reason is durable.
    std::vector<SiteId> conflicting_sites;
    std::vector<SiteId> returned_sites;
    RecoveryPlanId active_plan;
    Generation active_plan_generation;
    std::string status_detail;
    std::uint32_t blocked_steps = 0;
    std::uint32_t failed_safety_critical_steps = 0;
};

struct DisasterDeclaration {
    std::string declared_by;
    std::string reason;
    std::uint32_t severity = 0;
    std::vector<SiteId> affected_sites;
    std::vector<FailureDomainId> affected_domains;
    // A superseding incident names the event it replaces. Without it, an
    // overlapping declaration becomes a conflict, never a silent merge.
    DisasterEventId supersedes;
};

struct CreatePlanRequest {
    DisasterEventId event;
    std::string requested_by;
    // Allows a plan whose protected obligations could not all be placed. The
    // unplaceable obligations are still reported; nothing is invented.
    bool allow_partial_placement = false;
};

struct FailbackRequest {
    DisasterEventId event;
    SiteId target_site;
    std::string authorized_by;
    std::string justification;
};

struct AdvanceRequest {
    DisasterEventId event;
    // A bounded amount of work. There is no timeout: the caller decides how
    // many rounds to run, and the engine reports what remains.
    std::uint32_t max_rounds = 8;
    std::uint32_t max_dispatches = 64;
};

struct AdvanceReport {
    DisasterEventId event;
    RecoveryPlanId plan;
    Generation plan_generation;
    EventPhase phase_before = EventPhase::Declared;
    EventPhase phase_after = EventPhase::Declared;
    std::uint32_t rounds = 0;
    std::uint32_t dispatched = 0;
    std::uint32_t receipts_applied = 0;
    std::uint32_t receipts_ignored = 0;
    std::uint32_t steps_succeeded = 0;
    std::uint32_t steps_failed = 0;
    std::uint32_t steps_deferred = 0;
    std::uint32_t steps_indeterminate = 0;
    std::uint32_t steps_pending = 0;
    std::uint32_t steps_in_flight = 0;
    bool quiescent = false;
    bool blocked = false;
    std::vector<std::string> notes;
};

struct RestorationRequest {
    DisasterEventId event;
    std::string requested_by;
};

struct ReturnToServiceRequest {
    DisasterEventId event;
    SiteId site;
    std::string authorized_by;
    std::string justification;
};

struct BlockedResolution {
    DisasterEventId event;
    RecoveryStepId step;
    bool retry = false;
    std::string resolved_by;
    std::string justification;
};

struct EventClosure {
    DisasterEventId event;
    std::string closed_by;
    std::string summary;
};

struct CoordinatorStats {
    std::uint64_t api_calls = 0;
    std::uint64_t busy_rejections = 0;
    std::uint64_t records_appended = 0;
    std::uint64_t commits = 0;
    std::uint64_t dispatches = 0;
    std::uint64_t receipts_applied = 0;
    std::uint64_t receipts_ignored = 0;
    std::uint64_t stale_receipts = 0;
    std::uint64_t foreign_epoch_receipts = 0;
    std::uint64_t duplicate_receipts = 0;
    std::uint64_t deferred_dispatches = 0;
    std::uint64_t lock_order_violations = 0;
    std::uint64_t observer_failures = 0;
    std::uint64_t checkpoints = 0;
    std::uint64_t compactions = 0;
    std::uint64_t torn_tail_recoveries = 0;
    std::uint64_t recovered_records = 0;
    // Exchanges whose transport reported an error, and exchanges that ended
    // without any answer at all. Both are "the outcome is unknown", never
    // success.
    std::uint64_t transport_failures = 0;
    std::uint64_t unanswered_exchanges = 0;
};

// Transitions are reported after the fact, with no engine lock held, so an
// observer may read state and may not assume it is inside a mutation.
struct TransitionNotice {
    std::string kind;
    DisasterEventId event;
    RecoveryPlanId plan;
    RecoveryStepId step;
    std::string detail;
};

using Observer = std::function<void(const TransitionNotice&)>;

// Fault injection used to prove crash boundaries. Every field is zero (off) by
// default; the checks are a documented part of the runtime, not test-only code
// hidden behind a build flag.
struct FaultInjection {
    // Flush the records appended so far to storage and terminate the process
    // before the commit record is written: an uncommitted durable tail.
    std::uint64_t crash_after_appends = 0;
    // Terminate the process immediately after the given commit returns.
    std::uint64_t crash_after_commits = 0;
    // Terminate the process after the given dispatch has been sent.
    std::uint64_t crash_after_dispatches = 0;

    [[nodiscard]] bool active() const noexcept {
        return crash_after_appends != 0 || crash_after_commits != 0 ||
               crash_after_dispatches != 0;
    }
};

struct CoordinatorOptions {
    std::string directory;
    std::string journal_name = "coordinator.journal";
    std::string snapshot_name = "coordinator.snapshot";
    std::string lock_name = "coordinator.lock";
    std::string owner = "disaster-recovery-coordinator";
    Limits limits{};
    RecoveryPolicy policy{};
    std::uint32_t worker_threads = 4;
    // How long one request/response exchange with a participant may take before
    // the coordinator stops waiting and records the outcome as unknown. This is
    // an operational bound on delegated work, not a test timeout: without it a
    // participant that neither answers nor closes could wedge the engine.
    UnixNanos exchange_budget_nanos = 30 * kNanosPerSecond;
    std::shared_ptr<Clock> clock;
    bool allow_lock_takeover = true;
    FaultInjection fault;
    // Rebuild the journal by compaction once this many records have been
    // appended since the last snapshot.
    bool auto_compact = true;
};

struct JournalVerification {
    bool ok = false;
    bool torn_tail = false;
    bool interior_corruption = false;
    std::uint64_t file_bytes = 0;
    std::uint64_t committed_bytes = 0;
    std::uint64_t discarded_tail_bytes = 0;
    std::uint64_t record_count = 0;
    Sequence last_committed_sequence;
    Digest chain;
    std::string detail;
};

// The coordinator. It owns disaster-recovery orchestration state and the
// sequencing of delegated effects; it owns nothing about the sites, the
// capacity, the placements, the executions, or the network.
class Coordinator {
public:
    Coordinator() = default;
    Coordinator(const Coordinator&) = delete;
    Coordinator& operator=(const Coordinator&) = delete;
    ~Coordinator();

    [[nodiscard]] static Result<std::unique_ptr<Coordinator>> open(CoordinatorOptions options);

    // ---- registration and evidence -------------------------------------
    [[nodiscard]] Status define_failure_domain(FailureDomainRecord record);
    [[nodiscard]] Status define_site(SiteRecord record);
    [[nodiscard]] Status define_obligation(ProtectedObligation obligation);
    [[nodiscard]] Status set_policy(RecoveryPolicy policy);
    [[nodiscard]] Status record_capability(DestinationCapability capability);
    [[nodiscard]] Status record_readiness(SiteReadinessEvidence readiness);
    [[nodiscard]] Status record_federation_state(FederationState state);
    [[nodiscard]] Status register_endpoint(EndpointDescriptor descriptor);

    // ---- incident lifecycle --------------------------------------------
    [[nodiscard]] Result<DisasterEventId> declare_disaster(const DisasterDeclaration& declaration);
    [[nodiscard]] Status record_assessment(const AssessmentEvidence& assessment);
    [[nodiscard]] Result<RecoveryPlanId> create_plan(const CreatePlanRequest& request);
    [[nodiscard]] Status begin_recovery(DisasterEventId event);
    [[nodiscard]] Result<AdvanceReport> advance(const AdvanceRequest& request);
    [[nodiscard]] Status begin_restoration(const RestorationRequest& request);
    [[nodiscard]] Result<RecoveryPlanId> authorize_failback(const FailbackRequest& request);
    [[nodiscard]] Status return_site_to_service(const ReturnToServiceRequest& request);
    [[nodiscard]] Status resolve_blocked(const BlockedResolution& resolution);
    [[nodiscard]] Status close_event(const EventClosure& closure);

    // ---- durability and inspection -------------------------------------
    [[nodiscard]] Status checkpoint();
    [[nodiscard]] Status compact();
    [[nodiscard]] Result<JournalVerification> verify_journal() const;
    [[nodiscard]] Status shutdown();
    [[nodiscard]] Status set_observer(Observer observer);

    [[nodiscard]] Epoch epoch() const noexcept;
    [[nodiscard]] Digest state_digest() const;
    [[nodiscard]] CoordinatorStats stats() const;
    [[nodiscard]] const Limits& limits() const noexcept;
    [[nodiscard]] const std::string& directory() const noexcept;
    [[nodiscard]] Sequence last_committed_sequence() const;

    // ---- read-only views ------------------------------------------------
    [[nodiscard]] Result<RecoveryPolicy> policy() const;
    [[nodiscard]] Result<std::vector<FailureDomainId>> failure_domains() const;
    [[nodiscard]] Result<FailureDomainRecord> failure_domain(FailureDomainId id) const;
    [[nodiscard]] Result<std::vector<SiteId>> sites() const;
    [[nodiscard]] Result<SiteRecord> site(SiteId id) const;
    [[nodiscard]] Result<std::vector<ObligationId>> obligations() const;
    [[nodiscard]] Result<ProtectedObligation> obligation(ObligationId id) const;
    [[nodiscard]] Result<DestinationCapability> capability(SiteId id) const;
    [[nodiscard]] Result<SiteReadinessEvidence> readiness(SiteId id) const;
    [[nodiscard]] Result<FederationState> federation_state() const;
    [[nodiscard]] Result<std::vector<DisasterEventId>> events() const;
    [[nodiscard]] Result<DisasterEvent> event(DisasterEventId id) const;
    [[nodiscard]] Result<std::vector<RecoveryPlanId>> plans() const;
    [[nodiscard]] Result<RecoveryPlan> plan(RecoveryPlanId id) const;
    [[nodiscard]] Result<std::vector<RecoveryPlanId>> plans_for_event(DisasterEventId id) const;
    [[nodiscard]] Result<RecoveryPlanId> active_plan(DisasterEventId id) const;
    [[nodiscard]] Result<std::vector<RejectedEvidence>> rejected_evidence() const;
    [[nodiscard]] Result<std::vector<EffectReceipt>> receipts() const;
    [[nodiscard]] Result<std::vector<EffectRequest>> outstanding_requests() const;
    [[nodiscard]] Result<std::vector<EndpointDescriptor>> endpoints() const;
    // Freshness of the readiness observation for a site under the active policy.
    [[nodiscard]] Result<Freshness> readiness_freshness(SiteId id) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace drc
