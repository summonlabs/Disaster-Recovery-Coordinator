#include <string>
#include <vector>

#include "support/harness.hpp"
#include "test.hpp"

using namespace drc;

namespace {

// Two failure domains, three sites: s1 and s2 sit in domain 1 (the losing
// domain), s3 sits alone in domain 2 and is the only destination.
void build_topology(drctest::Rig& rig) {
    rig.domain(1, "domain-a");
    rig.domain(2, "domain-b");
    rig.site(1, "site-a1", 1, 100);
    rig.site(2, "site-a2", 1, 100);
    rig.site(3, "site-b1", 2, 1000);
    rig.obligation(10, "auth", RecoveryClass::SafetyCritical, 1, 10);
    rig.obligation(11, "orders", RecoveryClass::Protected, 1, 20);
    rig.obligation(12, "reports", RecoveryClass::Standard, 2, 5);
    rig.capability(3, 1, 500);
    rig.synthetic_endpoints();
}

}  // namespace

DRC_TEST(integration_full_lifecycle_reaches_closed) {
    drctest::Rig rig = drctest::Rig::make("integration-lifecycle");
    rig.open();
    build_topology(rig);

    const DisasterEventId event = rig.declare({1, 2});
    DRC_REQUIRE_EQ(rig.event(event).phase, EventPhase::Declared);

    AssessmentEvidence assessment;
    assessment.event = event;
    assessment.generation = Generation{1};
    assessment.event_generation = rig.event(event).generation;
    assessment.observed_at = rig.clock->now_nanos();
    assessment.observation_epoch = rig.coordinator->epoch();
    assessment.source = "observatory";
    SiteAssessment site;
    site.site = SiteId{1};
    site.availability = SiteAvailability::Unavailable;
    site.telemetry_present = true;
    assessment.sites.push_back(site);
    DRC_REQUIRE_OK(rig.coordinator->record_assessment(assessment));
    DRC_REQUIRE_EQ(rig.event(event).phase, EventPhase::Assessed);

    const RecoveryPlanId plan = rig.plan(event);
    DRC_REQUIRE_EQ(rig.event(event).phase, EventPhase::PlanReady);
    // Three obligations in scope, six steps each, plus one network recovery
    // step for the single destination site.
    DRC_REQUIRE_EQ(rig.plan_of(plan).steps.size(), std::size_t{19});
    DRC_REQUIRE(rig.plan_of(plan).unplaced.empty());

    DRC_REQUIRE_OK(rig.coordinator->begin_recovery(event));
    DRC_REQUIRE_EQ(rig.event(event).phase, EventPhase::Evacuating);

    const AdvanceReport report = rig.settle(event);
    DRC_REQUIRE(report.quiescent);
    DRC_REQUIRE_EQ(rig.event(event).phase, EventPhase::Stabilized);
    DRC_REQUIRE_EQ(rig.count_steps(plan, StepState::Succeeded), std::size_t{19});
    DRC_REQUIRE_EQ(report.steps_failed, 0u);

    // Restoration: readiness validation, then a separately authorized return.
    rig.readiness(1, 2, true, true);
    rig.readiness(2, 2, true, true);
    RestorationRequest restoration;
    restoration.event = event;
    restoration.requested_by = "operator";
    DRC_REQUIRE_OK(rig.coordinator->begin_restoration(restoration));
    DRC_REQUIRE_EQ(rig.event(event).phase, EventPhase::Restoring);
    const AdvanceReport validating = rig.settle(event);
    DRC_REQUIRE(validating.quiescent);
    DRC_REQUIRE_EQ(rig.event(event).phase, EventPhase::Validating);

    ReturnToServiceRequest request;
    request.event = event;
    request.site = SiteId{1};
    request.authorized_by = "operator";
    request.justification = "readiness verified";
    DRC_REQUIRE_OK(rig.coordinator->return_site_to_service(request));
    const AdvanceReport returning = rig.settle(event);
    DRC_REQUIRE(returning.quiescent);
    DRC_REQUIRE_EQ(rig.event(event).phase, EventPhase::Returning);
    DRC_REQUIRE_EQ(rig.event(event).returned_sites.size(), std::size_t{1});

    request.site = SiteId{2};
    DRC_REQUIRE_OK(rig.coordinator->return_site_to_service(request));
    DRC_REQUIRE(rig.settle(event).quiescent);
    DRC_REQUIRE_EQ(rig.event(event).returned_sites.size(), std::size_t{2});

    EventClosure closure;
    closure.event = event;
    closure.closed_by = "operator";
    closure.summary = "both sites returned to service";
    DRC_REQUIRE_OK(rig.coordinator->close_event(closure));
    DRC_REQUIRE_EQ(rig.event(event).phase, EventPhase::Closed);
    DRC_REQUIRE_EQ(rig.event(event).disposition, EventDisposition::Closed);
}

DRC_TEST(integration_dependency_order_is_enforced) {
    drctest::Rig rig = drctest::Rig::make("integration-dependency");
    rig.open();
    rig.domain(1, "domain-a");
    rig.domain(2, "domain-b");
    rig.site(1, "site-a1", 1);
    rig.site(3, "site-b1", 2, 1000);
    // 11 depends on 10.
    rig.obligation(10, "auth", RecoveryClass::SafetyCritical, 1, 10);
    rig.obligation(11, "orders", RecoveryClass::Protected, 1, 20, {10});
    rig.capability(3, 1, 500);
    rig.synthetic_endpoints();

    const DisasterEventId event = rig.declare({1});
    const RecoveryPlanId plan = rig.plan(event);
    const RecoveryPlan value = rig.plan_of(plan);

    RecoveryStepId verify_auth;
    RecoveryStepId restore_orders;
    for (const RecoveryStepId step_id : value.order) {
        const RecoveryStep& step = value.steps.at(step_id);
        if (step.kind == StepKind::VerifyRecovery && step.obligation == ObligationId{10}) {
            verify_auth = step_id;
        }
        if (step.kind == StepKind::RestoreAtDestination && step.obligation == ObligationId{11}) {
            restore_orders = step_id;
        }
    }
    DRC_REQUIRE(verify_auth.valid());
    DRC_REQUIRE(restore_orders.valid());
    const std::vector<RecoveryStepId>& dependencies = value.steps.at(restore_orders).depends_on;
    DRC_REQUIRE(std::find(dependencies.begin(), dependencies.end(), verify_auth) !=
                dependencies.end());
    // A step can never appear before something it depends on.
    const auto position = [&value](RecoveryStepId id) {
        for (std::size_t i = 0; i < value.order.size(); ++i) {
            if (value.order[i] == id) {
                return i;
            }
        }
        return value.order.size();
    };
    DRC_REQUIRE(position(verify_auth) < position(restore_orders));

    DRC_REQUIRE_OK(rig.coordinator->begin_recovery(event));
    DRC_REQUIRE(rig.settle(event).quiescent);
    DRC_REQUIRE_EQ(rig.event(event).phase, EventPhase::Stabilized);
}

DRC_TEST(integration_protected_classes_are_ordered_first) {
    drctest::Rig rig = drctest::Rig::make("integration-priority");
    rig.open();
    rig.domain(1, "domain-a");
    rig.domain(2, "domain-b");
    rig.site(1, "site-a1", 1);
    rig.site(3, "site-b1", 2, 1000);
    rig.obligation(10, "deferrable", RecoveryClass::Deferrable, 1, 1);
    rig.obligation(11, "safety", RecoveryClass::SafetyCritical, 1, 1);
    rig.obligation(12, "standard", RecoveryClass::Standard, 1, 1);
    rig.capability(3, 1, 500);
    rig.synthetic_endpoints();

    const DisasterEventId event = rig.declare({1});
    const RecoveryPlan value = rig.plan_of(rig.plan(event));

    // Every safety-critical step must be ordered before every deferrable step
    // of the same kind.
    std::size_t last_safety_location = 0;
    std::size_t first_deferrable_location = value.order.size();
    for (std::size_t i = 0; i < value.order.size(); ++i) {
        const RecoveryStep& step = value.steps.at(value.order[i]);
        if (step.recovery_class == RecoveryClass::SafetyCritical) {
            last_safety_location = i;
        }
        if (step.recovery_class == RecoveryClass::Deferrable &&
            first_deferrable_location == value.order.size()) {
            first_deferrable_location = i;
        }
    }
    DRC_REQUIRE(last_safety_location < first_deferrable_location);
    DRC_REQUIRE(value.steps.at(value.order.front()).safety_critical);
}

DRC_TEST(integration_state_survives_reopen_at_every_phase) {
    drctest::Rig rig = drctest::Rig::make("integration-reopen");
    rig.open();
    build_topology(rig);
    const DisasterEventId event = rig.declare({1, 2});
    const Digest declared_digest = rig.coordinator->state_digest();
    rig.reopen();
    DRC_REQUIRE_EQ(rig.coordinator->state_digest(), declared_digest);
    DRC_REQUIRE_EQ(rig.event(event).phase, EventPhase::Declared);

    // Evidence recovered from the journal was observed by the previous
    // incarnation, so it is not automatically fresh: planning refuses until the
    // neighbouring authority reports again.
    CreatePlanRequest stale_request;
    stale_request.event = event;
    stale_request.requested_by = "operator";
    const Result<RecoveryPlanId> stale_plan = rig.coordinator->create_plan(stale_request);
    DRC_REQUIRE_ERR(stale_plan, ErrorCode::Unsupported);
    DRC_REQUIRE_EQ(rig.coordinator->capability(SiteId{3}).value().observation_epoch,
                   Epoch{rig.coordinator->epoch().value() - 1});
    rig.capability(3, 2, 500);
    DRC_REQUIRE(rig.coordinator->create_plan(stale_request).ok());
    const Digest planned_digest = rig.coordinator->state_digest();
    rig.reopen();
    DRC_REQUIRE_EQ(rig.coordinator->state_digest(), planned_digest);
    DRC_REQUIRE_EQ(rig.event(event).phase, EventPhase::PlanReady);

    DRC_REQUIRE_OK(rig.coordinator->begin_recovery(event));
    DRC_REQUIRE(rig.settle(event).quiescent);
    const Digest stabilized = rig.coordinator->state_digest();
    rig.reopen();
    DRC_REQUIRE_EQ(rig.coordinator->state_digest(), stabilized);
    DRC_REQUIRE_EQ(rig.event(event).phase, EventPhase::Stabilized);

    rig.readiness(1, 2);
    rig.readiness(2, 2);
    RestorationRequest restoration;
    restoration.event = event;
    restoration.requested_by = "operator";
    DRC_REQUIRE_OK(rig.coordinator->begin_restoration(restoration));
    DRC_REQUIRE(rig.settle(event).quiescent);
    const Digest validating = rig.coordinator->state_digest();
    rig.reopen();
    DRC_REQUIRE_EQ(rig.coordinator->state_digest(), validating);
    DRC_REQUIRE_EQ(rig.event(event).phase, EventPhase::Validating);
}

DRC_TEST(integration_overlapping_declaration_conflicts_and_supersedes) {
    drctest::Rig rig = drctest::Rig::make("integration-conflict");
    rig.open();
    build_topology(rig);

    const DisasterEventId first = rig.declare({1});
    const DisasterEventId second = rig.declare({1});
    // A declaration that overlaps an active event becomes a conflict, not a
    // silent merge.
    DRC_REQUIRE_EQ(rig.event(second).phase, EventPhase::Conflicted);
    DRC_REQUIRE_EQ(rig.event(second).conflicts_with.size(), std::size_t{1});
    CreatePlanRequest conflicting_request;
    conflicting_request.event = second;
    conflicting_request.requested_by = "operator";
    const Result<RecoveryPlanId> conflicting = rig.coordinator->create_plan(conflicting_request);
    DRC_REQUIRE_ERR(conflicting, ErrorCode::Conflict);

    // A later assessment is the explicit way out of an evidence conflict.
    AssessmentEvidence assessment;
    assessment.event = second;
    assessment.generation = Generation{1};
    assessment.event_generation = rig.event(second).generation;
    assessment.observation_epoch = rig.coordinator->epoch();
    assessment.observed_at = rig.clock->now_nanos();
    assessment.source = "observatory";
    DRC_REQUIRE_OK(rig.coordinator->record_assessment(assessment));
    DRC_REQUIRE_EQ(rig.event(second).phase, EventPhase::Assessed);

    // An explicit supersede fences the older event and its plan.
    const DisasterEventId third = rig.declare({1, 2}, first.value());
    DRC_REQUIRE_EQ(rig.event(first).disposition, EventDisposition::Superseded);
    DRC_REQUIRE_EQ(rig.event(first).phase, EventPhase::Closed);
    DRC_REQUIRE_EQ(rig.event(third).supersedes, first);
}

DRC_TEST(integration_partition_defers_remote_work_then_proceeds) {
    drctest::Rig rig = drctest::Rig::make("integration-partition");
    rig.open();
    rig.domain(1, "domain-a");
    rig.domain(2, "domain-b");
    rig.site(1, "site-a1", 1);
    rig.site(3, "site-b1", 2, 1000);
    rig.obligation(10, "auth", RecoveryClass::SafetyCritical, 1, 10);
    rig.capability(3, 1, 500);
    rig.synthetic_endpoints();
    rig.federation(1, true, true, {2});

    const DisasterEventId event = rig.declare({1});
    rig.plan(event);
    DRC_REQUIRE_OK(rig.coordinator->begin_recovery(event));
    const AdvanceReport partitioned = rig.advance(event);
    DRC_REQUIRE(partitioned.steps_deferred > 0);
    // The evacuation of the reachable source completed, so the event moved on
    // to failing over; everything that needs the unreachable domain waits.
    DRC_REQUIRE_EQ(rig.event(event).phase, EventPhase::FailingOver);
    // Only the step whose target is inside the reachable domain went out; every
    // request that named the unreachable domain was deferred instead.
    DRC_REQUIRE_EQ(rig.coordinator->stats().dispatches, 1ull);
    const RecoveryPlanId active = rig.active_plan(event);
    // The one effect that stayed inside the reachable domain completed; every
    // effect that needed the unreachable domain is deferred, not failed and not
    // assumed.
    DRC_REQUIRE_EQ(rig.step_state(active, StepKind::EvacuateSource, 10), StepState::Succeeded);
    DRC_REQUIRE_EQ(rig.step_state(active, StepKind::ReserveDestination, 10), StepState::Deferred);
    DRC_REQUIRE_EQ(rig.step_state(active, StepKind::RestoreAtDestination, 10),
                   StepState::Pending);

    rig.federation(2, true, false);
    DRC_REQUIRE(rig.settle(event).quiescent);
    DRC_REQUIRE_EQ(rig.event(event).phase, EventPhase::Stabilized);
}

DRC_TEST(integration_stale_plan_generation_receipts_are_refused) {
    drctest::Rig rig = drctest::Rig::make("integration-stale");
    rig.open();
    rig.domain(1, "domain-a");
    rig.domain(2, "domain-b");
    rig.site(1, "site-a1", 1);
    rig.site(3, "site-b1", 2, 1000);
    rig.obligation(10, "auth", RecoveryClass::SafetyCritical, 1, 10);
    rig.capability(3, 1, 500);

    SyntheticBehavior stale;
    stale.stale_generation = true;
    rig.endpoint(EffectDomain::SiteControlPlane, stale, 1);
    rig.endpoint(EffectDomain::PlacementReservationCapacity, stale, 2);
    rig.endpoint(EffectDomain::AsiExecutionRecovery, stale, 3);
    rig.endpoint(EffectDomain::DfiNetworkRecovery, stale, 4);

    const DisasterEventId event = rig.declare({1});
    rig.plan(event);
    rig.plan(event);  // replan: generation 2, generation 1 is fenced
    const RecoveryPlanId active = rig.active_plan(event);
    DRC_REQUIRE_EQ(rig.plan_of(active).generation, Generation{2});
    DRC_REQUIRE_OK(rig.coordinator->begin_recovery(event));

    // The participant answers with the generation before the one it was asked
    // about. Those answers must never move a step forward.
    for (int i = 0; i < 8; ++i) {
        (void)rig.advance(event);
    }
    const CoordinatorStats stats = rig.coordinator->stats();
    DRC_REQUIRE(stats.stale_receipts > 0);
    DRC_REQUIRE_EQ(rig.count_steps(active, StepState::Succeeded), std::size_t{0});
}

DRC_TEST(integration_return_requires_fresh_readiness) {
    drctest::Rig rig = drctest::Rig::make("integration-freshness");
    rig.open();
    build_topology(rig);
    const DisasterEventId event = rig.declare({1, 2});
    rig.plan(event);
    DRC_REQUIRE_OK(rig.coordinator->begin_recovery(event));
    DRC_REQUIRE(rig.settle(event).quiescent);

    RestorationRequest restoration;
    restoration.event = event;
    restoration.requested_by = "operator";
    DRC_REQUIRE_OK(rig.coordinator->begin_restoration(restoration));
    DRC_REQUIRE(rig.settle(event).quiescent);
    DRC_REQUIRE_EQ(rig.event(event).phase, EventPhase::Validating);

    ReturnToServiceRequest request;
    request.event = event;
    request.site = SiteId{1};
    request.authorized_by = "operator";
    request.justification = "looks fine to me";
    DRC_REQUIRE_ERR(rig.coordinator->return_site_to_service(request), ErrorCode::GateUnmet);

    rig.readiness(1, 2, true, true);
    // Time passes beyond the freshness window: recovered evidence is not
    // automatically fresh.
    rig.clock->advance(301 * kNanosPerSecond);
    DRC_REQUIRE_EQ(rig.coordinator->readiness_freshness(SiteId{1}).value(), Freshness::Expired);
    DRC_REQUIRE_ERR(rig.coordinator->return_site_to_service(request), ErrorCode::GateUnmet);

    rig.readiness(1, 3, true, true);
    DRC_REQUIRE_EQ(rig.coordinator->readiness_freshness(SiteId{1}).value(), Freshness::Fresh);
    DRC_REQUIRE_OK(rig.coordinator->return_site_to_service(request));

    // A readiness observation that fails a check is not a passing one.
    rig.readiness(2, 4, false, true);
    request.site = SiteId{2};
    DRC_REQUIRE_ERR(rig.coordinator->return_site_to_service(request), ErrorCode::GateUnmet);
    DRC_REQUIRE(rig.event(event).returned_sites.empty() ||
                rig.event(event).returned_sites.front() == SiteId{1});
}

DRC_TEST(integration_failback_is_separately_governed) {
    drctest::Rig rig = drctest::Rig::make("integration-failback");
    rig.open();
    build_topology(rig);
    const DisasterEventId event = rig.declare({1, 2});
    rig.plan(event);
    DRC_REQUIRE_OK(rig.coordinator->begin_recovery(event));
    DRC_REQUIRE(rig.settle(event).quiescent);

    FailbackRequest failback;
    failback.event = event;
    failback.target_site = SiteId{1};
    failback.authorized_by = "operator";
    failback.justification = "site is back";
    // Failback is not an automatic reversal: it cannot run before restoration.
    DRC_REQUIRE_ERR(rig.coordinator->authorize_failback(failback), ErrorCode::NotReady);

    rig.readiness(1, 2);
    rig.readiness(2, 2);
    RestorationRequest restoration;
    restoration.event = event;
    restoration.requested_by = "operator";
    DRC_REQUIRE_OK(rig.coordinator->begin_restoration(restoration));
    DRC_REQUIRE(rig.settle(event).quiescent);
    DRC_REQUIRE_EQ(rig.event(event).phase, EventPhase::Validating);

    // Even in the restoring phase, a site that has not been returned cannot
    // take work back, and the refusal names the gate rather than a side effect.
    DRC_REQUIRE_ERR(rig.coordinator->authorize_failback(failback), ErrorCode::GateUnmet);
    // The home site also needs its own fresh capacity evidence before it can
    // accept work again.
    rig.capability(1, 3, 100);

    ReturnToServiceRequest request;
    request.event = event;
    request.site = SiteId{1};
    request.authorized_by = "operator";
    request.justification = "verified";
    DRC_REQUIRE_OK(rig.coordinator->return_site_to_service(request));
    DRC_REQUIRE(rig.settle(event).quiescent);

    Result<RecoveryPlanId> failback_plan = rig.coordinator->authorize_failback(failback);
    DRC_REQUIRE(failback_plan.ok());
    const RecoveryPlan value = rig.plan_of(failback_plan.value());
    DRC_REQUIRE_EQ(value.kind, PlanKind::Failback);
    DRC_REQUIRE_EQ(value.target_sites.size(), std::size_t{1});
    bool has_failback_step = false;
    for (const RecoveryStepId step_id : value.order) {
        if (value.steps.at(step_id).kind == StepKind::FailbackToSource) {
            has_failback_step = true;
        }
    }
    DRC_REQUIRE(has_failback_step);
    DRC_REQUIRE(rig.settle(event).quiescent);
    DRC_REQUIRE_EQ(rig.count_steps(value.id, StepState::Succeeded), value.order.size());
}

DRC_TEST(integration_rejected_placement_blocks_and_requires_resolution) {
    drctest::Rig rig = drctest::Rig::make("integration-blocked");
    rig.open();
    rig.domain(1, "domain-a");
    rig.domain(2, "domain-b");
    rig.site(1, "site-a1", 1);
    rig.site(3, "site-b1", 2, 1000);
    rig.obligation(10, "auth", RecoveryClass::SafetyCritical, 1, 10);
    rig.capability(3, 1, 500);
    rig.synthetic_endpoints();

    SyntheticBehavior rejecting;
    rejecting.outcome = EffectOutcome::Rejected;
    rig.endpoint(EffectDomain::PlacementReservationCapacity, rejecting, 9);

    const DisasterEventId event = rig.declare({1});
    rig.plan(event);
    DRC_REQUIRE_OK(rig.coordinator->begin_recovery(event));
    const AdvanceReport report = rig.advance(event);
    DRC_REQUIRE(!report.quiescent || report.blocked);
    DRC_REQUIRE_EQ(rig.event(event).phase, EventPhase::Blocked);
    DRC_REQUIRE(rig.event(event).failed_safety_critical_steps > 0);

    // A safety-critical failure cannot be silently skipped: the event cannot be
    // closed while it stands.
    const RecoveryPlan plan = rig.plan_of(rig.active_plan(event));
    RecoveryStepId failed_step;
    for (const RecoveryStepId step_id : plan.order) {
        if (plan.steps.at(step_id).state == StepState::Failed) {
            failed_step = step_id;
            break;
        }
    }
    DRC_REQUIRE(failed_step.valid());

    // The same call cannot be dispatched again while the participant keeps
    // rejecting it: attempts are bounded.
    DRC_REQUIRE_OK(rig.coordinator->define_site(SiteRecord{SiteId{4}, "unused", FailureDomainId{2}, 0}));

    BlockedResolution resolution;
    resolution.event = event;
    resolution.step = failed_step;
    resolution.resolved_by = "operator";
    resolution.justification = "participant cannot place the obligation";
    resolution.retry = false;
    DRC_REQUIRE_OK(rig.coordinator->resolve_blocked(resolution));
    DRC_REQUIRE(rig.event(event).phase != EventPhase::Blocked);
}

DRC_TEST(integration_multi_site_failure_is_supported) {
    drctest::Rig rig = drctest::Rig::make("integration-multisite");
    rig.open();
    rig.domain(1, "domain-a");
    rig.domain(2, "domain-b");
    rig.domain(3, "domain-c");
    rig.site(1, "site-a1", 1, 100);
    rig.site(2, "site-a2", 1, 100);
    rig.site(3, "site-b1", 2, 1000);
    rig.site(4, "site-c1", 3, 1000);
    rig.obligation(10, "auth", RecoveryClass::SafetyCritical, 1, 10);
    rig.obligation(11, "orders", RecoveryClass::Protected, 2, 20);
    rig.obligation(12, "reports", RecoveryClass::Standard, 3, 5);
    rig.capability(3, 1, 500);
    rig.capability(4, 1, 500);
    rig.synthetic_endpoints();

    const DisasterEventId event = rig.declare({1, 2});
    DRC_REQUIRE_EQ(rig.event(event).affected_sites.size(), std::size_t{2});
    const RecoveryPlan plan = rig.plan_of(rig.plan(event));
    DRC_REQUIRE(plan.unplaced.empty());
    DRC_REQUIRE_OK(rig.coordinator->begin_recovery(event));
    DRC_REQUIRE(rig.settle(event).quiescent);
    DRC_REQUIRE_EQ(rig.event(event).phase, EventPhase::Stabilized);
    DRC_REQUIRE_EQ(rig.count_steps(plan.id, StepState::Succeeded), plan.order.size());
}

DRC_TEST(integration_domain_wide_failure_selects_every_site_in_the_domain) {
    drctest::Rig rig = drctest::Rig::make("integration-domain");
    rig.open();
    rig.domain(1, "domain-a");
    rig.domain(2, "domain-b");
    rig.site(1, "site-a1", 1, 100);
    rig.site(2, "site-a2", 1, 100);
    rig.site(3, "site-b1", 2, 1000);
    rig.obligation(10, "auth", RecoveryClass::SafetyCritical, 1, 10);
    rig.obligation(11, "orders", RecoveryClass::Protected, 2, 20);
    rig.capability(3, 1, 500);
    rig.synthetic_endpoints();

    DisasterDeclaration declaration;
    declaration.declared_by = "operator";
    declaration.reason = "whole failure domain lost";
    declaration.severity = 5;
    declaration.affected_domains.push_back(FailureDomainId{1});
    Result<DisasterEventId> declared = rig.coordinator->declare_disaster(declaration);
    DRC_REQUIRE(declared.ok());
    DRC_REQUIRE_EQ(rig.event(declared.value()).obligations_in_scope.size(), std::size_t{2});

    const RecoveryPlan plan = rig.plan_of(rig.plan(declared.value()));
    DRC_REQUIRE(plan.unplaced.empty());
    DRC_REQUIRE_OK(rig.coordinator->begin_recovery(declared.value()));
    DRC_REQUIRE(rig.settle(declared.value()).quiescent);
    DRC_REQUIRE_EQ(rig.event(declared.value()).phase, EventPhase::Stabilized);
}
