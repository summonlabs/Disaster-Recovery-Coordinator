#pragma once

// Shared test rig: a temporary journal directory, a manual clock, and a
// coordinator, plus the small amount of topology setup almost every suite
// needs. Nothing here hides a failure: every helper requires success.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "drc/engine.hpp"
#include "drc/fileio.hpp"
#include "test.hpp"

namespace drctest {

// Path of a built tool. The directory is injected by CMake so the tests can
// spawn the real executables instead of guessing where a build tree lives.
[[nodiscard]] inline std::string tool_path(const std::string& name) {
#ifdef DRC_TOOLS_DIR
    std::string path = std::string{DRC_TOOLS_DIR} + "/" + name;
#else
    std::string path = name;
#endif
#if defined(_WIN32)
    path.append(".exe");
#endif
    return path;
}

inline std::uint64_t next_serial() {
    static std::atomic<std::uint64_t> counter{0};
    return counter.fetch_add(1) + 1;
}

struct TempDir {
    std::string path;

    explicit TempDir(const std::string& tag) {
        std::error_code error;
        const std::filesystem::path base =
            std::filesystem::temp_directory_path(error) / "drc-tests";
        std::filesystem::create_directories(base, error);
        const std::uint64_t stamp = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        const std::filesystem::path directory =
            base / (tag + "-" + std::to_string(stamp) + "-" + std::to_string(next_serial()));
        std::filesystem::create_directories(directory, error);
        path = directory.string();
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    TempDir(TempDir&& other) noexcept : path(std::move(other.path)) { other.path.clear(); }
    TempDir& operator=(TempDir&& other) noexcept {
        if (this != &other) {
            std::error_code error;
            std::filesystem::remove_all(path, error);
            path = std::move(other.path);
            other.path.clear();
        }
        return *this;
    }

    ~TempDir() {
        if (path.empty()) {
            return;
        }
        // The library's own removal walks long paths with the extended-length
        // prefix on Windows. There is deliberately no fallback to
        // std::filesystem::remove_all: with some toolchains that call does not
        // terminate on long paths, and a stuck cleanup would wedge the suite.
        const drc::Status removed = drc::fileio::remove_tree(path);
        if (!removed.ok()) {
            std::cerr << "cleanup failed for " << path << ": " << removed.to_string() << "\n";
        }
    }
};

inline std::uint32_t class_mask(std::initializer_list<drc::RecoveryClass> classes) {
    std::uint32_t mask = 0;
    for (const drc::RecoveryClass cls : classes) {
        mask |= 1u << static_cast<std::uint32_t>(cls);
    }
    return mask;
}

[[nodiscard]] inline std::uint32_t all_classes() {
    return (1u << drc::kRecoveryClassCount) - 1u;
}

struct Rig {
    std::unique_ptr<TempDir> directory;
    std::shared_ptr<drc::ManualClock> clock;
    std::unique_ptr<drc::Coordinator> coordinator;
    bool allow_lock_takeover = true;
    std::uint32_t worker_threads = 2;
    std::uint64_t exchange_budget_ms = 0;
    drc::FaultInjection fault;

    Rig() = default;
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;
    Rig(Rig&&) = default;
    Rig& operator=(Rig&&) = default;

    static Rig make(const std::string& tag) {
        Rig rig;
        rig.directory = std::make_unique<TempDir>(tag);
        rig.clock = std::make_shared<drc::ManualClock>();
        return rig;
    }

    [[nodiscard]] drc::CoordinatorOptions options() const {
        drc::CoordinatorOptions options;
        options.directory = directory->path;
        options.clock = clock;
        options.owner = "drc-tests";
        options.worker_threads = worker_threads;
        options.allow_lock_takeover = allow_lock_takeover;
        if (exchange_budget_ms != 0) {
            options.exchange_budget_nanos =
                static_cast<drc::UnixNanos>(exchange_budget_ms) * drc::kNanosPerMillisecond;
        }
        options.fault = fault;
        return options;
    }

    void open() {
        drc::Result<std::unique_ptr<drc::Coordinator>> opened =
            drc::Coordinator::open(options());
        DRC_REQUIRE(opened.ok());
        coordinator = std::move(opened).value();
    }

    void close() {
        if (coordinator != nullptr) {
            DRC_REQUIRE_OK(coordinator->shutdown());
            coordinator.reset();
        }
    }

    void reopen() {
        close();
        open();
    }

    // The destructor must never throw: a failed shutdown during unwinding would
    // terminate the process and hide the original failure.
    ~Rig() {
        if (coordinator != nullptr) {
            (void)coordinator->shutdown();
            coordinator.reset();
        }
    }

    // ---- topology helpers -------------------------------------------------
    void domain(std::uint64_t id, const std::string& name) {
        drc::FailureDomainRecord record;
        record.id = drc::FailureDomainId{id};
        record.name = name;
        DRC_REQUIRE_OK(coordinator->define_failure_domain(record));
    }

    void site(std::uint64_t id,
              const std::string& name,
              std::uint64_t domain_id,
              std::uint64_t capacity = 1000) {
        drc::SiteRecord record;
        record.id = drc::SiteId{id};
        record.name = name;
        record.domain = drc::FailureDomainId{domain_id};
        record.capacity_units = capacity;
        DRC_REQUIRE_OK(coordinator->define_site(record));
    }

    void obligation(std::uint64_t id,
                    const std::string& name,
                    drc::RecoveryClass cls,
                    std::uint64_t home,
                    std::uint64_t capacity,
                    const std::vector<std::uint64_t>& depends_on = {}) {
        drc::ProtectedObligation record;
        record.id = drc::ObligationId{id};
        record.name = name;
        record.recovery_class = cls;
        record.home_site = drc::SiteId{home};
        record.required_capacity_units = capacity;
        for (const std::uint64_t dependency : depends_on) {
            record.depends_on.push_back(drc::ObligationId{dependency});
        }
        DRC_REQUIRE_OK(coordinator->define_obligation(record));
    }

    void capability(std::uint64_t site_id,
                    std::uint64_t generation,
                    std::uint64_t units,
                    std::uint32_t mask = all_classes()) {
        drc::DestinationCapability evidence;
        evidence.site = drc::SiteId{site_id};
        evidence.generation = drc::Generation{generation};
        evidence.available_capacity_units = units;
        evidence.supported_classes_mask = mask;
        evidence.observed_at = clock->now_nanos();
        evidence.observation_epoch = coordinator->epoch();
        evidence.source = "test-harness";
        DRC_REQUIRE_OK(coordinator->record_capability(evidence));
    }

    void readiness(std::uint64_t site_id,
                   std::uint64_t generation,
                   bool power = true,
                   bool network = true) {
        drc::SiteReadinessEvidence evidence;
        evidence.site = drc::SiteId{site_id};
        evidence.generation = drc::Generation{generation};
        evidence.observed_at = clock->now_nanos();
        evidence.observation_epoch = coordinator->epoch();
        evidence.source = "test-harness";
        evidence.checks.push_back(drc::ReadinessCheck{"power", power});
        evidence.checks.push_back(drc::ReadinessCheck{"network", network});
        DRC_REQUIRE_OK(coordinator->record_readiness(evidence));
    }

    void federation(std::uint64_t generation,
                    bool known,
                    bool partitioned,
                    const std::vector<std::uint64_t>& unreachable = {}) {
        drc::FederationState state;
        state.generation = drc::Generation{generation};
        state.observed_at = clock->now_nanos();
        state.observation_epoch = coordinator->epoch();
        state.source = "test-harness";
        state.known = known;
        state.partitioned = partitioned;
        for (const std::uint64_t domain_id : unreachable) {
            state.unreachable_domains.push_back(drc::FailureDomainId{domain_id});
        }
        DRC_REQUIRE_OK(coordinator->record_federation_state(state));
    }

    void synthetic_endpoints(drc::EffectOutcome outcome = drc::EffectOutcome::Completed) {
        std::uint64_t id = 1;
        for (const drc::EffectDomain domain :
             {drc::EffectDomain::SiteControlPlane, drc::EffectDomain::PlacementReservationCapacity,
              drc::EffectDomain::AsiExecutionRecovery, drc::EffectDomain::DfiNetworkRecovery}) {
            drc::EndpointDescriptor descriptor;
            descriptor.domain = domain;
            descriptor.id = drc::EndpointId{id};
            descriptor.name = std::string{"synthetic-"} + std::string{drc::to_string(domain)};
            descriptor.synthetic.outcome = outcome;
            DRC_REQUIRE_OK(coordinator->register_endpoint(descriptor));
            id += 1;
        }
    }

    void endpoint(drc::EffectDomain domain,
                  const drc::SyntheticBehavior& behavior,
                  std::uint64_t id = 1) {
        drc::EndpointDescriptor descriptor;
        descriptor.domain = domain;
        descriptor.id = drc::EndpointId{id};
        descriptor.name = std::string{"synthetic-"} + std::string{drc::to_string(domain)};
        descriptor.synthetic = behavior;
        DRC_REQUIRE_OK(coordinator->register_endpoint(descriptor));
    }

    // ---- lifecycle helpers ------------------------------------------------
    drc::DisasterEventId declare(const std::vector<std::uint64_t>& sites,
                                 std::uint64_t supersedes = 0,
                                 const std::string& reason = "site is down") {
        drc::DisasterDeclaration declaration;
        declaration.declared_by = "operator";
        declaration.reason = reason;
        declaration.severity = 3;
        for (const std::uint64_t site_id : sites) {
            declaration.affected_sites.push_back(drc::SiteId{site_id});
        }
        if (supersedes != 0) {
            declaration.supersedes = drc::DisasterEventId{supersedes};
        }
        drc::Result<drc::DisasterEventId> declared = coordinator->declare_disaster(declaration);
        DRC_REQUIRE(declared.ok());
        return declared.value();
    }

    drc::RecoveryPlanId plan(drc::DisasterEventId event,
                             bool allow_partial = false,
                             bool expect_success = true) {
        drc::CreatePlanRequest request;
        request.event = event;
        request.requested_by = "operator";
        request.allow_partial_placement = allow_partial;
        drc::Result<drc::RecoveryPlanId> created = coordinator->create_plan(request);
        if (expect_success) {
            DRC_REQUIRE(created.ok());
            return created.value();
        }
        DRC_REQUIRE(!created.ok());
        return drc::RecoveryPlanId{};
    }

    drc::AdvanceReport advance(drc::DisasterEventId event,
                               std::uint32_t rounds = 8,
                               std::uint32_t dispatches = 64) {
        drc::AdvanceRequest request;
        request.event = event;
        request.max_rounds = rounds;
        request.max_dispatches = dispatches;
        drc::Result<drc::AdvanceReport> report = coordinator->advance(request);
        DRC_REQUIRE(report.ok());
        return report.value();
    }

    // Advances until the engine reports nothing left to do, with a hard bound
    // on the number of calls. There is no timeout: the bound is on work, not on
    // wall-clock time.
    drc::AdvanceReport settle(drc::DisasterEventId event, std::uint32_t max_calls = 32) {
        drc::AdvanceReport last;
        for (std::uint32_t call = 0; call < max_calls; ++call) {
            last = advance(event);
            if (last.quiescent) {
                return last;
            }
        }
        DRC_REQUIRE(false);
        return last;
    }

    [[nodiscard]] drc::DisasterEvent event(drc::DisasterEventId id) const {
        drc::Result<drc::DisasterEvent> value = coordinator->event(id);
        DRC_REQUIRE(value.ok());
        return value.value();
    }

    [[nodiscard]] drc::RecoveryPlan plan_of(drc::RecoveryPlanId id) const {
        drc::Result<drc::RecoveryPlan> value = coordinator->plan(id);
        DRC_REQUIRE(value.ok());
        return value.value();
    }

    [[nodiscard]] drc::RecoveryPlanId active_plan(drc::DisasterEventId id) const {
        drc::Result<drc::RecoveryPlanId> value = coordinator->active_plan(id);
        DRC_REQUIRE(value.ok());
        return value.value();
    }

    [[nodiscard]] drc::StepState step_state(drc::RecoveryPlanId plan_id,
                                            drc::StepKind kind,
                                            std::uint64_t obligation_id = 0) const {
        const drc::RecoveryPlan plan = plan_of(plan_id);
        for (const drc::RecoveryStepId step_id : plan.order) {
            const drc::RecoveryStep& step = plan.steps.at(step_id);
            if (step.kind != kind) {
                continue;
            }
            if (obligation_id != 0 && step.obligation.value() != obligation_id) {
                continue;
            }
            return step.state;
        }
        DRC_REQUIRE(false);
        return drc::StepState::Pending;
    }

    [[nodiscard]] std::size_t count_steps(drc::RecoveryPlanId plan_id, drc::StepState state) const {
        const drc::RecoveryPlan plan = plan_of(plan_id);
        std::size_t total = 0;
        for (const drc::RecoveryStepId step_id : plan.order) {
            if (plan.steps.at(step_id).state == state) {
                total += 1;
            }
        }
        return total;
    }
};

}  // namespace drctest
