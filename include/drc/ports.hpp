#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "drc/effect.hpp"
#include "drc/error.hpp"
#include "drc/id.hpp"

namespace drc {

// How a participant is expected to behave when it is a synthetic endpoint.
// These switches exist so failure, partition, delay, reorder, and duplication
// can be injected deterministically and without a timeout.
struct SyntheticBehavior {
    EffectOutcome outcome = EffectOutcome::Completed;
    bool drop_reply = false;
    bool duplicate_reply = false;
    bool reorder_reply = false;
    bool stale_generation = false;
    bool foreign_epoch = false;
    std::uint32_t reject_first_n = 0;
    std::uint32_t defer_first_n = 0;
    std::uint32_t unknown_first_n = 0;
    bool alternate_outcomes = false;
};

struct EndpointDescriptor {
    EffectDomain domain = EffectDomain::SiteControlPlane;
    EndpointId id;
    std::string name;
    // Empty command means the endpoint is synthetic and runs in-process. A
    // non-empty command is an independent OS process speaking the framed pipe
    // protocol.
    std::vector<std::string> command;
    std::string working_directory;
    SyntheticBehavior synthetic;
    // Zero means "wait as long as it takes". A non-zero value bounds one
    // exchange with this participant.
    UnixNanos exchange_budget_nanos = 0;
};

// An endpoint is somebody else's runtime seen through one narrow interface: it
// accepts a typed request and answers with zero or more receipts. It is never
// asked to expose its internal state.
class EffectEndpoint {
public:
    EffectEndpoint() = default;
    EffectEndpoint(const EffectEndpoint&) = delete;
    EffectEndpoint& operator=(const EffectEndpoint&) = delete;
    virtual ~EffectEndpoint();

    [[nodiscard]] virtual EffectDomain domain() const noexcept = 0;
    [[nodiscard]] virtual EndpointId id() const noexcept = 0;

    // Blocking. Returns transport failures as a Status; a participant that
    // simply does not answer yields no receipts and no error, because silence
    // is not the same as failure.
    [[nodiscard]] virtual Status dispatch(const EffectRequest& request,
                                          std::vector<EffectReceipt>& out) = 0;
    [[nodiscard]] virtual Status shutdown() = 0;
};

// Synthetic participant. Deterministic, in-process, and the default when a
// deployment has no real neighbour to talk to. Every request it answers is
// recorded so tests can assert exactly what was delegated.
class SyntheticEndpoint final : public EffectEndpoint {
public:
    SyntheticEndpoint(EndpointDescriptor descriptor, EffectReceiptId first_receipt_id);
    ~SyntheticEndpoint() override;

    [[nodiscard]] EffectDomain domain() const noexcept override { return descriptor_.domain; }
    [[nodiscard]] EndpointId id() const noexcept override { return descriptor_.id; }

    [[nodiscard]] Status dispatch(const EffectRequest& request,
                                  std::vector<EffectReceipt>& out) override;
    [[nodiscard]] Status shutdown() override;

    [[nodiscard]] std::vector<EffectRequest> requests() const;
    [[nodiscard]] std::uint64_t request_count() const;
    [[nodiscard]] std::uint64_t suppressed_count() const;

private:
    EndpointDescriptor descriptor_;
    std::uint64_t first_receipt_id_ = 1;
    std::uint64_t receipt_counter_ = 0;
    std::uint64_t dispatched_ = 0;
    std::uint64_t suppressed_ = 0;
    mutable std::mutex mutex_;   // leaf: never held while calling anything else
    std::vector<EffectRequest> requests_;
};

// Participant running as an independent OS process. Requests and receipts are
// framed over anonymous pipes; the process is terminated on shutdown.
class ProcessEndpoint final : public EffectEndpoint {
public:
    ~ProcessEndpoint() override;

    [[nodiscard]] static Result<std::unique_ptr<ProcessEndpoint>> start(
        const EndpointDescriptor& descriptor);

    [[nodiscard]] EffectDomain domain() const noexcept override { return descriptor_.domain; }
    [[nodiscard]] EndpointId id() const noexcept override { return descriptor_.id; }

    [[nodiscard]] Status dispatch(const EffectRequest& request,
                                  std::vector<EffectReceipt>& out) override;
    [[nodiscard]] Status shutdown() override;

    [[nodiscard]] std::uint64_t pid() const noexcept;
    [[nodiscard]] bool running() const;

private:
    ProcessEndpoint() = default;

    EndpointDescriptor descriptor_;
    std::unique_ptr<class ChildProcessHolder> child_;
};

// Creates the endpoint a descriptor asks for.
[[nodiscard]] Result<std::unique_ptr<EffectEndpoint>> make_endpoint(
    const EndpointDescriptor& descriptor,
    EffectReceiptId first_receipt_id);

// Framed batch of receipts: what a participant sends back for one request.
// Zero receipts means "no answer yet", which is not the same as failure.
[[nodiscard]] std::vector<std::uint8_t> encode_receipt_batch(const std::vector<EffectReceipt>&);
[[nodiscard]] Result<std::vector<EffectReceipt>> decode_receipt_batch(std::span<const std::uint8_t>);

[[nodiscard]] std::vector<std::uint8_t> encode_endpoint_descriptor(const EndpointDescriptor&);
[[nodiscard]] Result<EndpointDescriptor> decode_endpoint_descriptor(std::span<const std::uint8_t>);

}  // namespace drc
