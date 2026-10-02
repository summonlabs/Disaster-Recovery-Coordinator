#include "drc/evidence.hpp"

namespace drc {

bool capability_supports_class(const DestinationCapability& capability, RecoveryClass cls) noexcept {
    const std::uint32_t bit = 1u << static_cast<std::uint32_t>(cls);
    return (capability.supported_classes_mask & bit) != 0u;
}

bool readiness_passes_checks(const SiteReadinessEvidence& evidence,
                             const std::vector<std::string>& required_checks) {
    for (const ReadinessCheck& check : evidence.checks) {
        if (!check.passed) {
            return false;
        }
    }
    for (const std::string& required : required_checks) {
        bool found = false;
        for (const ReadinessCheck& check : evidence.checks) {
            if (check.name == required) {
                found = true;
                if (!check.passed) {
                    return false;
                }
                break;
            }
        }
        if (!found) {
            // A missing check is not a passing check.
            return false;
        }
    }
    return true;
}

std::string_view to_string(SiteAvailability value) noexcept {
    switch (value) {
        case SiteAvailability::Unknown: return "unknown";
        case SiteAvailability::Available: return "available";
        case SiteAvailability::Degraded: return "degraded";
        case SiteAvailability::Unavailable: return "unavailable";
    }
    return "unknown";
}

bool federation_reaches(const FederationState& state, FailureDomainId domain) noexcept {
    if (!state.known) {
        // No observation is not permission. An unknown partition state defers
        // remote work instead of assuming reachability.
        return false;
    }
    if (!state.partitioned) {
        return true;
    }
    for (const FailureDomainId unreachable : state.unreachable_domains) {
        if (unreachable == domain) {
            return false;
        }
    }
    return true;
}

std::string_view to_string(EvidenceKind kind) noexcept {
    switch (kind) {
        case EvidenceKind::Capability: return "capability";
        case EvidenceKind::Readiness: return "readiness";
        case EvidenceKind::Assessment: return "assessment";
        case EvidenceKind::Federation: return "federation";
    }
    return "unknown";
}

}  // namespace drc
