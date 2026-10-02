#include "drc/model.hpp"

namespace drc {

std::string_view to_string(RecoveryClass value) noexcept {
    switch (value) {
        case RecoveryClass::SafetyCritical: return "safety_critical";
        case RecoveryClass::Protected: return "protected";
        case RecoveryClass::Essential: return "essential";
        case RecoveryClass::Standard: return "standard";
        case RecoveryClass::Deferrable: return "deferrable";
    }
    return "unknown";
}

Result<RecoveryClass> parse_recovery_class(std::string_view text) {
    if (text == "safety_critical") return RecoveryClass::SafetyCritical;
    if (text == "protected") return RecoveryClass::Protected;
    if (text == "essential") return RecoveryClass::Essential;
    if (text == "standard") return RecoveryClass::Standard;
    if (text == "deferrable") return RecoveryClass::Deferrable;
    return Status{ErrorCode::Invalid, "unknown recovery class"};
}

bool is_protected_class(RecoveryClass value) noexcept {
    return value == RecoveryClass::SafetyCritical || value == RecoveryClass::Protected;
}

bool operator==(const RecoveryPolicy& a, const RecoveryPolicy& b) noexcept {
    return a.generation == b.generation &&
           a.evidence_freshness_window == b.evidence_freshness_window &&
           a.max_step_attempts == b.max_step_attempts &&
           a.max_in_flight_steps == b.max_in_flight_steps &&
           a.require_protected_placement == b.require_protected_placement &&
           a.required_readiness_checks == b.required_readiness_checks &&
           a.conflict_on_equal_generation_evidence == b.conflict_on_equal_generation_evidence &&
           a.require_federation_evidence == b.require_federation_evidence;
}

Result<std::uint64_t> checked_add_capacity(std::uint64_t a, std::uint64_t b) {
    if (a > UINT64_MAX - b) {
        return Status{ErrorCode::Overflow, "capacity addition overflows 64 bits"};
    }
    return a + b;
}

bool is_valid_utf8(std::string_view value) noexcept {
    std::size_t i = 0;
    while (i < value.size()) {
        const auto byte = static_cast<std::uint8_t>(value[i]);
        std::size_t extra = 0;
        std::uint32_t codepoint = 0;
        std::uint32_t lowest = 0;
        if (byte < 0x80u) {
            ++i;
            continue;
        } else if ((byte & 0xE0u) == 0xC0u) {
            extra = 1;
            codepoint = byte & 0x1Fu;
            lowest = 0x80u;
        } else if ((byte & 0xF0u) == 0xE0u) {
            extra = 2;
            codepoint = byte & 0x0Fu;
            lowest = 0x800u;
        } else if ((byte & 0xF8u) == 0xF0u) {
            extra = 3;
            codepoint = byte & 0x07u;
            lowest = 0x10000u;
        } else {
            return false;  // continuation byte where a lead byte is required
        }
        if (i + extra >= value.size()) {
            return false;
        }
        for (std::size_t k = 1; k <= extra; ++k) {
            const auto cont = static_cast<std::uint8_t>(value[i + k]);
            if ((cont & 0xC0u) != 0x80u) {
                return false;
            }
            codepoint = (codepoint << 6) | (cont & 0x3Fu);
        }
        if (codepoint < lowest || codepoint > 0x10FFFFu) {
            return false;
        }
        if (codepoint >= 0xD800u && codepoint <= 0xDFFFu) {
            return false;
        }
        i += extra + 1;
    }
    return true;
}

bool is_valid_text(std::string_view value, bool allow_empty) noexcept {
    if (value.empty()) {
        return allow_empty;
    }
    for (const char raw : value) {
        const auto byte = static_cast<unsigned char>(raw);
        if (byte < 0x20u && byte != 0x09u && byte != 0x0Au) {
            return false;
        }
        if (byte == 0x7Fu) {
            return false;
        }
    }
    return is_valid_utf8(value);
}

Result<std::uint64_t> checked_sub_capacity(std::uint64_t a, std::uint64_t b) {
    if (b > a) {
        return Status{ErrorCode::Overflow, "capacity subtraction would underflow"};
    }
    return a - b;
}

}  // namespace drc
