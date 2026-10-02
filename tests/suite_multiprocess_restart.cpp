// Real independent OS processes. Every participant in this suite is a separate
// executable speaking the framed pipe protocol, and the console under test is
// the real drcctl binary. A crash or a kill is the scenario, never the verdict:
// no timeout, watchdog, or kill-as-pass rule appears here. The only wall-clock
// bound is the engine's own documented exchange budget for a participant that
// answers nothing, and a failure to make progress is reported as a failure.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "drc/journal.hpp"
#include "drc/process.hpp"
#include "support/harness.hpp"
#include "test.hpp"

using namespace drc;
// The captured-console result type lives in the shared harness.
using drctest::ConsoleResult;

namespace {

// One request/response exchange with a participant that answers nothing is
// bounded by the library's documented exchange budget. It is set low here so a
// silent participant is reported quickly instead of holding a worker for the
// 30 s default; it is the engine's own bound, not a rule invented by the test.
constexpr UnixNanos kExchangeBudget = 200 * kNanosPerMillisecond;

// A silent participant still has to be given time to start and read the request
// before the coordinator gives up on the exchange; a participant that is merely
// slow to start is not the failure this test is about.
constexpr UnixNanos kSilentParticipantBudget = 1500 * kNanosPerMillisecond;

[[nodiscard]] const char* kTopologyScript() {
    return R"(# topology and evidence
domain --id 1 --name domain-a
domain --id 2 --name domain-b
site --id 1 --domain 1 --name site-a1 --capacity 100
site --id 3 --domain 2 --name site-b1 --capacity 1000
obligation --id 10 --home 1 --capacity 10 --class safety_critical --name auth
capability --site 3 --generation 1 --capacity 500
endpoint --domain site_control_plane --id 1 --name scp
endpoint --domain asi_execution_recovery --id 3 --name asi
endpoint --domain dfi_network_recovery --id 4 --name dfi
)";
}

[[nodiscard]] const char* kIncidentScript() {
    return R"(# incident
declare --sites 1 --by operator --reason site-a1-is-down
plan --event 1
begin --event 1
advance --event 1 --rounds 8 --dispatches 64 --repeat 6
advance --event 1 --rounds 8 --dispatches 64 --repeat 6
)";
}

// A console script that walks the whole recovery path with in-process synthetic
// participants, so every commit boundary of the lifecycle can be crashed on.
[[nodiscard]] std::string crash_script() {
    std::string script = kTopologyScript();
    script.append("endpoint --domain placement_reservation_capacity --id 2 --name prc\n");
    script.append(kIncidentScript());
    return script;
}

[[nodiscard]] std::filesystem::path leaf(const std::string& directory, const std::string& name) {
    return std::filesystem::path(directory) / name;
}

void write_text(const std::filesystem::path& path, const std::string& contents) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << contents;
    stream.close();
}

[[nodiscard]] std::string read_text(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::string contents;
    char buffer[4096];
    while (stream.read(buffer, static_cast<std::streamsize>(sizeof(buffer))) ||
           stream.gcount() > 0) {
        contents.append(buffer, static_cast<std::size_t>(stream.gcount()));
    }
    return contents;
}

// One line per request the participant read, as drc_endpoint --record writes
// it. This is the independent evidence that a request really arrived.
[[nodiscard]] std::vector<std::string> read_lines(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line != "\r") {
            lines.push_back(line);
        }
    }
    return lines;
}

[[nodiscard]] bool has_text(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

// Waits for a participant's record file to reach the expected number of lines.
// The line is written as soon as the request is read, which can be after the
// coordinator has already stopped waiting for an answer. Running out of
// attempts fails the calling test rather than passing it.
[[nodiscard]] bool wait_for_lines(const std::string& path, std::size_t wanted) {
    for (int attempt = 0; attempt < 400; ++attempt) {
        if (read_lines(path).size() >= wanted) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

[[nodiscard]] std::size_t count_nonempty_lines(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::size_t lines = 0;
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line != "\r") {
            lines += 1;
        }
    }
    return lines;
}

void note(const std::string& text) { std::cout << "    note " << text << "\n"; }

// The console, run with its output captured. The shell integration lives in the
// shared harness so the same test drives the real executable on every platform.
[[nodiscard]] drctest::ConsoleResult run_console(const std::string& scratch,
                                                const std::string& tag,
                                                const std::vector<std::string>& arguments) {
    std::vector<std::string> command{drctest::tool_path("drcctl")};
    command.insert(command.end(), arguments.begin(), arguments.end());
    return drctest::run_captured(scratch, tag, command);
}

// ---- coordinator helpers --------------------------------------------------

void open_with_exchange_budget(drctest::Rig& rig, UnixNanos budget_nanos) {
    CoordinatorOptions options = rig.options();
    options.exchange_budget_nanos = budget_nanos;
    Result<std::unique_ptr<Coordinator>> opened = Coordinator::open(std::move(options));
    DRC_REQUIRE(opened.ok());
    rig.coordinator = std::move(opened).value();
}

void build_single_obligation_topology(drctest::Rig& rig) {
    rig.domain(1, "domain-a");
    rig.domain(2, "domain-b");
    rig.site(1, "site-a1", 1, 100);
    rig.site(3, "site-b1", 2, 1000);
    rig.obligation(10, "auth", RecoveryClass::SafetyCritical, 1, 10);
    rig.capability(3, 1, 500);
}

void register_participant(drctest::Rig& rig,
                          EffectDomain domain,
                          const std::string& name,
                          const std::vector<std::string>& extra = {},
                          const std::string& record_path = {}) {
    std::vector<std::string> command{drctest::tool_path("drc_endpoint"),
                                     "--domain", std::string{to_string(domain)},
                                     "--id", "1",
                                     "--name", name};
    command.insert(command.end(), extra.begin(), extra.end());
    if (!record_path.empty()) {
        command.push_back("--record");
        command.push_back(record_path);
    }
    EndpointDescriptor descriptor;
    descriptor.domain = domain;
    descriptor.id = EndpointId{1};
    descriptor.name = name;
    descriptor.command = command;
    DRC_REQUIRE_OK(rig.coordinator->register_endpoint(descriptor));
}

// A synthetic participant for every domain except the one the test is
// exercising with a real OS process. Registering all four would overwrite the
// real descriptor: a domain has at most one endpoint.
void register_synthetic_endpoints_except(drctest::Rig& rig, EffectDomain keep) {
    std::uint64_t id = 1;
    for (const EffectDomain domain :
         {EffectDomain::SiteControlPlane, EffectDomain::PlacementReservationCapacity,
          EffectDomain::AsiExecutionRecovery, EffectDomain::DfiNetworkRecovery}) {
        if (domain != keep) {
            rig.endpoint(domain, SyntheticBehavior{}, id);
        }
        id += 1;
    }
}

void note_plan(drctest::Rig& rig, RecoveryPlanId plan_id, const std::string& tag) {
    const RecoveryPlan plan = rig.plan_of(plan_id);
    for (const RecoveryStepId step_id : plan.order) {
        const RecoveryStep& step = plan.steps.at(step_id);
        note(tag + " " + std::string{to_string(step.kind)} +
             " state=" + std::string{to_string(step.state)} +
             " attempts=" + std::to_string(step.attempts) + " detail=" + step.detail);
    }
}

[[nodiscard]] const RecoveryStep* find_step(const RecoveryPlan& plan,
                                            StepKind kind,
                                            std::uint64_t obligation) {
    for (const RecoveryStepId step_id : plan.order) {
        const RecoveryStep& step = plan.steps.at(step_id);
        if (step.kind != kind) {
            continue;
        }
        if (obligation != 0 && step.obligation.value() != obligation) {
            continue;
        }
        return &step;
    }
    return nullptr;
}

// Work the next advance call could still do. A single call is bounded work:
// its round budget can be spent waiting for a participant, so it may report
// itself quiescent while a step is still dispatchable. The loop below keeps
// asking until no dispatchable step is left, and it is bounded by work, never
// by wall-clock time.
[[nodiscard]] bool has_dispatchable_work(drctest::Rig& rig, DisasterEventId event) {
    const Result<RecoveryPlanId> active = rig.coordinator->active_plan(event);
    if (!active.ok()) {
        return false;
    }
    const Result<RecoveryPlan> plan = rig.coordinator->plan(active.value());
    if (!plan.ok()) {
        return false;
    }
    const Result<RecoveryPolicy> policy = rig.coordinator->policy();
    if (!policy.ok()) {
        return false;
    }
    for (const RecoveryStepId step_id : plan.value().order) {
        const RecoveryStep& step = plan.value().steps.at(step_id);
        const bool retryable = step.state == StepState::Pending ||
                               step.state == StepState::Ready ||
                               step.state == StepState::Deferred ||
                               step.state == StepState::Indeterminate;
        if (retryable && step.authorized &&
            step.attempts < policy.value().max_step_attempts) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] AdvanceReport advance_until_quiet(drctest::Rig& rig, DisasterEventId event) {
    AdvanceReport last;
    for (int call = 0; call < 32; ++call) {
        last = rig.advance(event);
        if (last.quiescent && has_dispatchable_work(rig, event) && call == 0) {
            note("advance reported quiescent with dispatchable work left: rounds=" +
                 std::to_string(last.rounds) + " dispatched=" + std::to_string(last.dispatched) +
                 " pending=" + std::to_string(last.steps_pending));
        }
        if (!has_dispatchable_work(rig, event)) {
            break;
        }
    }
    return last;
}

[[nodiscard]] int lifecycle_rank(EventPhase phase) {
    switch (phase) {
        case EventPhase::Declared: return 0;
        case EventPhase::Assessed: return 1;
        case EventPhase::PlanReady: return 2;
        case EventPhase::Evacuating: return 3;
        case EventPhase::FailingOver: return 4;
        case EventPhase::Stabilized: return 5;
        case EventPhase::Restoring: return 6;
        case EventPhase::Validating: return 7;
        case EventPhase::Returning: return 8;
        case EventPhase::Closed: return 9;
        case EventPhase::Blocked: return -1;
        case EventPhase::Conflicted: return -2;
    }
    return -3;
}

[[nodiscard]] std::size_t count_in_flight_steps(const Coordinator& coordinator) {
    std::size_t in_flight = 0;
    const Result<std::vector<RecoveryPlanId>> plans = coordinator.plans();
    DRC_REQUIRE(plans.ok());
    for (const RecoveryPlanId plan_id : plans.value()) {
        const Result<RecoveryPlan> plan = coordinator.plan(plan_id);
        DRC_REQUIRE(plan.ok());
        for (const RecoveryStepId step_id : plan.value().order) {
            if (plan.value().steps.at(step_id).state == StepState::InFlight) {
                in_flight += 1;
            }
        }
    }
    return in_flight;
}

// Recovery rewrites state for exactly two documented reasons: an effect that
// was in flight when the coordinator stopped is now unanswerable, and an
// outstanding request from the previous epoch can never be answered. Anything
// else that changes the recovered digest is a defect.
[[nodiscard]] bool recovery_recorded_stranded_work(const Coordinator& coordinator) {
    const Result<std::vector<RecoveryPlanId>> plans = coordinator.plans();
    if (!plans.ok()) {
        return false;
    }
    for (const RecoveryPlanId plan_id : plans.value()) {
        const Result<RecoveryPlan> plan = coordinator.plan(plan_id);
        if (!plan.ok()) {
            continue;
        }
        for (const RecoveryStepId step_id : plan.value().order) {
            const RecoveryStep& step = plan.value().steps.at(step_id);
            if (step.state == StepState::Indeterminate &&
                step.detail.find("restarted while this effect was in flight") !=
                    std::string::npos) {
                return true;
            }
        }
    }
    const Result<std::vector<EffectReceipt>> receipts = coordinator.receipts();
    if (receipts.ok()) {
        for (const EffectReceipt& receipt : receipts.value()) {
            if (receipt.message.find("declared unanswerable by recovery") != std::string::npos) {
                return true;
            }
        }
    }
    return false;
}

struct CommittedDigest {
    bool present = false;
    Digest digest;
};

[[nodiscard]] CommittedDigest last_committed_digest(const std::string& journal_path) {
    CommittedDigest result;
    Result<journal::Reader> reader =
        journal::Reader::open(journal_path, 64ull * 1024ull * 1024ull);
    DRC_REQUIRE(reader.ok());
    const Status walked = reader.value().for_each_committed([&](const journal::Record& record) {
        if (record.type != journal::RecordType::Commit) {
            return ok_status();
        }
        Result<journal::CommitPayload> payload = journal::decode_commit(record.payload);
        if (!payload.ok()) {
            return payload.status();
        }
        result.digest = payload.value().state_digest;
        result.present = true;
        return ok_status();
    });
    DRC_REQUIRE(walked.ok());
    return result;
}

[[nodiscard]] journal::ScanReport scan_journal(const std::string& journal_path) {
    Result<journal::Reader> reader =
        journal::Reader::open(journal_path, 64ull * 1024ull * 1024ull);
    DRC_REQUIRE(reader.ok());
    return reader.value().report();
}

// Reopens the journal a dead console left behind and checks the invariants
// recovery owes a caller: the committed state is reproduced, nothing stays in
// flight, the recovered state is a fixed point, and the event can still be
// carried to Stabilized.
void verify_recovered_journal(drctest::Rig& rig,
                              const std::string& directory,
                              const CommittedDigest& committed,
                              const std::string& tag,
                              bool* reached_stabilized) {
    const std::filesystem::path journal_path = leaf(directory, "coordinator.journal");
    DRC_REQUIRE(std::filesystem::exists(journal_path));

    // A fresh process opens the same directory: the dead owner's lock is taken
    // over, and the journal is replayed from its committed boundary.
    rig.open();
    const Result<JournalVerification> verification = rig.coordinator->verify_journal();
    DRC_REQUIRE(verification.ok());
    if (verification.value().interior_corruption) {
        note(tag + ": recovery left the journal internally corrupt: " +
             verification.value().detail);
    }
    DRC_REQUIRE(!verification.value().interior_corruption);
    DRC_REQUIRE_EQ(count_in_flight_steps(*rig.coordinator), std::size_t{0});

    const Result<std::vector<DisasterEventId>> events = rig.coordinator->events();
    DRC_REQUIRE(events.ok());
    if (events.value().empty()) {
        // The crash landed before the declaration: recovery must still
        // reproduce the state the journal committed, when a commit exists.
        if (committed.present) {
            DRC_REQUIRE_EQ(rig.coordinator->state_digest(), committed.digest);
        }
        note(tag + ": recovered with no event yet");
        return;
    }

    const DisasterEventId event = events.value().front();
    const Digest recovered = rig.coordinator->state_digest();
    if (committed.present && recovered != committed.digest) {
        // The only licensed reason for a different digest is that recovery had
        // to record an effect that can never be answered.
        DRC_REQUIRE(recovery_recorded_stranded_work(*rig.coordinator));
    }

    // Recovery is a fixed point: a second open reproduces the digest exactly.
    rig.reopen();
    DRC_REQUIRE_EQ(rig.coordinator->state_digest(), recovered);
    DRC_REQUIRE_EQ(count_in_flight_steps(*rig.coordinator), std::size_t{0});

    // The phase is a real phase, and it is consistent with the state that was
    // replayed: a plan cannot exist before planning, and Stabilized cannot be
    // reached while any step is outstanding.
    const DisasterEvent recovered_event = rig.event(event);
    const int rank = lifecycle_rank(recovered_event.phase);
    DRC_REQUIRE(rank != -3);
    const Result<std::vector<RecoveryPlanId>> plans = rig.coordinator->plans();
    DRC_REQUIRE(plans.ok());
    if (!plans.value().empty()) {
        DRC_REQUIRE(rank >= lifecycle_rank(EventPhase::PlanReady));
    }
    if (rank >= lifecycle_rank(EventPhase::Stabilized) &&
        rank <= lifecycle_rank(EventPhase::Returning)) {
        const RecoveryPlanId active = rig.active_plan(event);
        DRC_REQUIRE_EQ(rig.count_steps(active, StepState::Succeeded),
                       rig.plan_of(active).order.size());
    }

    // Continuing to advance still reaches Stabilized.
    if (recovered_event.phase == EventPhase::Evacuating ||
        recovered_event.phase == EventPhase::FailingOver) {
        const AdvanceReport report = advance_until_quiet(rig, event);
        DRC_REQUIRE(report.quiescent);
        DRC_REQUIRE_EQ(rig.event(event).phase, EventPhase::Stabilized);
        const RecoveryPlanId active = rig.active_plan(event);
        DRC_REQUIRE_EQ(rig.count_steps(active, StepState::Succeeded),
                       rig.plan_of(active).order.size());
        DRC_REQUIRE_EQ(count_in_flight_steps(*rig.coordinator), std::size_t{0});
        if (reached_stabilized != nullptr) {
            *reached_stabilized = true;
        }
    }
    note(tag + ": recovered phase " + std::string{to_string(rig.event(event).phase)});
}

}  // namespace

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

DRC_TEST(multiprocess_participant_processes_answer_real_requests) {
    drctest::Rig rig = drctest::Rig::make("multiprocess-live");
    rig.open();
    build_single_obligation_topology(rig);
    // Four independent OS processes, one per effect domain, each one writing
    // down every request it is handed.
    const std::string scp_record = leaf(rig.directory->path, "scp.record").string();
    const std::string prc_record = leaf(rig.directory->path, "prc.record").string();
    const std::string asi_record = leaf(rig.directory->path, "asi.record").string();
    const std::string dfi_record = leaf(rig.directory->path, "dfi.record").string();
    register_participant(rig, EffectDomain::SiteControlPlane, "scp", {}, scp_record);
    register_participant(rig, EffectDomain::PlacementReservationCapacity, "prc", {}, prc_record);
    register_participant(rig, EffectDomain::AsiExecutionRecovery, "asi", {}, asi_record);
    register_participant(rig, EffectDomain::DfiNetworkRecovery, "dfi", {}, dfi_record);

    const DisasterEventId event = rig.declare({1});
    const RecoveryPlanId plan = rig.plan(event);
    const RecoveryPlan before = rig.plan_of(plan);
    // Six steps for the obligation, one network recovery step for the target.
    DRC_REQUIRE_EQ(before.steps.size(), std::size_t{7});
    DRC_REQUIRE_OK(rig.coordinator->begin_recovery(event));
    const AdvanceReport report = advance_until_quiet(rig, event);
    if (rig.event(event).phase != EventPhase::Stabilized) {
        note_plan(rig, plan, "live");
    }
    DRC_REQUIRE(report.quiescent);
    DRC_REQUIRE_EQ(rig.event(event).phase, EventPhase::Stabilized);
    DRC_REQUIRE_EQ(rig.count_steps(plan, StepState::Succeeded), before.order.size());
    DRC_REQUIRE_EQ(report.steps_failed, 0u);

    // Every applied receipt was produced by a participant process: the
    // in-process synthetic endpoint calls itself "synthetic endpoint", the
    // reference participant names itself.
    const Result<std::vector<EffectReceipt>> receipts = rig.coordinator->receipts();
    DRC_REQUIRE(receipts.ok());
    DRC_REQUIRE_EQ(receipts.value().size(), before.order.size());
    for (const EffectReceipt& receipt : receipts.value()) {
        DRC_REQUIRE(receipt.message.find("reference participant") != std::string::npos);
        DRC_REQUIRE_EQ(receipt.endpoint, EndpointId{1});
        DRC_REQUIRE_EQ(receipt.outcome, EffectOutcome::Completed);
    }

    // The reservation and placement references are the ones the participant
    // invented for the step it was asked about, which is a different number
    // from the one an in-process synthetic endpoint would have produced.
    const RecoveryPlan after = rig.plan_of(plan);
    DRC_REQUIRE_EQ(after.assignments.size(), std::size_t{1});
    const PlacementAssignment& assignment = after.assignments.front();
    DRC_REQUIRE(assignment.reservation.valid());
    DRC_REQUIRE(assignment.placement.valid());
    DRC_REQUIRE_EQ(assignment.reservation.value(),
                   std::uint64_t{1000} + assignment.reservation_step.value());
    DRC_REQUIRE_EQ(assignment.placement.value(),
                   std::uint64_t{2000} + assignment.placement_step.value());
    // No step was double counted and the plan is the same plan.
    DRC_REQUIRE_EQ(after.steps.size(), before.steps.size());
    DRC_REQUIRE_EQ(after.order.size(), before.order.size());
    DRC_REQUIRE_EQ(rig.count_steps(plan, StepState::InFlight), std::size_t{0});

    // Every dispatched effect really reached the process it was addressed to,
    // exactly once per step: the transport cannot pass silently as "the
    // outcome is unknown".
    const std::vector<std::string> scp_lines = read_lines(scp_record);
    const std::vector<std::string> prc_lines = read_lines(prc_record);
    const std::vector<std::string> asi_lines = read_lines(asi_record);
    const std::vector<std::string> dfi_lines = read_lines(dfi_record);
    DRC_REQUIRE_EQ(scp_lines.size(), std::size_t{3});
    DRC_REQUIRE_EQ(prc_lines.size(), std::size_t{2});
    DRC_REQUIRE_EQ(asi_lines.size(), std::size_t{1});
    DRC_REQUIRE_EQ(dfi_lines.size(), std::size_t{1});
    DRC_REQUIRE_EQ(scp_lines.size() + prc_lines.size() + asi_lines.size() + dfi_lines.size(),
                   before.order.size());
    DRC_REQUIRE(has_text(scp_lines[0], "kind=evacuate"));
    DRC_REQUIRE(has_text(scp_lines[1], "kind=restore"));
    DRC_REQUIRE(has_text(scp_lines[2], "kind=verify"));
    DRC_REQUIRE(has_text(prc_lines[0], "kind=reserve"));
    DRC_REQUIRE(has_text(prc_lines[1], "kind=place"));
    DRC_REQUIRE(has_text(asi_lines[0], "kind=recover_execution"));
    DRC_REQUIRE(has_text(dfi_lines[0], "kind=recover_network"));
    // One attempt per step: nothing was dispatched twice.
    for (const std::string& line : scp_lines) {
        DRC_REQUIRE(has_text(line, "attempt=1"));
    }
}

DRC_TEST(multiprocess_participant_death_is_not_success) {
    drctest::Rig rig = drctest::Rig::make("multiprocess-death");
    rig.open();
    build_single_obligation_topology(rig);
    // The participant answers its first request and then exits mid-stream: the
    // coordinator sees EOF, not an answer, for everything after that.
    const std::string record = leaf(rig.directory->path, "prc.record").string();
    register_participant(rig, EffectDomain::PlacementReservationCapacity, "prc",
                         {"--fail-after", "1"}, record);
    register_synthetic_endpoints_except(rig, EffectDomain::PlacementReservationCapacity);

    const DisasterEventId event = rig.declare({1});
    const RecoveryPlanId plan = rig.plan(event);
    DRC_REQUIRE_OK(rig.coordinator->begin_recovery(event));
    const AdvanceReport report = advance_until_quiet(rig, event);

    note_plan(rig, plan, "death");
    const RecoveryPlan value = rig.plan_of(plan);
    const RecoveryStep* reserve = find_step(value, StepKind::ReserveDestination, 10);
    const RecoveryStep* place = find_step(value, StepKind::PlaceObligation, 10);
    DRC_REQUIRE(reserve != nullptr);
    DRC_REQUIRE(place != nullptr);

    // The request the participant answered is a success. The request it died
    // holding is not, and it is never reported as one.
    DRC_REQUIRE_EQ(reserve->state, StepState::Succeeded);
    DRC_REQUIRE(place->state != StepState::Succeeded);
    DRC_REQUIRE(place->state == StepState::Indeterminate || place->state == StepState::Failed);
    DRC_REQUIRE(!place->detail.empty());
    // The detail says the outcome is unknown, either as the transport failure
    // that ended the exchange or as the bound that then failed the step.
    DRC_REQUIRE(place->detail.find("exchange") != std::string::npos ||
                place->detail.find("attempts") != std::string::npos);
    note("dead participant detail: " + place->detail);
    // Retries stay inside the configured bound and nothing is left in flight.
    const Result<RecoveryPolicy> policy = rig.coordinator->policy();
    DRC_REQUIRE(policy.ok());
    DRC_REQUIRE(place->attempts <= policy.value().max_step_attempts);
    DRC_REQUIRE_EQ(rig.count_steps(plan, StepState::InFlight), std::size_t{0});
    DRC_REQUIRE(rig.event(event).phase != EventPhase::Stabilized);
    DRC_REQUIRE(report.quiescent);

    // The participant recorded both requests it was handed and answered only
    // the first one: the second is the effect it died holding.
    const std::vector<std::string> lines = read_lines(record);
    DRC_REQUIRE_EQ(lines.size(), std::size_t{2});
    DRC_REQUIRE(has_text(lines[0], "kind=reserve"));
    DRC_REQUIRE(has_text(lines[0], "attempt=1"));
    DRC_REQUIRE(has_text(lines[1], "kind=place"));
}

// The reference participant's --drop-after 0 is documented as "stop answering
// from the first request, but stay alive". A participant that never answers is
// never a success, its retries are bounded by the policy, and the event stops
// with a detail naming the step.
DRC_TEST(multiprocess_silent_participant_is_unknown_then_blocked) {
    drctest::Rig rig = drctest::Rig::make("multiprocess-silent");
    open_with_exchange_budget(rig, kSilentParticipantBudget);
    build_single_obligation_topology(rig);
    note("--drop-after 0 must make the participant silent from its first request");
    const std::string record = leaf(rig.directory->path, "prc.record").string();
    register_participant(rig, EffectDomain::PlacementReservationCapacity, "prc",
                         {"--drop-after", "0"}, record);
    register_synthetic_endpoints_except(rig, EffectDomain::PlacementReservationCapacity);

    const DisasterEventId event = rig.declare({1});
    const RecoveryPlanId plan = rig.plan(event);
    DRC_REQUIRE_OK(rig.coordinator->begin_recovery(event));
    const AdvanceReport report = advance_until_quiet(rig, event);

    note_plan(rig, plan, "silent");
    const RecoveryPlan value = rig.plan_of(plan);
    const RecoveryStep* reserve = find_step(value, StepKind::ReserveDestination, 10);
    DRC_REQUIRE(reserve != nullptr);
    note("reserve step ended as " + std::string{to_string(reserve->state)} + ": " +
         reserve->detail);
    // The participant answered nothing, so the effect it was asked for can
    // never be reported as done.
    DRC_REQUIRE(reserve->state != StepState::Succeeded);
    DRC_REQUIRE(reserve->state == StepState::Indeterminate || reserve->state == StepState::Failed);
    DRC_REQUIRE(!reserve->detail.empty());

    const Result<RecoveryPolicy> policy = rig.coordinator->policy();
    DRC_REQUIRE(policy.ok());
    DRC_REQUIRE(reserve->attempts >= 1);
    DRC_REQUIRE(reserve->attempts <= policy.value().max_step_attempts);

    // Exhausted attempts are a failure that stops the event, not a quiet pause.
    DRC_REQUIRE_EQ(rig.event(event).phase, EventPhase::Blocked);
    DRC_REQUIRE(!rig.event(event).status_detail.empty());
    DRC_REQUIRE(rig.event(event).status_detail.find("step") != std::string::npos);
    note("blocked detail: " + rig.event(event).status_detail);
    DRC_REQUIRE(report.blocked || report.quiescent);

    // The participant really read the request instead of the transport
    // swallowing it: silence here is the participant's, not a broken pipe.
    // After the silent exchange the engine quarantines the endpoint (a
    // half-consumed reply stream cannot be realigned), so the remaining
    // attempts are recorded without reaching the wire.
    DRC_REQUIRE(wait_for_lines(record, 1));
    const std::vector<std::string> lines = read_lines(record);
    DRC_REQUIRE(!lines.empty());
    DRC_REQUIRE(has_text(lines[0], "kind=reserve"));
    DRC_REQUIRE(has_text(lines[0], "attempt=1"));
    note("silent participant read " + std::to_string(lines.size()) +
         " request(s); step attempts=" + std::to_string(reserve->attempts));
    DRC_REQUIRE(reserve->attempts >= static_cast<std::uint32_t>(lines.size()));
}

// A participant that answers every request with "the outcome is unknown" never
// reports success. The coordinator must bound the retries and stop the event.
DRC_TEST(multiprocess_participant_that_never_reports_success_blocks_the_event) {
    drctest::Rig rig = drctest::Rig::make("multiprocess-unknown");
    open_with_exchange_budget(rig, kExchangeBudget);
    build_single_obligation_topology(rig);
    const std::string record = leaf(rig.directory->path, "prc.record").string();
    register_participant(rig, EffectDomain::PlacementReservationCapacity, "prc",
                         {"--outcome", "unknown"}, record);
    register_synthetic_endpoints_except(rig, EffectDomain::PlacementReservationCapacity);

    const DisasterEventId event = rig.declare({1});
    const RecoveryPlanId plan = rig.plan(event);
    DRC_REQUIRE_OK(rig.coordinator->begin_recovery(event));
    const AdvanceReport report = advance_until_quiet(rig, event);

    note_plan(rig, plan, "unknown");
    const RecoveryPlan value = rig.plan_of(plan);
    const RecoveryStep* reserve = find_step(value, StepKind::ReserveDestination, 10);
    DRC_REQUIRE(reserve != nullptr);
    note("unknown-outcome detail: " + reserve->detail);
    DRC_REQUIRE(reserve->state != StepState::Succeeded);
    DRC_REQUIRE(reserve->state == StepState::Failed || reserve->state == StepState::Indeterminate);
    const Result<RecoveryPolicy> policy = rig.coordinator->policy();
    DRC_REQUIRE(policy.ok());
    DRC_REQUIRE(reserve->attempts >= 1);
    DRC_REQUIRE(reserve->attempts <= policy.value().max_step_attempts);
    DRC_REQUIRE_EQ(rig.event(event).phase, EventPhase::Blocked);
    DRC_REQUIRE(!rig.event(event).status_detail.empty());
    DRC_REQUIRE(rig.event(event).status_detail.find("step") != std::string::npos);
    DRC_REQUIRE(report.blocked || report.quiescent);

    // Three attempts reached the participant and none of them was a success.
    const std::vector<std::string> lines = read_lines(record);
    DRC_REQUIRE_EQ(lines.size(), std::size_t{3});
    DRC_REQUIRE(has_text(lines[0], "attempt=1"));
    DRC_REQUIRE(has_text(lines[2], "attempt=3"));
}

DRC_TEST(multiprocess_duplicate_and_reordered_replies_are_idempotent) {
    drctest::Rig rig = drctest::Rig::make("multiprocess-duplicate");
    rig.open();
    build_single_obligation_topology(rig);
    // Every participant answers each request twice, in the wrong order, and
    // records what it was actually handed.
    std::map<EffectDomain, std::string> records;
    for (const EffectDomain domain :
         {EffectDomain::SiteControlPlane, EffectDomain::PlacementReservationCapacity,
          EffectDomain::AsiExecutionRecovery, EffectDomain::DfiNetworkRecovery}) {
        const std::string path =
            leaf(rig.directory->path, std::string{to_string(domain)} + ".record").string();
        records.emplace(domain, path);
        register_participant(rig, domain, std::string{to_string(domain)},
                             {"--duplicate-reply", "--reorder-reply"}, path);
    }

    const DisasterEventId event = rig.declare({1});
    const RecoveryPlanId plan = rig.plan(event);
    const RecoveryPlan before = rig.plan_of(plan);
    DRC_REQUIRE_OK(rig.coordinator->begin_recovery(event));
    const AdvanceReport report = advance_until_quiet(rig, event);
    if (rig.event(event).phase != EventPhase::Stabilized) {
        note_plan(rig, plan, "duplicate");
    }
    DRC_REQUIRE(report.quiescent);
    DRC_REQUIRE_EQ(rig.event(event).phase, EventPhase::Stabilized);

    // The plan is unchanged by the duplicated answers: same steps, same count,
    // every step succeeded exactly once.
    const RecoveryPlan after = rig.plan_of(plan);
    DRC_REQUIRE_EQ(after.steps.size(), before.steps.size());
    DRC_REQUIRE_EQ(after.order.size(), before.order.size());
    DRC_REQUIRE_EQ(rig.count_steps(plan, StepState::Succeeded), before.order.size());
    DRC_REQUIRE_EQ(rig.count_steps(plan, StepState::InFlight), std::size_t{0});

    // One receipt per request: the copy is recorded as a duplicate, never as a
    // second application.
    const CoordinatorStats stats = rig.coordinator->stats();
    DRC_REQUIRE(stats.duplicate_receipts > 0);
    DRC_REQUIRE_EQ(stats.dispatches, static_cast<std::uint64_t>(before.order.size()));
    DRC_REQUIRE_EQ(stats.receipts_applied, static_cast<std::uint64_t>(before.order.size()));
    const Result<std::vector<EffectReceipt>> receipts = rig.coordinator->receipts();
    DRC_REQUIRE(receipts.ok());
    DRC_REQUIRE_EQ(receipts.value().size(), before.order.size());
    std::vector<std::uint64_t> request_ids;
    request_ids.reserve(receipts.value().size());
    for (const EffectReceipt& receipt : receipts.value()) {
        request_ids.push_back(receipt.request.value());
    }
    std::sort(request_ids.begin(), request_ids.end());
    DRC_REQUIRE(std::adjacent_find(request_ids.begin(), request_ids.end()) == request_ids.end());

    // The participant's own references survive the duplicate: they are still
    // the ones it invented for the step, not a second pair.
    DRC_REQUIRE_EQ(after.assignments.size(), std::size_t{1});
    const PlacementAssignment& assignment = after.assignments.front();
    DRC_REQUIRE_EQ(assignment.reservation.value(),
                   std::uint64_t{1000} + assignment.reservation_step.value());
    DRC_REQUIRE_EQ(assignment.placement.value(),
                   std::uint64_t{2000} + assignment.placement_step.value());

    // A duplicated reply is not a duplicated effect: each step was dispatched
    // once, and the participant saw exactly one request per step.
    DRC_REQUIRE_EQ(read_lines(records.at(EffectDomain::SiteControlPlane)).size(), std::size_t{3});
    DRC_REQUIRE_EQ(read_lines(records.at(EffectDomain::PlacementReservationCapacity)).size(),
                   std::size_t{2});
    DRC_REQUIRE_EQ(read_lines(records.at(EffectDomain::AsiExecutionRecovery)).size(),
                   std::size_t{1});
    DRC_REQUIRE_EQ(read_lines(records.at(EffectDomain::DfiNetworkRecovery)).size(), std::size_t{1});
}

DRC_TEST(multiprocess_coordinator_crash_and_restart_at_every_transition) {
    bool reached_stabilized = false;
    std::vector<int> recovered_phases;
    int crashes = 0;

    // Every commit boundary of the lifecycle.
    for (std::uint64_t index = 1; index <= 16; ++index) {
        drctest::Rig rig = drctest::Rig::make("multiprocess-crash");
        const std::filesystem::path script = leaf(rig.directory->path, "recovery.txt");
        write_text(script, crash_script());
        const ConsoleResult run =
            run_console(rig.directory->path, "crash",
                        {"--dir", rig.directory->path, "--clock", "manual",
                         "--crash-after-commits", std::to_string(index),
                         "--script", script.string()});
        if (run.exit_code == 0) {
            break;
        }
        DRC_REQUIRE_EQ(run.exit_code, 9);

        const std::string journal = leaf(rig.directory->path, "coordinator.journal").string();
        const CommittedDigest committed = last_committed_digest(journal);
        verify_recovered_journal(rig, rig.directory->path, committed,
                                 "commits=" + std::to_string(index), &reached_stabilized);
        crashes += 1;
        const Result<std::vector<DisasterEventId>> events = rig.coordinator->events();
        DRC_REQUIRE(events.ok());
        recovered_phases.push_back(events.value().empty()
                                       ? -1
                                       : lifecycle_rank(rig.event(events.value().front()).phase));
        rig.close();
    }
    DRC_REQUIRE(crashes >= 6);
    DRC_REQUIRE(reached_stabilized);
    // Each extra commit can only move the recovered event forward.
    for (std::size_t i = 1; i < recovered_phases.size(); ++i) {
        if (recovered_phases[i - 1] >= 0 && recovered_phases[i] >= 0) {
            DRC_REQUIRE(recovered_phases[i] >= recovered_phases[i - 1]);
        }
    }
    DRC_REQUIRE(!recovered_phases.empty());
    DRC_REQUIRE_EQ(recovered_phases.back(), lifecycle_rank(EventPhase::Stabilized));

    // The uncommitted durable tail: the records were flushed but the commit
    // never happened, so recovery must fall back to the last committed state.
    for (std::uint64_t index = 2; index <= 9; ++index) {
        drctest::Rig rig = drctest::Rig::make("multiprocess-tail");
        const std::filesystem::path script = leaf(rig.directory->path, "recovery.txt");
        write_text(script, crash_script());
        const ConsoleResult run =
            run_console(rig.directory->path, "tail",
                        {"--dir", rig.directory->path, "--clock", "manual",
                         "--crash-after-appends", std::to_string(index),
                         "--script", script.string()});
        if (run.exit_code == 0) {
            break;
        }
        DRC_REQUIRE_EQ(run.exit_code, 9);
        const std::string journal = leaf(rig.directory->path, "coordinator.journal").string();
        const journal::ScanReport before_recovery = scan_journal(journal);
        DRC_REQUIRE(!before_recovery.interior_corruption);
        note("appends=" + std::to_string(index) + " raw journal: bytes=" +
             std::to_string(before_recovery.file_bytes) + " committed=" +
             std::to_string(before_recovery.committed_bytes) + " records=" +
             std::to_string(before_recovery.record_count) + " discarded=" +
             std::to_string(before_recovery.discarded_tail_bytes) + " torn=" +
             (before_recovery.torn_tail ? "1" : "0"));
        DRC_REQUIRE(before_recovery.discarded_tail_bytes > 0 || before_recovery.torn_tail);
        const CommittedDigest committed = last_committed_digest(journal);
        verify_recovered_journal(rig, rig.directory->path, committed,
                                 "appends=" + std::to_string(index), &reached_stabilized);
        rig.close();
    }

    // The dispatch boundary: a step is dispatched and committed, then the
    // coordinator dies while the effect is in flight.
    for (std::uint64_t index = 1; index <= 3; ++index) {
        drctest::Rig rig = drctest::Rig::make("multiprocess-dispatch");
        const std::filesystem::path script = leaf(rig.directory->path, "recovery.txt");
        write_text(script, crash_script());
        const ConsoleResult run =
            run_console(rig.directory->path, "dispatch",
                        {"--dir", rig.directory->path, "--clock", "manual",
                         "--crash-after-dispatches", std::to_string(index),
                         "--script", script.string()});
        if (run.exit_code == 0) {
            break;
        }
        DRC_REQUIRE_EQ(run.exit_code, 9);
        const std::string journal = leaf(rig.directory->path, "coordinator.journal").string();
        const CommittedDigest committed = last_committed_digest(journal);
        verify_recovered_journal(rig, rig.directory->path, committed,
                                 "dispatches=" + std::to_string(index), &reached_stabilized);
        rig.close();
    }
}

DRC_TEST(multiprocess_hard_kill_leaves_a_recoverable_journal) {
    drctest::Rig rig = drctest::Rig::make("multiprocess-hardkill");
    const std::filesystem::path record_path = leaf(rig.directory->path, "participant.record");
    const std::filesystem::path participant =
        leaf(rig.directory->path, std::string{"participant"} + drctest::script_extension());
    const std::filesystem::path script = leaf(rig.directory->path, "kill.txt");

    // The reference participant answers one request and then stops answering
    // while staying alive, so the console is provably mid-exchange when it is
    // killed: the record file is appended as each request is read. The wrapper
    // script keeps the participant's flags out of the console script, whose
    // tokenizer cannot express a value that starts with "--".
    (void)drctest::write_script(participant.string(),
                                {drctest::tool_path("drc_endpoint"), "--domain",
                                 "placement_reservation_capacity", "--id", "2", "--name", "prc",
                                 "--drop-after", "1", "--record", record_path.string()});
    const std::vector<std::string> wrapper = drctest::shell_command(participant.string());

    std::string console_script = kTopologyScript();
    console_script.append(
        "endpoint --domain placement_reservation_capacity --id 2 --name prc --exec ");
    console_script.append(wrapper.front());
    for (std::size_t index = 1; index < wrapper.size(); ++index) {
        console_script.append(" --arg ");
        console_script.append(wrapper[index]);
    }
    console_script.append("\n");
    console_script.append(kIncidentScript());
    write_text(script, console_script);

    process::SpawnOptions options;
    options.command = {drctest::tool_path("drcctl"), "--dir", rig.directory->path,
                       "--clock", "manual", "--script", script.string()};
    options.working_directory = rig.directory->path;
    Result<std::unique_ptr<process::Child>> child = process::Child::spawn(options);
    DRC_REQUIRE(child.ok());

    // Wait for the participant's second request: at that point the console has
    // committed a dispatch and is inside an exchange that will never be
    // answered. The bound is on attempts, and running out of attempts fails the
    // test rather than passing it.
    bool mid_exchange = false;
    for (int attempt = 0; attempt < 500 && !mid_exchange; ++attempt) {
        mid_exchange = std::filesystem::exists(record_path) &&
                       count_nonempty_lines(record_path) >= 2;
        if (!mid_exchange) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    DRC_REQUIRE(mid_exchange);
    // The console had already committed a dispatch to this participant: the
    // request it read first is the reservation, the one it is holding is the
    // placement.
    const std::vector<std::string> recorded = read_lines(record_path.string());
    DRC_REQUIRE_EQ(recorded.size(), std::size_t{2});
    DRC_REQUIRE(has_text(recorded[0], "kind=reserve"));
    DRC_REQUIRE(has_text(recorded[1], "kind=place"));

    // Hard kill: no cleanup, no flush, no lock release.
    DRC_REQUIRE_OK(child.value()->terminate());
    const Result<int> exit_code = child.value()->wait();
    DRC_REQUIRE(exit_code.ok());
    DRC_REQUIRE(exit_code.value() != 0);
    DRC_REQUIRE(child.value()->running() == false);

    // The journal is still readable, and its verdict is explicit.
    const std::string journal = leaf(rig.directory->path, "coordinator.journal").string();
    DRC_REQUIRE(std::filesystem::exists(journal));
    const journal::ScanReport scan = scan_journal(journal);
    DRC_REQUIRE(!scan.interior_corruption);
    const CommittedDigest committed = last_committed_digest(journal);
    DRC_REQUIRE(committed.present);

    // Recovery either succeeds with a consistent state, or refuses with the
    // offset of the damage. Anything else is a silent half-state.
    Result<std::unique_ptr<Coordinator>> opened = Coordinator::open(rig.options());
    if (!opened.ok()) {
        DRC_REQUIRE_EQ(opened.status().code(), ErrorCode::Corrupt);
        DRC_REQUIRE(opened.status().message().find("offset") != std::string::npos);
        note("recovery refused the killed journal: " + opened.status().message());
        return;
    }
    rig.coordinator = std::move(opened).value();
    DRC_REQUIRE_EQ(count_in_flight_steps(*rig.coordinator), std::size_t{0});
    const Digest recovered = rig.coordinator->state_digest();
    if (recovered != committed.digest) {
        DRC_REQUIRE(recovery_recorded_stranded_work(*rig.coordinator));
    }
    rig.reopen();
    DRC_REQUIRE_EQ(rig.coordinator->state_digest(), recovered);
    DRC_REQUIRE_EQ(count_in_flight_steps(*rig.coordinator), std::size_t{0});
    const Result<JournalVerification> verification = rig.coordinator->verify_journal();
    DRC_REQUIRE(verification.ok());
    DRC_REQUIRE(verification.value().ok);
    const Result<std::vector<DisasterEventId>> events = rig.coordinator->events();
    DRC_REQUIRE(events.ok());
    DRC_REQUIRE(!events.value().empty());
}

DRC_TEST(multiprocess_two_coordinators_cannot_share_a_journal) {
    drctest::Rig rig = drctest::Rig::make("multiprocess-lock");
    rig.open();
    rig.domain(1, "the-owner");
    const Digest before = rig.coordinator->state_digest();

    // A second OS process asks for the same journal directory.
    const std::filesystem::path script = leaf(rig.directory->path, "second.txt");
    write_text(script, "verify\n");
    const ConsoleResult second =
        run_console(rig.directory->path, "second",
                    {"--dir", rig.directory->path, "--script", script.string()});
    DRC_REQUIRE_EQ(second.exit_code, 1);
    DRC_REQUIRE(second.output.find("error open locked") != std::string::npos);
    const std::size_t end_of_line = second.output.find('\n');
    note("second console said: " +
         second.output.substr(0, end_of_line == std::string::npos ? second.output.size()
                                                                  : end_of_line));

    // The refusal changed nothing and the owner keeps working.
    DRC_REQUIRE_EQ(rig.coordinator->state_digest(), before);
    rig.domain(2, "still-the-owner");
    const Result<std::vector<FailureDomainId>> domains = rig.coordinator->failure_domains();
    DRC_REQUIRE(domains.ok());
    DRC_REQUIRE_EQ(domains.value().size(), std::size_t{2});
    const Result<JournalVerification> verification = rig.coordinator->verify_journal();
    DRC_REQUIRE(verification.ok());
    DRC_REQUIRE(verification.value().ok);
}

DRC_TEST(multiprocess_recovered_coordinator_takes_over_a_dead_owners_lock) {
    drctest::Rig rig = drctest::Rig::make("multiprocess-takeover");
    const std::filesystem::path script = leaf(rig.directory->path, "owner.txt");
    write_text(script,
               "domain --id 1 --name domain-a\nsite --id 1 --domain 1 --name site-a1\n");
    // The console dies after its second commit, leaving its lock file behind
    // with the pid of a process that no longer exists.
    const ConsoleResult run =
        run_console(rig.directory->path, "owner",
                    {"--dir", rig.directory->path, "--clock", "manual",
                     "--crash-after-commits", "2", "--script", script.string()});
    DRC_REQUIRE_EQ(run.exit_code, 9);
    const std::filesystem::path lock_path = leaf(rig.directory->path, "coordinator.lock");
    DRC_REQUIRE(std::filesystem::exists(lock_path));

    // Observers are installed before anything else is called, so the notice
    // recovery queued while taking the lock over is still delivered.
    Result<std::unique_ptr<Coordinator>> opened = Coordinator::open(rig.options());
    DRC_REQUIRE(opened.ok());
    std::vector<TransitionNotice> notices;
    DRC_REQUIRE_OK(opened.value()->set_observer(
        [&notices](const TransitionNotice& notice) { notices.push_back(notice); }));
    DRC_REQUIRE_OK(opened.value()->checkpoint());
    rig.coordinator = std::move(opened).value();

    bool took_over = false;
    for (const TransitionNotice& notice : notices) {
        if (notice.kind == "lock_takeover") {
            took_over = true;
            note("lock takeover: " + notice.detail);
        }
    }
    if (!took_over) {
        // A dead owner's lock may also be recovered as an uncommitted tail.
        DRC_REQUIRE(rig.coordinator->stats().torn_tail_recoveries > 0);
    }
    const Result<std::vector<FailureDomainId>> domains = rig.coordinator->failure_domains();
    DRC_REQUIRE(domains.ok());
    DRC_REQUIRE_EQ(domains.value().size(), std::size_t{1});
    DRC_REQUIRE(rig.coordinator->epoch().value() >= 2);
}
