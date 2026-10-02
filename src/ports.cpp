#include "drc/ports.hpp"

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "drc/canonical.hpp"
#include "drc/process.hpp"

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

std::vector<std::uint8_t> encode_receipt_batch(const std::vector<EffectReceipt>& receipts) {
    Writer w;
    w.sequence_count(static_cast<std::uint32_t>(receipts.size()));
    for (const EffectReceipt& receipt : receipts) {
        const std::vector<std::uint8_t> encoded = encode_effect_receipt(receipt);
        w.blob(encoded);
    }
    return w.take();
}

Result<std::vector<EffectReceipt>> decode_receipt_batch(std::span<const std::uint8_t> bytes) {
    Reader r(bytes);
    const std::uint32_t count = r.sequence_count(1024);
    std::vector<EffectReceipt> receipts;
    receipts.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::span<const std::uint8_t> encoded = r.blob();
        if (r.failed()) {
            return r.status();
        }
        Result<EffectReceipt> receipt = decode_effect_receipt(encoded);
        if (!receipt.ok()) {
            return receipt.status();
        }
        receipts.push_back(std::move(receipt).value());
    }
    const Status status = finish(r, "receipt batch");
    if (!status.ok()) {
        return status;
    }
    return receipts;
}

EffectEndpoint::~EffectEndpoint() = default;

SyntheticEndpoint::SyntheticEndpoint(EndpointDescriptor descriptor, EffectReceiptId first_receipt_id)
    : descriptor_(std::move(descriptor)), first_receipt_id_(first_receipt_id.value()) {}

SyntheticEndpoint::~SyntheticEndpoint() = default;

Status SyntheticEndpoint::dispatch(const EffectRequest& request,
                                   std::vector<EffectReceipt>& out) {
    std::lock_guard<std::mutex> guard(mutex_);
    requests_.push_back(request);
    const std::uint64_t index = dispatched_;
    dispatched_ += 1;

    const SyntheticBehavior& behavior = descriptor_.synthetic;
    if (behavior.drop_reply) {
        suppressed_ += 1;
        return ok_status();
    }
    EffectOutcome outcome = behavior.outcome;
    if (index < behavior.reject_first_n) {
        outcome = EffectOutcome::Rejected;
    } else if (index < behavior.reject_first_n + behavior.defer_first_n) {
        outcome = EffectOutcome::Deferred;
    } else if (index < behavior.reject_first_n + behavior.defer_first_n + behavior.unknown_first_n) {
        outcome = EffectOutcome::Unknown;
    } else if (behavior.alternate_outcomes && (index % 2u) == 1u) {
        outcome = EffectOutcome::Unknown;
    }

    EffectReceipt receipt;
    receipt.request = request.id;
    receipt.domain = request.domain;
    receipt.endpoint = descriptor_.id;
    receipt.outcome = outcome;
    receipt.detail = outcome == EffectOutcome::Completed || outcome == EffectOutcome::Accepted
                         ? ErrorCode::Ok
                         : ErrorCode::NotReady;
    receipt.event = request.event;
    receipt.plan = request.plan;
    receipt.step = request.step;
    receipt.event_generation = request.event_generation;
    receipt.plan_generation = request.plan_generation;
    receipt.observed_epoch = request.issuing_epoch;
    if (behavior.stale_generation && request.plan_generation.value() > 1) {
        receipt.plan_generation = Generation{request.plan_generation.value() - 1};
    }
    if (behavior.foreign_epoch) {
        receipt.observed_epoch = request.issuing_epoch.value() > 1
                                     ? Epoch{request.issuing_epoch.value() - 1}
                                     : Epoch{request.issuing_epoch.value() + 1};
    }
    receipt.endpoint_sequence = index + 1;
    receipt.message = "synthetic endpoint";
    if (request.kind == EffectKind::Reserve && outcome == EffectOutcome::Completed) {
        receipt.reservation = ReservationRef{1000 + index + 1};
    }
    if (request.kind == EffectKind::Place && outcome == EffectOutcome::Completed) {
        receipt.placement = PlacementRef{2000 + index + 1};
    }

    receipt.id = EffectReceiptId{first_receipt_id_ + receipt_counter_};
    receipt_counter_ += 1;
    out.push_back(receipt);
    if (behavior.duplicate_reply) {
        EffectReceipt copy = receipt;
        copy.id = EffectReceiptId{first_receipt_id_ + receipt_counter_};
        receipt_counter_ += 1;
        out.push_back(copy);
    }
    if (behavior.reorder_reply && out.size() > 1) {
        std::swap(out[0], out[1]);
    }
    return ok_status();
}

Status SyntheticEndpoint::shutdown() {
    return ok_status();
}

std::vector<EffectRequest> SyntheticEndpoint::requests() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return requests_;
}

std::uint64_t SyntheticEndpoint::request_count() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return dispatched_;
}

std::uint64_t SyntheticEndpoint::suppressed_count() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return suppressed_;
}

// Owns the participant process and serialises the request/response exchange on
// its pipe. The mutex is a leaf: it is never held while another lock is taken.
class ChildProcessHolder {
public:
    std::unique_ptr<process::Child> child;
    std::mutex mutex;
    std::atomic<bool> broken{false};
};

ProcessEndpoint::~ProcessEndpoint() = default;

Result<std::unique_ptr<ProcessEndpoint>> ProcessEndpoint::start(
    const EndpointDescriptor& descriptor) {
    if (descriptor.command.empty()) {
        return Status{ErrorCode::Invalid, "process endpoint needs a command"};
    }
    process::SpawnOptions options;
    options.command = descriptor.command;
    options.working_directory = descriptor.working_directory;
    Result<std::unique_ptr<process::Child>> child = process::Child::spawn(options);
    if (!child.ok()) {
        return child.status();
    }
    auto holder = std::make_unique<ChildProcessHolder>();
    holder->child = std::move(child).value();
    auto endpoint = std::unique_ptr<ProcessEndpoint>(new ProcessEndpoint());
    endpoint->descriptor_ = descriptor;
    endpoint->child_ = std::move(holder);
    return endpoint;
}

Status ProcessEndpoint::dispatch(const EffectRequest& request, std::vector<EffectReceipt>& out) {
    if (child_ == nullptr) {
        return Status{ErrorCode::Invalid, "endpoint is not started"};
    }
    std::lock_guard<std::mutex> guard(child_->mutex);
    if (child_->broken.load()) {
        return Status{ErrorCode::Io,
                      "endpoint is no longer usable: an earlier exchange ended mid-frame"};
    }
    const std::vector<std::uint8_t> encoded = encode_effect_request(request);
    const Status written = child_->child->write_frame(encoded);
    if (!written.ok()) {
        return written;
    }
    Result<std::vector<std::uint8_t>> frame =
        child_->child->read_frame(process::kMaxFrameBytes, descriptor_.exchange_budget_nanos);
    if (!frame.ok()) {
        if (frame.status().code() == ErrorCode::Io) {
            // The reply stream was half-consumed or closed mid-frame: a later
            // read would start inside a payload, so this participant is not
            // used again. A budget that expired before any byte arrived is
            // reported as NotReady instead, which stays retryable.
            child_->broken.store(true);
        }
        return frame.status();
    }
    if (frame.value().empty()) {
        // The participant closed its reply channel. That is a partition, not a
        // failure: the request outcome is unknown and nothing is assumed.
        return ok_status();
    }
    Result<std::vector<EffectReceipt>> receipts = decode_receipt_batch(frame.value());
    if (!receipts.ok()) {
        return receipts.status();
    }
    for (EffectReceipt& receipt : receipts.value()) {
        out.push_back(std::move(receipt));
    }
    return ok_status();
}

Status ProcessEndpoint::shutdown() {
    if (child_ == nullptr) {
        return ok_status();
    }
    std::lock_guard<std::mutex> guard(child_->mutex);
    const Status closed = child_->child->close_stdin();
    const Status terminated = child_->child->terminate();
    if (!closed.ok()) {
        return closed;
    }
    return terminated;
}

std::uint64_t ProcessEndpoint::pid() const noexcept {
    if (child_ == nullptr || child_->child == nullptr) {
        return 0;
    }
    return child_->child->pid();
}

bool ProcessEndpoint::running() const {
    if (child_ == nullptr || child_->child == nullptr) {
        return false;
    }
    return child_->child->running();
}

Result<std::unique_ptr<EffectEndpoint>> make_endpoint(const EndpointDescriptor& descriptor,
                                                      EffectReceiptId first_receipt_id) {
    if (descriptor.command.empty()) {
        return std::unique_ptr<EffectEndpoint>(
            new SyntheticEndpoint(descriptor, first_receipt_id));
    }
    Result<std::unique_ptr<ProcessEndpoint>> endpoint = ProcessEndpoint::start(descriptor);
    if (!endpoint.ok()) {
        return endpoint.status();
    }
    return std::unique_ptr<EffectEndpoint>(std::move(endpoint).value());
}

std::vector<std::uint8_t> encode_endpoint_descriptor(const EndpointDescriptor& descriptor) {
    Writer w;
    w.u32(static_cast<std::uint32_t>(descriptor.domain));
    w.u64(descriptor.id.value());
    w.text(descriptor.name);
    w.sequence_count(static_cast<std::uint32_t>(descriptor.command.size()));
    for (const std::string& argument : descriptor.command) {
        w.text(argument);
    }
    w.text(descriptor.working_directory);
    const SyntheticBehavior& behavior = descriptor.synthetic;
    w.u32(static_cast<std::uint32_t>(behavior.outcome));
    w.boolean(behavior.drop_reply);
    w.boolean(behavior.duplicate_reply);
    w.boolean(behavior.reorder_reply);
    w.boolean(behavior.stale_generation);
    w.boolean(behavior.foreign_epoch);
    w.u32(behavior.reject_first_n);
    w.u32(behavior.defer_first_n);
    w.u32(behavior.unknown_first_n);
    w.boolean(behavior.alternate_outcomes);
    w.i64(descriptor.exchange_budget_nanos);
    return w.take();
}

Result<EndpointDescriptor> decode_endpoint_descriptor(std::span<const std::uint8_t> bytes) {
    Reader r(bytes);
    EndpointDescriptor descriptor;
    const std::uint32_t domain = r.u32();
    descriptor.id = EndpointId{r.u64()};
    descriptor.name = std::string{r.text()};
    const std::uint32_t command_count = r.sequence_count(64);
    descriptor.command.reserve(command_count);
    for (std::uint32_t i = 0; i < command_count; ++i) {
        descriptor.command.emplace_back(r.text());
    }
    descriptor.working_directory = std::string{r.text()};
    const std::uint32_t outcome = r.u32();
    descriptor.synthetic.drop_reply = r.boolean();
    descriptor.synthetic.duplicate_reply = r.boolean();
    descriptor.synthetic.reorder_reply = r.boolean();
    descriptor.synthetic.stale_generation = r.boolean();
    descriptor.synthetic.foreign_epoch = r.boolean();
    descriptor.synthetic.reject_first_n = r.u32();
    descriptor.synthetic.defer_first_n = r.u32();
    descriptor.synthetic.unknown_first_n = r.u32();
    descriptor.synthetic.alternate_outcomes = r.boolean();
    descriptor.exchange_budget_nanos = r.i64();
    const Status status = finish(r, "endpoint descriptor");
    if (!status.ok()) {
        return status;
    }
    if (domain >= kEffectDomainCount) {
        return Status{ErrorCode::Invalid, "endpoint descriptor domain out of range"};
    }
    if (outcome > static_cast<std::uint32_t>(EffectOutcome::Superseded)) {
        return Status{ErrorCode::Invalid, "endpoint descriptor outcome out of range"};
    }
    if (!descriptor.id.valid()) {
        return Status{ErrorCode::Invalid, "endpoint descriptor identity is zero"};
    }
    descriptor.domain = static_cast<EffectDomain>(domain);
    descriptor.synthetic.outcome = static_cast<EffectOutcome>(outcome);
    return descriptor;
}

}  // namespace drc
