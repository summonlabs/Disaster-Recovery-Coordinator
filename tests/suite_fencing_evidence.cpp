// Fencing suite: generations, epochs, and stale authority. Evidence, policy,
// receipts, and locks are all fenced values; a stale claim must never move
// authoritative state, and it must never be dropped in silence.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "drc/canonical.hpp"
#include "drc/fileio.hpp"
#include "drc/journal.hpp"
#include "support/harness.hpp"
#include "test.hpp"

using namespace drc;

namespace {

constexpr std::uint64_t kMaxJournalBytes = 64ull * 1024ull * 1024ull;

void note(const std::string& message) {
    std::cout << "    note: " << message << "\n";
    std::cout.flush();
}

[[nodiscard]] std::uint64_t bytes_of(const std::string& path) {
    Result<std::uint64_t> size = fileio::file_size(path);
    DRC_REQUIRE(size.ok());
    return size.value();
}

// A recovery that is only reachable through a successful dispatch: one losing
// site, one destination with fresh capacity, one safety-critical obligation.
void build_recovery_topology(drctest::Rig& rig) {
    rig.domain(1, "domain-a");
    rig.domain(2, "domain-b");
    rig.site(1, "site-a1", 1, 100);
    rig.site(3, "site-b1", 2, 1000);
    rig.obligation(10, "auth", RecoveryClass::SafetyCritical, 1, 10);
    rig.capability(3, 1, 500);
}

// Installs one synthetic endpoint per effect domain with the given behaviour.
void install_endpoints(drctest::Rig& rig, const SyntheticBehavior& behavior) {
    rig.endpoint(EffectDomain::SiteControlPlane, behavior, 1);
    rig.endpoint(EffectDomain::PlacementReservationCapacity, behavior, 2);
    rig.endpoint(EffectDomain::AsiExecutionRecovery, behavior, 3);
    rig.endpoint(EffectDomain::DfiNetworkRecovery, behavior, 4);
}

// A step that was dispatched must have settled, and no step may ever be
// reported as having succeeded.
void require_never_succeeded(drctest::Rig& rig,
                             RecoveryPlanId plan_id,
                             const std::string& label) {
    const RecoveryPlan plan = rig.plan_of(plan_id);
    std::size_t succeeded = 0;
    std::size_t failed = 0;
    std::size_t indeterminate = 0;
    std::size_t deferred = 0;
    std::size_t in_flight = 0;
    std::size_t pending = 0;
    std::size_t dispatched_but_pending = 0;
    for (const RecoveryStepId step_id : plan.order) {
        const RecoveryStep& step = plan.steps.at(step_id);
        switch (step.state) {
            case StepState::Succeeded: succeeded += 1; break;
            case StepState::Failed: failed += 1; break;
            case StepState::Indeterminate: indeterminate += 1; break;
            case StepState::Deferred: deferred += 1; break;
            case StepState::InFlight: in_flight += 1; break;
            case StepState::Pending: pending += 1; break;
            default: break;
        }
        if (step.attempts > 0 && step.state == StepState::Pending) {
            dispatched_but_pending += 1;
        }
    }
    note(label + ": steps=" + std::to_string(plan.order.size()) +
         " succeeded=" + std::to_string(succeeded) + " failed=" + std::to_string(failed) +
         " indeterminate=" + std::to_string(indeterminate) +
         " deferred=" + std::to_string(deferred) + " in_flight=" + std::to_string(in_flight) +
         " pending=" + std::to_string(pending));
    if (succeeded != 0) {
        note(label + ": a step that only ever saw fenced receipts reached Succeeded");
        DRC_REQUIRE(false);
    }
    DRC_REQUIRE_EQ(in_flight, std::size_t{0});
    DRC_REQUIRE_EQ(dispatched_but_pending, std::size_t{0});
    DRC_REQUIRE(failed + indeterminate > 0);
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. Generations fence capability and readiness evidence.
// ---------------------------------------------------------------------------
DRC_TEST(fencing_stale_evidence_generations_are_refused) {
    drctest::Rig rig = drctest::Rig::make("fencing-generations");
    rig.open();
    rig.domain(1, "domain-a");
    rig.domain(2, "domain-b");
    rig.site(1, "site-a1", 1, 100);
    rig.site(3, "site-b1", 2, 1000);
    rig.capability(3, 5, 500);
    rig.readiness(3, 5, true, true);

    const DestinationCapability retained = rig.coordinator->capability(SiteId{3}).value();
    DRC_REQUIRE_EQ(retained.generation, Generation{5});
    DRC_REQUIRE_EQ(retained.available_capacity_units, 500ull);

    DestinationCapability older;
    older.site = SiteId{3};
    older.generation = Generation{4};
    older.available_capacity_units = 400;
    older.supported_classes_mask = drctest::all_classes();
    older.observed_at = rig.clock->now_nanos();
    older.observation_epoch = rig.coordinator->epoch();
    older.source = "test-harness";
    DRC_REQUIRE_ERR(rig.coordinator->record_capability(older), ErrorCode::Stale);
    const DestinationCapability after_stale = rig.coordinator->capability(SiteId{3}).value();
    DRC_REQUIRE_EQ(after_stale.generation, retained.generation);
    DRC_REQUIRE_EQ(after_stale.available_capacity_units, retained.available_capacity_units);
    DRC_REQUIRE_EQ(after_stale.supported_classes_mask, retained.supported_classes_mask);

    DestinationCapability contradictory = older;
    contradictory.generation = Generation{5};
    contradictory.available_capacity_units = 600;
    DRC_REQUIRE_ERR(rig.coordinator->record_capability(contradictory), ErrorCode::Conflict);
    const DestinationCapability after_contradiction = rig.coordinator->capability(SiteId{3}).value();
    DRC_REQUIRE_EQ(after_contradiction.available_capacity_units, 500ull);

    // The same generation and the same content is idempotent, not a conflict.
    DRC_REQUIRE_OK(rig.coordinator->record_capability(retained));

    rig.capability(3, 6, 700);
    const DestinationCapability newer_capability = rig.coordinator->capability(SiteId{3}).value();
    DRC_REQUIRE_EQ(newer_capability.generation, Generation{6});
    DRC_REQUIRE_EQ(newer_capability.available_capacity_units, 700ull);

    // Readiness follows exactly the same rules.
    const SiteReadinessEvidence retained_readiness = rig.coordinator->readiness(SiteId{3}).value();
    DRC_REQUIRE_EQ(retained_readiness.generation, Generation{5});
    SiteReadinessEvidence older_readiness = retained_readiness;
    older_readiness.generation = Generation{4};
    older_readiness.checks.clear();
    older_readiness.checks.push_back(ReadinessCheck{"power", false});
    DRC_REQUIRE_ERR(rig.coordinator->record_readiness(older_readiness), ErrorCode::Stale);
    const SiteReadinessEvidence after_stale_readiness =
        rig.coordinator->readiness(SiteId{3}).value();
    DRC_REQUIRE_EQ(after_stale_readiness.generation, Generation{5});
    DRC_REQUIRE_EQ(after_stale_readiness.checks.size(), std::size_t{2});

    SiteReadinessEvidence contradictory_readiness = retained_readiness;
    contradictory_readiness.checks.clear();
    contradictory_readiness.checks.push_back(ReadinessCheck{"power", false});
    DRC_REQUIRE_ERR(rig.coordinator->record_readiness(contradictory_readiness),
                    ErrorCode::Conflict);
    const SiteReadinessEvidence after_conflict_readiness =
        rig.coordinator->readiness(SiteId{3}).value();
    DRC_REQUIRE_EQ(after_conflict_readiness.checks.at(0).passed, true);
    DRC_REQUIRE_OK(rig.coordinator->record_readiness(retained_readiness));

    rig.readiness(3, 6, true, true);
    const SiteReadinessEvidence newer_readiness = rig.coordinator->readiness(SiteId{3}).value();
    DRC_REQUIRE_EQ(newer_readiness.generation, Generation{6});

    // Every refusal is durable evidence, not a silent drop.
    const std::vector<RejectedEvidence> rejected = rig.coordinator->rejected_evidence().value();
    bool stale_capability = false;
    bool conflict_capability = false;
    bool stale_readiness = false;
    bool conflict_readiness = false;
    for (const RejectedEvidence& entry : rejected) {
        if (entry.site != SiteId{3}) {
            continue;
        }
        if (entry.kind == EvidenceKind::Capability && entry.reason == ErrorCode::Stale &&
            entry.offered_generation == Generation{4} && entry.retained_generation == Generation{5}) {
            stale_capability = true;
        }
        if (entry.kind == EvidenceKind::Capability && entry.reason == ErrorCode::Conflict &&
            entry.offered_generation == Generation{5} && entry.retained_generation == Generation{5}) {
            conflict_capability = true;
        }
        if (entry.kind == EvidenceKind::Readiness && entry.reason == ErrorCode::Stale &&
            entry.offered_generation == Generation{4} && entry.retained_generation == Generation{5}) {
            stale_readiness = true;
        }
        if (entry.kind == EvidenceKind::Readiness && entry.reason == ErrorCode::Conflict &&
            entry.offered_generation == Generation{5} && entry.retained_generation == Generation{5}) {
            conflict_readiness = true;
        }
    }
    note("rejected evidence entries = " + std::to_string(rejected.size()) +
         " (stale capability " + std::string{stale_capability ? "yes" : "no"} +
         ", conflicting capability " + std::string{conflict_capability ? "yes" : "no"} +
         ", stale readiness " + std::string{stale_readiness ? "yes" : "no"} +
         ", conflicting readiness " + std::string{conflict_readiness ? "yes" : "no"} + ")");
    DRC_REQUIRE(stale_capability);
    DRC_REQUIRE(conflict_capability);
    DRC_REQUIRE(stale_readiness);
    DRC_REQUIRE(conflict_readiness);

    // ... and it survives a restart.
    rig.reopen();
    const std::vector<RejectedEvidence> durable = rig.coordinator->rejected_evidence().value();
    std::size_t matched = 0;
    for (const RejectedEvidence& entry : durable) {
        if (entry.reason == ErrorCode::Stale || entry.reason == ErrorCode::Conflict) {
            matched += 1;
        }
    }
    DRC_REQUIRE_EQ(matched, std::size_t{4});
}

// ---------------------------------------------------------------------------
// 2. Contradictory evidence moves the event to Conflicted, and only newer
//    evidence clears it.
// ---------------------------------------------------------------------------
DRC_TEST(fencing_contradictory_evidence_moves_the_event_to_conflicted) {
    drctest::Rig rig = drctest::Rig::make("fencing-conflict");
    rig.open();
    build_recovery_topology(rig);
    rig.synthetic_endpoints();
    const DisasterEventId event = rig.declare({1});
    rig.readiness(1, 5, true, true);

    SiteReadinessEvidence contradiction;
    contradiction.site = SiteId{1};
    contradiction.generation = Generation{5};
    contradiction.observed_at = rig.clock->now_nanos();
    contradiction.observation_epoch = rig.coordinator->epoch();
    contradiction.source = "rival-observatory";
    contradiction.checks.push_back(ReadinessCheck{"power", false});
    contradiction.checks.push_back(ReadinessCheck{"network", true});
    DRC_REQUIRE_ERR(rig.coordinator->record_readiness(contradiction), ErrorCode::Conflict);

    const DisasterEvent conflicted = rig.event(event);
    if (conflicted.phase != EventPhase::Conflicted) {
        note("the event phase after contradictory readiness is " +
             std::string{to_string(conflicted.phase)} + ": " + conflicted.status_detail);
    }
    DRC_REQUIRE_EQ(conflicted.phase, EventPhase::Conflicted);
    DRC_REQUIRE_EQ(conflicted.phase_before_interruption, EventPhase::Declared);
    DRC_REQUIRE_EQ(conflicted.conflicting_sites.size(), std::size_t{1});
    DRC_REQUIRE_EQ(conflicted.conflicting_sites.front(), SiteId{1});

    // Nothing is dispatched while the evidence is contradictory.
    const CoordinatorStats before = rig.coordinator->stats();
    const AdvanceReport report = rig.advance(event);
    DRC_REQUIRE(report.quiescent);
    DRC_REQUIRE_EQ(report.dispatched, 0u);
    DRC_REQUIRE_EQ(report.steps_succeeded, 0u);
    const CoordinatorStats after = rig.coordinator->stats();
    if (after.dispatches != before.dispatches) {
        note("advance dispatched " + std::to_string(after.dispatches - before.dispatches) +
             " effects while the event was conflicted");
    }
    DRC_REQUIRE_EQ(after.dispatches, before.dispatches);
    const DisasterEvent still_conflicted = rig.event(event);
    DRC_REQUIRE_EQ(still_conflicted.phase, EventPhase::Conflicted);

    CreatePlanRequest request;
    request.event = event;
    request.requested_by = "operator";
    DRC_REQUIRE_ERR(rig.coordinator->create_plan(request), ErrorCode::Conflict);

    // A newer generation for the contested site is what clears the conflict.
    rig.readiness(1, 6, true, true);
    const DisasterEvent cleared = rig.event(event);
    if (cleared.phase != EventPhase::Declared) {
        note("a newer readiness generation left the event in " +
             std::string{to_string(cleared.phase)} + ": " + cleared.status_detail);
    }
    DRC_REQUIRE_EQ(cleared.phase, EventPhase::Declared);
    DRC_REQUIRE(cleared.conflicting_sites.empty());
    DRC_REQUIRE(rig.coordinator->create_plan(request).ok());
}

// ---------------------------------------------------------------------------
// 3. A new epoch fences evidence recovered from the previous incarnation.
// ---------------------------------------------------------------------------
DRC_TEST(fencing_epoch_bump_invalidates_recovered_evidence) {
    drctest::Rig rig = drctest::Rig::make("fencing-epoch");
    rig.open();
    build_recovery_topology(rig);
    const DisasterEventId event = rig.declare({1});
    rig.capability(3, 1, 500);
    rig.readiness(1, 1, true, true);
    rig.readiness(3, 1, true, true);

    const Epoch epoch_before = rig.coordinator->epoch();
    const DestinationCapability stored_capability = rig.coordinator->capability(SiteId{3}).value();
    const SiteReadinessEvidence stored_readiness = rig.coordinator->readiness(SiteId{3}).value();
    const Digest before_reopen = rig.coordinator->state_digest();

    rig.reopen();
    const Epoch epoch_after = rig.coordinator->epoch();
    note("epoch " + epoch_before.to_string() + " -> " + epoch_after.to_string());
    DRC_REQUIRE(epoch_after > epoch_before);

    // The bytes are still there: fencing does not delete evidence.
    const DestinationCapability recovered_capability = rig.coordinator->capability(SiteId{3}).value();
    DRC_REQUIRE_EQ(recovered_capability.generation, stored_capability.generation);
    DRC_REQUIRE_EQ(recovered_capability.available_capacity_units,
                   stored_capability.available_capacity_units);
    DRC_REQUIRE_EQ(recovered_capability.supported_classes_mask,
                   stored_capability.supported_classes_mask);
    DRC_REQUIRE_EQ(recovered_capability.source, stored_capability.source);
    const SiteReadinessEvidence recovered_readiness = rig.coordinator->readiness(SiteId{3}).value();
    DRC_REQUIRE_EQ(recovered_readiness.generation, stored_readiness.generation);
    DRC_REQUIRE_EQ(recovered_readiness.checks.size(), stored_readiness.checks.size());
    DRC_REQUIRE_EQ(recovered_readiness.observed_at, stored_readiness.observed_at);

    const Freshness freshness = rig.coordinator->readiness_freshness(SiteId{3}).value();
    if (freshness != Freshness::ForeignEpoch) {
        note("recovered readiness evaluates as " + std::string{to_string(freshness)} +
             " instead of foreign_epoch");
    }
    DRC_REQUIRE_EQ(freshness, Freshness::ForeignEpoch);
    DRC_REQUIRE_EQ(rig.coordinator->state_digest(), before_reopen);

    // Planning refuses rather than inventing a destination.
    CreatePlanRequest request;
    request.event = event;
    request.requested_by = "operator";
    const Result<RecoveryPlanId> refused = rig.coordinator->create_plan(request);
    if (refused.ok()) {
        note("planning succeeded with only foreign-epoch evidence");
        DRC_REQUIRE(false);
    }
    DRC_REQUIRE_EQ(refused.code(), ErrorCode::Unsupported);
    note("planning refused with: " + refused.status().to_string());

    rig.capability(3, 2, 500);
    DRC_REQUIRE(rig.coordinator->create_plan(request).ok());
}

// ---------------------------------------------------------------------------
// 4. Policy generations only move forward.
// ---------------------------------------------------------------------------
DRC_TEST(fencing_policy_generation_change_is_refused_when_it_goes_backwards) {
    drctest::Rig rig = drctest::Rig::make("fencing-policy");
    rig.open();

    RecoveryPolicy three = rig.coordinator->policy().value();
    three.generation = Generation{3};
    three.max_step_attempts = 5;
    DRC_REQUIRE_OK(rig.coordinator->set_policy(three));

    RecoveryPolicy two = three;
    two.generation = Generation{2};
    DRC_REQUIRE_ERR(rig.coordinator->set_policy(two), ErrorCode::Stale);

    RecoveryPolicy contradictory = three;
    contradictory.max_in_flight_steps = three.max_in_flight_steps + 1;
    DRC_REQUIRE_ERR(rig.coordinator->set_policy(contradictory), ErrorCode::Conflict);

    // The same generation and the same content is idempotent.
    DRC_REQUIRE_OK(rig.coordinator->set_policy(three));
    const RecoveryPolicy retained = rig.coordinator->policy().value();
    DRC_REQUIRE_EQ(retained.generation, Generation{3});
    DRC_REQUIRE_EQ(retained.max_step_attempts, 5u);

    RecoveryPolicy four = three;
    four.generation = Generation{4};
    four.max_step_attempts = 7;
    DRC_REQUIRE_OK(rig.coordinator->set_policy(four));

    rig.reopen();
    const RecoveryPolicy persisted = rig.coordinator->policy().value();
    DRC_REQUIRE_EQ(persisted.generation, Generation{4});
    DRC_REQUIRE_EQ(persisted.max_step_attempts, 7u);
    DRC_REQUIRE(persisted == four);
    // A generation that is now older than the durable one is still refused.
    DRC_REQUIRE_ERR(rig.coordinator->set_policy(three), ErrorCode::Stale);
}

// ---------------------------------------------------------------------------
// 5. Receipts fenced by a stale plan generation cannot move a step.
// ---------------------------------------------------------------------------
DRC_TEST(fencing_stale_receipt_generation_cannot_move_a_step) {
    drctest::Rig rig = drctest::Rig::make("fencing-stale-receipts");
    rig.open();
    build_recovery_topology(rig);
    SyntheticBehavior stale;
    stale.stale_generation = true;
    install_endpoints(rig, stale);

    const DisasterEventId event = rig.declare({1});
    rig.plan(event);
    rig.plan(event);
    const RecoveryPlanId active = rig.active_plan(event);
    const RecoveryPlan active_plan = rig.plan_of(active);
    DRC_REQUIRE_EQ(active_plan.generation, Generation{2});
    DRC_REQUIRE_OK(rig.coordinator->begin_recovery(event));

    AdvanceReport last;
    for (int round = 0; round < 8; ++round) {
        last = rig.advance(event);
    }
    DRC_REQUIRE(last.quiescent);
    const CoordinatorStats stats = rig.coordinator->stats();
    note("stale_receipts=" + std::to_string(stats.stale_receipts) +
         " receipts_applied=" + std::to_string(stats.receipts_applied) +
         " receipts_ignored=" + std::to_string(stats.receipts_ignored) +
         " dispatches=" + std::to_string(stats.dispatches));
    DRC_REQUIRE(stats.stale_receipts > 0);
    require_never_succeeded(rig, active, "stale plan generation");
    DRC_REQUIRE_EQ(rig.count_steps(active, StepState::Succeeded), std::size_t{0});
    DRC_REQUIRE(rig.event(event).phase != EventPhase::Stabilized);
}

// ---------------------------------------------------------------------------
// 6. Receipts from a foreign epoch are ignored.
// ---------------------------------------------------------------------------
DRC_TEST(fencing_foreign_epoch_receipt_is_ignored) {
    drctest::Rig rig = drctest::Rig::make("fencing-foreign-epoch");
    rig.open();
    build_recovery_topology(rig);
    SyntheticBehavior foreign;
    foreign.foreign_epoch = true;
    install_endpoints(rig, foreign);

    const DisasterEventId event = rig.declare({1});
    rig.plan(event);
    const RecoveryPlanId active = rig.active_plan(event);
    DRC_REQUIRE_OK(rig.coordinator->begin_recovery(event));

    AdvanceReport last;
    for (int round = 0; round < 8; ++round) {
        last = rig.advance(event);
    }
    DRC_REQUIRE(last.quiescent);
    const CoordinatorStats stats = rig.coordinator->stats();
    note("foreign_epoch_receipts=" + std::to_string(stats.foreign_epoch_receipts) +
         " receipts_applied=" + std::to_string(stats.receipts_applied) +
         " dispatches=" + std::to_string(stats.dispatches));
    DRC_REQUIRE(stats.foreign_epoch_receipts > 0);
    require_never_succeeded(rig, active, "foreign epoch");
    DRC_REQUIRE_EQ(rig.count_steps(active, StepState::Succeeded), std::size_t{0});
    DRC_REQUIRE(rig.event(event).phase != EventPhase::Stabilized);
}

// ---------------------------------------------------------------------------
// 7. A duplicated receipt is applied exactly once.
// ---------------------------------------------------------------------------
DRC_TEST(fencing_duplicate_receipts_are_applied_once) {
    drctest::Rig rig = drctest::Rig::make("fencing-duplicate-receipts");
    rig.open();
    build_recovery_topology(rig);
    SyntheticBehavior duplicate;
    duplicate.duplicate_reply = true;
    install_endpoints(rig, duplicate);

    const DisasterEventId event = rig.declare({1});
    const RecoveryPlanId plan = rig.plan(event);
    DRC_REQUIRE_OK(rig.coordinator->begin_recovery(event));
    const AdvanceReport report = rig.settle(event);
    DRC_REQUIRE(report.quiescent);
    const RecoveryPlan settled = rig.plan_of(plan);

    const CoordinatorStats stats = rig.coordinator->stats();
    note("dispatches=" + std::to_string(stats.dispatches) +
         " receipts_applied=" + std::to_string(stats.receipts_applied) +
         " receipts_ignored=" + std::to_string(stats.receipts_ignored) +
         " duplicate_receipts=" + std::to_string(stats.duplicate_receipts) +
         " steps_succeeded=" + std::to_string(rig.count_steps(plan, StepState::Succeeded)));

    // The plan completes, and every step holds exactly one applied receipt.
    DRC_REQUIRE_EQ(rig.count_steps(plan, StepState::Succeeded), settled.order.size());
    const DisasterEvent settled_event = rig.event(event);
    DRC_REQUIRE_EQ(settled_event.phase, EventPhase::Stabilized);
    const std::vector<EffectReceipt> receipts = rig.coordinator->receipts().value();
    std::size_t steps_with_several_receipts = 0;
    for (const RecoveryStepId step_id : settled.order) {
        std::size_t count = 0;
        for (const EffectReceipt& receipt : receipts) {
            if (receipt.step == step_id) {
                count += 1;
            }
        }
        if (count > 1) {
            steps_with_several_receipts += 1;
        }
    }
    if (steps_with_several_receipts != 0) {
        note(std::to_string(steps_with_several_receipts) +
             " steps retained more than one receipt");
        DRC_REQUIRE(false);
    }
    if (receipts.size() != stats.dispatches) {
        note("retained receipts " + std::to_string(receipts.size()) + " != dispatches " +
             std::to_string(stats.dispatches));
    }
    DRC_REQUIRE_EQ(receipts.size(), static_cast<std::size_t>(stats.dispatches));

    // The duplicate delivery is accounted for as a duplicate, not as an
    // unknown request.
    if (stats.duplicate_receipts == 0) {
        note("two receipts were delivered for every request but duplicate_receipts is 0 "
             "(receipts_ignored=" + std::to_string(stats.receipts_ignored) + ")");
    }
    DRC_REQUIRE(stats.duplicate_receipts >= 1);
}

// ---------------------------------------------------------------------------
// 8. A live journal cannot be opened twice.
// ---------------------------------------------------------------------------
DRC_TEST(fencing_a_second_coordinator_cannot_open_a_live_journal) {
    drctest::Rig rig = drctest::Rig::make("fencing-lock");
    rig.open();
    rig.domain(1, "domain-a");
    const Digest before = rig.coordinator->state_digest();

    Result<std::unique_ptr<Coordinator>> second = Coordinator::open(rig.options());
    if (second.ok()) {
        note("a second coordinator opened the journal while the first was live");
        DRC_REQUIRE_OK(second.value()->shutdown());
        DRC_REQUIRE(false);
    }
    if (second.code() != ErrorCode::Locked) {
        note("the second open failed with " + std::string{to_string(second.code())} + ": " +
             second.status().to_string());
    }
    DRC_REQUIRE_EQ(second.code(), ErrorCode::Locked);
    note("second open refused with: " + second.status().to_string());

    // Even with takeover disabled the answer is the same: the owner is alive.
    CoordinatorOptions no_takeover = rig.options();
    no_takeover.allow_lock_takeover = false;
    DRC_REQUIRE_ERR(Coordinator::open(no_takeover), ErrorCode::Locked);

    // The live coordinator is untouched and still writable.
    DRC_REQUIRE_EQ(rig.coordinator->state_digest(), before);
    DRC_REQUIRE_OK(rig.coordinator->define_failure_domain(FailureDomainRecord{
        FailureDomainId{2}, "domain-b"}));
    const Digest after = rig.coordinator->state_digest();

    rig.close();
    rig.open();
    DRC_REQUIRE_EQ(rig.coordinator->state_digest(), after);
    DRC_REQUIRE(rig.coordinator->failure_domain(FailureDomainId{2}).ok());
}

// ---------------------------------------------------------------------------
// 9. Records the engine never committed are invisible after recovery.
// ---------------------------------------------------------------------------
DRC_TEST(fencing_uncommitted_state_is_invisible_after_recovery) {
    drctest::Rig rig = drctest::Rig::make("fencing-uncommitted");
    rig.open();
    rig.domain(1, "domain-a");
    rig.site(1, "site-a1", 1, 100);
    const Digest committed = rig.coordinator->state_digest();
    const std::string journal_path = fileio::join(rig.directory->path, "coordinator.journal");
    rig.close();
    const std::uint64_t committed_bytes = bytes_of(journal_path);

    // A well-formed Site record for a new site, written by a raw writer and
    // never committed.
    journal::Writer::Options options;
    options.path = journal_path;
    options.max_bytes = kMaxJournalBytes;
    Result<journal::Writer> writer = journal::Writer::open(options);
    DRC_REQUIRE(writer.ok());
    canonical::Writer payload;
    payload.u64(42);
    payload.text("smuggled-site");
    payload.u64(1);
    payload.u64(10);
    DRC_REQUIRE(!payload.failed());
    DRC_REQUIRE(writer.value().append(journal::RecordType::Site, payload.bytes()).ok());
    DRC_REQUIRE_OK(writer.value().flush());
    DRC_REQUIRE_OK(writer.value().close());

    const std::uint64_t with_tail = bytes_of(journal_path);
    DRC_REQUIRE(with_tail > committed_bytes);

    rig.open();
    const Result<SiteRecord> smuggled = rig.coordinator->site(SiteId{42});
    if (smuggled.ok()) {
        note("an uncommitted site record became visible after recovery: " +
             smuggled.value().name);
        DRC_REQUIRE(false);
    }
    DRC_REQUIRE_EQ(smuggled.code(), ErrorCode::NotFound);
    DRC_REQUIRE_EQ(rig.coordinator->state_digest(), committed);
    const SiteRecord original = rig.coordinator->site(SiteId{1}).value();
    DRC_REQUIRE_EQ(original.name, std::string{"site-a1"});
    const std::vector<SiteId> sites = rig.coordinator->sites().value();
    DRC_REQUIRE_EQ(sites.size(), std::size_t{1});
    rig.close();

    // The committed content of the journal holds the original site and nothing
    // the writer appended without a commit.
    Result<journal::Reader> reader = journal::Reader::open(journal_path, kMaxJournalBytes);
    DRC_REQUIRE(reader.ok());
    std::size_t committed_sites = 0;
    bool smuggled_committed = false;
    const Status walked =
        reader.value().for_each_committed([&](const journal::Record& record) -> Status {
            if (record.type != journal::RecordType::Site) {
                return ok_status();
            }
            committed_sites += 1;
            std::uint64_t id = 0;
            for (std::size_t i = 0; i < 8 && i < record.payload.size(); ++i) {
                id |= static_cast<std::uint64_t>(record.payload[i])
                      << (8u * static_cast<unsigned>(i));
            }
            if (id == 42) {
                smuggled_committed = true;
            }
            return ok_status();
        });
    DRC_REQUIRE(walked.ok());
    note("committed site records = " + std::to_string(committed_sites) +
         ", committed_bytes=" + std::to_string(reader.value().report().committed_bytes) +
         " (before the raw append the boundary was " + std::to_string(committed_bytes) +
         ", with the uncommitted tail " + std::to_string(with_tail) + ")");
    DRC_REQUIRE(!smuggled_committed);
    DRC_REQUIRE_EQ(committed_sites, std::size_t{1});
}
