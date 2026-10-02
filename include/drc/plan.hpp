#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "drc/digest.hpp"
#include "drc/effect.hpp"
#include "drc/error.hpp"
#include "drc/evidence.hpp"
#include "drc/id.hpp"
#include "drc/model.hpp"

namespace drc {

// A recovery step is a single delegated effect with a place in a dependency
// order. The coordinator owns the step, its ordering, and its state; the effect
// itself belongs to the neighbouring boundary the step is addressed to.
enum class StepKind : std::uint32_t {
    EvacuateSource = 0,
    ReserveDestination,
    PlaceObligation,
    RestoreAtDestination,
    RecoverExecution,
    RecoverNetwork,
    VerifyRecovery,
    ValidateReadiness,
    ReturnToService,
    FailbackToSource,
};

inline constexpr std::uint32_t kStepKindCount = 10;

[[nodiscard]] std::string_view to_string(StepKind kind) noexcept;
[[nodiscard]] Result<StepKind> parse_step_kind(std::string_view text);
[[nodiscard]] EffectDomain effect_domain_for(StepKind kind) noexcept;
[[nodiscard]] EffectKind effect_kind_for(StepKind kind) noexcept;

enum class StepState : std::uint32_t {
    Pending = 0,   // dependencies not satisfied
    Ready,         // dependencies satisfied, not yet dispatched
    InFlight,      // dispatched, no applying receipt yet
    Succeeded,
    Failed,
    Deferred,      // the boundary said "not now"; retry stays inside the plan
    Indeterminate, // the outcome is genuinely unknown (no reply / partition)
    Cancelled,
    Superseded,    // belongs to a plan generation that was replaced
};

inline constexpr std::uint32_t kStepStateCount = 9;

[[nodiscard]] std::string_view to_string(StepState state) noexcept;
[[nodiscard]] Result<StepState> parse_step_state(std::string_view text);
[[nodiscard]] bool is_terminal_step_state(StepState state) noexcept;
[[nodiscard]] bool is_success_step_state(StepState state) noexcept;

struct RecoveryStep {
    RecoveryStepId id;
    RecoveryPlanId plan;
    DisasterEventId event;
    Generation plan_generation;
    Generation event_generation;
    std::uint32_t ordinal = 0;
    StepKind kind = StepKind::VerifyRecovery;
    ObligationId obligation;
    SiteId source;
    SiteId destination;
    FailureDomainId destination_domain;
    RecoveryClass recovery_class = RecoveryClass::Standard;
    std::uint64_t required_capacity_units = 0;
    bool safety_critical = false;
    // Some effects are decisions, not consequences: returning a site to
    // service, and failing work back to it. Those steps exist in the plan but
    // stay unauthorized until a human-authorized call says otherwise.
    bool authorized = true;
    std::vector<RecoveryStepId> depends_on;
    StepState state = StepState::Pending;
    std::uint32_t attempts = 0;
    EffectRequestId last_request;
    EffectReceiptId last_receipt;
    Sequence last_receipt_sequence;
    std::string detail;
};

struct PlacementAssignment {
    ObligationId obligation;
    SiteId destination;
    ReservationRef reservation;
    PlacementRef placement;
    Generation plan_generation;
    RecoveryStepId reservation_step;
    RecoveryStepId placement_step;
};

enum class PlanState : std::uint32_t {
    Active = 0,
    Superseded,
    Completed,
    Abandoned,
};

[[nodiscard]] std::string_view to_string(PlanState state) noexcept;

enum class PlanKind : std::uint32_t {
    Recovery = 0,
    Failback,
    Restoration,
};

[[nodiscard]] std::string_view to_string(PlanKind kind) noexcept;

struct RecoveryPlan {
    RecoveryPlanId id;
    DisasterEventId event;
    PlanKind kind = PlanKind::Recovery;
    PlanState state = PlanState::Active;
    Generation generation;
    Generation event_generation;
    Generation policy_generation;
    RecoveryPlanId supersedes;
    Epoch created_epoch;
    UnixNanos created_at = 0;
    std::string requested_by;
    Digest digest;
    std::vector<RecoveryStepId> order;   // canonical evaluation order
    std::map<RecoveryStepId, RecoveryStep> steps;
    std::vector<PlacementAssignment> assignments;
    std::vector<ObligationId> unplaced;
    std::vector<ObligationId> deferred;
    std::vector<SiteId> target_sites;   // failback target sites
    // Why something was left out. An obligation that could not be planned is
    // reported durably rather than dropped.
    std::vector<std::string> diagnostics;
};

// The planner is a pure function of explicit inputs, so the ordering,
// capacity, and dependency invariants can be tested without the engine, the
// filesystem, or a clock.
struct PlanBuildInput {
    DisasterEventId event;
    Generation event_generation;
    RecoveryPlanId plan_id;
    Generation plan_generation;
    Generation policy_generation;
    RecoveryPlanId supersedes;
    PlanKind kind = PlanKind::Recovery;
    Epoch epoch;
    UnixNanos now = 0;
    RecoveryStepId first_step_id;
    const RecoveryPolicy* policy = nullptr;
    const Limits* limits = nullptr;
    const std::map<SiteId, SiteRecord>* sites = nullptr;
    const std::map<ObligationId, ProtectedObligation>* obligations = nullptr;
    const std::map<SiteId, DestinationCapability>* capabilities = nullptr;
    const std::map<SiteId, SiteReadinessEvidence>* readiness = nullptr;
    const std::vector<SiteId>* affected_sites = nullptr;
    const std::vector<FailureDomainId>* affected_domains = nullptr;
    const std::vector<SiteId>* failback_targets = nullptr;
};

struct PlanBuildOutcome {
    std::vector<RecoveryStep> steps;
    std::vector<PlacementAssignment> assignments;
    std::vector<ObligationId> unplaced;
    std::vector<ObligationId> deferred;
    std::vector<std::string> diagnostics;
    Digest digest;
    std::uint64_t capacity_committed = 0;
};

// Builds a recovery or failback plan. Fails with Unsupported when a protected
// obligation cannot be placed and the policy forbids partial plans; it never
// invents a destination, a capacity figure, or a reservation.
[[nodiscard]] Result<PlanBuildOutcome> build_plan(const PlanBuildInput& input);

// Canonical evaluation order for a set of steps: deterministic, dependency
// respecting, and stable across platforms. Returns a cycle error naming the
// involved steps when the graph is not a DAG.
[[nodiscard]] Result<std::vector<RecoveryStepId>> canonical_step_order(
    const std::map<RecoveryStepId, RecoveryStep>& steps);

[[nodiscard]] Digest digest_steps(const std::vector<RecoveryStep>& steps,
                                  const std::vector<PlacementAssignment>& assignments);

// Canonical encodings. These are part of the durable format.
[[nodiscard]] std::vector<std::uint8_t> encode_plan(const RecoveryPlan& plan);
[[nodiscard]] Result<RecoveryPlan> decode_plan(std::span<const std::uint8_t> bytes);
[[nodiscard]] std::vector<std::uint8_t> encode_step_transition(const RecoveryStep& step);
[[nodiscard]] Result<RecoveryStep> decode_step(const std::span<const std::uint8_t> bytes);

}  // namespace drc
