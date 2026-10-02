// benchmarks/benchmark_main.cpp
//
// Real-time benchmarks for the Disaster Recovery Coordinator public API.
//
// Every timing printed by this program is a direct std::chrono::steady_clock
// measurement of work this process actually performed. Nothing is estimated,
// extrapolated, or invented: when a measurement cannot be performed the
// benchmark says so, prints no number for it, and the process exits non-zero.
//
// The benchmarked topology is registered entirely through the public API and
// is entirely durable:
//   * 2 failure domains,
//   * 8 sites (4 in the affected domain A, 4 in the surviving domain B),
//   * 64 protected obligations homed in domain A, 15 dependency edges,
//   * 4 registered in-process synthetic endpoints, one per EffectDomain,
//   * fresh capability and readiness evidence for every site,
//   * one disaster event, one recovery plan, advanced to completion.
//
// The coordinator runs on drc::ManualClock, so time never advances on its own:
// evidence freshness and every timestamp in the run are deterministic.
//
// Usage:
//   drc_benchmarks [--quick] [--iterations N]
//
//   (no arguments)  full run
//   --quick         fewer iterations; this is the mode CTest uses
//   --iterations N  run N timed operations per benchmark (N is clamped where a
//                   hard library limit applies; clamping is reported)
//
// Exit status: 0 when every benchmark was performed, 1 when any measurement
// could not be performed, 2 for a command line error.

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "drc/engine.hpp"
#include "drc/evidence.hpp"
#include "drc/fileio.hpp"
#include "drc/model.hpp"
#include "drc/plan.hpp"
#include "drc/ports.hpp"
#include "drc/time.hpp"
#include "drc/version.hpp"

namespace {

using SteadyClock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------
// Benchmarked topology
// ---------------------------------------------------------------------------

constexpr std::uint32_t kSitesPerDomain = 4;
constexpr std::uint32_t kSiteCount = 2 * kSitesPerDomain;
constexpr std::uint32_t kObligationCount = 64;
constexpr std::uint32_t kDependencyStride = 4;
// Obligation i (i >= stride, i % stride == 0) depends on obligation i - stride.
constexpr std::uint32_t kDependencyEdges = kObligationCount / kDependencyStride - 1u;
// Destination capacity is deliberately finite: 80 units per surviving site and
// 4 units per obligation spreads the 64 obligations over all four destinations
// instead of letting one site absorb every placement.
constexpr std::uint64_t kSiteCapacityUnits = 80;
constexpr std::uint64_t kObligationCapacityUnits = 4;
constexpr std::uint32_t kAllRecoveryClassesMask = 0x1Fu;
constexpr std::uint32_t kWorkerThreads = 4;
constexpr std::uint64_t kFreshnessWindowNanos =
    300ULL * static_cast<std::uint64_t>(drc::kNanosPerSecond);
constexpr std::uint64_t kSiteIdBase = 100;
constexpr std::uint64_t kObligationIdBase = 1000;
constexpr std::uint64_t kEndpointIdBase = 10;
// Internal safety bound for one plan run. This is not a process timeout and not
// a measurement budget: a run that exceeds it is reported as a failed
// measurement instead of being allowed to spin forever.
constexpr std::uint64_t kPlanRunSafetySeconds = 300;
constexpr std::uint64_t kStallYieldLimit = 20000;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

[[nodiscard]] std::uint64_t nanos_between(SteadyClock::time_point first,
                                          SteadyClock::time_point last) {
    const auto delta = std::chrono::duration_cast<std::chrono::nanoseconds>(last - first).count();
    if (delta <= 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(delta);
}

[[nodiscard]] std::string to_utf8(const std::filesystem::path& value) {
    const std::u8string text = value.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

[[nodiscard]] std::string compiler_string() {
#if defined(_MSC_VER)
    return "MSVC " + std::to_string(static_cast<std::uint64_t>(_MSC_VER));
#elif defined(__clang__)
    return std::string{"clang "} + std::string{__clang_version__};
#elif defined(__GNUC__)
    return "gcc " + std::to_string(static_cast<std::uint64_t>(__GNUC__)) + "." +
           std::to_string(static_cast<std::uint64_t>(__GNUC_MINOR__)) + "." +
           std::to_string(static_cast<std::uint64_t>(__GNUC_PATCHLEVEL__));
#else
    return "unknown compiler";
#endif
}

[[nodiscard]] std::string build_type_string() {
#if defined(NDEBUG)
    return "Release / optimized (NDEBUG defined)";
#else
    return "Debug / unoptimized (NDEBUG not defined)";
#endif
}

// Integer formatting: whole nanoseconds plus two decimals, derived from the
// measured integers rather than from a floating point conversion.
[[nodiscard]] std::string format_ns_per_op(std::uint64_t total_nanos, std::uint64_t operations) {
    if (operations == 0) {
        return "not measured";
    }
    const std::uint64_t whole = total_nanos / operations;
    const std::uint64_t remainder = total_nanos % operations;
    const std::uint64_t hundredths = (remainder * 100ULL) / operations;
    std::string text = std::to_string(whole);
    text.push_back('.');
    if (hundredths < 10ULL) {
        text.push_back('0');
    }
    text.append(std::to_string(hundredths));
    return text;
}

[[nodiscard]] drc::RecoveryPolicy benchmark_policy() {
    drc::RecoveryPolicy policy;
    policy.generation = drc::Generation{1};
    policy.evidence_freshness_window = static_cast<drc::UnixNanos>(kFreshnessWindowNanos);
    policy.max_step_attempts = 3;
    policy.max_in_flight_steps = 8;
    policy.require_protected_placement = true;
    policy.conflict_on_equal_generation_evidence = true;
    // "No federation observation" stays "no known partition", which is the
    // documented default and keeps destination domains reachable.
    policy.require_federation_evidence = false;
    return policy;
}

// ---------------------------------------------------------------------------
// Measurement record and printing
// ---------------------------------------------------------------------------

struct Measurement {
    std::string label;
    std::string description;
    std::string note;
    std::uint64_t iterations = 0;
    std::uint64_t total_nanos = 0;
    bool performed = false;
    std::string failure;
};

void print_measurement(const Measurement& measurement) {
    std::cout << "benchmark    : " << measurement.label << "\n";
    if (!measurement.performed) {
        std::cout << "  iterations : not measured\n";
        std::cout << "  total_ns   : not measured\n";
        std::cout << "  ns_per_op  : not measured\n";
        std::cout << "  measured   : " << measurement.description << "\n";
        std::cout << "  FAILED     : " << measurement.failure << "\n";
        std::cout << "\n";
        return;
    }
    std::cout << "  iterations : " << measurement.iterations << "\n";
    std::cout << "  total_ns   : " << measurement.total_nanos << "\n";
    std::cout << "  ns_per_op  : "
              << format_ns_per_op(measurement.total_nanos, measurement.iterations) << "\n";
    if (!measurement.note.empty()) {
        std::cout << "  note       : " << measurement.note << "\n";
    }
    std::cout << "  measured   : " << measurement.description << "\n";
    std::cout << "  source     : REAL - measured with std::chrono::steady_clock, no estimation\n";
    std::cout << "\n";
}

// ---------------------------------------------------------------------------
// Fixture: one temporary directory plus one open coordinator
// ---------------------------------------------------------------------------

struct Fixture {
    std::filesystem::path path;
    std::string directory;
    std::shared_ptr<drc::ManualClock> clock;
    std::unique_ptr<drc::Coordinator> coordinator;

    Fixture() = default;
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    ~Fixture() { close(); }

    [[nodiscard]] bool open(const std::filesystem::path& root, const std::string& leaf,
                            bool auto_compact, std::string& error) {
        path = root / leaf;
        directory = to_utf8(path);
        const drc::Status created = drc::fileio::ensure_directory(directory);
        if (!created.ok()) {
            error = "fileio::ensure_directory(" + directory + "): " + created.to_string();
            return false;
        }
        clock = std::make_shared<drc::ManualClock>();
        drc::CoordinatorOptions options;
        options.directory = directory;
        options.owner = "drc-benchmarks";
        options.worker_threads = kWorkerThreads;
        options.clock = clock;
        options.allow_lock_takeover = true;
        options.auto_compact = auto_compact;
        options.policy = benchmark_policy();
        drc::Result<std::unique_ptr<drc::Coordinator>> opened =
            drc::Coordinator::open(std::move(options));
        if (!opened.ok()) {
            error = "Coordinator::open(" + directory + "): " + opened.status().to_string();
            return false;
        }
        coordinator = std::move(opened).value();
        return true;
    }

    void close() {
        if (coordinator != nullptr) {
            (void)coordinator->shutdown();
            coordinator.reset();
        }
        clock.reset();
        if (!path.empty()) {
            std::error_code code;
            std::filesystem::remove_all(path, code);
            if (code) {
                std::cout << "warning: could not remove temporary directory " << directory
                          << ": " << code.message() << "\n";
            }
            path.clear();
            directory.clear();
        }
    }
};

// ---------------------------------------------------------------------------
// Topology registration
// ---------------------------------------------------------------------------

struct Topology {
    drc::FailureDomainId domain_a;
    drc::FailureDomainId domain_b;
    std::vector<drc::SiteId> source_sites;
    std::vector<drc::SiteId> destination_sites;
    std::vector<drc::ObligationId> obligations;
};

[[nodiscard]] bool require_ok(const drc::Status& status, const char* what, std::string& error) {
    if (status.ok()) {
        return true;
    }
    error = std::string{what} + " failed: " + status.to_string();
    return false;
}

[[nodiscard]] drc::RecoveryClass class_for(std::uint32_t index) {
    if ((index % 8u) == 0u) {
        return drc::RecoveryClass::SafetyCritical;
    }
    if ((index % 2u) == 0u) {
        return drc::RecoveryClass::Protected;
    }
    return drc::RecoveryClass::Standard;
}

[[nodiscard]] bool register_topology(Fixture& fixture, Topology& topology, std::string& error) {
    drc::Coordinator& coordinator = *fixture.coordinator;
    const drc::Epoch epoch = coordinator.epoch();
    const drc::UnixNanos now = fixture.clock->now_nanos();

    topology.domain_a = drc::FailureDomainId{1};
    topology.domain_b = drc::FailureDomainId{2};
    for (const drc::FailureDomainId domain : {topology.domain_a, topology.domain_b}) {
        drc::FailureDomainRecord record;
        record.id = domain;
        record.name = domain == topology.domain_a ? "domain-a" : "domain-b";
        if (!require_ok(coordinator.define_failure_domain(record), "define_failure_domain",
                        error)) {
            return false;
        }
    }

    topology.source_sites.clear();
    topology.destination_sites.clear();
    topology.obligations.clear();
    for (std::uint32_t index = 0; index < kSiteCount; ++index) {
        const bool source = index < kSitesPerDomain;
        drc::SiteRecord record;
        record.id = drc::SiteId{kSiteIdBase + index};
        record.name = std::string{source ? "source-site-" : "destination-site-"} +
                      std::to_string(static_cast<std::uint64_t>(index));
        record.domain = source ? topology.domain_a : topology.domain_b;
        record.capacity_units = kSiteCapacityUnits;
        if (!require_ok(coordinator.define_site(record), "define_site", error)) {
            return false;
        }
        if (source) {
            topology.source_sites.push_back(record.id);
        } else {
            topology.destination_sites.push_back(record.id);
        }
    }

    for (std::uint32_t index = 0; index < kObligationCount; ++index) {
        drc::ProtectedObligation obligation;
        obligation.id = drc::ObligationId{kObligationIdBase + index};
        obligation.name = "obligation-" + std::to_string(static_cast<std::uint64_t>(index));
        obligation.recovery_class = class_for(index);
        obligation.home_site =
            topology.source_sites[static_cast<std::size_t>(index) % topology.source_sites.size()];
        obligation.home_domain = topology.domain_a;
        obligation.required_capacity_units = kObligationCapacityUnits;
        if (index >= kDependencyStride && (index % kDependencyStride) == 0u) {
            obligation.depends_on.push_back(drc::ObligationId{kObligationIdBase + index -
                                                              kDependencyStride});
        }
        if (!require_ok(coordinator.define_obligation(obligation), "define_obligation", error)) {
            return false;
        }
        topology.obligations.push_back(obligation.id);
    }

    for (std::uint32_t index = 0; index < kSiteCount; ++index) {
        const drc::SiteId site = drc::SiteId{kSiteIdBase + index};
        drc::DestinationCapability capability;
        capability.site = site;
        capability.generation = drc::Generation{1};
        capability.available_capacity_units = kSiteCapacityUnits;
        capability.supported_classes_mask = kAllRecoveryClassesMask;
        // Fresh evidence: observed now, in this coordinator incarnation.
        capability.observed_at = now;
        capability.observation_epoch = epoch;
        capability.source = "benchmark-capacity";
        if (!require_ok(coordinator.record_capability(capability), "record_capability", error)) {
            return false;
        }

        drc::SiteReadinessEvidence readiness;
        readiness.site = site;
        readiness.generation = drc::Generation{1};
        readiness.observed_at = now;
        readiness.observation_epoch = epoch;
        readiness.source = "benchmark-readiness";
        drc::ReadinessCheck check;
        check.name = "power";
        check.passed = true;
        readiness.checks.push_back(check);
        if (!require_ok(coordinator.record_readiness(readiness), "record_readiness", error)) {
            return false;
        }
    }

    for (std::uint32_t index = 0; index < drc::kEffectDomainCount; ++index) {
        drc::EndpointDescriptor descriptor;
        descriptor.domain = static_cast<drc::EffectDomain>(index);
        descriptor.id = drc::EndpointId{kEndpointIdBase + index};
        descriptor.name = "synthetic-" + std::string{drc::to_string(descriptor.domain)};
        // An empty command means the endpoint is in-process and deterministic:
        // no child process, no pipe, no timeout.
        descriptor.command.clear();
        descriptor.synthetic.outcome = drc::EffectOutcome::Completed;
        if (!require_ok(coordinator.register_endpoint(descriptor), "register_endpoint", error)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool verify_registered_state(drc::Coordinator& coordinator, std::string& error) {
    drc::Result<std::vector<drc::SiteId>> sites = coordinator.sites();
    if (!sites.ok()) {
        error = "sites(): " + sites.status().to_string();
        return false;
    }
    drc::Result<std::vector<drc::ObligationId>> obligations = coordinator.obligations();
    if (!obligations.ok()) {
        error = "obligations(): " + obligations.status().to_string();
        return false;
    }
    drc::Result<std::vector<drc::EndpointDescriptor>> endpoints = coordinator.endpoints();
    if (!endpoints.ok()) {
        error = "endpoints(): " + endpoints.status().to_string();
        return false;
    }
    if (sites.value().size() != kSiteCount) {
        error = "expected " + std::to_string(kSiteCount) + " registered sites, found " +
                std::to_string(sites.value().size());
        return false;
    }
    if (obligations.value().size() != kObligationCount) {
        error = "expected " + std::to_string(kObligationCount) + " registered obligations, found " +
                std::to_string(obligations.value().size());
        return false;
    }
    if (endpoints.value().size() != drc::kEffectDomainCount) {
        error = "expected " + std::to_string(drc::kEffectDomainCount) +
                " registered endpoints, found " + std::to_string(endpoints.value().size());
        return false;
    }
    return true;
}

[[nodiscard]] bool setup_full_topology(Fixture& fixture, Topology& topology,
                                       const std::filesystem::path& root,
                                       const std::string& leaf, bool auto_compact,
                                       std::string& error) {
    if (!fixture.open(root, leaf, auto_compact, error)) {
        return false;
    }
    if (!register_topology(fixture, topology, error)) {
        return false;
    }
    return verify_registered_state(*fixture.coordinator, error);
}

// ---------------------------------------------------------------------------
// Plan helpers
// ---------------------------------------------------------------------------

struct PlanShape {
    std::uint64_t steps = 0;
    std::uint64_t assignments = 0;
    std::uint64_t unplaced = 0;
    std::uint64_t deferred = 0;

    [[nodiscard]] bool operator==(const PlanShape& other) const {
        return steps == other.steps && assignments == other.assignments &&
               unplaced == other.unplaced && deferred == other.deferred;
    }
    [[nodiscard]] bool operator!=(const PlanShape& other) const { return !(*this == other); }
};

[[nodiscard]] std::string shape_description(const PlanShape& shape) {
    return std::to_string(shape.steps) + " steps, " + std::to_string(shape.assignments) +
           " placement assignments, " + std::to_string(shape.unplaced) + " unplaced, " +
           std::to_string(shape.deferred) + " deferred";
}

[[nodiscard]] bool read_plan_shape(drc::Coordinator& coordinator, drc::RecoveryPlanId plan_id,
                                   PlanShape& shape, std::string& error) {
    drc::Result<drc::RecoveryPlan> loaded = coordinator.plan(plan_id);
    if (!loaded.ok()) {
        error = "plan(" + plan_id.to_string() + "): " + loaded.status().to_string();
        return false;
    }
    const drc::RecoveryPlan& plan = loaded.value();
    shape.steps = static_cast<std::uint64_t>(plan.steps.size());
    shape.assignments = static_cast<std::uint64_t>(plan.assignments.size());
    shape.unplaced = static_cast<std::uint64_t>(plan.unplaced.size());
    shape.deferred = static_cast<std::uint64_t>(plan.deferred.size());
    return true;
}

[[nodiscard]] bool declare_benchmark_event(drc::Coordinator& coordinator, const Topology& topology,
                                           drc::DisasterEventId supersedes,
                                           drc::DisasterEventId& event, std::string& error) {
    drc::DisasterDeclaration declaration;
    declaration.declared_by = "benchmark";
    declaration.reason = "benchmark incident covering failure domain A";
    declaration.severity = 3;
    declaration.affected_domains.push_back(topology.domain_a);
    declaration.supersedes = supersedes;
    drc::Result<drc::DisasterEventId> declared = coordinator.declare_disaster(declaration);
    if (!declared.ok()) {
        error = "declare_disaster: " + declared.status().to_string();
        return false;
    }
    event = declared.value();
    return true;
}

struct RunningPlan {
    drc::DisasterEventId event;
    drc::RecoveryPlanId plan;
    std::uint64_t steps = 0;
};

// Declares a fresh event, builds its recovery plan and starts recovery. Every
// call is untimed by the caller; the returned step count is read back from the
// stored plan, never assumed.
[[nodiscard]] bool prepare_running_plan(drc::Coordinator& coordinator, const Topology& topology,
                                        RunningPlan& running, std::string& error) {
    if (!declare_benchmark_event(coordinator, topology, drc::DisasterEventId{}, running.event,
                                 error)) {
        return false;
    }
    drc::CreatePlanRequest request;
    request.event = running.event;
    request.requested_by = "benchmark";
    request.allow_partial_placement = false;
    drc::Result<drc::RecoveryPlanId> created = coordinator.create_plan(request);
    if (!created.ok()) {
        error = "create_plan: " + created.status().to_string();
        return false;
    }
    running.plan = created.value();
    PlanShape shape;
    if (!read_plan_shape(coordinator, running.plan, shape, error)) {
        return false;
    }
    if (shape.unplaced != 0 || shape.deferred != 0) {
        error = "plan is not complete: " + shape_description(shape);
        return false;
    }
    running.steps = shape.steps;
    if (!require_ok(coordinator.begin_recovery(running.event), "begin_recovery", error)) {
        return false;
    }
    return true;
}

[[nodiscard]] std::string notes_of(const drc::AdvanceReport& report) {
    if (report.notes.empty()) {
        return "(none)";
    }
    const std::size_t limit = report.notes.size() < 3 ? report.notes.size() : 3;
    std::string text;
    for (std::size_t index = 0; index < limit; ++index) {
        if (index != 0) {
            text.append("; ");
        }
        text.append(report.notes[index]);
    }
    if (report.notes.size() > limit) {
        text.append("; ...");
    }
    return text;
}

// Drives one plan to completion with repeated advance() calls and accumulates
// the time actually spent inside advance(). Every returned number is measured.
[[nodiscard]] bool run_plan_to_completion(drc::Coordinator& coordinator, drc::DisasterEventId event,
                                         std::uint64_t total_steps, std::uint64_t& advance_nanos,
                                         std::uint64_t& advance_calls,
                                         std::uint64_t& steps_succeeded, std::string& error) {
    advance_nanos = 0;
    advance_calls = 0;
    steps_succeeded = 0;
    const SteadyClock::time_point deadline =
        SteadyClock::now() + std::chrono::seconds(kPlanRunSafetySeconds);
    std::uint64_t idle_calls = 0;
    for (;;) {
        drc::AdvanceRequest request;
        request.event = event;
        request.max_rounds = 8;
        request.max_dispatches = 64;
        const SteadyClock::time_point started = SteadyClock::now();
        drc::Result<drc::AdvanceReport> advanced = coordinator.advance(request);
        const SteadyClock::time_point finished = SteadyClock::now();
        advance_nanos += nanos_between(started, finished);
        advance_calls += 1;
        if (!advanced.ok()) {
            error = "advance: " + advanced.status().to_string();
            return false;
        }
        const drc::AdvanceReport& report = advanced.value();
        steps_succeeded = static_cast<std::uint64_t>(report.steps_succeeded);
        if (report.steps_failed != 0) {
            error = "advance reported " + std::to_string(report.steps_failed) +
                    " failed step(s); notes: " + notes_of(report);
            return false;
        }
        if (steps_succeeded == total_steps) {
            return true;
        }
        const bool progress = report.dispatched != 0 || report.receipts_applied != 0;
        if (progress) {
            idle_calls = 0;
        } else {
            idle_calls += 1;
            // The dispatch worker is asynchronous; yield so it can publish its
            // receipt instead of burning the whole quantum.
            std::this_thread::yield();
        }
        if (idle_calls > kStallYieldLimit) {
            error = "plan stalled at " + std::to_string(steps_succeeded) + " of " +
                    std::to_string(total_steps) + " steps (" +
                    std::to_string(report.steps_deferred) + " deferred, " +
                    std::to_string(report.steps_indeterminate) + " indeterminate, " +
                    std::to_string(report.steps_in_flight) + " in flight); notes: " +
                    notes_of(report);
            return false;
        }
        if (SteadyClock::now() > deadline) {
            error = "plan did not complete inside the internal " +
                    std::to_string(kPlanRunSafetySeconds) + " s safety bound (" +
                    std::to_string(steps_succeeded) + " of " + std::to_string(total_steps) +
                    " steps succeeded)";
            return false;
        }
    }
}

// ---------------------------------------------------------------------------
// Benchmark 1: plan construction cost (create_plan) on a fresh event
// ---------------------------------------------------------------------------

[[nodiscard]] Measurement bench_create_plan(const std::filesystem::path& root,
                                            std::uint64_t iterations) {
    Measurement measurement;
    measurement.label = "create_plan";
    measurement.description =
        "one Coordinator::create_plan on a freshly declared event (plan build + plan record + "
        "commit), topology of 2 failure domains / 8 sites / 64 obligations / 4 synthetic endpoints";
    std::uint64_t total_nanos = 0;
    std::uint64_t performed = 0;
    PlanShape reference;
    bool have_reference = false;
    std::string error;
    for (std::uint64_t index = 0; index <= iterations; ++index) {
        const bool warmup = index == 0;
        Fixture fixture;
        Topology topology;
        if (!setup_full_topology(fixture, topology, root,
                                 "create_plan_" + std::to_string(index), true, error)) {
            measurement.failure = error;
            return measurement;
        }
        drc::Coordinator& coordinator = *fixture.coordinator;
        drc::DisasterEventId event;
        if (!declare_benchmark_event(coordinator, topology, drc::DisasterEventId{}, event,
                                     error)) {
            measurement.failure = error;
            return measurement;
        }
        drc::CreatePlanRequest request;
        request.event = event;
        request.requested_by = "benchmark";
        request.allow_partial_placement = false;
        const SteadyClock::time_point started = SteadyClock::now();
        drc::Result<drc::RecoveryPlanId> created = coordinator.create_plan(request);
        const SteadyClock::time_point finished = SteadyClock::now();
        if (!created.ok()) {
            measurement.failure = "create_plan: " + created.status().to_string();
            return measurement;
        }
        PlanShape shape;
        if (!read_plan_shape(coordinator, created.value(), shape, error)) {
            measurement.failure = error;
            return measurement;
        }
        if (shape.unplaced != 0 || shape.deferred != 0) {
            measurement.failure = "plan is not complete: " + shape_description(shape);
            return measurement;
        }
        if (!have_reference) {
            reference = shape;
            have_reference = true;
        } else if (shape != reference) {
            measurement.failure = "plan shape changed between iterations: " +
                                  shape_description(shape) + " vs " +
                                  shape_description(reference);
            return measurement;
        }
        if (warmup) {
            continue;
        }
        total_nanos += nanos_between(started, finished);
        performed += 1;
    }
    measurement.iterations = performed;
    measurement.total_nanos = total_nanos;
    measurement.performed = true;
    measurement.note = "plan shape: " + shape_description(reference) +
                       "; each iteration opens a fresh coordinator in a fresh directory so every "
                       "plan is built against an identical durable state (setup excluded)";
    return measurement;
}

// ---------------------------------------------------------------------------
// Benchmark 2: advance() to completion, per step and per whole plan
// ---------------------------------------------------------------------------

struct AdvanceMeasurements {
    Measurement per_step;
    Measurement completion;
};

[[nodiscard]] AdvanceMeasurements bench_plan_advance(const std::filesystem::path& root,
                                                    std::uint64_t runs) {
    AdvanceMeasurements results;
    results.per_step.label = "plan_step_advance";
    results.per_step.description =
        "time spent inside Coordinator::advance() divided by the recovery steps that reached "
        "Succeeded while driving one 8-site / 64-obligation plan to completion";
    results.completion.label = "plan_completion";
    results.completion.description =
        "wall clock from the first advance() call to the last one for one complete plan run "
        "(includes waiting for the asynchronous dispatch workers; create_plan excluded)";

    std::uint64_t advance_nanos = 0;
    std::uint64_t advance_calls = 0;
    std::uint64_t steps_done = 0;
    std::uint64_t completion_nanos = 0;
    std::uint64_t performed_runs = 0;
    std::uint64_t plan_steps = 0;
    std::uint64_t commits = 0;
    std::string error;
    for (std::uint64_t index = 0; index <= runs; ++index) {
        const bool warmup = index == 0;
        Fixture fixture;
        Topology topology;
        if (!setup_full_topology(fixture, topology, root, "plan_advance_" + std::to_string(index),
                                 true, error)) {
            results.per_step.failure = error;
            results.completion.failure = error;
            return results;
        }
        drc::Coordinator& coordinator = *fixture.coordinator;
        RunningPlan running;
        if (!prepare_running_plan(coordinator, topology, running, error)) {
            results.per_step.failure = error;
            results.completion.failure = error;
            return results;
        }
        plan_steps = running.steps;
        std::uint64_t run_advance_nanos = 0;
        std::uint64_t run_advance_calls = 0;
        std::uint64_t run_steps = 0;
        const SteadyClock::time_point started = SteadyClock::now();
        const bool completed =
            run_plan_to_completion(coordinator, running.event, running.steps, run_advance_nanos,
                                   run_advance_calls, run_steps, error);
        const SteadyClock::time_point finished = SteadyClock::now();
        if (!completed) {
            results.per_step.failure = error;
            results.completion.failure = error;
            return results;
        }
        if (warmup) {
            continue;
        }
        advance_nanos += run_advance_nanos;
        advance_calls += run_advance_calls;
        steps_done += run_steps;
        completion_nanos += nanos_between(started, finished);
        commits = coordinator.stats().commits;
        performed_runs += 1;
    }

    const std::string shape_note =
        "plan shape: " + std::to_string(plan_steps) +
        " steps; advance() calls in the whole benchmark: " + std::to_string(advance_calls) +
        "; last run ended with " + std::to_string(commits) + " commits in the coordinator";

    results.per_step.iterations = steps_done;
    results.per_step.total_nanos = advance_nanos;
    results.per_step.performed = steps_done != 0;
    if (!results.per_step.performed) {
        results.per_step.failure = "no step completed, so no per-step time exists";
    }
    results.per_step.note = shape_note;

    results.completion.iterations = performed_runs;
    results.completion.total_nanos = completion_nanos;
    results.completion.performed = performed_runs != 0;
    if (!results.completion.performed) {
        results.completion.failure = "no plan run completed";
    }
    results.completion.note = shape_note;
    return results;
}

// ---------------------------------------------------------------------------
// Benchmark 3: durable checkpoint on a state of the same size
// ---------------------------------------------------------------------------

[[nodiscard]] Measurement bench_checkpoint(const std::filesystem::path& root,
                                           std::uint64_t iterations) {
    Measurement measurement;
    measurement.label = "checkpoint";
    measurement.description =
        "one Coordinator::checkpoint() over the full 8-site / 64-obligation state with one fully "
        "executed plan: snapshot file write + snapshot reference record + commit";
    std::string error;
    Fixture fixture;
    Topology topology;
    if (!setup_full_topology(fixture, topology, root, "checkpoint", false, error)) {
        measurement.failure = error;
        return measurement;
    }
    drc::Coordinator& coordinator = *fixture.coordinator;
    RunningPlan running;
    if (!prepare_running_plan(coordinator, topology, running, error)) {
        measurement.failure = error;
        return measurement;
    }
    std::uint64_t warm_nanos = 0;
    std::uint64_t warm_calls = 0;
    std::uint64_t warm_steps = 0;
    if (!run_plan_to_completion(coordinator, running.event, running.steps, warm_nanos, warm_calls,
                                warm_steps, error)) {
        measurement.failure = "could not complete the plan that sizes the state: " + error;
        return measurement;
    }
    if (!require_ok(coordinator.checkpoint(), "warm-up checkpoint", error)) {
        measurement.failure = error;
        return measurement;
    }
    std::uint64_t total_nanos = 0;
    std::uint64_t performed = 0;
    for (std::uint64_t index = 0; index < iterations; ++index) {
        const SteadyClock::time_point started = SteadyClock::now();
        const drc::Status status = coordinator.checkpoint();
        const SteadyClock::time_point finished = SteadyClock::now();
        if (!status.ok()) {
            measurement.failure = "checkpoint: " + status.to_string();
            return measurement;
        }
        total_nanos += nanos_between(started, finished);
        performed += 1;
    }
    measurement.iterations = performed;
    measurement.total_nanos = total_nanos;
    measurement.performed = performed != 0;
    if (!measurement.performed) {
        measurement.failure = "no checkpoint completed";
    }
    measurement.note = "state: " + std::to_string(running.steps) +
                       " plan steps executed; auto_compact is disabled for this fixture so every "
                       "measured call is exactly one snapshot write plus one commit";
    return measurement;
}

// ---------------------------------------------------------------------------
// Benchmark 4: one durable commit boundary
// ---------------------------------------------------------------------------

[[nodiscard]] Measurement bench_durable_commit(const std::filesystem::path& root,
                                               std::uint64_t iterations) {
    Measurement measurement;
    measurement.label = "durable_commit";
    measurement.description =
        "one durable state-changing API call with a fresh identity (define_site): one journal "
        "append plus one commit record flushed to storage";
    std::string error;
    Fixture fixture;
    if (!fixture.open(root, "durable_commit", true, error)) {
        measurement.failure = error;
        return measurement;
    }
    drc::Coordinator& coordinator = *fixture.coordinator;
    drc::FailureDomainRecord domain;
    domain.id = drc::FailureDomainId{1};
    domain.name = "commit-domain";
    if (!require_ok(coordinator.define_failure_domain(domain), "define_failure_domain", error)) {
        measurement.failure = error;
        return measurement;
    }
    const auto define_one = [&coordinator](std::uint64_t index, std::string& failure) {
        drc::SiteRecord record;
        record.id = drc::SiteId{kSiteIdBase + index};
        record.name = "commit-site-" + std::to_string(index);
        record.domain = drc::FailureDomainId{1};
        record.capacity_units = 1;
        const drc::Status status = coordinator.define_site(record);
        if (!status.ok()) {
            failure = "define_site(" + record.id.to_string() + "): " + status.to_string();
            return false;
        }
        return true;
    };
    if (!define_one(0, error)) {
        measurement.failure = "warm-up " + error;
        return measurement;
    }
    std::uint64_t total_nanos = 0;
    std::uint64_t performed = 0;
    for (std::uint64_t index = 1; index <= iterations; ++index) {
        drc::SiteRecord record;
        record.id = drc::SiteId{kSiteIdBase + index};
        record.name = "commit-site-" + std::to_string(index);
        record.domain = drc::FailureDomainId{1};
        record.capacity_units = 1;
        const SteadyClock::time_point started = SteadyClock::now();
        const drc::Status status = coordinator.define_site(record);
        const SteadyClock::time_point finished = SteadyClock::now();
        if (!status.ok()) {
            measurement.failure = "define_site(" + record.id.to_string() + "): " + status.to_string();
            return measurement;
        }
        total_nanos += nanos_between(started, finished);
        performed += 1;
    }
    measurement.iterations = performed;
    measurement.total_nanos = total_nanos;
    measurement.performed = performed != 0;
    if (!measurement.performed) {
        measurement.failure = "no commit was performed";
    }
    measurement.note = "each iteration registers one new site identity, so every call is a distinct "
                       "durable change and there is no cached no-op path";
    return measurement;
}

// ---------------------------------------------------------------------------
// Iteration counts
// ---------------------------------------------------------------------------

struct IterationPlan {
    std::uint64_t create_plan = 0;
    std::uint64_t plan_runs = 0;
    std::uint64_t checkpoints = 0;
    std::uint64_t commits = 0;
};

[[nodiscard]] std::uint64_t clamp(std::uint64_t value, std::uint64_t low, std::uint64_t high,
                                  const char* name, std::vector<std::string>& notes) {
    if (value < low) {
        notes.push_back(std::string{name} + " raised to the minimum of " + std::to_string(low));
        return low;
    }
    if (value > high) {
        notes.push_back(std::string{name} + " clamped to the maximum of " + std::to_string(high));
        return high;
    }
    return value;
}

[[nodiscard]] IterationPlan make_iteration_plan(bool quick, bool explicit_iterations,
                                                std::uint64_t requested,
                                                std::vector<std::string>& notes) {
    IterationPlan plan;
    if (!explicit_iterations) {
        if (quick) {
            plan.create_plan = 4;
            plan.plan_runs = 1;
            plan.checkpoints = 4;
            plan.commits = 32;
        } else {
            plan.create_plan = 16;
            plan.plan_runs = 3;
            plan.checkpoints = 16;
            plan.commits = 256;
        }
        return plan;
    }
    // A plan run costs orders of magnitude more than a single call, and the
    // library retains at most max_events events per coordinator, so the shared
    // override is clamped per benchmark instead of being refused.
    plan.create_plan = clamp(requested, 1, 64, "--iterations (create_plan)", notes);
    plan.plan_runs = clamp(requested, 1, 16, "--iterations (plan runs)", notes);
    plan.checkpoints = clamp(requested, 1, 512, "--iterations (checkpoints)", notes);
    plan.commits = clamp(requested, 1, 4096, "--iterations (durable commits)", notes);
    return plan;
}

// ---------------------------------------------------------------------------
// Header and usage
// ---------------------------------------------------------------------------

void print_usage(const char* program) {
    std::cout << "usage: " << program << " [--quick] [--iterations N]\n"
              << "  --quick        fewer iterations (used by CTest)\n"
              << "  --iterations N run N timed operations per benchmark\n"
              << "  --help         print this text\n";
}

void print_header(bool quick, bool explicit_iterations, const IterationPlan& plan) {
    std::cout << "Disaster Recovery Coordinator benchmarks\n";
    std::cout << "library version     : " << drc::version_string() << "\n";
    std::cout << "compiler            : " << compiler_string() << "\n";
    std::cout << "build type          : " << build_type_string() << "\n";
    std::cout << "sizeof(void*)       : " << sizeof(void*) << "\n";
    std::cout << "hardware_concurrency: " << std::thread::hardware_concurrency() << "\n";
    std::cout << "mode                : "
              << (explicit_iterations ? "explicit --iterations" : (quick ? "quick" : "full"))
              << "\n";
    std::cout << "topology            : 2 failure domains, " << kSiteCount << " sites ("
              << kSitesPerDomain << " affected + " << kSitesPerDomain << " surviving), "
              << kObligationCount << " protected obligations, " << kDependencyEdges
              << " dependency edges, " << drc::kEffectDomainCount << " synthetic endpoints\n";
    std::cout << "worker threads      : " << kWorkerThreads << "\n";
    std::cout << "clock               : drc::ManualClock (deterministic, never advances)\n";
    std::cout << "iterations          : create_plan=" << plan.create_plan
              << ", plan_runs=" << plan.plan_runs << ", checkpoint=" << plan.checkpoints
              << ", durable_commit=" << plan.commits << "\n";
    std::cout << "note                : every number below is a real std::chrono::steady_clock "
                 "measurement; nothing is estimated or extrapolated\n";
    std::cout << "\n";
}

}  // namespace

int main(int argc, char** argv) {
    bool quick = false;
    bool explicit_iterations = false;
    std::uint64_t requested = 0;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--quick") {
            quick = true;
            continue;
        }
        if (argument == "--help" || argument == "-h") {
            print_usage(argv[0]);
            return 0;
        }
        if (argument == "--iterations") {
            if (index + 1 >= argc) {
                std::cerr << "--iterations requires a value\n";
                print_usage(argv[0]);
                return 2;
            }
            const std::string_view text{argv[index + 1]};
            std::uint64_t value = 0;
            const std::from_chars_result parsed =
                std::from_chars(text.data(), text.data() + text.size(), value);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value == 0) {
                std::cerr << "--iterations requires a positive integer\n";
                return 2;
            }
            requested = value;
            explicit_iterations = true;
            index += 1;
            continue;
        }
        std::cerr << "unknown argument: " << argument << "\n";
        print_usage(argv[0]);
        return 2;
    }

    std::vector<std::string> notes;
    const IterationPlan iteration_plan =
        make_iteration_plan(quick, explicit_iterations, requested, notes);
    for (const std::string& note : notes) {
        std::cout << "note: " << note << "\n";
    }

    std::error_code code;
    std::filesystem::path root = std::filesystem::temp_directory_path(code);
    if (code) {
        std::cerr << "could not locate a temporary directory: " << code.message() << "\n";
        return 1;
    }
    const auto stamp = static_cast<std::uint64_t>(SteadyClock::now().time_since_epoch().count());
    root /= "drc_benchmark_" + std::to_string(stamp);
    const drc::Status created = drc::fileio::ensure_directory(to_utf8(root));
    if (!created.ok()) {
        std::cerr << "could not create " << to_utf8(root) << ": " << created.to_string() << "\n";
        return 1;
    }

    print_header(quick, explicit_iterations, iteration_plan);

    const SteadyClock::time_point harness_started = SteadyClock::now();
    std::vector<Measurement> measurements;
    try {
        measurements.push_back(bench_create_plan(root, iteration_plan.create_plan));
        print_measurement(measurements.back());

        const AdvanceMeasurements advance = bench_plan_advance(root, iteration_plan.plan_runs);
        measurements.push_back(advance.per_step);
        print_measurement(advance.per_step);
        measurements.push_back(advance.completion);
        print_measurement(advance.completion);

        measurements.push_back(bench_checkpoint(root, iteration_plan.checkpoints));
        print_measurement(measurements.back());

        measurements.push_back(bench_durable_commit(root, iteration_plan.commits));
        print_measurement(measurements.back());
    } catch (...) {
        // The public API does not throw and this harness does not throw on
        // purpose; this is a last-resort guard so the temporary directory is
        // still removed instead of being left behind.
        std::cout << "FAILED     : unexpected exception escaped the benchmark harness\n";
        std::error_code cleanup_code;
        std::filesystem::remove_all(root, cleanup_code);
        return 1;
    }

    const std::uint64_t harness_nanos = nanos_between(harness_started, SteadyClock::now());
    std::size_t failed = 0;
    for (const Measurement& measurement : measurements) {
        if (!measurement.performed) {
            failed += 1;
        }
    }
    std::cout << "harness wall clock : " << harness_nanos << " ns ("
              << format_ns_per_op(harness_nanos, 1000000000ULL) << " s)\n";
    std::cout << "benchmarks         : " << measurements.size() << " measured, " << failed
              << " failed\n";

    // Leave nothing behind: each fixture already removed its own directory, and
    // the root directory is removed here.
    std::error_code cleanup_code;
    std::filesystem::remove_all(root, cleanup_code);
    if (cleanup_code) {
        std::cout << "warning: could not remove " << to_utf8(root) << ": "
                  << cleanup_code.message() << "\n";
    }

    if (failed != 0) {
        std::cout << "result             : FAILED (" << failed
                  << " benchmark(s) could not be performed)\n";
        return 1;
    }
    std::cout << "result             : all benchmarks performed\n";
    return 0;
}
