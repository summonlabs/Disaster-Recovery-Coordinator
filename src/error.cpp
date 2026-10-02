#include "drc/error.hpp"

namespace drc {

std::string_view to_string(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::Ok: return "ok";
        case ErrorCode::Invalid: return "invalid";
        case ErrorCode::NotFound: return "not_found";
        case ErrorCode::Duplicate: return "duplicate";
        case ErrorCode::Overflow: return "overflow";
        case ErrorCode::OutOfRange: return "out_of_range";
        case ErrorCode::Unsupported: return "unsupported";
        case ErrorCode::Unknown: return "unknown";
        case ErrorCode::Indeterminate: return "indeterminate";
        case ErrorCode::Stale: return "stale";
        case ErrorCode::Conflict: return "conflict";
        case ErrorCode::Unauthorized: return "unauthorized";
        case ErrorCode::DependencyUnmet: return "dependency_unmet";
        case ErrorCode::Partitioned: return "partitioned";
        case ErrorCode::PolicyViolation: return "policy_violation";
        case ErrorCode::GateUnmet: return "gate_unmet";
        case ErrorCode::Exhausted: return "exhausted";
        case ErrorCode::Busy: return "busy";
        case ErrorCode::Locked: return "locked";
        case ErrorCode::Cancelled: return "cancelled";
        case ErrorCode::NotReady: return "not_ready";
        case ErrorCode::Corrupt: return "corrupt";
        case ErrorCode::TornTail: return "torn_tail";
        case ErrorCode::FormatUnsupported: return "format_unsupported";
        case ErrorCode::Io: return "io";
        case ErrorCode::Shutdown: return "shutdown";
    }
    return "unknown_error_code";
}

std::string Status::to_string() const {
    std::string out;
    out.reserve(message_.size() + 24);
    out.append(drc::to_string(code_));
    if (!message_.empty()) {
        out.append(": ");
        out.append(message_);
    }
    return out;
}

}  // namespace drc
