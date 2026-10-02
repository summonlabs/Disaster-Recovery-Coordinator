#include "codec.hpp"

#include "drc/canonical.hpp"

namespace drc::codec {
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

void write_ids(Writer& writer, const std::vector<FailureDomainId>& ids) {
    writer.sequence_count(static_cast<std::uint32_t>(ids.size()));
    for (const FailureDomainId id : ids) {
        writer.u64(id.value());
    }
}

void write_site_ids(Writer& writer, const std::vector<SiteId>& ids) {
    writer.sequence_count(static_cast<std::uint32_t>(ids.size()));
    for (const SiteId id : ids) {
        writer.u64(id.value());
    }
}

void write_obligation_ids(Writer& writer, const std::vector<ObligationId>& ids) {
    writer.sequence_count(static_cast<std::uint32_t>(ids.size()));
    for (const ObligationId id : ids) {
        writer.u64(id.value());
    }
}

void write_event_ids(Writer& writer, const std::vector<DisasterEventId>& ids) {
    writer.sequence_count(static_cast<std::uint32_t>(ids.size()));
    for (const DisasterEventId id : ids) {
        writer.u64(id.value());
    }
}

std::vector<FailureDomainId> read_ids(Reader& reader, std::uint32_t max_entries) {
    const std::uint32_t count = reader.sequence_count(max_entries);
    std::vector<FailureDomainId> out;
    out.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        out.push_back(FailureDomainId{reader.u64()});
    }
    return out;
}

std::vector<SiteId> read_site_ids(Reader& reader, std::uint32_t max_entries) {
    const std::uint32_t count = reader.sequence_count(max_entries);
    std::vector<SiteId> out;
    out.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        out.push_back(SiteId{reader.u64()});
    }
    return out;
}

std::vector<ObligationId> read_obligation_ids(Reader& reader, std::uint32_t max_entries) {
    const std::uint32_t count = reader.sequence_count(max_entries);
    std::vector<ObligationId> out;
    out.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        out.push_back(ObligationId{reader.u64()});
    }
    return out;
}

std::vector<DisasterEventId> read_event_ids(Reader& reader, std::uint32_t max_entries) {
    const std::uint32_t count = reader.sequence_count(max_entries);
    std::vector<DisasterEventId> out;
    out.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        out.push_back(DisasterEventId{reader.u64()});
    }
    return out;
}

}  // namespace

bool utf8_is_valid(std::string_view value) {
    return drc::is_valid_utf8(value);
}

Status validate_text_field(std::string_view value, const char* field) {
    if (value.size() > canonical::kMaxTextBytes) {
        return Status{ErrorCode::OutOfRange, std::string{field} + " exceeds the maximum text size"};
    }
    if (!drc::is_valid_text(value, true)) {
        return Status{ErrorCode::Invalid,
                      std::string{field} + " contains a control character or is not well-formed "
                                           "UTF-8"};
    }
    return ok_status();
}

std::vector<std::uint8_t> encode_journal_header(const JournalHeader& value) {
    Writer w;
    w.u16(value.format_version);
    w.text(value.producer);
    w.i64(value.created_at);
    w.u64(value.epoch.value());
    return w.take();
}

Result<JournalHeader> decode_journal_header(std::span<const std::uint8_t> bytes) {
    Reader r(bytes);
    JournalHeader v;
    v.format_version = r.u16();
    v.producer = std::string{r.text()};
    v.created_at = r.i64();
    v.epoch = Epoch{r.u64()};
    const Status status = finish(r, "journal header");
    if (!status.ok()) {
        return status;
    }
    return v;
}

std::vector<std::uint8_t> encode_failure_domain(const FailureDomainRecord& value) {
    Writer w;
    w.u64(value.id.value());
    w.text(value.name);
    return w.take();
}

Result<FailureDomainRecord> decode_failure_domain(std::span<const std::uint8_t> bytes) {
    Reader r(bytes);
    FailureDomainRecord v;
    v.id = FailureDomainId{r.u64()};
    v.name = std::string{r.text()};
    const Status status = finish(r, "failure domain");
    if (!status.ok()) {
        return status;
    }
    if (!v.id.valid()) {
        return Status{ErrorCode::Invalid, "failure domain identity is zero"};
    }
    return v;
}

std::vector<std::uint8_t> encode_site(const SiteRecord& value) {
    Writer w;
    w.u64(value.id.value());
    w.text(value.name);
    w.u64(value.domain.value());
    w.u64(value.capacity_units);
    return w.take();
}

Result<SiteRecord> decode_site(std::span<const std::uint8_t> bytes) {
    Reader r(bytes);
    SiteRecord v;
    v.id = SiteId{r.u64()};
    v.name = std::string{r.text()};
    v.domain = FailureDomainId{r.u64()};
    v.capacity_units = r.u64();
    const Status status = finish(r, "site");
    if (!status.ok()) {
        return status;
    }
    if (!v.id.valid()) {
        return Status{ErrorCode::Invalid, "site identity is zero"};
    }
    return v;
}

std::vector<std::uint8_t> encode_obligation(const ProtectedObligation& value) {
    Writer w;
    w.u64(value.id.value());
    w.text(value.name);
    w.u32(static_cast<std::uint32_t>(value.recovery_class));
    w.u64(value.home_site.value());
    w.u64(value.home_domain.value());
    w.u64(value.required_capacity_units);
    write_obligation_ids(w, value.depends_on);
    return w.take();
}

Result<ProtectedObligation> decode_obligation(std::span<const std::uint8_t> bytes) {
    Reader r(bytes);
    ProtectedObligation v;
    v.id = ObligationId{r.u64()};
    v.name = std::string{r.text()};
    const std::uint32_t cls = r.u32();
    v.home_site = SiteId{r.u64()};
    v.home_domain = FailureDomainId{r.u64()};
    v.required_capacity_units = r.u64();
    if (cls >= kRecoveryClassCount) {
        return Status{ErrorCode::Invalid, "obligation recovery class out of range"};
    }
    v.recovery_class = static_cast<RecoveryClass>(cls);
    v.depends_on = read_obligation_ids(r, Limits{}.max_dependencies_per_obligation);
    const Status status = finish(r, "obligation");
    if (!status.ok()) {
        return status;
    }
    if (!v.id.valid()) {
        return Status{ErrorCode::Invalid, "obligation identity is zero"};
    }
    return v;
}

std::vector<std::uint8_t> encode_policy(const RecoveryPolicy& value) {
    Writer w;
    w.u64(value.generation.value());
    w.i64(value.evidence_freshness_window);
    w.u32(value.max_step_attempts);
    w.u32(value.max_in_flight_steps);
    w.boolean(value.require_protected_placement);
    w.boolean(value.conflict_on_equal_generation_evidence);
    w.boolean(value.require_federation_evidence);
    w.sequence_count(static_cast<std::uint32_t>(value.required_readiness_checks.size()));
    for (const std::string& check : value.required_readiness_checks) {
        w.text(check);
    }
    return w.take();
}

Result<RecoveryPolicy> decode_policy(std::span<const std::uint8_t> bytes) {
    Reader r(bytes);
    RecoveryPolicy v;
    v.generation = Generation{r.u64()};
    v.evidence_freshness_window = r.i64();
    v.max_step_attempts = r.u32();
    v.max_in_flight_steps = r.u32();
    v.require_protected_placement = r.boolean();
    v.conflict_on_equal_generation_evidence = r.boolean();
    v.require_federation_evidence = r.boolean();
    const std::uint32_t count = r.sequence_count(Limits{}.max_readiness_checks);
    v.required_readiness_checks.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        v.required_readiness_checks.emplace_back(r.text());
    }
    const Status status = finish(r, "policy");
    if (!status.ok()) {
        return status;
    }
    if (!v.generation.valid()) {
        return Status{ErrorCode::Invalid, "policy generation is zero"};
    }
    if (v.max_in_flight_steps == 0 || v.max_step_attempts == 0) {
        return Status{ErrorCode::Invalid, "policy bounds are zero"};
    }
    return v;
}

std::vector<std::uint8_t> encode_capability(const DestinationCapability& value) {
    Writer w;
    w.u64(value.site.value());
    w.u64(value.generation.value());
    w.u64(value.available_capacity_units);
    w.u32(value.supported_classes_mask);
    w.i64(value.observed_at);
    w.u64(value.observation_epoch.value());
    w.text(value.source);
    return w.take();
}

Result<DestinationCapability> decode_capability(std::span<const std::uint8_t> bytes) {
    Reader r(bytes);
    DestinationCapability v;
    v.site = SiteId{r.u64()};
    v.generation = Generation{r.u64()};
    v.available_capacity_units = r.u64();
    v.supported_classes_mask = r.u32();
    v.observed_at = r.i64();
    v.observation_epoch = Epoch{r.u64()};
    v.source = std::string{r.text()};
    const Status status = finish(r, "capability");
    if (!status.ok()) {
        return status;
    }
    if (v.supported_classes_mask > ((1u << kRecoveryClassCount) - 1u)) {
        return Status{ErrorCode::Invalid, "capability class mask has unknown bits set"};
    }
    return v;
}

std::vector<std::uint8_t> encode_readiness(const SiteReadinessEvidence& value) {
    Writer w;
    w.u64(value.site.value());
    w.u64(value.generation.value());
    w.i64(value.observed_at);
    w.u64(value.observation_epoch.value());
    w.text(value.source);
    w.sequence_count(static_cast<std::uint32_t>(value.checks.size()));
    for (const ReadinessCheck& check : value.checks) {
        w.text(check.name);
        w.boolean(check.passed);
    }
    return w.take();
}

Result<SiteReadinessEvidence> decode_readiness(std::span<const std::uint8_t> bytes) {
    Reader r(bytes);
    SiteReadinessEvidence v;
    v.site = SiteId{r.u64()};
    v.generation = Generation{r.u64()};
    v.observed_at = r.i64();
    v.observation_epoch = Epoch{r.u64()};
    v.source = std::string{r.text()};
    const std::uint32_t count = r.sequence_count(Limits{}.max_readiness_checks);
    v.checks.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        ReadinessCheck check;
        check.name = std::string{r.text()};
        check.passed = r.boolean();
        v.checks.push_back(std::move(check));
    }
    const Status status = finish(r, "readiness");
    if (!status.ok()) {
        return status;
    }
    return v;
}

std::vector<std::uint8_t> encode_assessment(const AssessmentEvidence& value) {
    Writer w;
    w.u64(value.event.value());
    w.u64(value.event_generation.value());
    w.u64(value.generation.value());
    w.i64(value.observed_at);
    w.u64(value.observation_epoch.value());
    w.text(value.source);
    w.sequence_count(static_cast<std::uint32_t>(value.sites.size()));
    for (const SiteAssessment& site : value.sites) {
        w.u64(site.site.value());
        w.u32(static_cast<std::uint32_t>(site.availability));
        w.boolean(site.telemetry_present);
        w.u32(site.failed_checks);
        w.text(site.note);
    }
    return w.take();
}

Result<AssessmentEvidence> decode_assessment(std::span<const std::uint8_t> bytes) {
    Reader r(bytes);
    AssessmentEvidence v;
    v.event = DisasterEventId{r.u64()};
    v.event_generation = Generation{r.u64()};
    v.generation = Generation{r.u64()};
    v.observed_at = r.i64();
    v.observation_epoch = Epoch{r.u64()};
    v.source = std::string{r.text()};
    const std::uint32_t count = r.sequence_count(Limits{}.max_sites);
    v.sites.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        SiteAssessment site;
        site.site = SiteId{r.u64()};
        const std::uint32_t availability = r.u32();
        if (availability > static_cast<std::uint32_t>(SiteAvailability::Unavailable)) {
            return Status{ErrorCode::Invalid, "assessment availability out of range"};
        }
        site.availability = static_cast<SiteAvailability>(availability);
        site.telemetry_present = r.boolean();
        site.failed_checks = r.u32();
        site.note = std::string{r.text()};
        v.sites.push_back(std::move(site));
    }
    const Status status = finish(r, "assessment");
    if (!status.ok()) {
        return status;
    }
    return v;
}

std::vector<std::uint8_t> encode_federation(const FederationState& value) {
    Writer w;
    w.u64(value.generation.value());
    w.i64(value.observed_at);
    w.u64(value.observation_epoch.value());
    w.text(value.source);
    w.boolean(value.known);
    w.boolean(value.partitioned);
    write_ids(w, value.unreachable_domains);
    return w.take();
}

Result<FederationState> decode_federation(std::span<const std::uint8_t> bytes) {
    Reader r(bytes);
    FederationState v;
    v.generation = Generation{r.u64()};
    v.observed_at = r.i64();
    v.observation_epoch = Epoch{r.u64()};
    v.source = std::string{r.text()};
    v.known = r.boolean();
    v.partitioned = r.boolean();
    v.unreachable_domains = read_ids(r, Limits{}.max_sites);
    const Status status = finish(r, "federation state");
    if (!status.ok()) {
        return status;
    }
    return v;
}

std::vector<std::uint8_t> encode_event(const DisasterEvent& value) {
    Writer w;
    w.u64(value.id.value());
    w.u64(value.generation.value());
    w.u32(static_cast<std::uint32_t>(value.phase));
    w.u32(static_cast<std::uint32_t>(value.phase_before_interruption));
    w.u32(static_cast<std::uint32_t>(value.disposition));
    w.u32(value.severity);
    w.text(value.declared_by);
    w.text(value.reason);
    w.i64(value.declared_at);
    w.u64(value.declared_epoch.value());
    w.i64(value.updated_at);
    w.u64(value.updated_epoch.value());
    write_site_ids(w, value.affected_sites);
    write_ids(w, value.affected_domains);
    write_obligation_ids(w, value.obligations_in_scope);
    w.u64(value.supersedes.value());
    write_event_ids(w, value.conflicts_with);
    write_site_ids(w, value.conflicting_sites);
    write_site_ids(w, value.returned_sites);
    w.u64(value.active_plan.value());
    w.u64(value.active_plan_generation.value());
    w.text(value.status_detail);
    w.u32(value.blocked_steps);
    w.u32(value.failed_safety_critical_steps);
    return w.take();
}

Result<DisasterEvent> decode_event(std::span<const std::uint8_t> bytes) {
    Reader r(bytes);
    DisasterEvent v;
    v.id = DisasterEventId{r.u64()};
    v.generation = Generation{r.u64()};
    const std::uint32_t phase = r.u32();
    const std::uint32_t previous = r.u32();
    const std::uint32_t disposition = r.u32();
    if (phase >= kEventPhaseCount || previous >= kEventPhaseCount) {
        return Status{ErrorCode::Invalid, "event phase out of range"};
    }
    if (disposition > static_cast<std::uint32_t>(EventDisposition::Closed)) {
        return Status{ErrorCode::Invalid, "event disposition out of range"};
    }
    v.phase = static_cast<EventPhase>(phase);
    v.phase_before_interruption = static_cast<EventPhase>(previous);
    v.disposition = static_cast<EventDisposition>(disposition);
    v.severity = r.u32();
    v.declared_by = std::string{r.text()};
    v.reason = std::string{r.text()};
    v.declared_at = r.i64();
    v.declared_epoch = Epoch{r.u64()};
    v.updated_at = r.i64();
    v.updated_epoch = Epoch{r.u64()};
    v.affected_sites = read_site_ids(r, Limits{}.max_sites);
    v.affected_domains = read_ids(r, Limits{}.max_sites);
    v.obligations_in_scope = read_obligation_ids(r, Limits{}.max_obligations);
    v.supersedes = DisasterEventId{r.u64()};
    v.conflicts_with = read_event_ids(r, Limits{}.max_events);
    v.conflicting_sites = read_site_ids(r, Limits{}.max_sites);
    v.returned_sites = read_site_ids(r, Limits{}.max_sites);
    v.active_plan = RecoveryPlanId{r.u64()};
    v.active_plan_generation = Generation{r.u64()};
    v.status_detail = std::string{r.text()};
    v.blocked_steps = r.u32();
    v.failed_safety_critical_steps = r.u32();
    const Status status = finish(r, "event");
    if (!status.ok()) {
        return status;
    }
    if (!v.id.valid()) {
        return Status{ErrorCode::Invalid, "event identity is zero"};
    }
    return v;
}

std::vector<std::uint8_t> encode_rejected_evidence(const RejectedEvidence& value) {
    Writer w;
    w.u32(static_cast<std::uint32_t>(value.kind));
    w.u64(value.site.value());
    w.u64(value.offered_generation.value());
    w.u64(value.retained_generation.value());
    w.u32(static_cast<std::uint32_t>(value.reason));
    w.i64(value.observed_at);
    w.text(value.detail);
    return w.take();
}

Result<RejectedEvidence> decode_rejected_evidence(std::span<const std::uint8_t> bytes) {
    Reader r(bytes);
    RejectedEvidence v;
    const std::uint32_t kind = r.u32();
    if (kind > static_cast<std::uint32_t>(EvidenceKind::Federation)) {
        return Status{ErrorCode::Invalid, "rejected evidence kind out of range"};
    }
    v.kind = static_cast<EvidenceKind>(kind);
    v.site = SiteId{r.u64()};
    v.offered_generation = Generation{r.u64()};
    v.retained_generation = Generation{r.u64()};
    const std::uint32_t reason = r.u32();
    if (reason > static_cast<std::uint32_t>(ErrorCode::Shutdown)) {
        return Status{ErrorCode::Invalid, "rejected evidence reason out of range"};
    }
    v.reason = static_cast<ErrorCode>(reason);
    v.observed_at = r.i64();
    v.detail = std::string{r.text()};
    const Status status = finish(r, "rejected evidence");
    if (!status.ok()) {
        return status;
    }
    return v;
}

std::vector<std::uint8_t> encode_return_record(const ReturnRecord& value) {
    Writer w;
    w.u64(value.event.value());
    w.u64(value.site.value());
    w.u64(value.epoch.value());
    w.i64(value.at);
    w.text(value.authorized_by);
    w.text(value.justification);
    return w.take();
}

Result<ReturnRecord> decode_return_record(std::span<const std::uint8_t> bytes) {
    Reader r(bytes);
    ReturnRecord v;
    v.event = DisasterEventId{r.u64()};
    v.site = SiteId{r.u64()};
    v.epoch = Epoch{r.u64()};
    v.at = r.i64();
    v.authorized_by = std::string{r.text()};
    v.justification = std::string{r.text()};
    const Status status = finish(r, "return record");
    if (!status.ok()) {
        return status;
    }
    return v;
}

std::vector<std::uint8_t> encode_failback_record(const FailbackRecord& value) {
    Writer w;
    w.u64(value.event.value());
    w.u64(value.plan.value());
    w.u64(value.generation.value());
    w.u64(value.target_site.value());
    w.u64(value.epoch.value());
    w.i64(value.at);
    w.text(value.authorized_by);
    w.text(value.justification);
    return w.take();
}

Result<FailbackRecord> decode_failback_record(std::span<const std::uint8_t> bytes) {
    Reader r(bytes);
    FailbackRecord v;
    v.event = DisasterEventId{r.u64()};
    v.plan = RecoveryPlanId{r.u64()};
    v.generation = Generation{r.u64()};
    v.target_site = SiteId{r.u64()};
    v.epoch = Epoch{r.u64()};
    v.at = r.i64();
    v.authorized_by = std::string{r.text()};
    v.justification = std::string{r.text()};
    const Status status = finish(r, "failback record");
    if (!status.ok()) {
        return status;
    }
    return v;
}

std::vector<std::uint8_t> encode_note(std::string_view text) {
    Writer w;
    w.text(text);
    return w.take();
}

Result<std::string> decode_note(std::span<const std::uint8_t> bytes) {
    Reader r(bytes);
    std::string v{r.text()};
    const Status status = finish(r, "note");
    if (!status.ok()) {
        return status;
    }
    return v;
}

}  // namespace drc::codec
