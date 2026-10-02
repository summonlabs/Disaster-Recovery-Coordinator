#include "state.hpp"

#include <algorithm>
#include <string>
#include <utility>

#include "drc/canonical.hpp"

namespace drc::internal {
namespace {

using canonical::Reader;
using canonical::Writer;

void write_blob(Writer& writer, const std::vector<std::uint8_t>& bytes) {
    writer.blob(std::span<const std::uint8_t>(bytes.data(), bytes.size()));
}

[[nodiscard]] Status finish(const Reader& reader, const char* what) {
    if (reader.failed()) {
        return reader.status();
    }
    if (!reader.at_end()) {
        return Status{ErrorCode::Invalid, std::string{"trailing bytes in "} + what};
    }
    return ok_status();
}

void bump(std::uint64_t& watermark, std::uint64_t value) {
    if (value >= watermark) {
        watermark = value + 1;
    }
}

void note(TransitionNotice& notice,
          const char* kind,
          DisasterEventId event,
          RecoveryPlanId plan,
          RecoveryStepId step,
          std::string detail) {
    notice.kind = kind;
    notice.event = event;
    notice.plan = plan;
    notice.step = step;
    notice.detail = std::move(detail);
}

}  // namespace

std::vector<std::uint8_t> encode_state(const CoordinatorState& state) {
    Writer w;
    w.u16(kStateFormatVersion);
    write_blob(w, codec::encode_policy(state.policy));

    w.sequence_count(static_cast<std::uint32_t>(state.domains.size()));
    for (const auto& entry : state.domains) {
        write_blob(w, codec::encode_failure_domain(entry.second));
    }
    w.sequence_count(static_cast<std::uint32_t>(state.sites.size()));
    for (const auto& entry : state.sites) {
        write_blob(w, codec::encode_site(entry.second));
    }
    w.sequence_count(static_cast<std::uint32_t>(state.obligations.size()));
    for (const auto& entry : state.obligations) {
        write_blob(w, codec::encode_obligation(entry.second));
    }
    w.sequence_count(static_cast<std::uint32_t>(state.capabilities.size()));
    for (const auto& entry : state.capabilities) {
        write_blob(w, codec::encode_capability(entry.second));
    }
    w.sequence_count(static_cast<std::uint32_t>(state.readiness.size()));
    for (const auto& entry : state.readiness) {
        write_blob(w, codec::encode_readiness(entry.second));
    }
    write_blob(w, codec::encode_federation(state.federation));

    w.sequence_count(static_cast<std::uint32_t>(state.events.size()));
    for (const auto& entry : state.events) {
        write_blob(w, codec::encode_event(entry.second));
    }
    w.sequence_count(static_cast<std::uint32_t>(state.assessments.size()));
    for (const auto& entry : state.assessments) {
        write_blob(w, codec::encode_assessment(entry.second));
    }
    w.sequence_count(static_cast<std::uint32_t>(state.plans.size()));
    for (const auto& entry : state.plans) {
        write_blob(w, encode_plan(entry.second));
    }
    w.sequence_count(static_cast<std::uint32_t>(state.active_plan.size()));
    for (const auto& entry : state.active_plan) {
        w.u64(entry.first.value());
        w.u64(entry.second.value());
    }
    w.sequence_count(static_cast<std::uint32_t>(state.outstanding.size()));
    for (const auto& entry : state.outstanding) {
        write_blob(w, encode_effect_request(entry.second));
    }
    w.sequence_count(static_cast<std::uint32_t>(state.receipts.size()));
    for (const auto& entry : state.receipts) {
        write_blob(w, encode_effect_receipt(entry.second));
    }
    w.sequence_count(static_cast<std::uint32_t>(state.receipt_by_request.size()));
    for (const auto& entry : state.receipt_by_request) {
        w.u64(entry.first.value());
        w.u64(entry.second.value());
    }
    w.sequence_count(static_cast<std::uint32_t>(state.endpoints.size()));
    for (const auto& entry : state.endpoints) {
        write_blob(w, encode_endpoint_descriptor(entry.second));
    }
    w.sequence_count(static_cast<std::uint32_t>(state.rejected_evidence.size()));
    for (const RejectedEvidence& rejected : state.rejected_evidence) {
        write_blob(w, codec::encode_rejected_evidence(rejected));
    }
    // Snapshot bookkeeping is deliberately not part of the digest: taking a
    // snapshot does not change authority, and the digest must be reproducible
    // from the journal alone.
    w.u64(state.next_failure_domain_id);
    w.u64(state.next_site_id);
    w.u64(state.next_obligation_id);
    w.u64(state.next_event_id);
    w.u64(state.next_plan_id);
    w.u64(state.next_step_id);
    w.u64(state.next_request_id);
    w.u64(state.next_receipt_id);
    w.u64(state.next_endpoint_id);

    w.sequence_count(static_cast<std::uint32_t>(state.plan_generation_count.size()));
    for (const auto& entry : state.plan_generation_count) {
        w.u64(entry.first.value());
        w.u32(entry.second);
    }
    w.sequence_count(static_cast<std::uint32_t>(state.plan_count.size()));
    for (const auto& entry : state.plan_count) {
        w.u64(entry.first.value());
        w.u32(entry.second);
    }
    return w.take();
}

Result<CoordinatorState> decode_state(std::span<const std::uint8_t> bytes) {
    Reader r(bytes);
    CoordinatorState state;
    const std::uint16_t version = r.u16();
    if (version != kStateFormatVersion) {
        return Status{ErrorCode::FormatUnsupported, "state encoding version is not supported"};
    }
    {
        const std::span<const std::uint8_t> blob = r.blob();
        if (r.failed()) {
            return r.status();
        }
        Result<RecoveryPolicy> policy = codec::decode_policy(blob);
        if (!policy.ok()) {
            return policy.status();
        }
        state.policy = std::move(policy).value();
    }
    const std::uint32_t domain_count = r.sequence_count(65536);
    for (std::uint32_t i = 0; i < domain_count; ++i) {
        const std::span<const std::uint8_t> blob = r.blob();
        Result<FailureDomainRecord> value = codec::decode_failure_domain(blob);
        if (!value.ok()) {
            return value.status();
        }
        state.domains.emplace(value.value().id, std::move(value).value());
    }
    const std::uint32_t site_count = r.sequence_count(65536);
    for (std::uint32_t i = 0; i < site_count; ++i) {
        const std::span<const std::uint8_t> blob = r.blob();
        Result<SiteRecord> value = codec::decode_site(blob);
        if (!value.ok()) {
            return value.status();
        }
        state.sites.emplace(value.value().id, std::move(value).value());
    }
    const std::uint32_t obligation_count = r.sequence_count(65536);
    for (std::uint32_t i = 0; i < obligation_count; ++i) {
        const std::span<const std::uint8_t> blob = r.blob();
        Result<ProtectedObligation> value = codec::decode_obligation(blob);
        if (!value.ok()) {
            return value.status();
        }
        state.obligations.emplace(value.value().id, std::move(value).value());
    }
    const std::uint32_t capability_count = r.sequence_count(65536);
    for (std::uint32_t i = 0; i < capability_count; ++i) {
        const std::span<const std::uint8_t> blob = r.blob();
        Result<DestinationCapability> value = codec::decode_capability(blob);
        if (!value.ok()) {
            return value.status();
        }
        state.capabilities.emplace(value.value().site, std::move(value).value());
    }
    const std::uint32_t readiness_count = r.sequence_count(65536);
    for (std::uint32_t i = 0; i < readiness_count; ++i) {
        const std::span<const std::uint8_t> blob = r.blob();
        Result<SiteReadinessEvidence> value = codec::decode_readiness(blob);
        if (!value.ok()) {
            return value.status();
        }
        state.readiness.emplace(value.value().site, std::move(value).value());
    }
    {
        const std::span<const std::uint8_t> blob = r.blob();
        Result<FederationState> value = codec::decode_federation(blob);
        if (!value.ok()) {
            return value.status();
        }
        state.federation = std::move(value).value();
    }
    const std::uint32_t event_count = r.sequence_count(65536);
    for (std::uint32_t i = 0; i < event_count; ++i) {
        const std::span<const std::uint8_t> blob = r.blob();
        Result<DisasterEvent> value = codec::decode_event(blob);
        if (!value.ok()) {
            return value.status();
        }
        state.events.emplace(value.value().id, std::move(value).value());
    }
    const std::uint32_t assessment_count = r.sequence_count(65536);
    for (std::uint32_t i = 0; i < assessment_count; ++i) {
        const std::span<const std::uint8_t> blob = r.blob();
        Result<AssessmentEvidence> value = codec::decode_assessment(blob);
        if (!value.ok()) {
            return value.status();
        }
        state.assessments.emplace(value.value().event, std::move(value).value());
    }
    const std::uint32_t plan_count = r.sequence_count(65536);
    for (std::uint32_t i = 0; i < plan_count; ++i) {
        const std::span<const std::uint8_t> blob = r.blob();
        Result<RecoveryPlan> value = decode_plan(blob);
        if (!value.ok()) {
            return value.status();
        }
        state.plans.emplace(value.value().id, std::move(value).value());
    }
    const std::uint32_t active_count = r.sequence_count(65536);
    for (std::uint32_t i = 0; i < active_count; ++i) {
        const DisasterEventId event{r.u64()};
        const RecoveryPlanId plan{r.u64()};
        state.active_plan.emplace(event, plan);
    }
    const std::uint32_t outstanding_count = r.sequence_count(65536);
    for (std::uint32_t i = 0; i < outstanding_count; ++i) {
        const std::span<const std::uint8_t> blob = r.blob();
        Result<EffectRequest> value = decode_effect_request(blob);
        if (!value.ok()) {
            return value.status();
        }
        state.outstanding.emplace(value.value().id, std::move(value).value());
    }
    const std::uint32_t receipt_count = r.sequence_count(65536);
    for (std::uint32_t i = 0; i < receipt_count; ++i) {
        const std::span<const std::uint8_t> blob = r.blob();
        Result<EffectReceipt> value = decode_effect_receipt(blob);
        if (!value.ok()) {
            return value.status();
        }
        state.receipts.emplace(value.value().id, std::move(value).value());
    }
    const std::uint32_t receipt_index_count = r.sequence_count(65536);
    for (std::uint32_t i = 0; i < receipt_index_count; ++i) {
        const EffectRequestId request{r.u64()};
        const EffectReceiptId receipt{r.u64()};
        state.receipt_by_request.emplace(request, receipt);
    }
    const std::uint32_t endpoint_count = r.sequence_count(64);
    for (std::uint32_t i = 0; i < endpoint_count; ++i) {
        const std::span<const std::uint8_t> blob = r.blob();
        Result<EndpointDescriptor> value = decode_endpoint_descriptor(blob);
        if (!value.ok()) {
            return value.status();
        }
        state.endpoints.emplace(value.value().domain, std::move(value).value());
    }
    const std::uint32_t rejected_count = r.sequence_count(65536);
    for (std::uint32_t i = 0; i < rejected_count; ++i) {
        const std::span<const std::uint8_t> blob = r.blob();
        Result<RejectedEvidence> value = codec::decode_rejected_evidence(blob);
        if (!value.ok()) {
            return value.status();
        }
        state.rejected_evidence.push_back(std::move(value).value());
    }
    state.next_failure_domain_id = r.u64();
    state.next_site_id = r.u64();
    state.next_obligation_id = r.u64();
    state.next_event_id = r.u64();
    state.next_plan_id = r.u64();
    state.next_step_id = r.u64();
    state.next_request_id = r.u64();
    state.next_receipt_id = r.u64();
    state.next_endpoint_id = r.u64();
    const std::uint32_t generation_count = r.sequence_count(65536);
    for (std::uint32_t i = 0; i < generation_count; ++i) {
        const DisasterEventId event{r.u64()};
        const std::uint32_t count = r.u32();
        state.plan_generation_count.emplace(event, count);
    }
    const std::uint32_t plans_per_event = r.sequence_count(65536);
    for (std::uint32_t i = 0; i < plans_per_event; ++i) {
        const DisasterEventId event{r.u64()};
        const std::uint32_t count = r.u32();
        state.plan_count.emplace(event, count);
    }
    const Status status = finish(r, "coordinator state");
    if (!status.ok()) {
        return status;
    }
    return state;
}

Digest state_digest(const CoordinatorState& state) {
    const std::vector<std::uint8_t> encoded = encode_state(state);
    return Digest{Sha256::hash(std::span<const std::uint8_t>(encoded.data(), encoded.size()))};
}

RecoveryPlan* find_plan_of_step(CoordinatorState& state, RecoveryStepId step) {
    for (auto& entry : state.plans) {
        if (entry.second.steps.find(step) != entry.second.steps.end()) {
            return &entry.second;
        }
    }
    return nullptr;
}

Status apply_record(CoordinatorState& state,
                    const journal::Record& record,
                    std::vector<TransitionNotice>& notices) {
    const std::span<const std::uint8_t> payload(record.payload.data(), record.payload.size());
    switch (record.type) {
        case journal::RecordType::Header: {
            Result<codec::JournalHeader> header = codec::decode_journal_header(payload);
            if (!header.ok()) {
                return header.status();
            }
            if (!state.epoch.valid()) {
                state.epoch = header.value().epoch;
            }
            return ok_status();
        }
        case journal::RecordType::FailureDomain: {
            Result<FailureDomainRecord> value = codec::decode_failure_domain(payload);
            if (!value.ok()) {
                return value.status();
            }
            bump(state.next_failure_domain_id, value.value().id.value());
            state.domains[value.value().id] = std::move(value).value();
            return ok_status();
        }
        case journal::RecordType::Site: {
            Result<SiteRecord> value = codec::decode_site(payload);
            if (!value.ok()) {
                return value.status();
            }
            bump(state.next_site_id, value.value().id.value());
            state.sites[value.value().id] = std::move(value).value();
            return ok_status();
        }
        case journal::RecordType::Obligation: {
            Result<ProtectedObligation> value = codec::decode_obligation(payload);
            if (!value.ok()) {
                return value.status();
            }
            bump(state.next_obligation_id, value.value().id.value());
            state.obligations[value.value().id] = std::move(value).value();
            return ok_status();
        }
        case journal::RecordType::Policy: {
            Result<RecoveryPolicy> value = codec::decode_policy(payload);
            if (!value.ok()) {
                return value.status();
            }
            if (value.value().generation >= state.policy.generation) {
                state.policy = std::move(value).value();
            }
            return ok_status();
        }
        case journal::RecordType::Capability: {
            Result<DestinationCapability> value = codec::decode_capability(payload);
            if (!value.ok()) {
                return value.status();
            }
            const auto existing = state.capabilities.find(value.value().site);
            if (existing == state.capabilities.end() ||
                value.value().generation >= existing->second.generation) {
                state.capabilities[value.value().site] = std::move(value).value();
            }
            return ok_status();
        }
        case journal::RecordType::Readiness: {
            Result<SiteReadinessEvidence> value = codec::decode_readiness(payload);
            if (!value.ok()) {
                return value.status();
            }
            const auto existing = state.readiness.find(value.value().site);
            if (existing == state.readiness.end() ||
                value.value().generation >= existing->second.generation) {
                state.readiness[value.value().site] = std::move(value).value();
            }
            return ok_status();
        }
        case journal::RecordType::Federation: {
            Result<FederationState> value = codec::decode_federation(payload);
            if (!value.ok()) {
                return value.status();
            }
            if (value.value().generation >= state.federation.generation) {
                state.federation = std::move(value).value();
            }
            return ok_status();
        }
        case journal::RecordType::Endpoint: {
            Result<EndpointDescriptor> value = decode_endpoint_descriptor(payload);
            if (!value.ok()) {
                return value.status();
            }
            bump(state.next_endpoint_id, value.value().id.value());
            state.endpoints[value.value().domain] = std::move(value).value();
            return ok_status();
        }
        case journal::RecordType::EventDeclaration:
        case journal::RecordType::EventTransition: {
            Result<DisasterEvent> value = codec::decode_event(payload);
            if (!value.ok()) {
                return value.status();
            }
            bump(state.next_event_id, value.value().id.value());
            TransitionNotice notice;
            note(notice, "event", value.value().id, RecoveryPlanId{}, RecoveryStepId{},
                 std::string{"phase "} + std::string{to_string(value.value().phase)});
            notices.push_back(std::move(notice));
            state.events[value.value().id] = std::move(value).value();
            return ok_status();
        }
        case journal::RecordType::EventAssessment: {
            Result<AssessmentEvidence> value = codec::decode_assessment(payload);
            if (!value.ok()) {
                return value.status();
            }
            state.assessments[value.value().event] = std::move(value).value();
            return ok_status();
        }
        case journal::RecordType::Plan: {
            Result<RecoveryPlan> value = decode_plan(payload);
            if (!value.ok()) {
                return value.status();
            }
            RecoveryPlan plan = std::move(value).value();
            for (const auto& entry : plan.steps) {
                bump(state.next_step_id, entry.first.value());
            }
            bump(state.next_plan_id, plan.id.value());
            if (plan.generation.value() > UINT32_MAX) {
                return Status{ErrorCode::Invalid, "plan generation does not fit the counter"};
            }
            state.plan_count[plan.event] += 1;
            std::uint32_t& recorded_generation = state.plan_generation_count[plan.event];
            const auto generation = static_cast<std::uint32_t>(plan.generation.value());
            if (generation > recorded_generation) {
                recorded_generation = generation;
            }
            if (plan.state == PlanState::Active) {
                state.active_plan[plan.event] = plan.id;
            } else {
                const auto active = state.active_plan.find(plan.event);
                if (active != state.active_plan.end() && active->second == plan.id) {
                    state.active_plan.erase(active);
                }
            }
            state.plans[plan.id] = std::move(plan);
            return ok_status();
        }
        case journal::RecordType::StepTransition: {
            Result<RecoveryStep> value = decode_step(payload);
            if (!value.ok()) {
                return value.status();
            }
            RecoveryStep step = std::move(value).value();
            RecoveryPlan* plan = state.plans.find(step.plan) == state.plans.end()
                                     ? nullptr
                                     : &state.plans[step.plan];
            if (plan == nullptr) {
                // A step transition for an unknown plan cannot be applied
                // without inventing the plan. It is reported, not guessed.
                TransitionNotice notice;
                note(notice, "step_without_plan", step.event, step.plan, step.id,
                     "step transition names an unknown plan");
                notices.push_back(std::move(notice));
                return ok_status();
            }
            bump(state.next_step_id, step.id.value());
            plan->steps[step.id] = std::move(step);
            return ok_status();
        }
        case journal::RecordType::Dispatch: {
            Result<EffectRequest> value = decode_effect_request(payload);
            if (!value.ok()) {
                return value.status();
            }
            bump(state.next_request_id, value.value().id.value());
            state.outstanding[value.value().id] = std::move(value).value();
            return ok_status();
        }
        case journal::RecordType::Receipt: {
            Result<EffectReceipt> value = decode_effect_receipt(payload);
            if (!value.ok()) {
                return value.status();
            }
            EffectReceipt receipt = std::move(value).value();
            bump(state.next_receipt_id, receipt.id.value());
            state.outstanding.erase(receipt.request);
            state.receipt_by_request[receipt.request] = receipt.id;
            state.receipts[receipt.id] = std::move(receipt);
            while (state.receipts.size() > state.limits.max_receipts_retained) {
                const auto oldest = state.receipts.begin();
                state.receipt_by_request.erase(oldest->second.request);
                state.receipts.erase(oldest);
            }
            return ok_status();
        }
        case journal::RecordType::RejectedEvidence: {
            Result<RejectedEvidence> value = codec::decode_rejected_evidence(payload);
            if (!value.ok()) {
                return value.status();
            }
            state.rejected_evidence.push_back(std::move(value).value());
            while (state.rejected_evidence.size() > state.limits.max_obligations) {
                state.rejected_evidence.erase(state.rejected_evidence.begin());
            }
            return ok_status();
        }
        case journal::RecordType::ReturnToService: {
            Result<codec::ReturnRecord> value = codec::decode_return_record(payload);
            if (!value.ok()) {
                return value.status();
            }
            const auto event = state.events.find(value.value().event);
            if (event == state.events.end()) {
                TransitionNotice notice;
                note(notice, "return_without_event", value.value().event, RecoveryPlanId{},
                     RecoveryStepId{}, "return record names an unknown event");
                notices.push_back(std::move(notice));
                return ok_status();
            }
            const SiteId site = value.value().site;
            if (std::find(event->second.returned_sites.begin(), event->second.returned_sites.end(),
                          site) == event->second.returned_sites.end()) {
                event->second.returned_sites.push_back(site);
                std::sort(event->second.returned_sites.begin(), event->second.returned_sites.end());
            }
            return ok_status();
        }
        case journal::RecordType::FailbackAuthorization:
        case journal::RecordType::Note:
            return ok_status();
        case journal::RecordType::SnapshotRef: {
            Result<journal::SnapshotRefPayload> value = journal::decode_snapshot_ref(payload);
            if (!value.ok()) {
                return value.status();
            }
            state.last_snapshot = std::move(value).value();
            state.has_snapshot = true;
            state.records_since_snapshot = 0;
            return ok_status();
        }
        case journal::RecordType::Commit: {
            Result<journal::CommitPayload> value = journal::decode_commit(payload);
            if (!value.ok()) {
                return value.status();
            }
            state.last_sequence = record.sequence;
            state.record_count = value.value().record_count;
            if (value.value().epoch.valid() && value.value().epoch > state.epoch) {
                state.epoch = value.value().epoch;
            }
            return ok_status();
        }
    }
    return Status{ErrorCode::Unsupported, "record type is not applied by this build"};
}

}  // namespace drc::internal
