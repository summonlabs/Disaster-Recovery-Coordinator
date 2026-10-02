#pragma once

// Internal canonical codecs for the durable objects. These are deliberately not
// part of the installed public surface: callers use typed API values, and only
// the journal, snapshot, and transport layers see bytes.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "drc/effect.hpp"
#include "drc/engine.hpp"
#include "drc/error.hpp"
#include "drc/evidence.hpp"
#include "drc/id.hpp"
#include "drc/model.hpp"
#include "drc/plan.hpp"
#include "drc/ports.hpp"
#include "drc/time.hpp"

namespace drc::codec {

// Text that reaches durable state is validated first: bounded length, no
// control characters, and well-formed UTF-8. Encoding an object whose text was
// never validated would be a silent corruption path, so encode() returns an
// empty buffer instead and every append site rejects empty payloads.
[[nodiscard]] Status validate_text_field(std::string_view value, const char* field);
[[nodiscard]] bool utf8_is_valid(std::string_view value);

struct JournalHeader {
    std::uint16_t format_version = 1;
    std::string producer;
    UnixNanos created_at = 0;
    Epoch epoch;
};

struct ReturnRecord {
    DisasterEventId event;
    SiteId site;
    Epoch epoch;
    UnixNanos at = 0;
    std::string authorized_by;
    std::string justification;
};

struct FailbackRecord {
    DisasterEventId event;
    RecoveryPlanId plan;
    Generation generation;
    SiteId target_site;
    Epoch epoch;
    UnixNanos at = 0;
    std::string authorized_by;
    std::string justification;
};

[[nodiscard]] std::vector<std::uint8_t> encode_journal_header(const JournalHeader& value);
[[nodiscard]] Result<JournalHeader> decode_journal_header(std::span<const std::uint8_t> bytes);

[[nodiscard]] std::vector<std::uint8_t> encode_failure_domain(const FailureDomainRecord& value);
[[nodiscard]] Result<FailureDomainRecord> decode_failure_domain(std::span<const std::uint8_t> bytes);

[[nodiscard]] std::vector<std::uint8_t> encode_site(const SiteRecord& value);
[[nodiscard]] Result<SiteRecord> decode_site(std::span<const std::uint8_t> bytes);

[[nodiscard]] std::vector<std::uint8_t> encode_obligation(const ProtectedObligation& value);
[[nodiscard]] Result<ProtectedObligation> decode_obligation(std::span<const std::uint8_t> bytes);

[[nodiscard]] std::vector<std::uint8_t> encode_policy(const RecoveryPolicy& value);
[[nodiscard]] Result<RecoveryPolicy> decode_policy(std::span<const std::uint8_t> bytes);

[[nodiscard]] std::vector<std::uint8_t> encode_capability(const DestinationCapability& value);
[[nodiscard]] Result<DestinationCapability> decode_capability(std::span<const std::uint8_t> bytes);

[[nodiscard]] std::vector<std::uint8_t> encode_readiness(const SiteReadinessEvidence& value);
[[nodiscard]] Result<SiteReadinessEvidence> decode_readiness(std::span<const std::uint8_t> bytes);

[[nodiscard]] std::vector<std::uint8_t> encode_assessment(const AssessmentEvidence& value);
[[nodiscard]] Result<AssessmentEvidence> decode_assessment(std::span<const std::uint8_t> bytes);

[[nodiscard]] std::vector<std::uint8_t> encode_federation(const FederationState& value);
[[nodiscard]] Result<FederationState> decode_federation(std::span<const std::uint8_t> bytes);

[[nodiscard]] std::vector<std::uint8_t> encode_event(const struct DisasterEvent& value);
[[nodiscard]] Result<struct DisasterEvent> decode_event(std::span<const std::uint8_t> bytes);

[[nodiscard]] std::vector<std::uint8_t> encode_rejected_evidence(const RejectedEvidence& value);
[[nodiscard]] Result<RejectedEvidence> decode_rejected_evidence(std::span<const std::uint8_t> bytes);

[[nodiscard]] std::vector<std::uint8_t> encode_return_record(const ReturnRecord& value);
[[nodiscard]] Result<ReturnRecord> decode_return_record(std::span<const std::uint8_t> bytes);

[[nodiscard]] std::vector<std::uint8_t> encode_failback_record(const FailbackRecord& value);
[[nodiscard]] Result<FailbackRecord> decode_failback_record(std::span<const std::uint8_t> bytes);

[[nodiscard]] std::vector<std::uint8_t> encode_note(std::string_view text);
[[nodiscard]] Result<std::string> decode_note(std::span<const std::uint8_t> bytes);

}  // namespace drc::codec
