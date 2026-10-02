#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "support/harness.hpp"
#include "test.hpp"

using namespace drc;

namespace {

void build_topology(drctest::Rig& rig) {
    rig.domain(1, "domain-a");
    rig.domain(2, "domain-b");
    rig.site(1, "site-a1", 1, 100);
    rig.site(3, "site-b1", 2, 1000);
    rig.obligation(10, "auth", RecoveryClass::SafetyCritical, 1, 10);
    rig.capability(3, 1, 500);
    rig.synthetic_endpoints();
}

// A participant that never answers. The runtime's exchange budget is what makes
// this a bounded, honest "unknown" instead of a hang.
drc::EndpointDescriptor silent_participant(const std::string& domain, std::uint64_t id) {
    drc::EndpointDescriptor descriptor;
    descriptor.domain = EffectDomain::SiteControlPlane;
    const Result<EffectDomain> parsed = parse_effect_domain(domain);
    DRC_REQUIRE(parsed.ok());
    descriptor.domain = parsed.value();
    descriptor.id = EndpointId{id};
    descriptor.name = "silent-" + domain;
    descriptor.command = {drctest::tool_path("drc_endpoint"), "--domain", domain, "--id",
                          std::to_string(id), "--name", "silent", "--drop-after", "0"};
    return descriptor;
}

}  // namespace

DRC_TEST(concurrency_observer_reentrancy_is_safe) {
    drctest::Rig rig = drctest::Rig::make("concurrency-observer");
    rig.open();
    build_topology(rig);

    std::atomic<std::uint64_t> notifications{0};
    std::atomic<bool> read_failed{false};
    DRC_REQUIRE_OK(rig.coordinator->set_observer([&](const TransitionNotice& notice) {
        // The observer runs with no engine lock held, so reading state from
        // inside it must work. A re-entrant read that deadlocked would hang
        // this test; that is the assertion.
        if (!notice.kind.empty()) {
            notifications.fetch_add(1);
        }
        const Result<std::vector<SiteId>> sites = rig.coordinator->sites();
        if (!sites.ok()) {
            read_failed.store(true);
        }
        const Result<RecoveryPolicy> policy = rig.coordinator->policy();
        if (!policy.ok()) {
            read_failed.store(true);
        }
    }));

    const DisasterEventId event = rig.declare({1});
    rig.plan(event);
    DRC_REQUIRE_OK(rig.coordinator->begin_recovery(event));
    DRC_REQUIRE(rig.settle(event).quiescent);

    DRC_REQUIRE(notifications.load() > 0);
    DRC_REQUIRE(!read_failed.load());
    DRC_REQUIRE_EQ(rig.coordinator->stats().observer_failures, 0ull);
    DRC_REQUIRE_EQ(rig.event(event).phase, EventPhase::Stabilized);
}

DRC_TEST(concurrency_a_throwing_observer_does_not_damage_state) {
    drctest::Rig quiet = drctest::Rig::make("concurrency-throwing-a");
    quiet.open();
    build_topology(quiet);
    const DisasterEventId quiet_event = quiet.declare({1});
    quiet.plan(quiet_event);
    DRC_REQUIRE_OK(quiet.coordinator->begin_recovery(quiet_event));
    DRC_REQUIRE(quiet.settle(quiet_event).quiescent);
    const Digest expected = quiet.coordinator->state_digest();

    drctest::Rig hostile = drctest::Rig::make("concurrency-throwing-b");
    hostile.open();
    build_topology(hostile);
    DRC_REQUIRE_OK(hostile.coordinator->set_observer([](const TransitionNotice&) {
        throw std::runtime_error{"observer refuses to cooperate"};
    }));
    const DisasterEventId hostile_event = hostile.declare({1});
    hostile.plan(hostile_event);
    DRC_REQUIRE_OK(hostile.coordinator->begin_recovery(hostile_event));
    DRC_REQUIRE(hostile.settle(hostile_event).quiescent);

    // The throw is counted, and the authoritative state is bit-identical to the
    // run without the hostile observer.
    DRC_REQUIRE(hostile.coordinator->stats().observer_failures > 0);
    DRC_REQUIRE_EQ(hostile.coordinator->state_digest(), expected);
    DRC_REQUIRE_EQ(hostile.event(hostile_event).phase, EventPhase::Stabilized);
}

DRC_TEST(concurrency_a_second_mutation_is_refused_while_one_is_in_progress) {
    drctest::Rig rig = drctest::Rig::make("concurrency-busy");
    rig.open();
    build_topology(rig);
    rig.endpoint(EffectDomain::SiteControlPlane, SyntheticBehavior{}, 1);

    // Make every exchange take a while, so the advancing thread holds the
    // engine lock for a measurable window.
    EndpointDescriptor slow;
    slow.domain = EffectDomain::SiteControlPlane;
    slow.id = EndpointId{2};
    slow.name = "slow";
    slow.command = {drctest::tool_path("drc_endpoint"), "--domain", "site_control_plane", "--id",
                    "2", "--name", "slow", "--delay-ms", "250"};
    DRC_REQUIRE_OK(rig.coordinator->register_endpoint(slow));

    const DisasterEventId event = rig.declare({1});
    rig.plan(event);
    DRC_REQUIRE_OK(rig.coordinator->begin_recovery(event));

    std::atomic<int> outcome{0};  // 0 = not run, 1 = Busy, 2 = something else
    // Owned by a group that joins on scope exit: a failing requirement below
    // must report itself rather than terminate the process.
    drctest::ThreadGroup advancer;
    advancer.add(std::thread([&]() { (void)rig.advance(event, 4, 8); }));
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    const Status concurrent = rig.coordinator->define_site(
        SiteRecord{SiteId{99}, "concurrent", FailureDomainId{2}, 10});
    if (concurrent.code() == ErrorCode::Busy) {
        outcome.store(1);
    } else if (concurrent.ok()) {
        outcome.store(2);
    } else {
        outcome.store(3);
    }
    advancer.join();

    // Either the concurrent call was refused as busy, or the advancing call had
    // already finished. It must never be a silent data race.
    DRC_REQUIRE(outcome.load() == 1 || outcome.load() == 2);
    if (outcome.load() == 1) {
        DRC_REQUIRE(rig.coordinator->stats().busy_rejections > 0);
    }
    const AdvanceReport settled = rig.settle(event);
    DRC_REQUIRE(settled.quiescent);
    if (rig.event(event).phase != EventPhase::Stabilized) {
        const CoordinatorStats stats = rig.coordinator->stats();
        std::cout << "    plan did not stabilize: phase="
                  << to_string(rig.event(event).phase) << " dispatches=" << stats.dispatches
                  << " receipts=" << stats.receipts_applied
                  << " transport_failures=" << stats.transport_failures
                  << " unanswered=" << stats.unanswered_exchanges << "\n";
        const RecoveryPlanId active = rig.active_plan(event);
        const RecoveryPlan plan = rig.plan_of(active);
        for (const RecoveryStepId step_id : plan.order) {
            const RecoveryStep& step = plan.steps.at(step_id);
            std::cout << "    step " << step.id.to_string() << " " << to_string(step.kind)
                      << " " << to_string(step.state) << " attempts=" << step.attempts << " "
                      << step.detail << "\n";
        }
    }
    DRC_REQUIRE_EQ(rig.event(event).phase, EventPhase::Stabilized);
}

DRC_TEST(concurrency_readers_never_observe_a_torn_state) {
    drctest::Rig rig = drctest::Rig::make("concurrency-readers");
    rig.open();
    build_topology(rig);
    const DisasterEventId event = rig.declare({1});
    rig.plan(event);
    DRC_REQUIRE_OK(rig.coordinator->begin_recovery(event));

    std::atomic<bool> stop{false};
    std::atomic<bool> failed{false};
    drctest::ThreadGroup readers([&stop]() { stop.store(true); });
    for (int i = 0; i < 4; ++i) {
        readers.add(std::thread([&]() {
            while (!stop.load()) {
                const Digest digest = rig.coordinator->state_digest();
                if (digest.is_zero()) {
                    failed.store(true);
                }
                const Result<DisasterEvent> value = rig.coordinator->event(event);
                if (!value.ok()) {
                    failed.store(true);
                    break;
                }
                const Result<RecoveryPlanId> plan = rig.coordinator->active_plan(event);
                if (!plan.ok()) {
                    failed.store(true);
                    break;
                }
                const Result<RecoveryPlan> view = rig.coordinator->plan(plan.value());
                if (!view.ok() || view.value().order.size() != view.value().steps.size()) {
                    failed.store(true);
                    break;
                }
            }
        }));
    }
    const AdvanceReport report = rig.settle(event);
    readers.join();

    DRC_REQUIRE(!failed.load());
    DRC_REQUIRE(report.quiescent);
    DRC_REQUIRE_EQ(rig.event(event).phase, EventPhase::Stabilized);
    // One lock order, no inversions, for the whole run.
    DRC_REQUIRE_EQ(rig.coordinator->stats().lock_order_violations, 0ull);
    DRC_REQUIRE_EQ(rig.coordinator->stats().observer_failures, 0ull);
}

DRC_TEST(concurrency_silence_is_bounded_and_never_becomes_success) {
    drctest::Rig rig = drctest::Rig::make("concurrency-silence");
    rig.exchange_budget_ms = 40;
    rig.open();
    rig.domain(1, "domain-a");
    rig.domain(2, "domain-b");
    rig.site(1, "site-a1", 1, 100);
    rig.site(3, "site-b1", 2, 1000);
    rig.obligation(10, "auth", RecoveryClass::SafetyCritical, 1, 10);
    rig.capability(3, 1, 500);
    for (const std::string domain :
         {"site_control_plane", "placement_reservation_capacity", "asi_execution_recovery",
          "dfi_network_recovery"}) {
        DRC_REQUIRE_OK(rig.coordinator->register_endpoint(silent_participant(domain, 1)));
    }

    const DisasterEventId event = rig.declare({1});
    const RecoveryPlanId plan = rig.plan(event);
    DRC_REQUIRE_OK(rig.coordinator->begin_recovery(event));

    // Every participant is silent. Nothing may be reported as complete, and the
    // engine must return instead of waiting forever.
    const AdvanceReport report = rig.settle(event);
    DRC_REQUIRE(report.quiescent);
    DRC_REQUIRE_EQ(rig.count_steps(plan, StepState::Succeeded), std::size_t{0});
    DRC_REQUIRE_EQ(rig.event(event).phase, EventPhase::Blocked);
    DRC_REQUIRE(rig.coordinator->stats().transport_failures > 0);
    DRC_REQUIRE(rig.coordinator->stats().unanswered_exchanges > 0);

    // Shutdown while a participant is still silent must complete, and nothing
    // that never finished may be published afterwards.
    DRC_REQUIRE_OK(rig.coordinator->shutdown());
    DRC_REQUIRE_EQ(rig.count_steps(plan, StepState::Succeeded), std::size_t{0});
    DRC_REQUIRE_ERR(rig.coordinator->define_site(
                        SiteRecord{SiteId{500}, "after-shutdown", FailureDomainId{2}, 1}),
                    ErrorCode::Shutdown);
    // Reopening records the requests that can never be answered as such, so the
    // digest after one recovery is the baseline; a second recovery must not
    // change anything further.
    rig.open();
    const Digest after_recovery = rig.coordinator->state_digest();
    DRC_REQUIRE_EQ(rig.count_steps(plan, StepState::Succeeded), std::size_t{0});
    rig.reopen();
    DRC_REQUIRE_EQ(rig.coordinator->state_digest(), after_recovery);
    DRC_REQUIRE_EQ(rig.count_steps(plan, StepState::Succeeded), std::size_t{0});
}
