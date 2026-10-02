// drc_endpoint: a reference participant process. It stands in for a
// neighbouring boundary (site control plane, placement/reservation/capacity,
// accelerator execution recovery, network recovery) and speaks the framed pipe
// protocol defined by drc/ports.hpp. It owns its own state and never exposes
// it: the coordinator only ever sees typed receipts.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "drc/effect.hpp"
#include "drc/ports.hpp"
#include "drc/process.hpp"

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

namespace {

using namespace drc;

struct Options {
    EffectDomain domain = EffectDomain::SiteControlPlane;
    EndpointId id{1};
    std::string name = "endpoint";
    SyntheticBehavior behavior;
    std::uint64_t exit_after = 0;
    std::uint64_t drop_after = UINT64_MAX;
    std::uint64_t fail_after = 0;
    std::uint64_t delay_ms = 0;
    bool idempotent = true;
    std::string record_path;
};

struct Memory {
    EffectOutcome outcome = EffectOutcome::Completed;
    ReservationRef reservation;
    PlacementRef placement;
    std::uint64_t sequence = 0;
};

[[nodiscard]] bool read_exact(void* buffer, std::size_t bytes) {
    return std::fread(buffer, 1, bytes, stdin) == bytes;
}

[[nodiscard]] bool write_exact(const void* buffer, std::size_t bytes) {
    if (std::fwrite(buffer, 1, bytes, stdout) != bytes) {
        return false;
    }
    return std::fflush(stdout) == 0;
}

[[nodiscard]] std::string describe(const EffectRequest& request) {
    std::string line = "domain=";
    line.append(to_string(request.domain));
    line.append(" kind=");
    line.append(to_string(request.kind));
    line.append(" request=");
    line.append(request.id.to_string());
    line.append(" step=");
    line.append(request.step.to_string());
    line.append(" plan=");
    line.append(request.plan.to_string());
    line.append(" plan_generation=");
    line.append(request.plan_generation.to_string());
    line.append(" attempt=");
    line.append(std::to_string(request.attempt));
    return line;
}

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    (void)_setmode(_fileno(stdin), _O_BINARY);
    (void)_setmode(_fileno(stdout), _O_BINARY);
#endif
    Options options;
    std::vector<std::string> tokens;
    for (int i = 1; i < argc; ++i) {
        tokens.emplace_back(argv[i]);
    }
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        const std::string& token = tokens[i];
        auto next = [&]() -> std::string {
            if (i + 1 < tokens.size()) {
                return tokens[++i];
            }
            return {};
        };
        auto number = [&](const std::string& text) -> std::uint64_t {
            std::uint64_t value = 0;
            for (const char c : text) {
                if (c < '0' || c > '9') {
                    return UINT64_MAX;
                }
                value = value * 10u + static_cast<std::uint64_t>(c - '0');
            }
            return value;
        };
        if (token == "--domain") {
            const std::string value = next();
            Result<EffectDomain> domain = parse_effect_domain(value);
            if (!domain.ok()) {
                std::fprintf(stderr, "unknown domain %s\n", value.c_str());
                return 2;
            }
            options.domain = domain.value();
        } else if (token == "--id") {
            options.id = EndpointId{number(next())};
        } else if (token == "--name") {
            options.name = next();
        } else if (token == "--outcome") {
            Result<EffectOutcome> outcome = parse_effect_outcome(next());
            if (!outcome.ok()) {
                return 2;
            }
            options.behavior.outcome = outcome.value();
        } else if (token == "--reject-first") {
            options.behavior.reject_first_n = static_cast<std::uint32_t>(number(next()));
        } else if (token == "--defer-first") {
            options.behavior.defer_first_n = static_cast<std::uint32_t>(number(next()));
        } else if (token == "--unknown-first") {
            options.behavior.unknown_first_n = static_cast<std::uint32_t>(number(next()));
        } else if (token == "--duplicate-reply") {
            options.behavior.duplicate_reply = true;
        } else if (token == "--reorder-reply") {
            options.behavior.reorder_reply = true;
        } else if (token == "--stale-generation") {
            options.behavior.stale_generation = true;
        } else if (token == "--foreign-epoch") {
            options.behavior.foreign_epoch = true;
        } else if (token == "--exit-after") {
            options.exit_after = number(next());
        } else if (token == "--drop-after") {
            options.drop_after = number(next());
        } else if (token == "--fail-after") {
            options.fail_after = number(next());
        } else if (token == "--delay-ms") {
            options.delay_ms = number(next());
        } else if (token == "--non-idempotent") {
            options.idempotent = false;
        } else if (token == "--record") {
            options.record_path = next();
        } else if (token == "--help") {
            std::printf(
                "drc_endpoint --domain D --id N [--outcome completed|rejected|deferred|accepted|"
                "unknown|unsupported]\n"
                "            [--reject-first N] [--defer-first N] [--unknown-first N]\n"
                "            [--duplicate-reply] [--reorder-reply] [--stale-generation]\n"
                "            [--foreign-epoch] [--exit-after N] [--drop-after N] [--fail-after N]\n"
                "            [--delay-ms N] [--non-idempotent] [--record FILE]\n");
            return 0;
        }
    }

    std::ofstream record;
    if (!options.record_path.empty()) {
        record.open(options.record_path, std::ios::app);
    }

    std::map<std::uint64_t, Memory> memory;
    std::uint64_t handled = 0;
    for (;;) {
        std::uint8_t header[4] = {};
        if (!read_exact(header, sizeof(header))) {
            break;
        }
        const std::uint32_t length = static_cast<std::uint32_t>(header[0]) |
                                     (static_cast<std::uint32_t>(header[1]) << 8) |
                                     (static_cast<std::uint32_t>(header[2]) << 16) |
                                     (static_cast<std::uint32_t>(header[3]) << 24);
        if (length > process::kMaxFrameBytes) {
            std::fprintf(stderr, "frame too large\n");
            return 3;
        }
        std::vector<std::uint8_t> payload(length);
        if (length > 0 && !read_exact(payload.data(), payload.size())) {
            std::fprintf(stderr, "short frame\n");
            return 4;
        }
        Result<EffectRequest> request =
            decode_effect_request(std::span<const std::uint8_t>(payload.data(), payload.size()));
        if (!request.ok()) {
            std::fprintf(stderr, "undecodable request: %s\n", request.status().to_string().c_str());
            return 5;
        }
        handled += 1;
        if (record.is_open()) {
            record << describe(request.value()) << "\n";
            record.flush();
        }
        if (options.fail_after != 0 && handled > options.fail_after) {
            std::fprintf(stderr, "participant failure after %llu requests\n",
                         static_cast<unsigned long long>(options.fail_after));
            return 6;
        }
        if (options.delay_ms != 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(options.delay_ms));
        }
        if (handled > options.drop_after) {
            // A partition: the participant stays alive but stops answering.
            continue;
        }

        EffectOutcome outcome = options.behavior.outcome;
        if (handled <= options.behavior.reject_first_n) {
            outcome = EffectOutcome::Rejected;
        } else if (handled <= options.behavior.reject_first_n + options.behavior.defer_first_n) {
            outcome = EffectOutcome::Deferred;
        } else if (handled <= options.behavior.reject_first_n + options.behavior.defer_first_n +
                                  options.behavior.unknown_first_n) {
            outcome = EffectOutcome::Unknown;
        } else if (options.behavior.alternate_outcomes && (handled % 2u) == 0u) {
            outcome = EffectOutcome::Unknown;
        }

        Memory entry;
        const std::uint64_t key = request.value().step.value();
        const auto known = memory.find(key);
        if (options.idempotent && known != memory.end()) {
            entry = known->second;
        } else {
            entry.outcome = outcome;
            entry.sequence = handled;
            if (outcome == EffectOutcome::Completed) {
                if (request.value().kind == EffectKind::Reserve) {
                    entry.reservation = ReservationRef{1000 + key};
                }
                if (request.value().kind == EffectKind::Place) {
                    entry.placement = PlacementRef{2000 + key};
                }
            }
            memory[key] = entry;
        }

        EffectReceipt receipt;
        receipt.id = EffectReceiptId{handled};
        receipt.request = request.value().id;
        receipt.domain = request.value().domain;
        receipt.endpoint = options.id;
        receipt.outcome = entry.outcome;
        receipt.detail = entry.outcome == EffectOutcome::Completed ||
                                 entry.outcome == EffectOutcome::Accepted
                             ? ErrorCode::Ok
                             : ErrorCode::NotReady;
        receipt.event = request.value().event;
        receipt.plan = request.value().plan;
        receipt.step = request.value().step;
        receipt.event_generation = request.value().event_generation;
        receipt.plan_generation = request.value().plan_generation;
        receipt.observed_epoch = request.value().issuing_epoch;
        receipt.observed_at = 0;
        receipt.endpoint_sequence = entry.sequence;
        receipt.reservation = entry.reservation;
        receipt.placement = entry.placement;
        receipt.message = "reference participant " + options.name;
        if (options.behavior.stale_generation && request.value().plan_generation.value() > 1) {
            receipt.plan_generation = Generation{request.value().plan_generation.value() - 1};
        }
        if (options.behavior.foreign_epoch) {
            receipt.observed_epoch = request.value().issuing_epoch.value() > 1
                                         ? Epoch{request.value().issuing_epoch.value() - 1}
                                         : Epoch{request.value().issuing_epoch.value() + 1};
        }

        std::vector<EffectReceipt> receipts{receipt};
        if (options.behavior.duplicate_reply) {
            EffectReceipt copy = receipt;
            copy.id = EffectReceiptId{handled + 1000000};
            receipts.push_back(copy);
        }
        if (options.behavior.reorder_reply && receipts.size() > 1) {
            std::swap(receipts[0], receipts[1]);
        }
        const std::vector<std::uint8_t> encoded = encode_receipt_batch(receipts);
        if (encoded.empty()) {
            std::fprintf(stderr, "could not encode receipts\n");
            return 7;
        }
        std::vector<std::uint8_t> frame;
        const std::uint32_t size = static_cast<std::uint32_t>(encoded.size());
        frame.push_back(static_cast<std::uint8_t>(size & 0xFFu));
        frame.push_back(static_cast<std::uint8_t>((size >> 8) & 0xFFu));
        frame.push_back(static_cast<std::uint8_t>((size >> 16) & 0xFFu));
        frame.push_back(static_cast<std::uint8_t>((size >> 24) & 0xFFu));
        frame.insert(frame.end(), encoded.begin(), encoded.end());
        if (!write_exact(frame.data(), frame.size())) {
            return 8;
        }
        if (options.exit_after != 0 && handled >= options.exit_after) {
            break;
        }
    }
    return 0;
}
