#include "drc/effect.hpp"

#include <span>

#include "drc/canonical.hpp"
#include "drc/error.hpp"

namespace drc {
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

}  // namespace

std::string_view to_string(EffectDomain domain) noexcept {
    switch (domain) {
        case EffectDomain::SiteControlPlane: return "site_control_plane";
        case EffectDomain::PlacementReservationCapacity: return "placement_reservation_capacity";
        case EffectDomain::AsiExecutionRecovery: return "asi_execution_recovery";
        case EffectDomain::DfiNetworkRecovery: return "dfi_network_recovery";
    }
    return "unknown";
}

Result<EffectDomain> parse_effect_domain(std::string_view text) {
    if (text == "site_control_plane") return EffectDomain::SiteControlPlane;
    if (text == "placement_reservation_capacity") return EffectDomain::PlacementReservationCapacity;
    if (text == "asi_execution_recovery") return EffectDomain::AsiExecutionRecovery;
    if (text == "dfi_network_recovery") return EffectDomain::DfiNetworkRecovery;
    return Status{ErrorCode::Invalid, "unknown effect domain"};
}

bool is_declared_step_domain(EffectDomain domain) noexcept {
    switch (domain) {
        case EffectDomain::SiteControlPlane:
        case EffectDomain::PlacementReservationCapacity:
        case EffectDomain::AsiExecutionRecovery:
        case EffectDomain::DfiNetworkRecovery:
            return true;
    }
    return false;
}

std::string_view to_string(EffectKind kind) noexcept {
    switch (kind) {
        case EffectKind::Evacuate: return "evacuate";
        case EffectKind::Reserve: return "reserve";
        case EffectKind::Place: return "place";
        case EffectKind::Restore: return "restore";
        case EffectKind::RecoverExecution: return "recover_execution";
        case EffectKind::RecoverNetwork: return "recover_network";
        case EffectKind::Verify: return "verify";
        case EffectKind::ValidateReadiness: return "validate_readiness";
        case EffectKind::ReturnToService: return "return_to_service";
        case EffectKind::Failback: return "failback";
    }
    return "unknown";
}

Result<EffectKind> parse_effect_kind(std::string_view text) {
    if (text == "evacuate") return EffectKind::Evacuate;
    if (text == "reserve") return EffectKind::Reserve;
    if (text == "place") return EffectKind::Place;
    if (text == "restore") return EffectKind::Restore;
    if (text == "recover_execution") return EffectKind::RecoverExecution;
    if (text == "recover_network") return EffectKind::RecoverNetwork;
    if (text == "verify") return EffectKind::Verify;
    if (text == "validate_readiness") return EffectKind::ValidateReadiness;
    if (text == "return_to_service") return EffectKind::ReturnToService;
    if (text == "failback") return EffectKind::Failback;
    return Status{ErrorCode::Invalid, "unknown effect kind"};
}

std::string_view to_string(EffectOutcome outcome) noexcept {
    switch (outcome) {
        case EffectOutcome::Accepted: return "accepted";
        case EffectOutcome::Completed: return "completed";
        case EffectOutcome::Rejected: return "rejected";
        case EffectOutcome::Deferred: return "deferred";
        case EffectOutcome::Unsupported: return "unsupported";
        case EffectOutcome::Unknown: return "unknown";
        case EffectOutcome::Superseded: return "superseded";
    }
    return "unknown";
}

Result<EffectOutcome> parse_effect_outcome(std::string_view text) {
    if (text == "accepted") return EffectOutcome::Accepted;
    if (text == "completed") return EffectOutcome::Completed;
    if (text == "rejected") return EffectOutcome::Rejected;
    if (text == "deferred") return EffectOutcome::Deferred;
    if (text == "unsupported") return EffectOutcome::Unsupported;
    if (text == "unknown") return EffectOutcome::Unknown;
    if (text == "superseded") return EffectOutcome::Superseded;
    return Status{ErrorCode::Invalid, "unknown effect outcome"};
}

std::string_view to_string(ReceiptDisposition disposition) noexcept {
    switch (disposition) {
        case ReceiptDisposition::Applies: return "applies";
        case ReceiptDisposition::Duplicate: return "duplicate";
        case ReceiptDisposition::UnknownRequest: return "unknown_request";
        case ReceiptDisposition::StaleGeneration: return "stale_generation";
        case ReceiptDisposition::ForeignEpoch: return "foreign_epoch";
        case ReceiptDisposition::Unsupported: return "unsupported";
        case ReceiptDisposition::NotApplicable: return "not_applicable";
    }
    return "unknown";
}

std::vector<std::uint8_t> encode_effect_request(const EffectRequest& request) {
    Writer w;
    w.u64(request.id.value());
    w.u32(static_cast<std::uint32_t>(request.domain));
    w.u32(static_cast<std::uint32_t>(request.kind));
    w.u64(request.event.value());
    w.u64(request.plan.value());
    w.u64(request.step.value());
    w.u64(request.event_generation.value());
    w.u64(request.plan_generation.value());
    w.u64(request.issuing_epoch.value());
    w.u64(request.source.value());
    w.u64(request.destination.value());
    w.u64(request.obligation.value());
    w.u64(request.destination_domain.value());
    w.u32(static_cast<std::uint32_t>(request.recovery_class));
    w.u64(request.required_capacity_units);
    w.u32(request.attempt);
    return w.take();
}

Result<EffectRequest> decode_effect_request(std::span<const std::uint8_t> bytes) {
    Reader r(bytes);
    EffectRequest v;
    v.id = EffectRequestId{r.u64()};
    const std::uint32_t domain = r.u32();
    const std::uint32_t kind = r.u32();
    v.event = DisasterEventId{r.u64()};
    v.plan = RecoveryPlanId{r.u64()};
    v.step = RecoveryStepId{r.u64()};
    v.event_generation = Generation{r.u64()};
    v.plan_generation = Generation{r.u64()};
    v.issuing_epoch = Epoch{r.u64()};
    v.source = SiteId{r.u64()};
    v.destination = SiteId{r.u64()};
    v.obligation = ObligationId{r.u64()};
    v.destination_domain = FailureDomainId{r.u64()};
    const std::uint32_t cls = r.u32();
    v.required_capacity_units = r.u64();
    v.attempt = r.u32();
    const Status status = finish(r, "effect request");
    if (!status.ok()) {
        return status;
    }
    if (domain >= kEffectDomainCount) {
        return Status{ErrorCode::Invalid, "effect request domain out of range"};
    }
    if (kind > static_cast<std::uint32_t>(EffectKind::Failback)) {
        return Status{ErrorCode::Invalid, "effect request kind out of range"};
    }
    if (cls >= kRecoveryClassCount) {
        return Status{ErrorCode::Invalid, "effect request recovery class out of range"};
    }
    if (!v.id.valid() || !v.step.valid() || !v.plan.valid() || !v.event.valid()) {
        return Status{ErrorCode::Invalid, "effect request identity is zero"};
    }
    if (!v.issuing_epoch.valid()) {
        return Status{ErrorCode::Invalid, "effect request epoch is zero"};
    }
    if (v.attempt == 0) {
        return Status{ErrorCode::Invalid, "effect request attempt is zero"};
    }
    v.domain = static_cast<EffectDomain>(domain);
    v.kind = static_cast<EffectKind>(kind);
    v.recovery_class = static_cast<RecoveryClass>(cls);
    return v;
}

std::vector<std::uint8_t> encode_effect_receipt(const EffectReceipt& receipt) {
    Writer w;
    w.u64(receipt.id.value());
    w.u64(receipt.request.value());
    w.u32(static_cast<std::uint32_t>(receipt.domain));
    w.u64(receipt.endpoint.value());
    w.u32(static_cast<std::uint32_t>(receipt.outcome));
    w.u32(static_cast<std::uint32_t>(receipt.detail));
    w.u64(receipt.event.value());
    w.u64(receipt.plan.value());
    w.u64(receipt.step.value());
    w.u64(receipt.event_generation.value());
    w.u64(receipt.plan_generation.value());
    w.u64(receipt.observed_epoch.value());
    w.i64(receipt.observed_at);
    w.u64(receipt.endpoint_sequence);
    w.u64(receipt.reservation.value());
    w.u64(receipt.placement.value());
    w.text(receipt.message);
    return w.take();
}

Result<EffectReceipt> decode_effect_receipt(std::span<const std::uint8_t> bytes) {
    Reader r(bytes);
    EffectReceipt v;
    v.id = EffectReceiptId{r.u64()};
    v.request = EffectRequestId{r.u64()};
    const std::uint32_t domain = r.u32();
    v.endpoint = EndpointId{r.u64()};
    const std::uint32_t outcome = r.u32();
    const std::uint32_t detail = r.u32();
    v.event = DisasterEventId{r.u64()};
    v.plan = RecoveryPlanId{r.u64()};
    v.step = RecoveryStepId{r.u64()};
    v.event_generation = Generation{r.u64()};
    v.plan_generation = Generation{r.u64()};
    v.observed_epoch = Epoch{r.u64()};
    v.observed_at = r.i64();
    v.endpoint_sequence = r.u64();
    v.reservation = ReservationRef{r.u64()};
    v.placement = PlacementRef{r.u64()};
    v.message = std::string{r.text()};
    const Status status = finish(r, "effect receipt");
    if (!status.ok()) {
        return status;
    }
    if (domain >= kEffectDomainCount) {
        return Status{ErrorCode::Invalid, "effect receipt domain out of range"};
    }
    if (outcome > static_cast<std::uint32_t>(EffectOutcome::Superseded)) {
        return Status{ErrorCode::Invalid, "effect receipt outcome out of range"};
    }
    if (detail > static_cast<std::uint32_t>(ErrorCode::Shutdown)) {
        return Status{ErrorCode::Invalid, "effect receipt detail out of range"};
    }
    v.domain = static_cast<EffectDomain>(domain);
    v.outcome = static_cast<EffectOutcome>(outcome);
    v.detail = static_cast<ErrorCode>(detail);
    return v;
}

}  // namespace drc
