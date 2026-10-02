#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "drc/plan.hpp"
#include "support/harness.hpp"
#include "test.hpp"

using namespace drc;

namespace {

// Deterministic generator: the same seed always produces the same topology, so
// a failure prints reproduction data that is enough to replay the exact case.
class Xorshift {
public:
    explicit Xorshift(std::uint64_t seed) : state_(seed == 0 ? 0x9E3779B97F4A7C15ull : seed) {}

    std::uint64_t next() {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 7;
        state_ ^= state_ << 17;
        return state_;
    }

    std::uint64_t range(std::uint64_t low, std::uint64_t high) {
        if (high <= low) {
            return low;
        }
        return low + (next() % ((high - low) + 1));
    }

    bool chance(std::uint32_t percent) { return (next() % 100u) < percent; }

private:
    std::uint64_t state_;
};

struct Generated {
    std::uint32_t domains = 0;
    std::vector<std::uint64_t> sites;
    std::map<std::uint64_t, std::uint64_t> site_domain;
    std::map<std::uint64_t, std::uint64_t> site_capacity;
    std::vector<std::uint64_t> affected;
    std::vector<std::uint64_t> obligations;
    std::map<std::uint64_t, std::uint64_t> obligation_home;
    std::map<std::uint64_t, std::uint64_t> obligation_capacity;
    std::map<std::uint64_t, RecoveryClass> obligation_class;
    std::map<std::uint64_t, std::vector<std::uint64_t>> obligation_dependencies;
    std::map<std::uint64_t, std::uint64_t> capability_units;
    std::map<std::uint64_t, std::uint32_t> capability_mask;
};

Generated generate(std::uint64_t seed) {
    Xorshift rng{seed};
    Generated generated;
    generated.domains = static_cast<std::uint32_t>(rng.range(2, 4));
    const std::uint64_t site_count = rng.range(3, 7);
    for (std::uint64_t i = 1; i <= site_count; ++i) {
        generated.sites.push_back(i);
        generated.site_domain[i] = rng.range(1, generated.domains);
        generated.site_capacity[i] = rng.range(0, 500);
    }
    for (const std::uint64_t site : generated.sites) {
        if (rng.chance(45)) {
            generated.affected.push_back(site);
        }
    }
    if (generated.affected.empty()) {
        generated.affected.push_back(generated.sites.front());
    }
    const std::uint64_t obligation_count = rng.range(1, 8);
    for (std::uint64_t i = 1; i <= obligation_count; ++i) {
        const std::uint64_t id = 100 + i;
        generated.obligations.push_back(id);
        generated.obligation_dependencies[id];  // always present, even when empty
        generated.obligation_home[id] =
            generated.sites[rng.range(0, generated.sites.size() - 1)];
        generated.obligation_capacity[id] = rng.range(1, 40);
        generated.obligation_class[id] =
            static_cast<RecoveryClass>(rng.range(0, kRecoveryClassCount - 1));
        for (std::uint64_t j = 1; j < i; ++j) {
            if (rng.chance(25)) {
                generated.obligation_dependencies[id].push_back(100 + j);
            }
        }
    }
    for (const std::uint64_t site : generated.sites) {
        const bool affected = std::find(generated.affected.begin(), generated.affected.end(),
                                        site) != generated.affected.end();
        if (affected) {
            continue;
        }
        if (!rng.chance(85)) {
            continue;  // no observation at all for this destination
        }
        generated.capability_units[site] = rng.range(0, 300);
        std::uint32_t mask = 0;
        for (std::uint32_t cls = 0; cls < kRecoveryClassCount; ++cls) {
            if (rng.chance(70)) {
                mask |= 1u << cls;
            }
        }
        generated.capability_mask[site] = mask;
    }
    return generated;
}

void apply(drctest::Rig& rig, const Generated& generated) {
    for (std::uint32_t domain = 1; domain <= generated.domains; ++domain) {
        rig.domain(domain, "domain-" + std::to_string(domain));
    }
    for (const std::uint64_t site : generated.sites) {
        rig.site(site, "site-" + std::to_string(site), generated.site_domain.at(site),
                 generated.site_capacity.at(site));
    }
    for (const std::uint64_t id : generated.obligations) {
        rig.obligation(id, "obligation-" + std::to_string(id),
                       generated.obligation_class.at(id), generated.obligation_home.at(id),
                       generated.obligation_capacity.at(id),
                       generated.obligation_dependencies.at(id));
    }
    for (const auto& entry : generated.capability_units) {
        rig.capability(entry.first, 1, entry.second, generated.capability_mask.at(entry.first));
    }
    rig.synthetic_endpoints();
}

[[nodiscard]] bool is_in_scope(const Generated& generated, std::uint64_t obligation) {
    const std::uint64_t home = generated.obligation_home.at(obligation);
    return std::find(generated.affected.begin(), generated.affected.end(), home) !=
           generated.affected.end();
}

// Every invariant a plan must hold, checked against the generated inputs.
void check_plan_invariants(const Generated& generated, const RecoveryPlan& plan) {
    std::map<RecoveryStepId, std::size_t> position;
    for (std::size_t i = 0; i < plan.order.size(); ++i) {
        position[plan.order[i]] = i;
    }
    DRC_REQUIRE_EQ(position.size(), plan.steps.size());
    for (const RecoveryStepId step_id : plan.order) {
        const RecoveryStep& step = plan.steps.at(step_id);
        for (const RecoveryStepId dependency : step.depends_on) {
            const auto found = position.find(dependency);
            DRC_REQUIRE(found != position.end());
            DRC_REQUIRE(found->second < position.at(step_id));
        }
    }

    // Capacity is never overcommitted against the evidence that was supplied.
    std::map<std::uint64_t, std::uint64_t> committed;
    for (const PlacementAssignment& assignment : plan.assignments) {
        const std::uint64_t destination = assignment.destination.value();
        const std::uint64_t demand = generated.obligation_capacity.at(assignment.obligation.value());
        committed[destination] += demand;
    }
    for (const auto& entry : committed) {
        const auto available = generated.capability_units.find(entry.first);
        if (available == generated.capability_units.end()) {
            DRC_REQUIRE(false);
        }
        DRC_REQUIRE(entry.second <= available->second);
    }

    // Placement accounting: every in-scope obligation is placed, unplaced, or
    // deferred, and the three sets are disjoint.
    std::vector<std::uint64_t> placed;
    for (const PlacementAssignment& assignment : plan.assignments) {
        placed.push_back(assignment.obligation.value());
    }
    for (const std::uint64_t obligation : generated.obligations) {
        if (!is_in_scope(generated, obligation)) {
            continue;
        }
        const bool is_placed =
            std::find(placed.begin(), placed.end(), obligation) != placed.end();
        const bool is_unplaced =
            std::find(plan.unplaced.begin(), plan.unplaced.end(), ObligationId{obligation}) !=
            plan.unplaced.end();
        const bool is_deferred =
            std::find(plan.deferred.begin(), plan.deferred.end(), ObligationId{obligation}) !=
            plan.deferred.end();
        const int count = (is_placed ? 1 : 0) + (is_unplaced ? 1 : 0) + (is_deferred ? 1 : 0);
        DRC_REQUIRE_EQ(count, 1);
    }

    // The stored digest is exactly the digest of the stored plan.
    std::vector<RecoveryStep> steps;
    for (const RecoveryStepId step_id : plan.order) {
        steps.push_back(plan.steps.at(step_id));
    }
    DRC_REQUIRE_EQ(digest_steps(steps, plan.assignments), plan.digest);

    // The stored order is the canonical order.
    const Result<std::vector<RecoveryStepId>> canonical = canonical_step_order(plan.steps);
    DRC_REQUIRE_OK(canonical);
    DRC_REQUIRE(canonical.value() == plan.order);
}

[[nodiscard]] std::string describe_seed(std::uint64_t seed) {
    return "seed " + std::to_string(seed);
}

}  // namespace

DRC_TEST(property_generated_plans_hold_every_invariant) {
    for (std::uint64_t seed = 1; seed <= 24; ++seed) {
        const Generated generated = generate(seed);
        drctest::Rig rig = drctest::Rig::make("property-plan");
        rig.open();
        apply(rig, generated);

        DisasterDeclaration declaration;
        declaration.declared_by = "property-test";
        declaration.reason = "generated topology " + describe_seed(seed);
        declaration.severity = 2;
        declaration.affected_sites = [&generated]() {
            std::vector<SiteId> sites;
            for (const std::uint64_t site : generated.affected) {
                sites.push_back(SiteId{site});
            }
            return sites;
        }();
        const Result<DisasterEventId> event = rig.coordinator->declare_disaster(declaration);
        if (!event.ok()) {
            // A generated topology may legitimately be rejected; a rejection
            // must still be typed and must not be a crash.
            DRC_REQUIRE(!event.status().message().empty());
            continue;
        }

        CreatePlanRequest request;
        request.event = event.value();
        request.requested_by = "property-test";
        const Result<RecoveryPlanId> plan_id = rig.coordinator->create_plan(request);
        if (!plan_id.ok()) {
            // Refusing to fabricate a destination for a protected obligation is
            // the documented behaviour, and it must be the only reason.
            DRC_REQUIRE_EQ(plan_id.status().code(), ErrorCode::Unsupported);
            bool protected_unplaced = false;
            for (const std::uint64_t obligation : generated.obligations) {
                if (!is_in_scope(generated, obligation)) {
                    continue;
                }
                if (!is_protected_class(generated.obligation_class.at(obligation))) {
                    continue;
                }
                const std::uint64_t home = generated.obligation_home.at(obligation);
                const std::uint64_t demand = generated.obligation_capacity.at(obligation);
                bool placeable = false;
                for (const auto& candidate : generated.capability_units) {
                    const std::uint64_t site = candidate.first;
                    if (std::find(generated.affected.begin(), generated.affected.end(), site) !=
                        generated.affected.end()) {
                        continue;
                    }
                    const std::uint32_t bit =
                        1u << static_cast<std::uint32_t>(generated.obligation_class.at(obligation));
                    if ((generated.capability_mask.at(site) & bit) == 0u) {
                        continue;
                    }
                    if (candidate.second >= demand && home != 0) {
                        placeable = true;
                    }
                }
                if (!placeable) {
                    protected_unplaced = true;
                }
            }
            DRC_REQUIRE(protected_unplaced);
            continue;
        }

        const RecoveryPlan plan = rig.plan_of(plan_id.value());
        check_plan_invariants(generated, plan);
    }
}

DRC_TEST(property_identical_inputs_produce_identical_plans) {
    for (std::uint64_t seed = 100; seed <= 112; ++seed) {
        const Generated generated = generate(seed);

        drctest::Rig first = drctest::Rig::make("property-determinism-a");
        drctest::Rig second = drctest::Rig::make("property-determinism-b");
        first.open();
        second.open();
        apply(first, generated);
        apply(second, generated);

        const DisasterEventId first_event = first.declare(generated.affected);
        const DisasterEventId second_event = second.declare(generated.affected);
        DRC_REQUIRE_EQ(first_event, second_event);

        CreatePlanRequest first_request;
        first_request.event = first_event;
        first_request.requested_by = "property-test";
        CreatePlanRequest second_request;
        second_request.event = second_event;
        second_request.requested_by = "property-test";
        const Result<RecoveryPlanId> first_plan = first.coordinator->create_plan(first_request);
        const Result<RecoveryPlanId> second_plan = second.coordinator->create_plan(second_request);
        DRC_REQUIRE_EQ(first_plan.ok(), second_plan.ok());
        if (!first_plan.ok()) {
            DRC_REQUIRE_EQ(first_plan.status().code(), second_plan.status().code());
            continue;
        }

        const RecoveryPlan left = first.plan_of(first_plan.value());
        const RecoveryPlan right = second.plan_of(second_plan.value());
        DRC_REQUIRE_EQ(left.id, right.id);
        DRC_REQUIRE_EQ(left.digest, right.digest);
        DRC_REQUIRE_EQ(left.order.size(), right.order.size());
        for (std::size_t i = 0; i < left.order.size(); ++i) {
            DRC_REQUIRE_EQ(left.order[i], right.order[i]);
            const RecoveryStep& a = left.steps.at(left.order[i]);
            const RecoveryStep& b = right.steps.at(right.order[i]);
            DRC_REQUIRE_EQ(a.kind, b.kind);
            DRC_REQUIRE_EQ(a.obligation, b.obligation);
            DRC_REQUIRE_EQ(a.destination, b.destination);
            DRC_REQUIRE_EQ(a.source, b.source);
            DRC_REQUIRE_EQ(a.depends_on, b.depends_on);
            DRC_REQUIRE_EQ(a.recovery_class, b.recovery_class);
        }
    }
}

DRC_TEST(property_dependency_cycles_are_always_rejected) {
    for (std::uint64_t seed = 200; seed <= 208; ++seed) {
        Xorshift rng{seed};
        drctest::Rig rig = drctest::Rig::make("property-cycles");
        rig.open();
        rig.domain(1, "domain-a");
        rig.site(1, "site-a", 1);
        rig.obligation(1, "first", RecoveryClass::Standard, 1, 1);
        // Any cycle back to the first obligation must be refused, whatever the
        // shape of the chain that leads back to it.
        const std::uint64_t length = rng.range(2, 5);
        std::uint64_t previous = 1;
        for (std::uint64_t i = 2; i <= length; ++i) {
            rig.obligation(i, "chain-" + std::to_string(i), RecoveryClass::Standard, 1, 1,
                           {previous});
            previous = i;
        }
        ProtectedObligation cyclic;
        cyclic.id = ObligationId{1};
        cyclic.name = "first";
        cyclic.recovery_class = RecoveryClass::Standard;
        cyclic.home_site = SiteId{1};
        cyclic.required_capacity_units = 1;
        cyclic.depends_on.push_back(ObligationId{previous});
        const Status status = rig.coordinator->define_obligation(cyclic);
        DRC_REQUIRE(!status.ok());
        DRC_REQUIRE_EQ(status.code(), ErrorCode::Conflict);
        // The refused registration changed nothing.
        DRC_REQUIRE(rig.coordinator->obligation(ObligationId{1}).value().depends_on.empty());
    }
}

DRC_TEST(property_plan_generation_fences_every_earlier_plan) {
    for (std::uint64_t seed = 300; seed <= 306; ++seed) {
        const Generated generated = generate(seed);
        drctest::Rig rig = drctest::Rig::make("property-generations");
        rig.open();
        apply(rig, generated);
        const DisasterEventId event = rig.declare(generated.affected);

        std::vector<RecoveryPlanId> plans;
        for (int attempt = 0; attempt < 3; ++attempt) {
            CreatePlanRequest request;
            request.event = event;
            request.requested_by = "property-test";
            const Result<RecoveryPlanId> plan = rig.coordinator->create_plan(request);
            if (!plan.ok()) {
                DRC_REQUIRE_EQ(plan.status().code(), ErrorCode::Unsupported);
                break;
            }
            plans.push_back(plan.value());
        }
        for (std::size_t i = 0; i < plans.size(); ++i) {
            const RecoveryPlan plan = rig.plan_of(plans[i]);
            DRC_REQUIRE_EQ(plan.generation, Generation{static_cast<std::uint64_t>(i + 1)});
            const bool is_last = i + 1 == plans.size();
            DRC_REQUIRE_EQ(plan.state, is_last ? PlanState::Active : PlanState::Superseded);
            for (const RecoveryStepId step_id : plan.order) {
                const RecoveryStep& step = plan.steps.at(step_id);
                if (!is_last) {
                    DRC_REQUIRE(step.state == StepState::Superseded ||
                                is_terminal_step_state(step.state));
                }
                DRC_REQUIRE_EQ(step.plan_generation, plan.generation);
            }
        }
        if (!plans.empty()) {
            DRC_REQUIRE_EQ(rig.active_plan(event), plans.back());
        }
    }
}

DRC_TEST(property_repeated_close_and_reopen_never_changes_the_state) {
    const Generated generated = generate(400);
    drctest::Rig rig = drctest::Rig::make("property-reopen");
    rig.open();
    apply(rig, generated);
    const DisasterEventId event = rig.declare(generated.affected);
    CreatePlanRequest request;
    request.event = event;
    request.requested_by = "property-test";
    const Result<RecoveryPlanId> plan = rig.coordinator->create_plan(request);
    DRC_REQUIRE(plan.ok());
    DRC_REQUIRE_OK(rig.coordinator->begin_recovery(event));
    (void)rig.advance(event);

    const Digest digest = rig.coordinator->state_digest();
    for (int cycle = 0; cycle < 6; ++cycle) {
        rig.reopen();
        DRC_REQUIRE_EQ(rig.coordinator->state_digest(), digest);
    }
}
