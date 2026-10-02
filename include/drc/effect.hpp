#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "drc/error.hpp"
#include "drc/id.hpp"
#include "drc/model.hpp"
#include "drc/time.hpp"

namespace drc {

// The four neighbouring boundaries this coordinator issues typed requests to.
enum class EffectDomain : std::uint32_t {
    SiteControlPlane = 0,
    PlacementReservationCapacity,
    AsiExecutionRecovery,
    DfiNetworkRecovery,
};

inline constexpr std::uint32_t kEffectDomainCount = 4;

[[nodiscard]] std::string_view to_string(EffectDomain domain) noexcept;
[[nodiscard]] Result<EffectDomain> parse_effect_domain(std::string_view text);
[[nodiscard]] bool is_declared_step_domain(EffectDomain domain) noexcept;

enum class EffectKind : std::uint32_t {
    Evacuate = 0,
    Reserve,
    Place,
    Restore,
    RecoverExecution,
    RecoverNetwork,
    Verify,
    ValidateReadiness,
    ReturnToService,
    Failback,
};

[[nodiscard]] std::string_view to_string(EffectKind kind) noexcept;
[[nodiscard]] Result<EffectKind> parse_effect_kind(std::string_view text);

// The outcome vocabulary of a neighbouring boundary. "Accepted" is not
// completion; "Unknown" is not failure; "Superseded" is not a result for the
// generation it names.
enum class EffectOutcome : std::uint32_t {
    Accepted = 0,
    Completed,
    Rejected,
    Deferred,
    Unsupported,
    Unknown,
    Superseded,
};

[[nodiscard]] std::string_view to_string(EffectOutcome outcome) noexcept;
[[nodiscard]] Result<EffectOutcome> parse_effect_outcome(std::string_view text);

// A typed request. It carries the authority under which it was issued: the
// event generation, the plan generation, and the coordinator epoch. A
// neighbouring boundary may reject any of them; the coordinator may not widen
// them after the fact.
struct EffectRequest {
    EffectRequestId id;
    EffectDomain domain = EffectDomain::SiteControlPlane;
    EffectKind kind = EffectKind::Verify;
    DisasterEventId event;
    RecoveryPlanId plan;
    RecoveryStepId step;
    Generation event_generation;
    Generation plan_generation;
    Epoch issuing_epoch;
    SiteId source;
    SiteId destination;
    ObligationId obligation;
    FailureDomainId destination_domain;
    RecoveryClass recovery_class = RecoveryClass::Standard;
    std::uint64_t required_capacity_units = 0;
    std::uint32_t attempt = 1;
};

// A receipt is the neighbour's own statement about one request. It is applied
// only when it names this coordinator's epoch and a generation that is still
// current; anything else is recorded and ignored.
struct EffectReceipt {
    EffectReceiptId id;
    EffectRequestId request;
    EffectDomain domain = EffectDomain::SiteControlPlane;
    EndpointId endpoint;
    EffectOutcome outcome = EffectOutcome::Unknown;
    ErrorCode detail = ErrorCode::Ok;
    DisasterEventId event;
    RecoveryPlanId plan;
    RecoveryStepId step;
    Generation event_generation;
    Generation plan_generation;
    Epoch observed_epoch;
    UnixNanos observed_at = 0;
    std::uint64_t endpoint_sequence = 0;
    ReservationRef reservation;
    PlacementRef placement;
    std::string message;
};

// Canonical encodings shared by the durable journal, the snapshot, and the
// process transport. One encoding, one decoder, one set of bounds.
[[nodiscard]] std::vector<std::uint8_t> encode_effect_request(const EffectRequest& request);
[[nodiscard]] Result<EffectRequest> decode_effect_request(std::span<const std::uint8_t> bytes);
[[nodiscard]] std::vector<std::uint8_t> encode_effect_receipt(const EffectReceipt& receipt);
[[nodiscard]] Result<EffectReceipt> decode_effect_receipt(std::span<const std::uint8_t> bytes);

// How a receipt is judged before it may change authoritative state.
enum class ReceiptDisposition : std::uint32_t {
    Applies = 0,
    Duplicate,
    UnknownRequest,
    StaleGeneration,
    ForeignEpoch,
    Unsupported,
    // The step it names has already settled, so the answer cannot change
    // authoritative state.
    NotApplicable,
};

[[nodiscard]] std::string_view to_string(ReceiptDisposition disposition) noexcept;

}  // namespace drc
