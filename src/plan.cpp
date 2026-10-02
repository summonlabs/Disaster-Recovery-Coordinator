#include "drc/plan.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <queue>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "drc/canonical.hpp"

namespace drc {
namespace {

using canonical::Reader;
using canonical::Writer;

[[nodiscard]] Status finish(const Reader& reader, const char* what) {
    if (reader.failed()) {
        return reader.status();
    }
    if (!reader.at_end()) {
        return Status{ErrorCode::Invalid, std::string{"trailing bytes in "} + what};
    }
    return ok_status();
}

struct Candidate {
    SiteId site;
    FailureDomainId domain;
    std::uint64_t available = 0;
};

[[nodiscard]] bool contains_site(const std::vector<SiteId>& sites, SiteId id) {
    return std::find(sites.begin(), sites.end(), id) != sites.end();
}

[[nodiscard]] bool contains_domain(const std::vector<FailureDomainId>& domains, FailureDomainId id) {
    return std::find(domains.begin(), domains.end(), id) != domains.end();
}

[[nodiscard]] std::string describe_obligation(ObligationId id, RecoveryClass cls) {
    std::string out = "obligation ";
    out.append(id.to_string());
    out.append(" (");
    out.append(to_string(cls));
    out.append(")");
    return out;
}

// Deterministic destination order: most remaining capacity first, then by
// identity. Capacity evidence must be fresh, so a candidate with stale or
// foreign-epoch evidence simply is not a candidate.
[[nodiscard]] std::vector<Candidate> build_candidates(const PlanBuildInput& input) {
    std::vector<Candidate> candidates;
    if (input.capabilities == nullptr || input.sites == nullptr) {
        return candidates;
    }
    const RecoveryPolicy& policy = *input.policy;
    for (const auto& entry : *input.capabilities) {
        const DestinationCapability& capability = entry.second;
        const auto site = input.sites->find(capability.site);
        if (site == input.sites->end()) {
            continue;
        }
        if (contains_site(*input.affected_sites, capability.site)) {
            continue;
        }
        if (contains_domain(*input.affected_domains, site->second.domain)) {
            continue;
        }
        const Freshness freshness = evaluate_freshness(
            input.now, capability.observed_at, capability.observation_epoch.value(),
            input.epoch.value(), policy.evidence_freshness_window);
        if (!is_fresh(freshness)) {
            continue;
        }
        if (capability.available_capacity_units == 0) {
            continue;
        }
        candidates.push_back(Candidate{capability.site, site->second.domain,
                                       capability.available_capacity_units});
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.available != b.available) {
            return a.available > b.available;
        }
        return a.site < b.site;
    });
    return candidates;
}

[[nodiscard]] RecoveryStepId alloc_step_id(const PlanBuildInput& input, std::uint32_t index) {
    return RecoveryStepId{input.first_step_id.value() + index};
}

}  // namespace

std::string_view to_string(StepKind kind) noexcept {
    switch (kind) {
        case StepKind::EvacuateSource: return "evacuate_source";
        case StepKind::ReserveDestination: return "reserve_destination";
        case StepKind::PlaceObligation: return "place_obligation";
        case StepKind::RestoreAtDestination: return "restore_at_destination";
        case StepKind::RecoverExecution: return "recover_execution";
        case StepKind::RecoverNetwork: return "recover_network";
        case StepKind::VerifyRecovery: return "verify_recovery";
        case StepKind::ValidateReadiness: return "validate_readiness";
        case StepKind::ReturnToService: return "return_to_service";
        case StepKind::FailbackToSource: return "failback_to_source";
    }
    return "unknown";
}

Result<StepKind> parse_step_kind(std::string_view text) {
    if (text == "evacuate_source") return StepKind::EvacuateSource;
    if (text == "reserve_destination") return StepKind::ReserveDestination;
    if (text == "place_obligation") return StepKind::PlaceObligation;
    if (text == "restore_at_destination") return StepKind::RestoreAtDestination;
    if (text == "recover_execution") return StepKind::RecoverExecution;
    if (text == "recover_network") return StepKind::RecoverNetwork;
    if (text == "verify_recovery") return StepKind::VerifyRecovery;
    if (text == "validate_readiness") return StepKind::ValidateReadiness;
    if (text == "return_to_service") return StepKind::ReturnToService;
    if (text == "failback_to_source") return StepKind::FailbackToSource;
    return Status{ErrorCode::Invalid, "unknown recovery step kind"};
}

EffectDomain effect_domain_for(StepKind kind) noexcept {
    switch (kind) {
        case StepKind::EvacuateSource:
        case StepKind::RestoreAtDestination:
        case StepKind::VerifyRecovery:
        case StepKind::ValidateReadiness:
        case StepKind::ReturnToService:
        case StepKind::FailbackToSource:
            return EffectDomain::SiteControlPlane;
        case StepKind::ReserveDestination:
        case StepKind::PlaceObligation:
            return EffectDomain::PlacementReservationCapacity;
        case StepKind::RecoverExecution:
            return EffectDomain::AsiExecutionRecovery;
        case StepKind::RecoverNetwork:
            return EffectDomain::DfiNetworkRecovery;
    }
    return EffectDomain::SiteControlPlane;
}

EffectKind effect_kind_for(StepKind kind) noexcept {
    switch (kind) {
        case StepKind::EvacuateSource: return EffectKind::Evacuate;
        case StepKind::ReserveDestination: return EffectKind::Reserve;
        case StepKind::PlaceObligation: return EffectKind::Place;
        case StepKind::RestoreAtDestination: return EffectKind::Restore;
        case StepKind::RecoverExecution: return EffectKind::RecoverExecution;
        case StepKind::RecoverNetwork: return EffectKind::RecoverNetwork;
        case StepKind::VerifyRecovery: return EffectKind::Verify;
        case StepKind::ValidateReadiness: return EffectKind::ValidateReadiness;
        case StepKind::ReturnToService: return EffectKind::ReturnToService;
        case StepKind::FailbackToSource: return EffectKind::Failback;
    }
    return EffectKind::Verify;
}

std::string_view to_string(StepState state) noexcept {
    switch (state) {
        case StepState::Pending: return "pending";
        case StepState::Ready: return "ready";
        case StepState::InFlight: return "in_flight";
        case StepState::Succeeded: return "succeeded";
        case StepState::Failed: return "failed";
        case StepState::Deferred: return "deferred";
        case StepState::Indeterminate: return "indeterminate";
        case StepState::Cancelled: return "cancelled";
        case StepState::Superseded: return "superseded";
    }
    return "unknown";
}

Result<StepState> parse_step_state(std::string_view text) {
    if (text == "pending") return StepState::Pending;
    if (text == "ready") return StepState::Ready;
    if (text == "in_flight") return StepState::InFlight;
    if (text == "succeeded") return StepState::Succeeded;
    if (text == "failed") return StepState::Failed;
    if (text == "deferred") return StepState::Deferred;
    if (text == "indeterminate") return StepState::Indeterminate;
    if (text == "cancelled") return StepState::Cancelled;
    if (text == "superseded") return StepState::Superseded;
    return Status{ErrorCode::Invalid, "unknown recovery step state"};
}

bool is_terminal_step_state(StepState state) noexcept {
    switch (state) {
        case StepState::Succeeded:
        case StepState::Failed:
        case StepState::Cancelled:
        case StepState::Superseded:
            return true;
        case StepState::Pending:
        case StepState::Ready:
        case StepState::InFlight:
        case StepState::Deferred:
        case StepState::Indeterminate:
            return false;
    }
    return false;
}

bool is_success_step_state(StepState state) noexcept {
    return state == StepState::Succeeded;
}

std::string_view to_string(PlanState state) noexcept {
    switch (state) {
        case PlanState::Active: return "active";
        case PlanState::Superseded: return "superseded";
        case PlanState::Completed: return "completed";
        case PlanState::Abandoned: return "abandoned";
    }
    return "unknown";
}

std::string_view to_string(PlanKind kind) noexcept {
    switch (kind) {
        case PlanKind::Recovery: return "recovery";
        case PlanKind::Failback: return "failback";
        case PlanKind::Restoration: return "restoration";
    }
    return "unknown";
}

Result<std::vector<RecoveryStepId>> canonical_step_order(
    const std::map<RecoveryStepId, RecoveryStep>& steps) {
    std::map<RecoveryStepId, std::uint32_t> indegree;
    std::map<RecoveryStepId, std::vector<RecoveryStepId>> successors;
    for (const auto& entry : steps) {
        indegree.emplace(entry.first, 0);
    }
    for (const auto& entry : steps) {
        const RecoveryStep& step = entry.second;
        for (const RecoveryStepId dependency : step.depends_on) {
            if (steps.find(dependency) == steps.end()) {
                return Status{ErrorCode::Invalid,
                              "step " + step.id.to_string() + " depends on unknown step " +
                                  dependency.to_string()};
            }
            if (dependency == step.id) {
                return Status{ErrorCode::Conflict,
                              "step " + step.id.to_string() + " depends on itself"};
            }
            successors[dependency].push_back(step.id);
            indegree[step.id] += 1u;
        }
    }

    auto priority = [&steps](RecoveryStepId id) {
        const RecoveryStep& step = steps.at(id);
        return std::make_tuple(static_cast<std::uint32_t>(step.recovery_class), step.ordinal,
                               step.id.value());
    };
    using Key = std::tuple<std::uint32_t, std::uint32_t, std::uint64_t>;
    struct Node {
        Key key;
        RecoveryStepId id;
    };
    struct Compare {
        bool operator()(const Node& a, const Node& b) const { return a.key > b.key; }
    };
    std::priority_queue<Node, std::vector<Node>, Compare> ready;
    for (const auto& entry : indegree) {
        if (entry.second == 0) {
            ready.push(Node{priority(entry.first), entry.first});
        }
    }

    std::vector<RecoveryStepId> order;
    order.reserve(steps.size());
    while (!ready.empty()) {
        const RecoveryStepId id = ready.top().id;
        ready.pop();
        order.push_back(id);
        for (const RecoveryStepId next : successors[id]) {
            std::uint32_t& degree = indegree[next];
            if (degree > 0) {
                degree -= 1u;
            }
            if (degree == 0) {
                ready.push(Node{priority(next), next});
            }
        }
    }
    if (order.size() != steps.size()) {
        std::string message = "recovery step dependency cycle among ";
        message.append(std::to_string(steps.size() - order.size()));
        message.append(" steps");
        return Status{ErrorCode::Conflict, std::move(message)};
    }
    return order;
}

// The planner. Pure: the same inputs always produce the same steps, the same
// identities, and the same digest.
Result<PlanBuildOutcome> build_plan(const PlanBuildInput& input) {
    if (input.policy == nullptr || input.limits == nullptr || input.sites == nullptr ||
        input.obligations == nullptr || input.affected_sites == nullptr ||
        input.affected_domains == nullptr) {
        return Status{ErrorCode::Invalid, "plan build inputs are incomplete"};
    }
    if (!input.first_step_id.valid()) {
        return Status{ErrorCode::Invalid, "plan build needs a valid first step identity"};
    }

    const RecoveryPolicy& policy = *input.policy;
    const Limits& limits = *input.limits;
    PlanBuildOutcome outcome;
    const std::vector<SiteId> no_targets;
    const std::vector<SiteId>& targets =
        input.failback_targets != nullptr ? *input.failback_targets : no_targets;
    std::vector<Candidate> candidates = build_candidates(input);
    std::map<SiteId, std::uint64_t> remaining;
    for (const Candidate& candidate : candidates) {
        remaining[candidate.site] = candidate.available;
    }

    std::vector<ObligationId> in_scope;
    for (const auto& entry : *input.obligations) {
        const ProtectedObligation& obligation = entry.second;
        bool selected = false;
        if (input.kind == PlanKind::Failback) {
            selected = input.failback_targets != nullptr &&
                       contains_site(*input.failback_targets, obligation.home_site);
        } else {
            selected = contains_site(*input.affected_sites, obligation.home_site) ||
                       contains_domain(*input.affected_domains, obligation.home_domain);
        }
        if (selected) {
            in_scope.push_back(obligation.id);
        }
    }
    // Protected obligations keep their priority in every ordering decision.
    std::sort(in_scope.begin(), in_scope.end(), [&input](ObligationId a, ObligationId b) {
        const ProtectedObligation& left = input.obligations->at(a);
        const ProtectedObligation& right = input.obligations->at(b);
        if (left.recovery_class != right.recovery_class) {
            return left.recovery_class < right.recovery_class;
        }
        return a < b;
    });
    if (in_scope.size() > limits.max_obligations) {
        return Status{ErrorCode::Exhausted, "obligations in scope exceed the configured limit"};
    }

    std::map<ObligationId, RecoveryStepId> evacuate_step;
    std::map<ObligationId, RecoveryStepId> reserve_step;
    std::map<ObligationId, RecoveryStepId> place_step;
    std::map<ObligationId, RecoveryStepId> restore_step;
    std::map<ObligationId, RecoveryStepId> execute_step;
    std::map<ObligationId, RecoveryStepId> verify_step;
    std::map<SiteId, RecoveryStepId> network_step;
    std::map<SiteId, RecoveryStepId> readiness_step;
    std::map<ObligationId, SiteId> destination_of;
    std::vector<RecoveryStep> steps;
    std::uint32_t next_index = 0;

    auto push_step = [&](RecoveryStep step) -> RecoveryStepId {
        step.id = alloc_step_id(input, next_index);
        step.ordinal = next_index;
        next_index += 1u;
        const RecoveryStepId id = step.id;
        steps.push_back(std::move(step));
        return id;
    };

    auto ensure_network_step = [&](SiteId destination, FailureDomainId domain) -> RecoveryStepId {
        const auto existing = network_step.find(destination);
        if (existing != network_step.end()) {
            return existing->second;
        }
        RecoveryStep step;
        step.plan = input.plan_id;
        step.event = input.event;
        step.plan_generation = input.plan_generation;
        step.event_generation = input.event_generation;
        step.kind = StepKind::RecoverNetwork;
        step.destination = destination;
        step.destination_domain = domain;
        step.recovery_class = RecoveryClass::Essential;
        step.safety_critical = false;
        const RecoveryStepId id = push_step(std::move(step));
        network_step.emplace(destination, id);
        return id;
    };

    auto ensure_readiness_step = [&](SiteId site, FailureDomainId domain) -> RecoveryStepId {
        const auto existing = readiness_step.find(site);
        if (existing != readiness_step.end()) {
            return existing->second;
        }
        RecoveryStep step;
        step.plan = input.plan_id;
        step.event = input.event;
        step.plan_generation = input.plan_generation;
        step.event_generation = input.event_generation;
        step.kind = StepKind::ValidateReadiness;
        step.destination = site;
        step.destination_domain = domain;
        step.recovery_class = RecoveryClass::Essential;
        step.safety_critical = true;
        const RecoveryStepId id = push_step(std::move(step));
        readiness_step.emplace(site, id);
        return id;
    };

    // Orders the steps for evaluation and stores them in the outcome.
    auto order_and_store = [&]() -> Status {
        std::map<RecoveryStepId, RecoveryStep> indexed;
        for (const RecoveryStep& step : steps) {
            indexed.emplace(step.id, step);
        }
        Result<std::vector<RecoveryStepId>> order = canonical_step_order(indexed);
        if (!order.ok()) {
            return order.status();
        }
        outcome.steps.clear();
        outcome.steps.reserve(steps.size());
        std::uint32_t ordinal = 0;
        for (const RecoveryStepId id : order.value()) {
            RecoveryStep step = indexed.at(id);
            step.ordinal = ordinal;
            ordinal += 1u;
            outcome.steps.push_back(std::move(step));
        }
        return ok_status();
    };

    // A restoration plan carries no obligation movement: it asks the site
    // control plane to validate readiness and then to return each affected
    // site to service. Returning is a separate authorization, not an automatic
    // consequence of validation.
    if (input.kind == PlanKind::Restoration) {
        std::vector<SiteId> ordered_sites = *input.affected_sites;
        std::sort(ordered_sites.begin(), ordered_sites.end());
        if (ordered_sites.size() > limits.max_sites) {
            return Status{ErrorCode::Exhausted, "affected sites exceed the configured limit"};
        }
        for (const SiteId site_id : ordered_sites) {
            const auto site = input.sites->find(site_id);
            if (site == input.sites->end()) {
                outcome.diagnostics.push_back("affected site " + site_id.to_string() +
                                              " is not a registered site");
                continue;
            }
            const RecoveryStepId validate = ensure_readiness_step(site_id, site->second.domain);
            RecoveryStep ret;
            ret.plan = input.plan_id;
            ret.event = input.event;
            ret.plan_generation = input.plan_generation;
            ret.event_generation = input.event_generation;
            ret.kind = StepKind::ReturnToService;
            ret.source = site_id;
            ret.destination = site_id;
            ret.destination_domain = site->second.domain;
            ret.recovery_class = RecoveryClass::Protected;
            ret.safety_critical = true;
            ret.authorized = false;
            ret.detail = "awaiting explicit return-to-service authorization";
            ret.depends_on.push_back(validate);
            push_step(std::move(ret));
        }
        if (steps.size() > limits.max_steps_per_plan) {
            return Status{ErrorCode::Exhausted, "plan exceeds the configured step limit"};
        }
        const Status ordering = order_and_store();
        if (!ordering.ok()) {
            return ordering;
        }
        outcome.digest = digest_steps(outcome.steps, outcome.assignments);
        return outcome;
    }

    if (input.kind == PlanKind::Failback) {
        for (const SiteId target : targets) {
            const auto site = input.sites->find(target);
            if (site == input.sites->end()) {
                outcome.diagnostics.push_back("failback target " + target.to_string() +
                                              " is not a registered site");
                continue;
            }
            ensure_readiness_step(target, site->second.domain);
        }
    }

    for (const ObligationId id : in_scope) {
        const ProtectedObligation& obligation = input.obligations->at(id);
        if (obligation.depends_on.size() > limits.max_dependencies_per_obligation) {
            return Status{ErrorCode::Exhausted,
                          "obligation " + id.to_string() + " has too many dependencies"};
        }
        for (const ObligationId dependency : obligation.depends_on) {
            if (input.obligations->find(dependency) == input.obligations->end()) {
                return Status{ErrorCode::NotFound,
                              "obligation " + id.to_string() + " depends on unregistered obligation " +
                                  dependency.to_string()};
            }
        }

        SiteId destination;
        FailureDomainId destination_domain;
        bool placed = false;
        if (input.kind == PlanKind::Failback) {
            const SiteId home = obligation.home_site;
            if (!contains_site(targets, home)) {
                continue;
            }
            const auto site = input.sites->find(home);
            if (site == input.sites->end()) {
                outcome.unplaced.push_back(id);
                outcome.diagnostics.push_back(describe_obligation(id, obligation.recovery_class) +
                                              " cannot fail back: home site is not registered");
                continue;
            }
            const auto capability = input.capabilities->find(home);
            if (capability == input.capabilities->end()) {
                outcome.unplaced.push_back(id);
                outcome.diagnostics.push_back(
                    describe_obligation(id, obligation.recovery_class) +
                    " cannot fail back: no capability evidence for home site " + home.to_string());
                continue;
            }
            const Freshness freshness = evaluate_freshness(
                input.now, capability->second.observed_at,
                capability->second.observation_epoch.value(), input.epoch.value(),
                policy.evidence_freshness_window);
            if (!is_fresh(freshness)) {
                outcome.unplaced.push_back(id);
                outcome.diagnostics.push_back(
                    describe_obligation(id, obligation.recovery_class) +
                    " cannot fail back: capability evidence for home site is " +
                    std::string{to_string(freshness)});
                continue;
            }
            if (!capability_supports_class(capability->second, obligation.recovery_class)) {
                outcome.unplaced.push_back(id);
                outcome.diagnostics.push_back(
                    describe_obligation(id, obligation.recovery_class) +
                    " cannot fail back: home site does not claim support for its class");
                continue;
            }
            auto capacity = remaining.find(home);
            const std::uint64_t available =
                capacity == remaining.end() ? capability->second.available_capacity_units
                                            : capacity->second;
            if (available < obligation.required_capacity_units) {
                outcome.unplaced.push_back(id);
                outcome.diagnostics.push_back(
                    describe_obligation(id, obligation.recovery_class) +
                    " cannot fail back: capacity evidence for home site is smaller than required");
                continue;
            }
            remaining[home] = available - obligation.required_capacity_units;
            outcome.capacity_committed += obligation.required_capacity_units;
            destination = home;
            destination_domain = site->second.domain;
            placed = true;
        } else {
            for (const Candidate& candidate : candidates) {
                const auto capability = input.capabilities->find(candidate.site);
                if (capability == input.capabilities->end()) {
                    continue;
                }
                if (!capability_supports_class(capability->second, obligation.recovery_class)) {
                    continue;
                }
                const std::uint64_t available = remaining[candidate.site];
                if (available < obligation.required_capacity_units) {
                    continue;
                }
                remaining[candidate.site] = available - obligation.required_capacity_units;
                outcome.capacity_committed += obligation.required_capacity_units;
                destination = candidate.site;
                destination_domain = candidate.domain;
                placed = true;
                break;
            }
            if (!placed) {
                outcome.unplaced.push_back(id);
                std::string note = describe_obligation(id, obligation.recovery_class);
                note.append(candidates.empty()
                                ? " has no destination: no fresh capability evidence outside the "
                                  "affected failure domains"
                                : " has no destination: remaining fresh capacity is smaller than "
                                  "the obligation requires");
                outcome.diagnostics.push_back(std::move(note));
                continue;
            }
        }

        destination_of[id] = destination;
        const RecoveryStepId network = ensure_network_step(destination, destination_domain);
        if (input.kind == PlanKind::Failback) {
            ensure_readiness_step(destination, destination_domain);
        }

        RecoveryStep evacuate;
        evacuate.plan = input.plan_id;
        evacuate.event = input.event;
        evacuate.plan_generation = input.plan_generation;
        evacuate.event_generation = input.event_generation;
        evacuate.kind = StepKind::EvacuateSource;
        evacuate.obligation = id;
        evacuate.source = obligation.home_site;
        evacuate.destination = obligation.home_site;
        evacuate.destination_domain = obligation.home_domain;
        evacuate.recovery_class = obligation.recovery_class;
        evacuate.required_capacity_units = obligation.required_capacity_units;
        evacuate.safety_critical = is_protected_class(obligation.recovery_class);

        RecoveryStep reserve;
        reserve.plan = input.plan_id;
        reserve.event = input.event;
        reserve.plan_generation = input.plan_generation;
        reserve.event_generation = input.event_generation;
        reserve.kind = StepKind::ReserveDestination;
        reserve.obligation = id;
        reserve.destination = destination;
        reserve.destination_domain = destination_domain;
        reserve.recovery_class = obligation.recovery_class;
        reserve.required_capacity_units = obligation.required_capacity_units;
        reserve.safety_critical = is_protected_class(obligation.recovery_class);

        RecoveryStep place;
        place.plan = input.plan_id;
        place.event = input.event;
        place.plan_generation = input.plan_generation;
        place.event_generation = input.event_generation;
        place.kind = StepKind::PlaceObligation;
        place.obligation = id;
        place.destination = destination;
        place.destination_domain = destination_domain;
        place.recovery_class = obligation.recovery_class;
        place.required_capacity_units = obligation.required_capacity_units;
        place.safety_critical = is_protected_class(obligation.recovery_class);

        RecoveryStep restore;
        restore.plan = input.plan_id;
        restore.event = input.event;
        restore.plan_generation = input.plan_generation;
        restore.event_generation = input.event_generation;
        restore.kind = StepKind::RestoreAtDestination;
        restore.obligation = id;
        restore.destination = destination;
        restore.destination_domain = destination_domain;
        restore.recovery_class = obligation.recovery_class;
        restore.required_capacity_units = obligation.required_capacity_units;
        restore.safety_critical = is_protected_class(obligation.recovery_class);

        RecoveryStep execute;
        execute.plan = input.plan_id;
        execute.event = input.event;
        execute.plan_generation = input.plan_generation;
        execute.event_generation = input.event_generation;
        execute.kind = StepKind::RecoverExecution;
        execute.obligation = id;
        execute.destination = destination;
        execute.destination_domain = destination_domain;
        execute.recovery_class = obligation.recovery_class;
        execute.safety_critical = is_protected_class(obligation.recovery_class);

        RecoveryStep verify;
        verify.plan = input.plan_id;
        verify.event = input.event;
        verify.plan_generation = input.plan_generation;
        verify.event_generation = input.event_generation;
        verify.kind = StepKind::VerifyRecovery;
        verify.obligation = id;
        verify.destination = destination;
        verify.destination_domain = destination_domain;
        verify.recovery_class = obligation.recovery_class;
        verify.safety_critical = is_protected_class(obligation.recovery_class);

        if (input.kind == PlanKind::Failback) {
            const RecoveryStepId readiness = ensure_readiness_step(destination, destination_domain);
            place.depends_on.push_back(readiness);
            restore.depends_on.push_back(readiness);
        }

        if (input.kind == PlanKind::Failback) {
            // Failing back is not the recovery plan run backwards: the work
            // already runs somewhere else, so there is nothing to evacuate, and
            // the move home is its own authorized effect.
            const RecoveryStepId reserve_id = push_step(std::move(reserve));
            RecoveryStep place_final = std::move(place);
            place_final.depends_on.push_back(reserve_id);
            const RecoveryStepId place_id = push_step(std::move(place_final));

            RecoveryStep restore_final = std::move(restore);
            restore_final.depends_on.push_back(place_id);
            restore_final.depends_on.push_back(network);
            const RecoveryStepId restore_id = push_step(std::move(restore_final));

            RecoveryStep back;
            back.plan = input.plan_id;
            back.event = input.event;
            back.plan_generation = input.plan_generation;
            back.event_generation = input.event_generation;
            back.kind = StepKind::FailbackToSource;
            back.obligation = id;
            back.source = destination;
            back.destination = destination;
            back.destination_domain = destination_domain;
            back.recovery_class = obligation.recovery_class;
            back.required_capacity_units = obligation.required_capacity_units;
            back.safety_critical = is_protected_class(obligation.recovery_class);
            back.depends_on.push_back(restore_id);
            const RecoveryStepId back_id = push_step(std::move(back));

            RecoveryStep verify_final = std::move(verify);
            verify_final.depends_on.push_back(back_id);
            verify_final.depends_on.push_back(network);

            reserve_step[id] = reserve_id;
            place_step[id] = place_id;
            restore_step[id] = restore_id;
            verify_step[id] = push_step(std::move(verify_final));
        } else {
            const RecoveryStepId evacuate_id = push_step(std::move(evacuate));
            const RecoveryStepId reserve_id = push_step(std::move(reserve));
            RecoveryStep place_final = std::move(place);
            place_final.depends_on.push_back(reserve_id);
            place_final.depends_on.push_back(evacuate_id);
            const RecoveryStepId place_id = push_step(std::move(place_final));

            RecoveryStep restore_final = std::move(restore);
            restore_final.depends_on.push_back(place_id);
            restore_final.depends_on.push_back(network);
            const RecoveryStepId restore_id = push_step(std::move(restore_final));

            RecoveryStep execute_final = std::move(execute);
            execute_final.depends_on.push_back(restore_id);
            const RecoveryStepId execute_id = push_step(std::move(execute_final));

            RecoveryStep verify_final = std::move(verify);
            verify_final.depends_on.push_back(execute_id);
            verify_final.depends_on.push_back(network);

            evacuate_step[id] = evacuate_id;
            reserve_step[id] = reserve_id;
            place_step[id] = place_id;
            restore_step[id] = restore_id;
            execute_step[id] = execute_id;
            verify_step[id] = push_step(std::move(verify_final));
        }
    }

    // Dependency ordering across obligations: a dependent obligation is
    // restored only after the obligation it depends on has been verified.
    for (const ObligationId id : in_scope) {
        const auto restore = restore_step.find(id);
        if (restore == restore_step.end()) {
            continue;
        }
        for (RecoveryStep& step : steps) {
            if (step.id != restore->second) {
                continue;
            }
            for (const ObligationId dependency : input.obligations->at(id).depends_on) {
                const auto verify = verify_step.find(dependency);
                if (verify != verify_step.end()) {
                    step.depends_on.push_back(verify->second);
                }
            }
            break;
        }
    }

    // Cascade: anything that depends, directly or transitively, on an
    // obligation that could not be placed cannot be restored either. It is
    // reported as deferred instead of being planned or silently dropped.
    std::vector<ObligationId> deferred{outcome.unplaced};
    bool grew = true;
    while (grew) {
        grew = false;
        for (const ObligationId id : in_scope) {
            if (std::find(deferred.begin(), deferred.end(), id) != deferred.end()) {
                continue;
            }
            if (restore_step.find(id) == restore_step.end()) {
                continue;
            }
            for (const ObligationId dependency : input.obligations->at(id).depends_on) {
                if (std::find(deferred.begin(), deferred.end(), dependency) != deferred.end()) {
                    deferred.push_back(id);
                    grew = true;
                    break;
                }
            }
        }
    }
    for (const ObligationId id : deferred) {
        if (std::find(outcome.unplaced.begin(), outcome.unplaced.end(), id) !=
            outcome.unplaced.end()) {
            continue;
        }
        outcome.deferred.push_back(id);
        outcome.diagnostics.push_back(
            "obligation " + id.to_string() +
            " is deferred: an obligation it depends on could not be placed");
    }
    if (!outcome.deferred.empty()) {
        std::vector<RecoveryStep> kept;
        for (RecoveryStep& step : steps) {
            const bool deferred_owner =
                step.obligation.valid() &&
                std::find(outcome.deferred.begin(), outcome.deferred.end(), step.obligation) !=
                    outcome.deferred.end();
            if (!deferred_owner) {
                kept.push_back(std::move(step));
                continue;
            }
            // Remove the deferred obligation's steps and any reference to them.
            const RecoveryStepId removed = step.id;
            for (RecoveryStep& other : steps) {
                other.depends_on.erase(
                    std::remove(other.depends_on.begin(), other.depends_on.end(), removed),
                    other.depends_on.end());
            }
        }
        steps = std::move(kept);
    }

    if (policy.require_protected_placement) {
        for (const ObligationId id : outcome.unplaced) {
            const ProtectedObligation& obligation = input.obligations->at(id);
            if (is_protected_class(obligation.recovery_class)) {
                return Status{ErrorCode::Unsupported,
                              "no destination with fresh capability evidence for protected "
                              "obligation " +
                                  id.to_string()};
            }
        }
    }

    if (steps.size() > limits.max_steps_per_plan) {
        return Status{ErrorCode::Exhausted, "plan exceeds the configured step limit"};
    }

    const Status ordering = order_and_store();
    if (!ordering.ok()) {
        return ordering;
    }

    for (const ObligationId id : in_scope) {
        const auto placement = place_step.find(id);
        if (placement == place_step.end()) {
            continue;
        }
        // A deferred obligation is not moved anywhere: it has no assignment.
        if (std::find(outcome.deferred.begin(), outcome.deferred.end(), id) !=
            outcome.deferred.end()) {
            continue;
        }
        PlacementAssignment assignment;
        assignment.obligation = id;
        assignment.destination = destination_of.at(id);
        assignment.plan_generation = input.plan_generation;
        assignment.reservation_step = reserve_step.at(id);
        assignment.placement_step = placement->second;
        outcome.assignments.push_back(std::move(assignment));
    }
    outcome.digest = digest_steps(outcome.steps, outcome.assignments);
    return outcome;
}

Digest digest_steps(const std::vector<RecoveryStep>& steps,
                    const std::vector<PlacementAssignment>& assignments) {
    Writer w;
    w.sequence_count(static_cast<std::uint32_t>(steps.size()));
    for (const RecoveryStep& step : steps) {
        w.u64(step.id.value());
        w.u64(step.plan.value());
        w.u64(step.event.value());
        w.u64(step.plan_generation.value());
        w.u64(step.event_generation.value());
        w.u32(step.ordinal);
        w.u32(static_cast<std::uint32_t>(step.kind));
        w.u64(step.obligation.value());
        w.u64(step.source.value());
        w.u64(step.destination.value());
        w.u64(step.destination_domain.value());
        w.u32(static_cast<std::uint32_t>(step.recovery_class));
        w.u64(step.required_capacity_units);
        w.boolean(step.safety_critical);
        w.boolean(step.authorized);
        w.sequence_count(static_cast<std::uint32_t>(step.depends_on.size()));
        for (const RecoveryStepId dependency : step.depends_on) {
            w.u64(dependency.value());
        }
    }
    w.sequence_count(static_cast<std::uint32_t>(assignments.size()));
    for (const PlacementAssignment& assignment : assignments) {
        w.u64(assignment.obligation.value());
        w.u64(assignment.destination.value());
        w.u64(assignment.plan_generation.value());
    }
    return Digest{Sha256::hash(w.bytes())};
}

std::vector<std::uint8_t> encode_step(const RecoveryStep& step) {
    Writer w;
    w.u64(step.id.value());
    w.u64(step.plan.value());
    w.u64(step.event.value());
    w.u64(step.plan_generation.value());
    w.u64(step.event_generation.value());
    w.u32(step.ordinal);
    w.u32(static_cast<std::uint32_t>(step.kind));
    w.u64(step.obligation.value());
    w.u64(step.source.value());
    w.u64(step.destination.value());
    w.u64(step.destination_domain.value());
    w.u32(static_cast<std::uint32_t>(step.recovery_class));
    w.u64(step.required_capacity_units);
    w.boolean(step.safety_critical);
    w.boolean(step.authorized);
    w.sequence_count(static_cast<std::uint32_t>(step.depends_on.size()));
    for (const RecoveryStepId dependency : step.depends_on) {
        w.u64(dependency.value());
    }
    w.u32(static_cast<std::uint32_t>(step.state));
    w.u32(step.attempts);
    w.u64(step.last_request.value());
    w.u64(step.last_receipt.value());
    w.u64(step.last_receipt_sequence.value());
    w.text(step.detail);
    return w.take();
}

Result<RecoveryStep> decode_step(std::span<const std::uint8_t> bytes) {
    Reader r(bytes);
    RecoveryStep v;
    v.id = RecoveryStepId{r.u64()};
    v.plan = RecoveryPlanId{r.u64()};
    v.event = DisasterEventId{r.u64()};
    v.plan_generation = Generation{r.u64()};
    v.event_generation = Generation{r.u64()};
    v.ordinal = r.u32();
    const std::uint32_t kind = r.u32();
    v.obligation = ObligationId{r.u64()};
    v.source = SiteId{r.u64()};
    v.destination = SiteId{r.u64()};
    v.destination_domain = FailureDomainId{r.u64()};
    const std::uint32_t cls = r.u32();
    v.required_capacity_units = r.u64();
    v.safety_critical = r.boolean();
    v.authorized = r.boolean();
    const std::uint32_t count = r.sequence_count(Limits{}.max_dependencies_per_obligation);
    v.depends_on.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        v.depends_on.push_back(RecoveryStepId{r.u64()});
    }
    const std::uint32_t state = r.u32();
    v.attempts = r.u32();
    v.last_request = EffectRequestId{r.u64()};
    v.last_receipt = EffectReceiptId{r.u64()};
    v.last_receipt_sequence = Sequence{r.u64()};
    v.detail = std::string{r.text()};
    const Status status = finish(r, "recovery step");
    if (!status.ok()) {
        return status;
    }
    if (kind >= kStepKindCount) {
        return Status{ErrorCode::Invalid, "recovery step kind out of range"};
    }
    if (state >= kStepStateCount) {
        return Status{ErrorCode::Invalid, "recovery step state out of range"};
    }
    if (cls >= kRecoveryClassCount) {
        return Status{ErrorCode::Invalid, "recovery step recovery class out of range"};
    }
    if (!v.id.valid() || !v.plan.valid() || !v.event.valid()) {
        return Status{ErrorCode::Invalid, "recovery step identity is zero"};
    }
    v.kind = static_cast<StepKind>(kind);
    v.state = static_cast<StepState>(state);
    v.recovery_class = static_cast<RecoveryClass>(cls);
    return v;
}

std::vector<std::uint8_t> encode_step_transition(const RecoveryStep& step) {
    return encode_step(step);
}

std::vector<std::uint8_t> encode_plan(const RecoveryPlan& plan) {
    Writer w;
    w.u64(plan.id.value());
    w.u64(plan.event.value());
    w.u32(static_cast<std::uint32_t>(plan.kind));
    w.u32(static_cast<std::uint32_t>(plan.state));
    w.u64(plan.generation.value());
    w.u64(plan.event_generation.value());
    w.u64(plan.policy_generation.value());
    w.u64(plan.supersedes.value());
    w.u64(plan.created_epoch.value());
    w.i64(plan.created_at);
    w.text(plan.requested_by);
    const Sha256::DigestBytes& digest = plan.digest.bytes();
    w.blob(std::span<const std::uint8_t>(digest.data(), digest.size()));
    w.sequence_count(static_cast<std::uint32_t>(plan.order.size()));
    for (const RecoveryStepId id : plan.order) {
        w.u64(id.value());
    }
    w.sequence_count(static_cast<std::uint32_t>(plan.steps.size()));
    for (const auto& entry : plan.steps) {
        const std::vector<std::uint8_t> encoded = encode_step(entry.second);
        w.blob(encoded);
    }
    w.sequence_count(static_cast<std::uint32_t>(plan.assignments.size()));
    for (const PlacementAssignment& assignment : plan.assignments) {
        w.u64(assignment.obligation.value());
        w.u64(assignment.destination.value());
        w.u64(assignment.reservation.value());
        w.u64(assignment.placement.value());
        w.u64(assignment.plan_generation.value());
        w.u64(assignment.reservation_step.value());
        w.u64(assignment.placement_step.value());
    }
    w.sequence_count(static_cast<std::uint32_t>(plan.unplaced.size()));
    for (const ObligationId id : plan.unplaced) {
        w.u64(id.value());
    }
    w.sequence_count(static_cast<std::uint32_t>(plan.deferred.size()));
    for (const ObligationId id : plan.deferred) {
        w.u64(id.value());
    }
    w.sequence_count(static_cast<std::uint32_t>(plan.target_sites.size()));
    for (const SiteId id : plan.target_sites) {
        w.u64(id.value());
    }
    w.sequence_count(static_cast<std::uint32_t>(plan.diagnostics.size()));
    for (const std::string& note : plan.diagnostics) {
        w.text(note);
    }
    return w.take();
}

Result<RecoveryPlan> decode_plan(std::span<const std::uint8_t> bytes) {
    Reader r(bytes);
    RecoveryPlan v;
    v.id = RecoveryPlanId{r.u64()};
    v.event = DisasterEventId{r.u64()};
    const std::uint32_t kind = r.u32();
    const std::uint32_t state = r.u32();
    v.generation = Generation{r.u64()};
    v.event_generation = Generation{r.u64()};
    v.policy_generation = Generation{r.u64()};
    v.supersedes = RecoveryPlanId{r.u64()};
    v.created_epoch = Epoch{r.u64()};
    v.created_at = r.i64();
    v.requested_by = std::string{r.text()};
    const std::span<const std::uint8_t> digest = r.blob();
    if (digest.size() != 32) {
        return Status{ErrorCode::Invalid, "plan digest is not 32 bytes"};
    }
    Sha256::DigestBytes digest_bytes{};
    for (std::size_t i = 0; i < digest_bytes.size(); ++i) {
        digest_bytes[i] = digest[i];
    }
    v.digest = Digest{digest_bytes};
    const std::uint32_t order_count = r.sequence_count(Limits{}.max_steps_per_plan);
    v.order.reserve(order_count);
    for (std::uint32_t i = 0; i < order_count; ++i) {
        v.order.push_back(RecoveryStepId{r.u64()});
    }
    const std::uint32_t step_count = r.sequence_count(Limits{}.max_steps_per_plan);
    for (std::uint32_t i = 0; i < step_count; ++i) {
        const std::span<const std::uint8_t> encoded = r.blob();
        if (r.failed()) {
            return r.status();
        }
        Result<RecoveryStep> step = decode_step(encoded);
        if (!step.ok()) {
            return step.status();
        }
        RecoveryStep decoded = std::move(step).value();
        const RecoveryStepId decoded_id = decoded.id;
        v.steps.emplace(decoded_id, std::move(decoded));
    }
    const std::uint32_t assignment_count = r.sequence_count(Limits{}.max_obligations);
    v.assignments.reserve(assignment_count);
    for (std::uint32_t i = 0; i < assignment_count; ++i) {
        PlacementAssignment assignment;
        assignment.obligation = ObligationId{r.u64()};
        assignment.destination = SiteId{r.u64()};
        assignment.reservation = ReservationRef{r.u64()};
        assignment.placement = PlacementRef{r.u64()};
        assignment.plan_generation = Generation{r.u64()};
        assignment.reservation_step = RecoveryStepId{r.u64()};
        assignment.placement_step = RecoveryStepId{r.u64()};
        v.assignments.push_back(std::move(assignment));
    }
    const std::uint32_t unplaced_count = r.sequence_count(Limits{}.max_obligations);
    v.unplaced.reserve(unplaced_count);
    for (std::uint32_t i = 0; i < unplaced_count; ++i) {
        v.unplaced.push_back(ObligationId{r.u64()});
    }
    const std::uint32_t deferred_count = r.sequence_count(Limits{}.max_obligations);
    v.deferred.reserve(deferred_count);
    for (std::uint32_t i = 0; i < deferred_count; ++i) {
        v.deferred.push_back(ObligationId{r.u64()});
    }
    const std::uint32_t target_count = r.sequence_count(Limits{}.max_sites);
    v.target_sites.reserve(target_count);
    for (std::uint32_t i = 0; i < target_count; ++i) {
        v.target_sites.push_back(SiteId{r.u64()});
    }
    const std::uint32_t diagnostic_count = r.sequence_count(512);
    v.diagnostics.reserve(diagnostic_count);
    for (std::uint32_t i = 0; i < diagnostic_count; ++i) {
        v.diagnostics.emplace_back(r.text());
    }
    const Status status = finish(r, "recovery plan");
    if (!status.ok()) {
        return status;
    }
    if (kind > static_cast<std::uint32_t>(PlanKind::Restoration)) {
        return Status{ErrorCode::Invalid, "plan kind out of range"};
    }
    if (state > static_cast<std::uint32_t>(PlanState::Abandoned)) {
        return Status{ErrorCode::Invalid, "plan state out of range"};
    }
    if (!v.id.valid() || !v.event.valid()) {
        return Status{ErrorCode::Invalid, "plan identity is zero"};
    }
    if (v.order.size() != v.steps.size()) {
        return Status{ErrorCode::Invalid, "plan step order does not match its step set"};
    }
    v.kind = static_cast<PlanKind>(kind);
    v.state = static_cast<PlanState>(state);
    return v;
}

}  // namespace drc
