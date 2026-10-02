// drcctl: the Disaster Recovery Coordinator operating console. It drives the
// public API and nothing else, so anything it can do a downstream program can
// do too.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "drc/engine.hpp"
#include "drc/fileio.hpp"
#include "drc/version.hpp"

namespace {

using namespace drc;

struct Session {
    std::unique_ptr<Coordinator> coordinator;
    std::shared_ptr<ManualClock> manual_clock;
    // The clock the coordinator actually uses. Evidence recorded from the
    // command line must carry a real observation time, or it can never be
    // fresh and every planning attempt fails.
    std::shared_ptr<Clock> clock;
    bool continue_on_error = false;
};

[[nodiscard]] std::string join_values(const std::vector<std::string>& values, std::size_t from) {
    std::string out;
    for (std::size_t i = from; i < values.size(); ++i) {
        if (!out.empty()) {
            out.push_back(' ');
        }
        out.append(values[i]);
    }
    return out;
}

void split(const std::string& text, char separator, std::vector<std::string>& out) {
    std::string current;
    for (const char c : text) {
        if (c == separator) {
            if (!current.empty()) {
                out.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(c);
    }
    if (!current.empty()) {
        out.push_back(current);
    }
}

[[nodiscard]] Result<std::uint64_t> parse_u64(const std::string& text, const char* what) {
    if (text.empty() || text.size() > 20) {
        return Status{ErrorCode::Invalid, std::string{what} + " is not a number"};
    }
    std::uint64_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return Status{ErrorCode::Invalid, std::string{what} + " is not a decimal integer"};
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
        if (value > (UINT64_MAX - digit) / 10u) {
            return Status{ErrorCode::Overflow, std::string{what} + " overflows"};
        }
        value = value * 10u + digit;
    }
    return value;
}

struct Arguments {
    std::vector<std::string> values;
    std::map<std::string, std::vector<std::string>> options;

    [[nodiscard]] bool has(const std::string& key) const {
        return options.find(key) != options.end();
    }
    [[nodiscard]] std::string get(const std::string& key, const std::string& fallback = {}) const {
        const auto found = options.find(key);
        if (found == options.end() || found->second.empty()) {
            return fallback;
        }
        return found->second.front();
    }
    [[nodiscard]] Result<std::uint64_t> number(const std::string& key, bool required) const {
        const auto found = options.find(key);
        if (found == options.end() || found->second.empty()) {
            if (required) {
                return Status{ErrorCode::Invalid, "--" + key + " is required"};
            }
            return std::uint64_t{0};
        }
        return parse_u64(found->second.front(), key.c_str());
    }
    [[nodiscard]] std::vector<std::string> list(const std::string& key) const {
        const auto found = options.find(key);
        if (found == options.end()) {
            return {};
        }
        std::vector<std::string> out;
        for (const std::string& entry : found->second) {
            split(entry, ',', out);
        }
        return out;
    }
};

[[nodiscard]] Result<Arguments> parse_arguments(const std::vector<std::string>& tokens,
                                                std::size_t from) {
    Arguments arguments;
    for (std::size_t i = from; i < tokens.size(); ++i) {
        const std::string& token = tokens[i];
        if (token.size() > 2 && token[0] == '-' && token[1] == '-') {
            std::string key = token.substr(2);
            // --key=value is the only spelling that can carry a value starting
            // with "--", such as a participant argument.
            const std::size_t equals = key.find('=');
            if (equals != std::string::npos) {
                arguments.options[key.substr(0, equals)].push_back(key.substr(equals + 1));
                continue;
            }
            if (i + 1 < tokens.size() && tokens[i + 1].rfind("--", 0) != 0) {
                arguments.options[key].push_back(tokens[i + 1]);
                i += 1;
            } else {
                arguments.options[key] = {};
            }
            continue;
        }
        arguments.values.push_back(token);
    }
    return arguments;
}

[[nodiscard]] Result<RecoveryClass> recovery_class_of(const std::string& text) {
    return parse_recovery_class(text);
}

[[nodiscard]] Result<EffectDomain> domain_of(const std::string& text) {
    return parse_effect_domain(text);
}

[[nodiscard]] Result<EffectOutcome> outcome_of(const std::string& text) {
    return parse_effect_outcome(text);
}

[[nodiscard]] Status ok_line(const std::string& command, const std::string& detail = {}) {
    std::cout << "ok " << command;
    if (!detail.empty()) {
        std::cout << " " << detail;
    }
    std::cout << "\n";
    return ok_status();
}

[[nodiscard]] Status error_line(const std::string& command, const Status& status) {
    std::cout << "error " << command << " " << to_string(status.code()) << " " << status.message()
              << "\n";
    return status;
}

[[nodiscard]] std::string phase_name(EventPhase phase) {
    return std::string{to_string(phase)};
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

[[nodiscard]] Status command_domain(Session& session, const Arguments& arguments) {
    const Result<std::uint64_t> id = arguments.number("id", true);
    if (!id.ok()) {
        return id.status();
    }
    FailureDomainRecord record;
    record.id = FailureDomainId{id.value()};
    record.name = arguments.get("name");
    return session.coordinator->define_failure_domain(record);
}

[[nodiscard]] Status command_site(Session& session, const Arguments& arguments) {
    const Result<std::uint64_t> id = arguments.number("id", true);
    const Result<std::uint64_t> domain = arguments.number("domain", true);
    if (!id.ok()) {
        return id.status();
    }
    if (!domain.ok()) {
        return domain.status();
    }
    SiteRecord record;
    record.id = SiteId{id.value()};
    record.name = arguments.get("name");
    record.domain = FailureDomainId{domain.value()};
    const Result<std::uint64_t> capacity = arguments.number("capacity", false);
    if (!capacity.ok()) {
        return capacity.status();
    }
    record.capacity_units = capacity.value();
    return session.coordinator->define_site(record);
}

[[nodiscard]] Status command_obligation(Session& session, const Arguments& arguments) {
    const Result<std::uint64_t> id = arguments.number("id", true);
    const Result<std::uint64_t> home = arguments.number("home", true);
    const Result<std::uint64_t> capacity = arguments.number("capacity", true);
    if (!id.ok()) {
        return id.status();
    }
    if (!home.ok()) {
        return home.status();
    }
    if (!capacity.ok()) {
        return capacity.status();
    }
    ProtectedObligation obligation;
    obligation.id = ObligationId{id.value()};
    obligation.name = arguments.get("name");
    obligation.home_site = SiteId{home.value()};
    obligation.required_capacity_units = capacity.value();
    const Result<RecoveryClass> cls = recovery_class_of(arguments.get("class", "standard"));
    if (!cls.ok()) {
        return cls.status();
    }
    obligation.recovery_class = cls.value();
    for (const std::string& dependency : arguments.list("depends")) {
        const Result<std::uint64_t> parsed = parse_u64(dependency, "depends");
        if (!parsed.ok()) {
            return parsed.status();
        }
        obligation.depends_on.push_back(ObligationId{parsed.value()});
    }
    return session.coordinator->define_obligation(obligation);
}

[[nodiscard]] Status command_policy(Session& session, const Arguments& arguments) {
    Result<RecoveryPolicy> current = session.coordinator->policy();
    if (!current.ok()) {
        return current.status();
    }
    RecoveryPolicy policy = current.value();
    const Result<Generation> next = policy.generation.next();
    if (!next.ok()) {
        return next.status();
    }
    policy.generation = next.value();
    const Result<std::uint64_t> freshness = arguments.number("freshness-ns", false);
    if (freshness.ok() && freshness.value() != 0) {
        policy.evidence_freshness_window = static_cast<UnixNanos>(freshness.value());
    }
    const Result<std::uint64_t> attempts = arguments.number("attempts", false);
    if (attempts.ok() && attempts.value() != 0) {
        policy.max_step_attempts = static_cast<std::uint32_t>(attempts.value());
    }
    const Result<std::uint64_t> in_flight = arguments.number("in-flight", false);
    if (in_flight.ok() && in_flight.value() != 0) {
        policy.max_in_flight_steps = static_cast<std::uint32_t>(in_flight.value());
    }
    if (arguments.has("allow-partial-placement")) {
        policy.require_protected_placement = false;
    }
    if (arguments.has("require-federation-evidence")) {
        policy.require_federation_evidence = true;
    }
    policy.required_readiness_checks = arguments.list("required-check");
    return session.coordinator->set_policy(policy);
}

[[nodiscard]] Result<std::uint32_t> class_mask(const std::vector<std::string>& names) {
    std::uint32_t mask = 0;
    for (const std::string& name : names) {
        const Result<RecoveryClass> cls = recovery_class_of(name);
        if (!cls.ok()) {
            return cls.status();
        }
        mask |= 1u << static_cast<std::uint32_t>(cls.value());
    }
    return mask;
}

[[nodiscard]] Status command_capability(Session& session, const Arguments& arguments) {
    const Result<std::uint64_t> site = arguments.number("site", true);
    const Result<std::uint64_t> generation = arguments.number("generation", true);
    const Result<std::uint64_t> capacity = arguments.number("capacity", true);
    if (!site.ok()) {
        return site.status();
    }
    if (!generation.ok()) {
        return generation.status();
    }
    if (!capacity.ok()) {
        return capacity.status();
    }
    std::vector<std::string> classes = arguments.list("classes");
    if (classes.empty()) {
        for (const char* name : {"safety_critical", "protected", "essential", "standard", "deferrable"}) {
            classes.emplace_back(name);
        }
    }
    const Result<std::uint32_t> mask = class_mask(classes);
    if (!mask.ok()) {
        return mask.status();
    }
    DestinationCapability capability;
    capability.site = SiteId{site.value()};
    capability.generation = Generation{generation.value()};
    capability.available_capacity_units = capacity.value();
    capability.supported_classes_mask = mask.value();
    capability.source = arguments.get("source", "cli");
    const Result<std::uint64_t> observed = arguments.number("observed-at", false);
    capability.observed_at = observed.ok() && observed.value() != 0
                                 ? static_cast<UnixNanos>(observed.value())
                                 : session.clock->now_nanos();
    const Result<std::uint64_t> epoch = arguments.number("epoch", false);
    capability.observation_epoch = epoch.ok() && epoch.value() != 0
                                       ? Epoch{epoch.value()}
                                       : session.coordinator->epoch();
    return session.coordinator->record_capability(capability);
}

[[nodiscard]] Status command_readiness(Session& session, const Arguments& arguments) {
    const Result<std::uint64_t> site = arguments.number("site", true);
    const Result<std::uint64_t> generation = arguments.number("generation", true);
    if (!site.ok()) {
        return site.status();
    }
    if (!generation.ok()) {
        return generation.status();
    }
    SiteReadinessEvidence evidence;
    evidence.site = SiteId{site.value()};
    evidence.generation = Generation{generation.value()};
    evidence.source = arguments.get("source", "cli");
    for (const std::string& entry : arguments.list("check")) {
        const std::size_t equals = entry.find('=');
        if (equals == std::string::npos) {
            return Status{ErrorCode::Invalid, "check must be name=pass or name=fail"};
        }
        ReadinessCheck check;
        check.name = entry.substr(0, equals);
        const std::string value = entry.substr(equals + 1);
        if (value == "pass") {
            check.passed = true;
        } else if (value == "fail") {
            check.passed = false;
        } else {
            return Status{ErrorCode::Invalid, "check value must be pass or fail"};
        }
        evidence.checks.push_back(std::move(check));
    }
    const Result<std::uint64_t> observed = arguments.number("observed-at", false);
    evidence.observed_at = observed.ok() && observed.value() != 0
                               ? static_cast<UnixNanos>(observed.value())
                               : session.clock->now_nanos();
    const Result<std::uint64_t> epoch = arguments.number("epoch", false);
    evidence.observation_epoch = epoch.ok() && epoch.value() != 0
                                     ? Epoch{epoch.value()}
                                     : session.coordinator->epoch();
    return session.coordinator->record_readiness(evidence);
}

[[nodiscard]] Status command_federation(Session& session, const Arguments& arguments) {
    FederationState state;
    const Result<std::uint64_t> generation = arguments.number("generation", true);
    if (!generation.ok()) {
        return generation.status();
    }
    state.generation = Generation{generation.value()};
    state.source = arguments.get("source", "cli");
    state.known = true;
    state.partitioned = arguments.has("partition");
    for (const std::string& domain : arguments.list("unreachable")) {
        const Result<std::uint64_t> parsed = parse_u64(domain, "unreachable");
        if (!parsed.ok()) {
            return parsed.status();
        }
        state.unreachable_domains.push_back(FailureDomainId{parsed.value()});
    }
    const Result<std::uint64_t> observed = arguments.number("observed-at", false);
    state.observed_at = observed.ok() && observed.value() != 0
                            ? static_cast<UnixNanos>(observed.value())
                            : session.clock->now_nanos();
    const Result<std::uint64_t> epoch = arguments.number("epoch", false);
    state.observation_epoch = epoch.ok() && epoch.value() != 0
                                  ? Epoch{epoch.value()}
                                  : session.coordinator->epoch();
    return session.coordinator->record_federation_state(state);
}

[[nodiscard]] Status command_endpoint(Session& session, const Arguments& arguments) {
    const Result<EffectDomain> domain = domain_of(arguments.get("domain"));
    if (!domain.ok()) {
        return domain.status();
    }
    const Result<std::uint64_t> id = arguments.number("id", true);
    if (!id.ok()) {
        return id.status();
    }
    EndpointDescriptor descriptor;
    descriptor.domain = domain.value();
    descriptor.id = EndpointId{id.value()};
    descriptor.name = arguments.get("name", "endpoint");
    descriptor.command = arguments.list("exec");
    for (const std::string& argument : arguments.list("arg")) {
        descriptor.command.push_back(argument);
    }
    for (const std::string& entry : arguments.list("behavior")) {
        const std::size_t equals = entry.find('=');
        if (equals == std::string::npos) {
            return Status{ErrorCode::Invalid, "behavior must be name=value"};
        }
        const std::string key = entry.substr(0, equals);
        const std::string value = entry.substr(equals + 1);
        const Result<std::uint64_t> number = parse_u64(value, key.c_str());
        const bool flag = value == "1" || value == "true";
        if (key == "outcome") {
            const Result<EffectOutcome> outcome = outcome_of(value);
            if (!outcome.ok()) {
                return outcome.status();
            }
            descriptor.synthetic.outcome = outcome.value();
        } else if (key == "drop_reply") {
            descriptor.synthetic.drop_reply = flag;
        } else if (key == "duplicate_reply") {
            descriptor.synthetic.duplicate_reply = flag;
        } else if (key == "reorder_reply") {
            descriptor.synthetic.reorder_reply = flag;
        } else if (key == "stale_generation") {
            descriptor.synthetic.stale_generation = flag;
        } else if (key == "foreign_epoch") {
            descriptor.synthetic.foreign_epoch = flag;
        } else if (key == "alternate_outcomes") {
            descriptor.synthetic.alternate_outcomes = flag;
        } else if (key == "reject_first_n" && number.ok()) {
            descriptor.synthetic.reject_first_n = static_cast<std::uint32_t>(number.value());
        } else if (key == "defer_first_n" && number.ok()) {
            descriptor.synthetic.defer_first_n = static_cast<std::uint32_t>(number.value());
        } else if (key == "unknown_first_n" && number.ok()) {
            descriptor.synthetic.unknown_first_n = static_cast<std::uint32_t>(number.value());
        } else {
            return Status{ErrorCode::Invalid, "unknown endpoint behavior key"};
        }
    }
    return session.coordinator->register_endpoint(descriptor);
}

[[nodiscard]] Status command_declare(Session& session, const Arguments& arguments) {
    DisasterDeclaration declaration;
    declaration.declared_by = arguments.get("by");
    declaration.reason = arguments.get("reason");
    const Result<std::uint64_t> severity = arguments.number("severity", false);
    declaration.severity = static_cast<std::uint32_t>(severity.value());
    for (const std::string& site : arguments.list("sites")) {
        const Result<std::uint64_t> parsed = parse_u64(site, "sites");
        if (!parsed.ok()) {
            return parsed.status();
        }
        declaration.affected_sites.push_back(SiteId{parsed.value()});
    }
    for (const std::string& domain : arguments.list("domains")) {
        const Result<std::uint64_t> parsed = parse_u64(domain, "domains");
        if (!parsed.ok()) {
            return parsed.status();
        }
        declaration.affected_domains.push_back(FailureDomainId{parsed.value()});
    }
    const Result<std::uint64_t> supersedes = arguments.number("supersedes", false);
    if (supersedes.ok() && supersedes.value() != 0) {
        declaration.supersedes = DisasterEventId{supersedes.value()};
    }
    Result<DisasterEventId> declared = session.coordinator->declare_disaster(declaration);
    if (!declared.ok()) {
        return declared.status();
    }
    std::cout << "ok declare event=" << declared.value().to_string() << "\n";
    return ok_status();
}

[[nodiscard]] Status command_assess(Session& session, const Arguments& arguments) {
    const Result<std::uint64_t> event = arguments.number("event", true);
    const Result<std::uint64_t> generation = arguments.number("generation", true);
    if (!event.ok()) {
        return event.status();
    }
    if (!generation.ok()) {
        return generation.status();
    }
    AssessmentEvidence assessment;
    assessment.event = DisasterEventId{event.value()};
    assessment.generation = Generation{generation.value()};
    assessment.source = arguments.get("source", "cli");
    Result<DisasterEvent> current = session.coordinator->event(assessment.event);
    if (!current.ok()) {
        return current.status();
    }
    assessment.event_generation = current.value().generation;
    for (const std::string& entry : arguments.list("site")) {
        std::vector<std::string> parts;
        split(entry, ':', parts);
        if (parts.size() != 3) {
            return Status{ErrorCode::Invalid, "site must be id:availability:telemetry"};
        }
        const Result<std::uint64_t> id = parse_u64(parts[0], "site");
        if (!id.ok()) {
            return id.status();
        }
        SiteAssessment site;
        site.site = SiteId{id.value()};
        if (parts[1] == "available") {
            site.availability = SiteAvailability::Available;
        } else if (parts[1] == "degraded") {
            site.availability = SiteAvailability::Degraded;
        } else if (parts[1] == "unavailable") {
            site.availability = SiteAvailability::Unavailable;
        } else if (parts[1] == "unknown") {
            site.availability = SiteAvailability::Unknown;
        } else {
            return Status{ErrorCode::Invalid, "unknown availability"};
        }
        site.telemetry_present = parts[2] == "present";
        assessment.sites.push_back(std::move(site));
    }
    assessment.observed_at = session.clock->now_nanos();
    assessment.observation_epoch = session.coordinator->epoch();
    return session.coordinator->record_assessment(assessment);
}

[[nodiscard]] Status command_plan(Session& session, const Arguments& arguments) {
    const Result<std::uint64_t> event = arguments.number("event", true);
    if (!event.ok()) {
        return event.status();
    }
    CreatePlanRequest request;
    request.event = DisasterEventId{event.value()};
    request.requested_by = arguments.get("by", "cli");
    request.allow_partial_placement = arguments.has("allow-partial");
    Result<RecoveryPlanId> plan = session.coordinator->create_plan(request);
    if (!plan.ok()) {
        return plan.status();
    }
    std::cout << "ok plan plan=" << plan.value().to_string() << "\n";
    return ok_status();
}

[[nodiscard]] Status command_begin(Session& session, const Arguments& arguments) {
    const Result<std::uint64_t> event = arguments.number("event", true);
    if (!event.ok()) {
        return event.status();
    }
    return session.coordinator->begin_recovery(DisasterEventId{event.value()});
}

[[nodiscard]] Status command_advance(Session& session, const Arguments& arguments) {
    const Result<std::uint64_t> event = arguments.number("event", true);
    if (!event.ok()) {
        return event.status();
    }
    const Result<std::uint64_t> rounds = arguments.number("rounds", false);
    const Result<std::uint64_t> dispatches = arguments.number("dispatches", false);
    const Result<std::uint64_t> repeat = arguments.number("repeat", false);
    AdvanceRequest request;
    request.event = DisasterEventId{event.value()};
    if (rounds.ok() && rounds.value() != 0) {
        request.max_rounds = static_cast<std::uint32_t>(rounds.value());
    }
    if (dispatches.ok() && dispatches.value() != 0) {
        request.max_dispatches = static_cast<std::uint32_t>(dispatches.value());
    }
    const std::uint64_t iterations = repeat.ok() && repeat.value() != 0 ? repeat.value() : 1;
    for (std::uint64_t i = 0; i < iterations; ++i) {
        Result<AdvanceReport> report = session.coordinator->advance(request);
        if (!report.ok()) {
            return report.status();
        }
        std::cout << "advance event=" << report.value().event.to_string()
                  << " phase=" << phase_name(report.value().phase_after)
                  << " dispatched=" << report.value().dispatched
                  << " receipts=" << report.value().receipts_applied
                  << " ignored=" << report.value().receipts_ignored
                  << " succeeded=" << report.value().steps_succeeded
                  << " failed=" << report.value().steps_failed
                  << " pending=" << report.value().steps_pending
                  << " inflight=" << report.value().steps_in_flight
                  << " deferred=" << report.value().steps_deferred
                  << " quiescent=" << (report.value().quiescent ? 1 : 0) << "\n";
    }
    return ok_status();
}

[[nodiscard]] Status command_restore(Session& session, const Arguments& arguments) {
    const Result<std::uint64_t> event = arguments.number("event", true);
    if (!event.ok()) {
        return event.status();
    }
    RestorationRequest request;
    request.event = DisasterEventId{event.value()};
    request.requested_by = arguments.get("by", "cli");
    return session.coordinator->begin_restoration(request);
}

[[nodiscard]] Status command_failback(Session& session, const Arguments& arguments) {
    const Result<std::uint64_t> event = arguments.number("event", true);
    const Result<std::uint64_t> site = arguments.number("site", true);
    if (!event.ok()) {
        return event.status();
    }
    if (!site.ok()) {
        return site.status();
    }
    FailbackRequest request;
    request.event = DisasterEventId{event.value()};
    request.target_site = SiteId{site.value()};
    request.authorized_by = arguments.get("by", "cli");
    request.justification = arguments.get("justification", "operator decision");
    Result<RecoveryPlanId> plan = session.coordinator->authorize_failback(request);
    if (!plan.ok()) {
        return plan.status();
    }
    std::cout << "ok failback plan=" << plan.value().to_string() << "\n";
    return ok_status();
}

[[nodiscard]] Status command_return(Session& session, const Arguments& arguments) {
    const Result<std::uint64_t> event = arguments.number("event", true);
    const Result<std::uint64_t> site = arguments.number("site", true);
    if (!event.ok()) {
        return event.status();
    }
    if (!site.ok()) {
        return site.status();
    }
    ReturnToServiceRequest request;
    request.event = DisasterEventId{event.value()};
    request.site = SiteId{site.value()};
    request.authorized_by = arguments.get("by", "cli");
    request.justification = arguments.get("justification", "operator decision");
    return session.coordinator->return_site_to_service(request);
}

[[nodiscard]] Status command_resolve(Session& session, const Arguments& arguments) {
    const Result<std::uint64_t> event = arguments.number("event", true);
    const Result<std::uint64_t> step = arguments.number("step", true);
    if (!event.ok()) {
        return event.status();
    }
    if (!step.ok()) {
        return step.status();
    }
    BlockedResolution resolution;
    resolution.event = DisasterEventId{event.value()};
    resolution.step = RecoveryStepId{step.value()};
    resolution.retry = arguments.has("retry");
    resolution.resolved_by = arguments.get("by", "cli");
    resolution.justification = arguments.get("justification", "operator decision");
    return session.coordinator->resolve_blocked(resolution);
}

[[nodiscard]] Status command_close(Session& session, const Arguments& arguments) {
    const Result<std::uint64_t> event = arguments.number("event", true);
    if (!event.ok()) {
        return event.status();
    }
    EventClosure closure;
    closure.event = DisasterEventId{event.value()};
    closure.closed_by = arguments.get("by", "cli");
    closure.summary = arguments.get("summary", "closed by operator");
    return session.coordinator->close_event(closure);
}

[[nodiscard]] Status command_checkpoint(Session& session) {
    return session.coordinator->checkpoint();
}

[[nodiscard]] Status command_compact(Session& session) {
    return session.coordinator->compact();
}

[[nodiscard]] Status command_verify(Session& session) {
    Result<JournalVerification> verification = session.coordinator->verify_journal();
    if (!verification.ok()) {
        return verification.status();
    }
    const JournalVerification& value = verification.value();
    std::cout << "verify ok=" << (value.ok ? 1 : 0) << " torn_tail=" << (value.torn_tail ? 1 : 0)
              << " interior_corruption=" << (value.interior_corruption ? 1 : 0)
              << " file_bytes=" << value.file_bytes
              << " committed_bytes=" << value.committed_bytes
              << " discarded_tail_bytes=" << value.discarded_tail_bytes
              << " records=" << value.record_count
              << " last_committed=" << value.last_committed_sequence.to_string()
              << " digest=" << value.chain.to_hex() << "\n";
    return ok_status();
}

[[nodiscard]] Status command_status(Session& session, const Arguments& arguments) {
    const Result<std::uint64_t> event = arguments.number("event", true);
    if (!event.ok()) {
        return event.status();
    }
    Result<DisasterEvent> value = session.coordinator->event(DisasterEventId{event.value()});
    if (!value.ok()) {
        return value.status();
    }
    const DisasterEvent& e = value.value();
    std::cout << "event id=" << e.id.to_string() << " phase=" << phase_name(e.phase)
              << " disposition=" << to_string(e.disposition)
              << " generation=" << e.generation.to_string()
              << " plan=" << e.active_plan.to_string() << " returned=" << e.returned_sites.size()
              << " affected=" << e.affected_sites.size()
              << " blocked_steps=" << e.blocked_steps
              << " failed_safety_critical=" << e.failed_safety_critical_steps << "\n";
    Result<RecoveryPlanId> plan_id = session.coordinator->active_plan(e.id);
    if (plan_id.ok()) {
        Result<RecoveryPlan> plan = session.coordinator->plan(plan_id.value());
        if (plan.ok()) {
            std::cout << "plan id=" << plan.value().id.to_string()
                      << " kind=" << to_string(plan.value().kind)
                      << " state=" << to_string(plan.value().state)
                      << " generation=" << plan.value().generation.to_string()
                      << " steps=" << plan.value().steps.size()
                      << " unplaced=" << plan.value().unplaced.size()
                      << " deferred=" << plan.value().deferred.size()
                      << " digest=" << plan.value().digest.to_hex() << "\n";
            for (const RecoveryStepId step_id : plan.value().order) {
                const RecoveryStep& step = plan.value().steps.at(step_id);
                std::cout << "step id=" << step.id.to_string() << " " << to_string(step.kind)
                          << " state=" << to_string(step.state)
                          << " obligation=" << step.obligation.to_string()
                          << " source=" << step.source.to_string()
                          << " destination=" << step.destination.to_string()
                          << " attempts=" << step.attempts
                          << " safety_critical=" << (step.safety_critical ? 1 : 0) << "\n";
            }
        }
    }
    Result<CoordinatorStats> stats = Result<CoordinatorStats>(session.coordinator->stats());
    std::cout << "stats records=" << stats.value().records_appended
              << " commits=" << stats.value().commits
              << " dispatches=" << stats.value().dispatches
              << " receipts=" << stats.value().receipts_applied
              << " ignored=" << stats.value().receipts_ignored
              << " stale=" << stats.value().stale_receipts
              << " foreign_epoch=" << stats.value().foreign_epoch_receipts
              << " duplicates=" << stats.value().duplicate_receipts
              << " checkpoints=" << stats.value().checkpoints
              << " compactions=" << stats.value().compactions
              << " torn_tail=" << stats.value().torn_tail_recoveries << "\n";
    std::cout << "digest " << session.coordinator->state_digest().to_hex() << "\n";
    std::cout << "epoch " << session.coordinator->epoch().to_string() << "\n";
    return ok_status();
}

[[nodiscard]] Status command_tick(Session& session, const Arguments& arguments) {
    if (session.manual_clock == nullptr) {
        return Status{ErrorCode::Unsupported, "the coordinator is not running a manual clock"};
    }
    const Result<std::uint64_t> nanos = arguments.number("nanos", true);
    if (!nanos.ok()) {
        return nanos.status();
    }
    session.manual_clock->advance(static_cast<UnixNanos>(nanos.value()));
    std::cout << "clock " << session.manual_clock->now_nanos() << "\n";
    return ok_status();
}

[[nodiscard]] Status command_dump(Session& session) {
    Result<RecoveryPolicy> policy = session.coordinator->policy();
    if (policy.ok()) {
        std::cout << "policy generation=" << policy.value().generation.to_string()
                  << " freshness_ns=" << policy.value().evidence_freshness_window
                  << " attempts=" << policy.value().max_step_attempts
                  << " in_flight=" << policy.value().max_in_flight_steps
                  << " require_protected_placement="
                  << (policy.value().require_protected_placement ? 1 : 0)
                  << " require_federation_evidence="
                  << (policy.value().require_federation_evidence ? 1 : 0) << "\n";
    }
    Result<std::vector<FailureDomainId>> domains = session.coordinator->failure_domains();
    if (domains.ok()) {
        for (const FailureDomainId id : domains.value()) {
            Result<FailureDomainRecord> record = session.coordinator->failure_domain(id);
            if (!record.ok()) {
                continue;
            }
            std::cout << "domain id=" << id.to_string() << " name=" << record.value().name << "\n";
        }
    }
    Result<std::vector<SiteId>> sites = session.coordinator->sites();
    if (sites.ok()) {
        for (const SiteId id : sites.value()) {
            Result<SiteRecord> record = session.coordinator->site(id);
            if (!record.ok()) {
                continue;
            }
            std::cout << "site id=" << id.to_string() << " name=" << record.value().name
                      << " domain=" << record.value().domain.to_string()
                      << " capacity=" << record.value().capacity_units << "\n";
        }
    }
    Result<std::vector<ObligationId>> obligations = session.coordinator->obligations();
    if (obligations.ok()) {
        for (const ObligationId id : obligations.value()) {
            Result<ProtectedObligation> record = session.coordinator->obligation(id);
            if (!record.ok()) {
                continue;
            }
            std::cout << "obligation id=" << id.to_string() << " name=" << record.value().name
                      << " class=" << to_string(record.value().recovery_class)
                      << " home=" << record.value().home_site.to_string()
                      << " capacity=" << record.value().required_capacity_units << "\n";
        }
    }
    Result<std::vector<DisasterEventId>> events = session.coordinator->events();
    if (events.ok()) {
        for (const DisasterEventId id : events.value()) {
            Result<DisasterEvent> record = session.coordinator->event(id);
            if (!record.ok()) {
                continue;
            }
            std::cout << "event id=" << id.to_string() << " phase=" << phase_name(record.value().phase)
                      << " disposition=" << to_string(record.value().disposition)
                      << " generation=" << record.value().generation.to_string() << "\n";
        }
    }
    Result<std::vector<RejectedEvidence>> rejected = session.coordinator->rejected_evidence();
    if (rejected.ok()) {
        for (const RejectedEvidence& evidence : rejected.value()) {
            std::cout << "rejected kind=" << to_string(evidence.kind)
                      << " site=" << evidence.site.to_string()
                      << " offered=" << evidence.offered_generation.to_string()
                      << " retained=" << evidence.retained_generation.to_string()
                      << " reason=" << to_string(evidence.reason) << "\n";
        }
    }
    std::cout << "digest " << session.coordinator->state_digest().to_hex() << "\n";
    std::cout << "epoch " << session.coordinator->epoch().to_string() << "\n";
    return ok_status();
}

[[nodiscard]] Status dispatch_command(Session& session, const std::vector<std::string>& tokens);

[[nodiscard]] Status run_script(Session& session, const std::string& path) {
    std::ifstream stream(path);
    if (!stream) {
        return Status{ErrorCode::Io, "cannot open script " + path};
    }
    std::string line;
    std::size_t line_number = 0;
    bool failed = false;
    while (std::getline(stream, line)) {
        line_number += 1;
        std::vector<std::string> tokens;
        std::string current;
        char quote = 0;
        for (const char c : line) {
            if (quote == 0 && c == '#') {
                break;
            }
            if (quote != 0) {
                if (c == quote) {
                    quote = 0;
                    continue;
                }
                current.push_back(c);
                continue;
            }
            if (c == '"' || c == '\'') {
                quote = c;
                continue;
            }
            if (c == ' ' || c == '\t' || c == '\r') {
                if (!current.empty()) {
                    tokens.push_back(current);
                    current.clear();
                }
                continue;
            }
            current.push_back(c);
        }
        if (!current.empty()) {
            tokens.push_back(current);
        }
        if (tokens.empty()) {
            continue;
        }
        const Status status = dispatch_command(session, tokens);
        if (!status.ok()) {
            failed = true;
            std::cout << "script " << path << ":" << line_number << " failed: "
                      << status.to_string() << "\n";
            if (!session.continue_on_error) {
                return status;
            }
        }
    }
    if (failed) {
        return Status{ErrorCode::Invalid, "script completed with failures"};
    }
    return ok_status();
}

[[nodiscard]] Status dispatch_command(Session& session, const std::vector<std::string>& tokens) {
    const std::string& command = tokens.front();
    Result<Arguments> parsed = parse_arguments(tokens, 1);
    if (!parsed.ok()) {
        return parsed.status();
    }
    const Arguments& arguments = parsed.value();
    Status status = ok_status();
    if (command == "domain") {
        status = command_domain(session, arguments);
    } else if (command == "site") {
        status = command_site(session, arguments);
    } else if (command == "obligation") {
        status = command_obligation(session, arguments);
    } else if (command == "policy") {
        status = command_policy(session, arguments);
    } else if (command == "capability") {
        status = command_capability(session, arguments);
    } else if (command == "readiness") {
        status = command_readiness(session, arguments);
    } else if (command == "federation") {
        status = command_federation(session, arguments);
    } else if (command == "endpoint") {
        status = command_endpoint(session, arguments);
    } else if (command == "declare") {
        status = command_declare(session, arguments);
    } else if (command == "assess") {
        status = command_assess(session, arguments);
    } else if (command == "plan") {
        status = command_plan(session, arguments);
    } else if (command == "begin") {
        status = command_begin(session, arguments);
    } else if (command == "advance") {
        status = command_advance(session, arguments);
    } else if (command == "restore") {
        status = command_restore(session, arguments);
    } else if (command == "failback") {
        status = command_failback(session, arguments);
    } else if (command == "return") {
        status = command_return(session, arguments);
    } else if (command == "resolve") {
        status = command_resolve(session, arguments);
    } else if (command == "close") {
        status = command_close(session, arguments);
    } else if (command == "checkpoint") {
        status = command_checkpoint(session);
    } else if (command == "compact") {
        status = command_compact(session);
    } else if (command == "verify") {
        status = command_verify(session);
    } else if (command == "status") {
        status = command_status(session, arguments);
    } else if (command == "dump") {
        status = command_dump(session);
    } else if (command == "tick") {
        status = command_tick(session, arguments);
    } else if (command == "script") {
        status = run_script(session, arguments.get("file"));
    } else if (command == "echo") {
        std::cout << "echo " << join_values(tokens, 1) << "\n";
        return ok_status();
    } else {
        // Every failure path reports a machine-readable line, including this one.
        const Status unknown{ErrorCode::Unsupported, "unknown command " + command};
        return error_line(command, unknown);
    }
    if (status.ok()) {
        if (command == "advance" || command == "status" || command == "dump" ||
            command == "verify" || command == "tick" || command == "declare" ||
            command == "plan" || command == "failback") {
            return ok_status();
        }
        return ok_line(command);
    }
    return error_line(command, status);
}

void print_usage() {
    std::cout << "drcctl " << version_string()
              << " - Disaster Recovery Coordinator console\n"
                 "\n"
                 "usage: drcctl --dir DIR [--clock manual] [--start-nanos N] [--owner NAME]\n"
                 "              [--crash-after-appends N] [--crash-after-commits N]\n"
                 "              [--crash-after-dispatches N] [--exchange-budget-ms N]\n"
                 "              [--crash-after-dispatches N] COMMAND [--key value ...]\n"
                 "       drcctl --dir DIR --script FILE [--continue-on-error]\n"
                 "\n"
                 "commands: domain site obligation policy capability readiness federation\n"
                 "          endpoint declare assess plan begin advance restore failback\n"
                 "          return resolve close checkpoint compact verify status dump tick\n";
}

}  // namespace

namespace {

// Splits the raw command line into the global flags that precede the command
// word and the command with its own flags. Without this the command's options
// would be consumed by the global parse and never reach the command.
void split_command_line(const std::vector<std::string>& tokens,
                        std::vector<std::string>& global_tokens,
                        std::vector<std::string>& command_tokens) {
    const auto is_flag = [](const std::string& token) {
        return token.size() > 2 && token[0] == '-' && token[1] == '-';
    };
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        if (command_tokens.empty() && is_flag(tokens[i])) {
            global_tokens.push_back(tokens[i]);
            const bool has_inline_value = tokens[i].find('=') != std::string::npos;
            if (!has_inline_value && i + 1 < tokens.size() && !is_flag(tokens[i + 1])) {
                global_tokens.push_back(tokens[i + 1]);
                i += 1;
            }
            continue;
        }
        command_tokens.push_back(tokens[i]);
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> tokens;
    for (int i = 1; i < argc; ++i) {
        tokens.emplace_back(argv[i]);
    }
    if (tokens.empty()) {
        print_usage();
        return 2;
    }
    std::vector<std::string> global_tokens;
    std::vector<std::string> command_tokens;
    split_command_line(tokens, global_tokens, command_tokens);

    Result<Arguments> parsed = parse_arguments(global_tokens, 0);
    if (!parsed.ok()) {
        std::cout << "error arguments " << to_string(parsed.status().code()) << " "
                  << parsed.status().message() << "\n";
        return 2;
    }
    const Arguments& arguments = parsed.value();
    const std::string directory = arguments.get("dir");
    if (directory.empty()) {
        print_usage();
        return 2;
    }
    CoordinatorOptions options;
    options.directory = directory;
    options.owner = arguments.get("owner", "drcctl");
    if (arguments.has("clock") && arguments.get("clock") == "manual") {
        const Result<std::uint64_t> start = arguments.number("start-nanos", false);
        options.clock = std::make_shared<ManualClock>(
            start.value() == 0 ? 1767225600LL * kNanosPerSecond
                               : static_cast<UnixNanos>(start.value()));
    }
    const Result<std::uint64_t> exchange_budget = arguments.number("exchange-budget-ms", false);
    if (exchange_budget.ok() && exchange_budget.value() != 0) {
        options.exchange_budget_nanos =
            static_cast<UnixNanos>(exchange_budget.value()) * kNanosPerMillisecond;
    }
    const Result<std::uint64_t> crash_appends = arguments.number("crash-after-appends", false);
    const Result<std::uint64_t> crash_commits = arguments.number("crash-after-commits", false);
    const Result<std::uint64_t> crash_dispatches = arguments.number("crash-after-dispatches", false);
    options.fault.crash_after_appends = crash_appends.value();
    options.fault.crash_after_commits = crash_commits.value();
    options.fault.crash_after_dispatches = crash_dispatches.value();
    if (arguments.has("allow-lock-takeover")) {
        options.allow_lock_takeover = arguments.get("allow-lock-takeover") != "0";
    }

    Session session;
    if (options.clock == nullptr) {
        options.clock = std::make_shared<SystemClock>();
    }
    session.clock = options.clock;
    session.manual_clock = std::dynamic_pointer_cast<ManualClock>(options.clock);
    session.continue_on_error = arguments.has("continue-on-error");

    const std::shared_ptr<Clock> clock_for_notices = options.clock;
    Result<std::unique_ptr<Coordinator>> opened = Coordinator::open(std::move(options));
    if (!opened.ok()) {
        std::cout << "error open " << to_string(opened.status().code()) << " "
                  << opened.status().message() << "\n";
        return 1;
    }
    session.coordinator = std::move(opened).value();
    // Transitions are reported verbatim: a lock takeover, a recovered tail, or a
    // participant that could not be started must be visible to the operator.
    (void)session.coordinator->set_observer([](const TransitionNotice& notice) {
        std::cout << "notice " << notice.kind;
        if (notice.event.valid()) {
            std::cout << " event=" << notice.event.to_string();
        }
        if (notice.step.valid()) {
            std::cout << " step=" << notice.step.to_string();
        }
        if (!notice.detail.empty()) {
            std::cout << " " << notice.detail;
        }
        std::cout << "\n";
    });
    (void)clock_for_notices;

    Status status = ok_status();
    if (command_tokens.empty() && !arguments.has("script")) {
        print_usage();
        return 2;
    }
    if (!command_tokens.empty()) {
        status = dispatch_command(session, command_tokens);
    }
    if (status.ok() && arguments.has("script")) {
        status = run_script(session, arguments.get("script"));
    }
    (void)session.coordinator->shutdown();
    if (!status.ok()) {
        return 1;
    }
    return 0;
}
