#include "drc/engine.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>

#include "codec.hpp"
#include "drc/canonical.hpp"
#include "drc/checkpoint.hpp"
#include "drc/fileio.hpp"
#include "engine_internal.hpp"

namespace drc {
namespace {

[[nodiscard]] Status require_text(const std::string& value, const char* field, bool allow_empty) {
    if (!allow_empty && value.empty()) {
        return Status{ErrorCode::Invalid, std::string{field} + " must not be empty"};
    }
    return codec::validate_text_field(value, field);
}

[[nodiscard]] Status require_id(bool valid, const char* what) {
    if (!valid) {
        return Status{ErrorCode::Invalid, std::string{what} + " identity is zero"};
    }
    return ok_status();
}

[[nodiscard]] bool contains_site(const std::vector<SiteId>& ids, SiteId id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

[[nodiscard]] bool is_active_event(const DisasterEvent& event) {
    return event.disposition == EventDisposition::Active && event.phase != EventPhase::Closed;
}

}  // namespace

namespace internal {

bool Mailbox::push_entry(MailboxEntry entry, std::uint64_t* dropped) {
    std::unique_lock<std::mutex> lock(mutex_);
    LockOrderGuard order(LockLevel::Mailbox, nullptr);
    while (queue_.size() >= capacity_ && !closed_) {
        // Back-pressure rather than unbounded growth. The engine drains on
        // every advance and on shutdown, so this cannot deadlock.
        space_.wait(lock);
    }
    if (closed_) {
        if (dropped != nullptr) {
            *dropped += 1;
        }
        return false;
    }
    queue_.push_back(std::move(entry));
    filled_.notify_all();
    return true;
}

bool Mailbox::push(const EffectReceipt& receipt, std::uint64_t* dropped) {
    MailboxEntry entry;
    entry.receipt = receipt;
    entry.is_completion = false;
    return push_entry(std::move(entry), dropped);
}

bool Mailbox::push(const Completion& completion, std::uint64_t* dropped) {
    MailboxEntry entry;
    entry.completion = completion;
    entry.is_completion = true;
    return push_entry(std::move(entry), dropped);
}

std::vector<MailboxEntry> Mailbox::drain() {
    std::unique_lock<std::mutex> lock(mutex_);
    LockOrderGuard order(LockLevel::Mailbox, nullptr);
    std::vector<MailboxEntry> out;
    out.reserve(queue_.size());
    while (!queue_.empty()) {
        out.push_back(std::move(queue_.front()));
        queue_.pop_front();
    }
    space_.notify_all();
    return out;
}

bool Mailbox::wait_for_entry() {
    std::unique_lock<std::mutex> lock(mutex_);
    LockOrderGuard order(LockLevel::Mailbox, nullptr);
    filled_.wait(lock, [this]() { return !queue_.empty() || closed_; });
    return !queue_.empty();
}

void Mailbox::close() {
    std::unique_lock<std::mutex> lock(mutex_);
    LockOrderGuard order(LockLevel::Mailbox, nullptr);
    closed_ = true;
    space_.notify_all();
    filled_.notify_all();
}

std::size_t Mailbox::size() const {
    std::unique_lock<std::mutex> lock(mutex_);
    LockOrderGuard order(LockLevel::Mailbox, nullptr);
    return queue_.size();
}

WorkerPool::WorkerPool(std::uint32_t threads, Mailbox& mailbox, std::size_t queue_capacity)
    : capacity_(queue_capacity == 0 ? 1 : queue_capacity), mailbox_(&mailbox) {
    const std::uint32_t count = threads == 0 ? 1 : threads;
    threads_.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        threads_.emplace_back([this]() { run(); });
    }
}

WorkerPool::~WorkerPool() {
    stop();
}

Status WorkerPool::submit(std::shared_ptr<EffectEndpoint> endpoint, EffectRequest request) {
    std::unique_lock<std::mutex> lock(mutex_);
    LockOrderGuard order(LockLevel::Mailbox, nullptr);
    if (closed_) {
        return Status{ErrorCode::Shutdown, "worker pool is stopped"};
    }
    if (queue_.size() >= capacity_) {
        return Status{ErrorCode::Exhausted, "dispatch queue is full"};
    }
    queue_.push_back(Job{std::move(request), std::move(endpoint)});
    work_.notify_one();
    return ok_status();
}

void WorkerPool::stop() {
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!closed_) {
            closed_ = true;
        }
        work_.notify_all();
        space_.notify_all();
    }
    for (std::thread& thread : threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    threads_.clear();
    std::unique_lock<std::mutex> lock(mutex_);
    queue_.clear();
}

std::size_t WorkerPool::queued() const {
    std::unique_lock<std::mutex> lock(mutex_);
    return queue_.size();
}

std::uint64_t WorkerPool::dispatches() const {
    return dispatches_.load();
}

std::uint64_t WorkerPool::transport_failures() const {
    return transport_failures_.load();
}

std::uint64_t WorkerPool::unanswered() const {
    return unanswered_.load();
}

void WorkerPool::run() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            work_.wait(lock, [this]() { return closed_ || !queue_.empty(); });
            if (queue_.empty()) {
                if (closed_) {
                    return;
                }
                continue;
            }
            job = std::move(queue_.front());
            queue_.pop_front();
            space_.notify_all();
        }
        std::vector<EffectReceipt> receipts;
        const Status status = job.endpoint->dispatch(job.request, receipts);
        dispatches_.fetch_add(1);
        if (!status.ok() && status.code() != ErrorCode::Cancelled) {
            transport_failures_.fetch_add(1);
        }
        if (receipts.empty()) {
            unanswered_.fetch_add(1);
        }
        std::uint64_t dropped = 0;
        for (const EffectReceipt& receipt : receipts) {
            (void)mailbox_->push(receipt, &dropped);
        }
        // The engine must always learn that an exchange is over, even when the
        // participant said nothing at all.
        Completion completion;
        completion.request = job.request.id;
        completion.transport = status;
        completion.delivered_any = !receipts.empty();
        (void)mailbox_->push(completion, &dropped);
    }
}

}  // namespace internal

std::string_view to_string(EventPhase phase) noexcept {
    switch (phase) {
        case EventPhase::Declared: return "declared";
        case EventPhase::Assessed: return "assessed";
        case EventPhase::PlanReady: return "plan_ready";
        case EventPhase::Evacuating: return "evacuating";
        case EventPhase::FailingOver: return "failing_over";
        case EventPhase::Stabilized: return "stabilized";
        case EventPhase::Restoring: return "restoring";
        case EventPhase::Validating: return "validating";
        case EventPhase::Returning: return "returning";
        case EventPhase::Closed: return "closed";
        case EventPhase::Blocked: return "blocked";
        case EventPhase::Conflicted: return "conflicted";
    }
    return "unknown";
}

Result<EventPhase> parse_event_phase(std::string_view text) {
    if (text == "declared") return EventPhase::Declared;
    if (text == "assessed") return EventPhase::Assessed;
    if (text == "plan_ready") return EventPhase::PlanReady;
    if (text == "evacuating") return EventPhase::Evacuating;
    if (text == "failing_over") return EventPhase::FailingOver;
    if (text == "stabilized") return EventPhase::Stabilized;
    if (text == "restoring") return EventPhase::Restoring;
    if (text == "validating") return EventPhase::Validating;
    if (text == "returning") return EventPhase::Returning;
    if (text == "closed") return EventPhase::Closed;
    if (text == "blocked") return EventPhase::Blocked;
    if (text == "conflicted") return EventPhase::Conflicted;
    return Status{ErrorCode::Invalid, "unknown event phase"};
}

bool is_terminal_phase(EventPhase phase) noexcept {
    return phase == EventPhase::Closed;
}

std::string_view to_string(EventDisposition disposition) noexcept {
    switch (disposition) {
        case EventDisposition::Active: return "active";
        case EventDisposition::Superseded: return "superseded";
        case EventDisposition::Closed: return "closed";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Impl helpers
// ---------------------------------------------------------------------------

UnixNanos Coordinator::Impl::now() const {
    return options.clock->now_nanos();
}

Generation Coordinator::Impl::next_generation_for(DisasterEventId event) const {
    const auto found = state.plan_generation_count.find(event);
    const std::uint64_t current = found == state.plan_generation_count.end() ? 0 : found->second;
    return Generation{current + 1};
}

Status Coordinator::Impl::append_and_apply(journal::RecordType type,
                                           std::vector<std::uint8_t> payload) {
    if (writer == nullptr) {
        return Status{ErrorCode::Invalid, "coordinator is not open for writing"};
    }
    if (payload.empty()) {
        return Status{ErrorCode::Invalid, "refusing to append an empty record payload"};
    }
    Result<Sequence> sequence = writer->append(type, payload);
    if (!sequence.ok()) {
        return sequence.status();
    }
    journal::Record record;
    record.type = type;
    record.sequence = sequence.value();
    record.payload = std::move(payload);
    const Status applied = internal::apply_record(state, record, notices);
    if (!applied.ok()) {
        return applied;
    }
    stats.records_appended += 1;
    if (type != journal::RecordType::SnapshotRef) {
        retained.push_back(record);
    }
    state.records_since_snapshot += 1;
    if (options.fault.crash_after_appends != 0 &&
        stats.records_appended >= options.fault.crash_after_appends) {
        // Make the records durable but leave them uncommitted, then die. This
        // is the crash boundary the recovery tests exercise.
        (void)writer->flush();
        std::_Exit(9);
    }
    return ok_status();
}

Status Coordinator::Impl::commit(const char* reason) {
    if (writer == nullptr) {
        return Status{ErrorCode::Invalid, "coordinator is not open for writing"};
    }
    journal::CommitPayload payload;
    payload.state_digest = internal::state_digest(state);
    payload.epoch = state.epoch;
    payload.committed_at = now();
    payload.record_count = writer->last_sequence().valid() ? writer->last_sequence().value() + 1 : 1;
    const Status committed = writer->commit(payload);
    if (!committed.ok()) {
        return committed;
    }
    state.last_sequence = writer->last_sequence();
    state.record_count = payload.record_count;
    stats.commits += 1;
    (void)reason;
    if (options.fault.crash_after_commits != 0 &&
        stats.commits >= options.fault.crash_after_commits) {
        std::_Exit(9);
    }
    return ok_status();
}

Status Coordinator::Impl::publish_notices() {
    if (notices.empty()) {
        return ok_status();
    }
    std::vector<TransitionNotice> local;
    local.swap(notices);
    if (observer) {
        for (const TransitionNotice& notice : local) {
            try {
                observer(notice);
            } catch (...) {
                // A callback that throws must not damage authoritative state.
                stats.observer_failures += 1;
            }
        }
    }
    return ok_status();
}

std::shared_ptr<EffectEndpoint> Coordinator::Impl::endpoint_for(EffectDomain domain) {
    const auto running = endpoints.find(domain);
    if (running != endpoints.end()) {
        return running->second;
    }
    const auto descriptor = state.endpoints.find(domain);
    if (descriptor == state.endpoints.end()) {
        return nullptr;
    }
    EndpointDescriptor live = descriptor->second;
    live.exchange_budget_nanos = options.exchange_budget_nanos;
    Result<std::unique_ptr<EffectEndpoint>> created =
        make_endpoint(live, EffectReceiptId{state.next_receipt_id});
    if (!created.ok()) {
        last_endpoint_failure = std::string{to_string(domain)} + ": " + created.status().to_string();
        notices.push_back(TransitionNotice{"endpoint_failed", DisasterEventId{}, RecoveryPlanId{},
                                           RecoveryStepId{}, last_endpoint_failure});
        return nullptr;
    }
    std::shared_ptr<EffectEndpoint> shared(std::move(created).value());
    endpoints.emplace(domain, shared);
    return shared;
}

Status Coordinator::Impl::shutdown_endpoints() {
    Status first = ok_status();
    for (auto& entry : endpoints) {
        const Status status = entry.second->shutdown();
        if (!status.ok() && first.ok()) {
            first = status;
        }
    }
    endpoints.clear();
    return first;
}

Result<Freshness> Coordinator::Impl::readiness_freshness_for(SiteId site) const {
    const auto found = state.readiness.find(site);
    if (found == state.readiness.end()) {
        return Freshness::Unknown;
    }
    const SiteReadinessEvidence& evidence = found->second;
    return evaluate_freshness(now(), evidence.observed_at, evidence.observation_epoch.value(),
                              state.epoch.value(), state.policy.evidence_freshness_window);
}

Status Coordinator::Impl::transition_event(DisasterEventId id, EventPhase phase, std::string detail) {
    const auto found = state.events.find(id);
    if (found == state.events.end()) {
        return Status{ErrorCode::NotFound, "event does not exist"};
    }
    DisasterEvent event = found->second;
    if (event.phase == phase && event.status_detail == detail) {
        return ok_status();
    }
    event.phase = phase;
    event.status_detail = std::move(detail);
    event.updated_at = now();
    event.updated_epoch = state.epoch;
    const std::vector<std::uint8_t> encoded = codec::encode_event(event);
    if (encoded.empty()) {
        return Status{ErrorCode::OutOfRange, "event could not be encoded"};
    }
    return append_and_apply(journal::RecordType::EventTransition, encoded);
}

Status Coordinator::Impl::transition_step(RecoveryPlanId plan_id,
                                          RecoveryStepId step_id,
                                          StepState step_state,
                                          std::string detail,
                                          bool authorize) {
    const auto plan = state.plans.find(plan_id);
    if (plan == state.plans.end()) {
        return Status{ErrorCode::NotFound, "plan does not exist"};
    }
    const auto step = plan->second.steps.find(step_id);
    if (step == plan->second.steps.end()) {
        return Status{ErrorCode::NotFound, "step does not exist in this plan"};
    }
    RecoveryStep updated = step->second;
    updated.state = step_state;
    updated.detail = std::move(detail);
    if (authorize) {
        updated.authorized = true;
    }
    const std::vector<std::uint8_t> encoded = encode_step_transition(updated);
    if (encoded.empty()) {
        return Status{ErrorCode::OutOfRange, "step could not be encoded"};
    }
    return append_and_apply(journal::RecordType::StepTransition, encoded);
}

Status Coordinator::Impl::mark_event_blocked(DisasterEventId id, std::string detail) {
    const auto found = state.events.find(id);
    if (found == state.events.end()) {
        return Status{ErrorCode::NotFound, "event does not exist"};
    }
    DisasterEvent event = found->second;
    if (event.phase != EventPhase::Blocked && event.phase != EventPhase::Conflicted) {
        event.phase_before_interruption = event.phase;
    }
    event.phase = EventPhase::Blocked;
    event.status_detail = std::move(detail);
    event.updated_at = now();
    event.updated_epoch = state.epoch;
    const std::vector<std::uint8_t> encoded = codec::encode_event(event);
    if (encoded.empty()) {
        return Status{ErrorCode::OutOfRange, "event could not be encoded"};
    }
    return append_and_apply(journal::RecordType::EventTransition, encoded);
}

Status Coordinator::Impl::write_snapshot(bool compact_after) {
    if (writer == nullptr) {
        return Status{ErrorCode::Invalid, "coordinator is not open for writing"};
    }
    checkpoint::Header header;
    header.epoch = state.epoch;
    header.covered_through = state.last_sequence;
    header.written_at = now();
    header.state_digest = internal::state_digest(state);
    const std::vector<std::uint8_t> payload = internal::encode_state(state);
    Status status = checkpoint::write_file(snapshot_path, header,
                                           std::span<const std::uint8_t>(payload));
    if (!status.ok()) {
        return status;
    }
    journal::SnapshotRefPayload ref;
    ref.covered_through = state.last_sequence;
    ref.snapshot_epoch = state.epoch;
    ref.payload_bytes = payload.size();
    ref.state_digest = header.state_digest;
    ref.payload_digest = Digest{Sha256::hash(std::span<const std::uint8_t>(payload))};
    ref.written_at = header.written_at;
    status = append_and_apply(journal::RecordType::SnapshotRef, journal::encode_snapshot_ref(ref));
    if (!status.ok()) {
        return status;
    }
    // Everything before the snapshot reference is now covered by the snapshot.
    retained.clear();
    status = commit("snapshot");
    if (!status.ok()) {
        return status;
    }
    stats.checkpoints += 1;
    if (compact_after) {
        return compact_journal();
    }
    return ok_status();
}

Status Coordinator::Impl::compact_journal() {
    if (writer == nullptr) {
        return Status{ErrorCode::Invalid, "coordinator is not open for writing"};
    }
    std::vector<journal::Record> records;
    codec::JournalHeader header;
    header.format_version = journal::kFormatVersion;
    header.producer = "disaster-recovery-coordinator";
    header.created_at = now();
    header.epoch = state.epoch;
    journal::Record header_record;
    header_record.type = journal::RecordType::Header;
    header_record.sequence = Sequence{1};
    header_record.payload = codec::encode_journal_header(header);
    records.push_back(std::move(header_record));

    journal::Record snapshot_record;
    snapshot_record.type = journal::RecordType::SnapshotRef;
    snapshot_record.sequence = Sequence{2};
    snapshot_record.payload = journal::encode_snapshot_ref(state.last_snapshot);
    records.push_back(std::move(snapshot_record));

    for (const journal::Record& record : retained) {
        if (record.type == journal::RecordType::Header ||
            record.type == journal::RecordType::SnapshotRef) {
            continue;
        }
        records.push_back(record);
    }

    journal::CommitPayload payload;
    payload.state_digest = internal::state_digest(state);
    payload.epoch = state.epoch;
    payload.committed_at = now();
    payload.record_count = records.size() + 1;
    const Status rewritten = writer->rewrite(records, payload);
    if (!rewritten.ok()) {
        return rewritten;
    }
    state.last_sequence = writer->last_sequence();
    state.record_count = payload.record_count;
    state.records_since_snapshot = retained.size();
    stats.compactions += 1;
    return ok_status();
}

Result<RecoveryPlanId> Coordinator::Impl::build_plan_for(DisasterEventId event_id,
                                                         Generation generation,
                                                         PlanKind kind,
                                                         const std::string& requested_by,
                                                         const std::vector<SiteId>& targets,
                                                         bool allow_partial) {
    const auto event = state.events.find(event_id);
    if (event == state.events.end()) {
        return Status{ErrorCode::NotFound, "event does not exist"};
    }
    const DisasterEvent& current = event->second;
    std::vector<SiteId> affected = kind == PlanKind::Restoration || kind == PlanKind::Failback
                                       ? targets
                                       : current.affected_sites;

    RecoveryPolicy effective = state.policy;
    if (allow_partial) {
        effective.require_protected_placement = false;
    }
    PlanBuildInput input;
    input.event = event_id;
    input.event_generation = generation;
    input.plan_id = RecoveryPlanId{state.next_plan_id};
    input.plan_generation = generation;
    input.policy_generation = state.policy.generation;
    input.supersedes = current.active_plan;
    input.kind = kind;
    input.epoch = state.epoch;
    input.now = now();
    input.first_step_id = RecoveryStepId{state.next_step_id};
    input.policy = &effective;
    input.limits = &state.limits;
    input.sites = &state.sites;
    input.obligations = &state.obligations;
    input.capabilities = &state.capabilities;
    input.readiness = &state.readiness;
    input.affected_sites = &affected;
    input.affected_domains = &current.affected_domains;
    input.failback_targets = &targets;

    Result<PlanBuildOutcome> built = build_plan(input);
    if (!built.ok()) {
        return built.status();
    }

    RecoveryPlan plan;
    plan.id = input.plan_id;
    plan.event = event_id;
    plan.kind = kind;
    plan.state = PlanState::Active;
    plan.generation = generation;
    plan.event_generation = generation;
    plan.policy_generation = state.policy.generation;
    plan.supersedes = current.active_plan;
    plan.created_epoch = state.epoch;
    plan.created_at = input.now;
    plan.requested_by = requested_by;
    plan.digest = built.value().digest;
    plan.assignments = built.value().assignments;
    plan.unplaced = built.value().unplaced;
    plan.deferred = built.value().deferred;
    plan.diagnostics = built.value().diagnostics;
    plan.target_sites = kind == PlanKind::Failback ? targets : std::vector<SiteId>{};
    for (const RecoveryStep& step : built.value().steps) {
        plan.order.push_back(step.id);
        plan.steps.emplace(step.id, step);
    }

    // A new plan generation fences the previous one: its steps cannot execute
    // any more, and any effect already in flight is judged stale when it
    // finally answers.
    const RecoveryPlanId previous_plan = current.active_plan;
    if (previous_plan.valid()) {
        const auto previous = state.plans.find(previous_plan);
        if (previous != state.plans.end()) {
            RecoveryPlan superseded = previous->second;
            superseded.state = PlanState::Superseded;
            const std::vector<std::uint8_t> encoded = encode_plan(superseded);
            if (encoded.empty()) {
                return Status{ErrorCode::OutOfRange, "plan could not be encoded"};
            }
            Status status = append_and_apply(journal::RecordType::Plan, encoded);
            if (!status.ok()) {
                return status;
            }
            for (const RecoveryStepId step_id : superseded.order) {
                const RecoveryStep& step = superseded.steps.at(step_id);
                if (is_terminal_step_state(step.state) || step.state == StepState::Superseded) {
                    continue;
                }
                status = transition_step(superseded.id, step_id, StepState::Superseded,
                                         "plan generation replaced by a newer plan");
                if (!status.ok()) {
                    return status;
                }
            }
        }
    }

    const std::vector<std::uint8_t> encoded_plan = encode_plan(plan);
    if (encoded_plan.empty()) {
        return Status{ErrorCode::OutOfRange, "plan could not be encoded"};
    }
    Status status = append_and_apply(journal::RecordType::Plan, encoded_plan);
    if (!status.ok()) {
        return status;
    }

    DisasterEvent updated = current;
    updated.generation = generation;
    updated.active_plan = plan.id;
    updated.active_plan_generation = generation;
    if (kind == PlanKind::Recovery && updated.phase < EventPhase::PlanReady) {
        updated.phase = EventPhase::PlanReady;
    }
    updated.updated_at = now();
    updated.updated_epoch = state.epoch;
    updated.status_detail = built.value().unplaced.empty()
                                ? "plan ready"
                                : "plan ready with unplaced obligations";
    const std::vector<std::uint8_t> encoded_event = codec::encode_event(updated);
    if (encoded_event.empty()) {
        return Status{ErrorCode::OutOfRange, "event could not be encoded"};
    }
    status = append_and_apply(journal::RecordType::EventTransition, encoded_event);
    if (!status.ok()) {
        return status;
    }
    status = commit("plan");
    if (!status.ok()) {
        return status;
    }
    return plan.id;
}

// ---------------------------------------------------------------------------
// Registration and evidence
// ---------------------------------------------------------------------------

Status Coordinator::define_failure_domain(FailureDomainRecord record) {
    if (impl_ == nullptr) {
        return Status{ErrorCode::Invalid, "coordinator is not open"};
    }
    Impl& impl = *impl_;
    std::unique_lock<std::shared_mutex> lock(impl.mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        impl.busy_rejections.fetch_add(1);
        return Status{ErrorCode::Busy, "another coordinator call is in progress"};
    }
    internal::LockOrderGuard order(internal::LockLevel::Engine, &impl.lock_order_violations);
    impl.stats.api_calls += 1;
    if (impl.shutting_down) {
        return Status{ErrorCode::Shutdown, "coordinator is shutting down"};
    }
    Status status = require_id(record.id.valid(), "failure domain");
    if (!status.ok()) {
        return status;
    }
    status = require_text(record.name, "failure domain name", false);
    if (!status.ok()) {
        return status;
    }
    if (impl.state.domains.size() >= impl.state.limits.max_sites &&
        impl.state.domains.find(record.id) == impl.state.domains.end()) {
        return Status{ErrorCode::Exhausted, "failure domain limit reached"};
    }
    const auto existing = impl.state.domains.find(record.id);
    if (existing != impl.state.domains.end()) {
        if (existing->second.name == record.name) {
            return ok_status();
        }
        return Status{ErrorCode::Conflict, "failure domain already exists with different content"};
    }
    status = impl.append_and_apply(journal::RecordType::FailureDomain,
                                   codec::encode_failure_domain(record));
    if (!status.ok()) {
        return status;
    }
    status = impl.commit("failure domain");
    if (!status.ok()) {
        return status;
    }
    lock.unlock();
    return impl.publish_notices();
}

Status Coordinator::define_site(SiteRecord record) {
    if (impl_ == nullptr) {
        return Status{ErrorCode::Invalid, "coordinator is not open"};
    }
    Impl& impl = *impl_;
    std::unique_lock<std::shared_mutex> lock(impl.mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        impl.busy_rejections.fetch_add(1);
        return Status{ErrorCode::Busy, "another coordinator call is in progress"};
    }
    internal::LockOrderGuard order(internal::LockLevel::Engine, &impl.lock_order_violations);
    impl.stats.api_calls += 1;
    if (impl.shutting_down) {
        return Status{ErrorCode::Shutdown, "coordinator is shutting down"};
    }
    Status status = require_id(record.id.valid(), "site");
    if (!status.ok()) {
        return status;
    }
    status = require_text(record.name, "site name", false);
    if (!status.ok()) {
        return status;
    }
    if (!record.domain.valid() || impl.state.domains.find(record.domain) == impl.state.domains.end()) {
        return Status{ErrorCode::NotFound, "site names a failure domain that is not registered"};
    }
    if (impl.state.sites.size() >= impl.state.limits.max_sites &&
        impl.state.sites.find(record.id) == impl.state.sites.end()) {
        return Status{ErrorCode::Exhausted, "site limit reached"};
    }
    const auto existing = impl.state.sites.find(record.id);
    if (existing != impl.state.sites.end()) {
        const SiteRecord& current = existing->second;
        if (current.name == record.name && current.domain == record.domain &&
            current.capacity_units == record.capacity_units) {
            return ok_status();
        }
        return Status{ErrorCode::Conflict, "site already exists with different content"};
    }
    status = impl.append_and_apply(journal::RecordType::Site, codec::encode_site(record));
    if (!status.ok()) {
        return status;
    }
    status = impl.commit("site");
    if (!status.ok()) {
        return status;
    }
    lock.unlock();
    return impl.publish_notices();
}

Status Coordinator::define_obligation(ProtectedObligation obligation) {
    if (impl_ == nullptr) {
        return Status{ErrorCode::Invalid, "coordinator is not open"};
    }
    Impl& impl = *impl_;
    std::unique_lock<std::shared_mutex> lock(impl.mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        impl.busy_rejections.fetch_add(1);
        return Status{ErrorCode::Busy, "another coordinator call is in progress"};
    }
    internal::LockOrderGuard order(internal::LockLevel::Engine, &impl.lock_order_violations);
    impl.stats.api_calls += 1;
    if (impl.shutting_down) {
        return Status{ErrorCode::Shutdown, "coordinator is shutting down"};
    }
    Status status = require_id(obligation.id.valid(), "obligation");
    if (!status.ok()) {
        return status;
    }
    status = require_text(obligation.name, "obligation name", false);
    if (!status.ok()) {
        return status;
    }
    if (obligation.required_capacity_units == 0) {
        return Status{ErrorCode::Invalid, "obligation requires zero capacity"};
    }
    const auto site = impl.state.sites.find(obligation.home_site);
    if (!obligation.home_site.valid() || site == impl.state.sites.end()) {
        return Status{ErrorCode::NotFound, "obligation names a home site that is not registered"};
    }
    if (obligation.home_domain.valid() && obligation.home_domain != site->second.domain) {
        return Status{ErrorCode::Conflict, "obligation home domain does not match its home site"};
    }
    obligation.home_domain = site->second.domain;
    if (obligation.depends_on.size() > impl.state.limits.max_dependencies_per_obligation) {
        return Status{ErrorCode::Exhausted, "obligation has too many dependencies"};
    }
    std::vector<ObligationId> seen;
    for (const ObligationId dependency : obligation.depends_on) {
        if (dependency == obligation.id) {
            return Status{ErrorCode::Conflict, "obligation depends on itself"};
        }
        if (std::find(seen.begin(), seen.end(), dependency) != seen.end()) {
            return Status{ErrorCode::Duplicate, "obligation lists a dependency twice"};
        }
        seen.push_back(dependency);
        if (impl.state.obligations.find(dependency) == impl.state.obligations.end()) {
            return Status{ErrorCode::NotFound, "obligation depends on an unregistered obligation"};
        }
    }
    // Reject cycles at registration: a cycle could never be ordered, and a plan
    // that cannot be ordered must never be produced.
    {
        std::vector<ObligationId> stack{obligation.id};
        std::vector<ObligationId> visited;
        while (!stack.empty()) {
            const ObligationId current = stack.back();
            stack.pop_back();
            if (std::find(visited.begin(), visited.end(), current) != visited.end()) {
                continue;
            }
            visited.push_back(current);
            const auto found = impl.state.obligations.find(current);
            const std::vector<ObligationId>* edges = nullptr;
            if (current == obligation.id) {
                edges = &obligation.depends_on;
            } else if (found != impl.state.obligations.end()) {
                edges = &found->second.depends_on;
            }
            if (edges == nullptr) {
                continue;
            }
            for (const ObligationId next : *edges) {
                if (next == obligation.id) {
                    return Status{ErrorCode::Conflict,
                                  "obligation dependency graph would contain a cycle"};
                }
                stack.push_back(next);
            }
        }
    }
    if (impl.state.obligations.size() >= impl.state.limits.max_obligations &&
        impl.state.obligations.find(obligation.id) == impl.state.obligations.end()) {
        return Status{ErrorCode::Exhausted, "obligation limit reached"};
    }
    const auto existing = impl.state.obligations.find(obligation.id);
    if (existing != impl.state.obligations.end()) {
        const ProtectedObligation& current = existing->second;
        if (current.name == obligation.name && current.home_site == obligation.home_site &&
            current.required_capacity_units == obligation.required_capacity_units &&
            current.recovery_class == obligation.recovery_class &&
            current.depends_on == obligation.depends_on) {
            return ok_status();
        }
        return Status{ErrorCode::Conflict, "obligation already exists with different content"};
    }
    status = impl.append_and_apply(journal::RecordType::Obligation,
                                   codec::encode_obligation(obligation));
    if (!status.ok()) {
        return status;
    }
    status = impl.commit("obligation");
    if (!status.ok()) {
        return status;
    }
    lock.unlock();
    return impl.publish_notices();
}

Status Coordinator::set_policy(RecoveryPolicy policy) {
    if (impl_ == nullptr) {
        return Status{ErrorCode::Invalid, "coordinator is not open"};
    }
    Impl& impl = *impl_;
    std::unique_lock<std::shared_mutex> lock(impl.mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        impl.busy_rejections.fetch_add(1);
        return Status{ErrorCode::Busy, "another coordinator call is in progress"};
    }
    internal::LockOrderGuard order(internal::LockLevel::Engine, &impl.lock_order_violations);
    impl.stats.api_calls += 1;
    if (impl.shutting_down) {
        return Status{ErrorCode::Shutdown, "coordinator is shutting down"};
    }
    if (!policy.generation.valid()) {
        return Status{ErrorCode::Invalid, "policy generation is zero"};
    }
    if (policy.max_in_flight_steps == 0 || policy.max_step_attempts == 0) {
        return Status{ErrorCode::Invalid, "policy bounds must be positive"};
    }
    if (policy.max_in_flight_steps > impl.state.limits.max_worker_threads * 64u) {
        return Status{ErrorCode::OutOfRange, "policy in-flight bound is unreasonably large"};
    }
    if (policy.evidence_freshness_window <= 0) {
        return Status{ErrorCode::OutOfRange, "freshness window must be positive"};
    }
    if (policy.required_readiness_checks.size() > impl.state.limits.max_readiness_checks) {
        return Status{ErrorCode::Exhausted, "too many required readiness checks"};
    }
    for (const std::string& check : policy.required_readiness_checks) {
        const Status status = require_text(check, "readiness check name", false);
        if (!status.ok()) {
            return status;
        }
    }
    if (policy.generation < impl.state.policy.generation) {
        return Status{ErrorCode::Stale, "policy generation is older than the active policy"};
    }
    if (policy.generation == impl.state.policy.generation) {
        if (policy == impl.state.policy) {
            return ok_status();
        }
        return Status{ErrorCode::Conflict,
                      "policy content changed without a generation bump"};
    }
    const Status status =
        impl.append_and_apply(journal::RecordType::Policy, codec::encode_policy(policy));
    if (!status.ok()) {
        return status;
    }
    const Status committed = impl.commit("policy");
    if (!committed.ok()) {
        return committed;
    }
    lock.unlock();
    return impl.publish_notices();
}

Status Coordinator::record_capability(DestinationCapability capability) {
    if (impl_ == nullptr) {
        return Status{ErrorCode::Invalid, "coordinator is not open"};
    }
    Impl& impl = *impl_;
    std::unique_lock<std::shared_mutex> lock(impl.mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        impl.busy_rejections.fetch_add(1);
        return Status{ErrorCode::Busy, "another coordinator call is in progress"};
    }
    internal::LockOrderGuard order(internal::LockLevel::Engine, &impl.lock_order_violations);
    impl.stats.api_calls += 1;
    if (impl.shutting_down) {
        return Status{ErrorCode::Shutdown, "coordinator is shutting down"};
    }
    Status status = require_id(capability.site.valid(), "capability site");
    if (!status.ok()) {
        return status;
    }
    status = require_text(capability.source, "capability source", true);
    if (!status.ok()) {
        return status;
    }
    if (impl.state.sites.find(capability.site) == impl.state.sites.end()) {
        return Status{ErrorCode::NotFound, "capability names an unregistered site"};
    }
    if (!capability.generation.valid()) {
        return Status{ErrorCode::Invalid, "capability generation is zero"};
    }
    if ((capability.supported_classes_mask >> kRecoveryClassCount) != 0u) {
        return Status{ErrorCode::Invalid, "capability class mask has unknown bits set"};
    }
    const auto existing = impl.state.capabilities.find(capability.site);
    if (existing != impl.state.capabilities.end()) {
        const DestinationCapability& current = existing->second;
        const bool identical = current.generation == capability.generation &&
                               current.available_capacity_units ==
                                   capability.available_capacity_units &&
                               current.supported_classes_mask == capability.supported_classes_mask;
        if (identical) {
            return ok_status();
        }
        if (capability.generation < current.generation) {
            RejectedEvidence rejected;
            rejected.kind = EvidenceKind::Capability;
            rejected.site = capability.site;
            rejected.offered_generation = capability.generation;
            rejected.retained_generation = current.generation;
            rejected.reason = ErrorCode::Stale;
            rejected.observed_at = capability.observed_at;
            rejected.detail = "capability evidence is older than the retained generation";
            const Status recorded = impl.append_and_apply(
                journal::RecordType::RejectedEvidence, codec::encode_rejected_evidence(rejected));
            if (!recorded.ok()) {
                return recorded;
            }
            const Status committed = impl.commit("rejected evidence");
            if (!committed.ok()) {
                return committed;
            }
            lock.unlock();
            (void)impl.publish_notices();
            return Status{ErrorCode::Stale, "capability evidence is older than the retained one"};
        }
        if (capability.generation == current.generation &&
            impl.state.policy.conflict_on_equal_generation_evidence) {
            RejectedEvidence rejected;
            rejected.kind = EvidenceKind::Capability;
            rejected.site = capability.site;
            rejected.offered_generation = capability.generation;
            rejected.retained_generation = current.generation;
            rejected.reason = ErrorCode::Conflict;
            rejected.observed_at = capability.observed_at;
            rejected.detail = "two capability observations claim the same generation";
            const Status recorded = impl.append_and_apply(
                journal::RecordType::RejectedEvidence, codec::encode_rejected_evidence(rejected));
            if (!recorded.ok()) {
                return recorded;
            }
            for (auto& entry : impl.state.events) {
                DisasterEvent& event = entry.second;
                if (event.disposition != EventDisposition::Active ||
                    event.phase == EventPhase::Closed) {
                    continue;
                }
                if (!contains_site(event.affected_sites, capability.site) &&
                    !contains_site(event.conflicting_sites, capability.site)) {
                    continue;
                }
                if (event.phase != EventPhase::Conflicted) {
                    event.phase_before_interruption = event.phase;
                    event.phase = EventPhase::Conflicted;
                }
                if (!contains_site(event.conflicting_sites, capability.site)) {
                    event.conflicting_sites.push_back(capability.site);
                    std::sort(event.conflicting_sites.begin(), event.conflicting_sites.end());
                }
                event.status_detail = "contradictory capability evidence for site " +
                                      capability.site.to_string();
                event.updated_at = impl.now();
                event.updated_epoch = impl.state.epoch;
                const Status transitioned = impl.append_and_apply(
                    journal::RecordType::EventTransition, codec::encode_event(event));
                if (!transitioned.ok()) {
                    return transitioned;
                }
            }
            const Status committed = impl.commit("capability conflict");
            if (!committed.ok()) {
                return committed;
            }
            lock.unlock();
            (void)impl.publish_notices();
            return Status{ErrorCode::Conflict,
                          "capability evidence contradicts the observation with the same generation"};
        }
    }
    status = impl.append_and_apply(journal::RecordType::Capability,
                                   codec::encode_capability(capability));
    if (!status.ok()) {
        return status;
    }
    // A newer observation for a contested site is what clears the conflict.
    for (auto& entry : impl.state.events) {
        DisasterEvent& event = entry.second;
        if (event.phase != EventPhase::Conflicted) {
            continue;
        }
        if (!contains_site(event.conflicting_sites, capability.site)) {
            continue;
        }
        event.conflicting_sites.erase(
            std::remove(event.conflicting_sites.begin(), event.conflicting_sites.end(),
                        capability.site),
            event.conflicting_sites.end());
        if (event.conflicting_sites.empty()) {
            event.phase = event.phase_before_interruption;
            event.status_detail = "conflict cleared by newer capability evidence";
        }
        event.updated_at = impl.now();
        event.updated_epoch = impl.state.epoch;
        const Status transitioned =
            impl.append_and_apply(journal::RecordType::EventTransition, codec::encode_event(event));
        if (!transitioned.ok()) {
            return transitioned;
        }
    }
    status = impl.commit("capability");
    if (!status.ok()) {
        return status;
    }
    lock.unlock();
    return impl.publish_notices();
}

Status Coordinator::record_readiness(SiteReadinessEvidence readiness) {
    if (impl_ == nullptr) {
        return Status{ErrorCode::Invalid, "coordinator is not open"};
    }
    Impl& impl = *impl_;
    std::unique_lock<std::shared_mutex> lock(impl.mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        impl.busy_rejections.fetch_add(1);
        return Status{ErrorCode::Busy, "another coordinator call is in progress"};
    }
    internal::LockOrderGuard order(internal::LockLevel::Engine, &impl.lock_order_violations);
    impl.stats.api_calls += 1;
    if (impl.shutting_down) {
        return Status{ErrorCode::Shutdown, "coordinator is shutting down"};
    }
    Status status = require_id(readiness.site.valid(), "readiness site");
    if (!status.ok()) {
        return status;
    }
    status = require_text(readiness.source, "readiness source", true);
    if (!status.ok()) {
        return status;
    }
    if (impl.state.sites.find(readiness.site) == impl.state.sites.end()) {
        return Status{ErrorCode::NotFound, "readiness names an unregistered site"};
    }
    if (!readiness.generation.valid()) {
        return Status{ErrorCode::Invalid, "readiness generation is zero"};
    }
    if (readiness.checks.size() > impl.state.limits.max_readiness_checks) {
        return Status{ErrorCode::Exhausted, "too many readiness checks"};
    }
    for (std::size_t i = 0; i < readiness.checks.size(); ++i) {
        status = require_text(readiness.checks[i].name, "readiness check name", false);
        if (!status.ok()) {
            return status;
        }
        for (std::size_t j = i + 1; j < readiness.checks.size(); ++j) {
            if (readiness.checks[i].name == readiness.checks[j].name) {
                return Status{ErrorCode::Duplicate, "readiness reports the same check twice"};
            }
        }
    }

    const auto existing = impl.state.readiness.find(readiness.site);
    if (existing != impl.state.readiness.end()) {
        const SiteReadinessEvidence& current = existing->second;
        const bool identical = current.generation == readiness.generation &&
                               current.observed_at == readiness.observed_at &&
                               readiness_passes_checks(current, {}) ==
                                   readiness_passes_checks(readiness, {});
        if (identical) {
            bool same_checks = current.checks.size() == readiness.checks.size();
            if (same_checks) {
                for (std::size_t i = 0; i < current.checks.size(); ++i) {
                    if (current.checks[i].name != readiness.checks[i].name ||
                        current.checks[i].passed != readiness.checks[i].passed) {
                        same_checks = false;
                        break;
                    }
                }
            }
            if (same_checks) {
                return ok_status();
            }
        }
        if (readiness.generation < current.generation) {
            RejectedEvidence rejected;
            rejected.kind = EvidenceKind::Readiness;
            rejected.site = readiness.site;
            rejected.offered_generation = readiness.generation;
            rejected.retained_generation = current.generation;
            rejected.reason = ErrorCode::Stale;
            rejected.observed_at = readiness.observed_at;
            rejected.detail = "readiness evidence is older than the retained generation";
            const Status recorded = impl.append_and_apply(
                journal::RecordType::RejectedEvidence, codec::encode_rejected_evidence(rejected));
            if (!recorded.ok()) {
                return recorded;
            }
            const Status committed = impl.commit("rejected evidence");
            if (!committed.ok()) {
                return committed;
            }
            lock.unlock();
            (void)impl.publish_notices();
            return Status{ErrorCode::Stale, "readiness evidence is older than the retained one"};
        }
        if (readiness.generation == current.generation &&
            impl.state.policy.conflict_on_equal_generation_evidence) {
            RejectedEvidence rejected;
            rejected.kind = EvidenceKind::Readiness;
            rejected.site = readiness.site;
            rejected.offered_generation = readiness.generation;
            rejected.retained_generation = current.generation;
            rejected.reason = ErrorCode::Conflict;
            rejected.observed_at = readiness.observed_at;
            rejected.detail = "two readiness observations claim the same generation";
            const Status recorded = impl.append_and_apply(
                journal::RecordType::RejectedEvidence, codec::encode_rejected_evidence(rejected));
            if (!recorded.ok()) {
                return recorded;
            }
            for (auto& entry : impl.state.events) {
                DisasterEvent& event = entry.second;
                if (event.disposition != EventDisposition::Active ||
                    event.phase == EventPhase::Closed) {
                    continue;
                }
                if (!contains_site(event.affected_sites, readiness.site) &&
                    !contains_site(event.conflicting_sites, readiness.site)) {
                    continue;
                }
                if (event.phase != EventPhase::Conflicted) {
                    event.phase_before_interruption = event.phase;
                    event.phase = EventPhase::Conflicted;
                }
                if (!contains_site(event.conflicting_sites, readiness.site)) {
                    event.conflicting_sites.push_back(readiness.site);
                    std::sort(event.conflicting_sites.begin(), event.conflicting_sites.end());
                }
                event.status_detail =
                    "contradictory readiness evidence for site " + readiness.site.to_string();
                event.updated_at = impl.now();
                event.updated_epoch = impl.state.epoch;
                const Status transitioned = impl.append_and_apply(
                    journal::RecordType::EventTransition, codec::encode_event(event));
                if (!transitioned.ok()) {
                    return transitioned;
                }
            }
            const Status committed = impl.commit("readiness conflict");
            if (!committed.ok()) {
                return committed;
            }
            lock.unlock();
            (void)impl.publish_notices();
            return Status{ErrorCode::Conflict,
                          "readiness evidence contradicts the observation with the same generation"};
        }
    }

    status = impl.append_and_apply(journal::RecordType::Readiness,
                                   codec::encode_readiness(readiness));
    if (!status.ok()) {
        return status;
    }
    for (auto& entry : impl.state.events) {
        DisasterEvent& event = entry.second;
        if (event.phase != EventPhase::Conflicted) {
            continue;
        }
        if (!contains_site(event.conflicting_sites, readiness.site)) {
            continue;
        }
        event.conflicting_sites.erase(
            std::remove(event.conflicting_sites.begin(), event.conflicting_sites.end(),
                        readiness.site),
            event.conflicting_sites.end());
        if (event.conflicting_sites.empty()) {
            event.phase = event.phase_before_interruption;
            event.status_detail = "conflict cleared by newer readiness evidence";
        }
        event.updated_at = impl.now();
        event.updated_epoch = impl.state.epoch;
        const Status transitioned =
            impl.append_and_apply(journal::RecordType::EventTransition, codec::encode_event(event));
        if (!transitioned.ok()) {
            return transitioned;
        }
    }
    status = impl.commit("readiness");
    if (!status.ok()) {
        return status;
    }
    lock.unlock();
    return impl.publish_notices();
}

Status Coordinator::record_federation_state(FederationState federation) {
    if (impl_ == nullptr) {
        return Status{ErrorCode::Invalid, "coordinator is not open"};
    }
    Impl& impl = *impl_;
    std::unique_lock<std::shared_mutex> lock(impl.mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        impl.busy_rejections.fetch_add(1);
        return Status{ErrorCode::Busy, "another coordinator call is in progress"};
    }
    internal::LockOrderGuard order(internal::LockLevel::Engine, &impl.lock_order_violations);
    impl.stats.api_calls += 1;
    if (impl.shutting_down) {
        return Status{ErrorCode::Shutdown, "coordinator is shutting down"};
    }
    Status status = require_text(federation.source, "federation source", true);
    if (!status.ok()) {
        return status;
    }
    if (!federation.generation.valid()) {
        return Status{ErrorCode::Invalid, "federation generation is zero"};
    }
    if (federation.unreachable_domains.size() > impl.state.limits.max_sites) {
        return Status{ErrorCode::Exhausted, "too many unreachable domains"};
    }
    for (const FailureDomainId domain : federation.unreachable_domains) {
        if (impl.state.domains.find(domain) == impl.state.domains.end()) {
            return Status{ErrorCode::NotFound,
                          "federation state names an unregistered failure domain"};
        }
    }
    const FederationState& current = impl.state.federation;
    if (current.known && federation.generation < current.generation) {
        return Status{ErrorCode::Stale, "federation state is older than the retained one"};
    }
    if (federation.generation == current.generation && current.known &&
        federation.known == current.known && federation.partitioned == current.partitioned &&
        federation.unreachable_domains == current.unreachable_domains) {
        return ok_status();
    }
    if (current.known && federation.generation == current.generation &&
        impl.state.policy.conflict_on_equal_generation_evidence &&
        (federation.partitioned != current.partitioned ||
         federation.unreachable_domains != current.unreachable_domains)) {
        return Status{ErrorCode::Conflict,
                      "federation state contradicts the observation with the same generation"};
    }
    status = impl.append_and_apply(journal::RecordType::Federation,
                                   codec::encode_federation(federation));
    if (!status.ok()) {
        return status;
    }
    status = impl.commit("federation");
    if (!status.ok()) {
        return status;
    }
    lock.unlock();
    return impl.publish_notices();
}

Status Coordinator::register_endpoint(EndpointDescriptor descriptor) {
    if (impl_ == nullptr) {
        return Status{ErrorCode::Invalid, "coordinator is not open"};
    }
    Impl& impl = *impl_;
    std::unique_lock<std::shared_mutex> lock(impl.mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        impl.busy_rejections.fetch_add(1);
        return Status{ErrorCode::Busy, "another coordinator call is in progress"};
    }
    internal::LockOrderGuard order(internal::LockLevel::Engine, &impl.lock_order_violations);
    impl.stats.api_calls += 1;
    if (impl.shutting_down) {
        return Status{ErrorCode::Shutdown, "coordinator is shutting down"};
    }
    Status status = require_id(descriptor.id.valid(), "endpoint");
    if (!status.ok()) {
        return status;
    }
    status = require_text(descriptor.name, "endpoint name", false);
    if (!status.ok()) {
        return status;
    }
    status = require_text(descriptor.working_directory, "endpoint working directory", true);
    if (!status.ok()) {
        return status;
    }
    if (descriptor.command.size() > 64) {
        return Status{ErrorCode::Exhausted, "endpoint command has too many arguments"};
    }
    for (const std::string& argument : descriptor.command) {
        status = require_text(argument, "endpoint argument", true);
        if (!status.ok()) {
            return status;
        }
    }
    if (!is_declared_step_domain(descriptor.domain)) {
        return Status{ErrorCode::Unsupported, "endpoint domain is not supported"};
    }
    if (impl.state.endpoints.size() >= impl.state.limits.max_endpoints &&
        impl.state.endpoints.find(descriptor.domain) == impl.state.endpoints.end()) {
        return Status{ErrorCode::Exhausted, "endpoint limit reached"};
    }
    const auto existing = impl.state.endpoints.find(descriptor.domain);
    if (existing != impl.state.endpoints.end()) {
        const EndpointDescriptor& current = existing->second;
        if (current.id == descriptor.id && current.command == descriptor.command &&
            current.working_directory == descriptor.working_directory && current.name ==
                descriptor.name) {
            return ok_status();
        }
    }
    status = impl.append_and_apply(journal::RecordType::Endpoint,
                                   encode_endpoint_descriptor(descriptor));
    if (!status.ok()) {
        return status;
    }
    const auto running = impl.endpoints.find(descriptor.domain);
    if (running != impl.endpoints.end()) {
        (void)running->second->shutdown();
        impl.endpoints.erase(running);
    }
    status = impl.commit("endpoint");
    if (!status.ok()) {
        return status;
    }
    lock.unlock();
    return impl.publish_notices();
}

// ---------------------------------------------------------------------------
// Incident lifecycle
// ---------------------------------------------------------------------------

Result<DisasterEventId> Coordinator::declare_disaster(const DisasterDeclaration& declaration) {
    if (impl_ == nullptr) {
        return Status{ErrorCode::Invalid, "coordinator is not open"};
    }
    Impl& impl = *impl_;
    std::unique_lock<std::shared_mutex> lock(impl.mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        impl.busy_rejections.fetch_add(1);
        return Status{ErrorCode::Busy, "another coordinator call is in progress"};
    }
    internal::LockOrderGuard order(internal::LockLevel::Engine, &impl.lock_order_violations);
    impl.stats.api_calls += 1;
    if (impl.shutting_down) {
        return Status{ErrorCode::Shutdown, "coordinator is shutting down"};
    }
    Status status = require_text(declaration.declared_by, "declared_by", false);
    if (!status.ok()) {
        return status;
    }
    status = require_text(declaration.reason, "reason", false);
    if (!status.ok()) {
        return status;
    }
    if (declaration.affected_sites.empty() && declaration.affected_domains.empty()) {
        return Status{ErrorCode::Invalid,
                      "a disaster declaration must name an affected site or failure domain"};
    }
    if (declaration.affected_sites.size() > impl.state.limits.max_sites ||
        declaration.affected_domains.size() > impl.state.limits.max_sites) {
        return Status{ErrorCode::Exhausted, "declaration names too many sites or domains"};
    }
    for (const SiteId site : declaration.affected_sites) {
        if (impl.state.sites.find(site) == impl.state.sites.end()) {
            return Status{ErrorCode::NotFound, "declaration names an unregistered site"};
        }
    }
    for (const FailureDomainId domain : declaration.affected_domains) {
        if (impl.state.domains.find(domain) == impl.state.domains.end()) {
            return Status{ErrorCode::NotFound, "declaration names an unregistered failure domain"};
        }
    }
    if (impl.state.events.size() >= impl.state.limits.max_events) {
        return Status{ErrorCode::Exhausted, "disaster event limit reached"};
    }

    std::vector<DisasterEventId> overlapping;
    for (const auto& entry : impl.state.events) {
        if (!is_active_event(entry.second)) {
            continue;
        }
        bool overlap = false;
        for (const SiteId site : declaration.affected_sites) {
            if (contains_site(entry.second.affected_sites, site)) {
                overlap = true;
                break;
            }
        }
        for (const FailureDomainId domain : declaration.affected_domains) {
            if (std::find(entry.second.affected_domains.begin(),
                          entry.second.affected_domains.end(),
                          domain) != entry.second.affected_domains.end()) {
                overlap = true;
                break;
            }
        }
        if (overlap) {
            overlapping.push_back(entry.first);
        }
    }

    DisasterEvent superseded_event;
    bool have_superseded = false;
    if (declaration.supersedes.valid()) {
        const auto found = impl.state.events.find(declaration.supersedes);
        if (found == impl.state.events.end()) {
            return Status{ErrorCode::NotFound, "superseded event does not exist"};
        }
        if (!is_active_event(found->second)) {
            return Status{ErrorCode::Conflict, "superseded event is no longer active"};
        }
        superseded_event = found->second;
        have_superseded = true;
        overlapping.erase(std::remove(overlapping.begin(), overlapping.end(),
                                      declaration.supersedes),
                          overlapping.end());
    }

    DisasterEventId id{impl.state.next_event_id};
    if (!id.valid()) {
        return Status{ErrorCode::Exhausted, "disaster event identity space is exhausted"};
    }
    DisasterEvent event;
    event.id = id;
    event.generation = Generation{1};
    event.phase = overlapping.empty() ? EventPhase::Declared : EventPhase::Conflicted;
    event.phase_before_interruption = EventPhase::Declared;
    event.disposition = EventDisposition::Active;
    event.severity = declaration.severity;
    event.declared_by = declaration.declared_by;
    event.reason = declaration.reason;
    event.declared_at = impl.now();
    event.declared_epoch = impl.state.epoch;
    event.updated_at = event.declared_at;
    event.updated_epoch = impl.state.epoch;
    event.affected_sites = declaration.affected_sites;
    std::sort(event.affected_sites.begin(), event.affected_sites.end());
    event.affected_domains = declaration.affected_domains;
    std::sort(event.affected_domains.begin(), event.affected_domains.end());
    event.supersedes = declaration.supersedes;
    event.conflicts_with = overlapping;
    if (overlapping.empty()) {
        event.status_detail = "declared";
    } else {
        event.conflicting_sites = event.affected_sites;
        event.status_detail = "declared while another active event covers the same sites";
    }
    for (const auto& entry : impl.state.obligations) {
        const ProtectedObligation& obligation = entry.second;
        if (contains_site(event.affected_sites, obligation.home_site) ||
            std::find(event.affected_domains.begin(), event.affected_domains.end(),
                      obligation.home_domain) != event.affected_domains.end()) {
            event.obligations_in_scope.push_back(obligation.id);
        }
    }
    std::sort(event.obligations_in_scope.begin(), event.obligations_in_scope.end());

    status = impl.append_and_apply(journal::RecordType::EventDeclaration,
                                   codec::encode_event(event));
    if (!status.ok()) {
        return status;
    }

    if (have_superseded) {
        // A superseding incident fences the event it replaces: its plan
        // generation stops executing and its unfinished steps are cancelled,
        // never silently dropped.
        const RecoveryPlanId previous_plan = superseded_event.active_plan;
        if (previous_plan.valid()) {
            const auto previous = impl.state.plans.find(previous_plan);
            if (previous != impl.state.plans.end() && previous->second.state == PlanState::Active) {
                RecoveryPlan closed = previous->second;
                closed.state = PlanState::Abandoned;
                const Status recorded =
                    impl.append_and_apply(journal::RecordType::Plan, encode_plan(closed));
                if (!recorded.ok()) {
                    return recorded;
                }
                for (const RecoveryStepId step_id : closed.order) {
                    const RecoveryStep& step = closed.steps.at(step_id);
                    if (is_terminal_step_state(step.state)) {
                        continue;
                    }
                    const Status cancelled = impl.transition_step(
                        closed.id, step_id, StepState::Cancelled,
                        "the event was superseded by a newer declaration");
                    if (!cancelled.ok()) {
                        return cancelled;
                    }
                }
            }
        }
        DisasterEvent replaced = superseded_event;
        replaced.disposition = EventDisposition::Superseded;
        replaced.phase = EventPhase::Closed;
        replaced.status_detail = "superseded by event " + id.to_string();
        replaced.updated_at = impl.now();
        replaced.updated_epoch = impl.state.epoch;
        const Status transitioned = impl.append_and_apply(journal::RecordType::EventTransition,
                                                          codec::encode_event(replaced));
        if (!transitioned.ok()) {
            return transitioned;
        }
    }

    status = impl.commit("disaster declared");
    if (!status.ok()) {
        return status;
    }
    lock.unlock();
    const Status published = impl.publish_notices();
    if (!published.ok()) {
        return published;
    }
    return id;
}

Status Coordinator::record_assessment(const AssessmentEvidence& assessment) {
    if (impl_ == nullptr) {
        return Status{ErrorCode::Invalid, "coordinator is not open"};
    }
    Impl& impl = *impl_;
    std::unique_lock<std::shared_mutex> lock(impl.mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        impl.busy_rejections.fetch_add(1);
        return Status{ErrorCode::Busy, "another coordinator call is in progress"};
    }
    internal::LockOrderGuard order(internal::LockLevel::Engine, &impl.lock_order_violations);
    impl.stats.api_calls += 1;
    if (impl.shutting_down) {
        return Status{ErrorCode::Shutdown, "coordinator is shutting down"};
    }
    Status status = require_text(assessment.source, "assessment source", false);
    if (!status.ok()) {
        return status;
    }
    const auto found = impl.state.events.find(assessment.event);
    if (found == impl.state.events.end()) {
        return Status{ErrorCode::NotFound, "assessment names an unknown event"};
    }
    if (found->second.disposition != EventDisposition::Active) {
        return Status{ErrorCode::Conflict, "assessment names an event that is no longer active"};
    }
    if (!assessment.generation.valid()) {
        return Status{ErrorCode::Invalid, "assessment generation is zero"};
    }
    if (assessment.event_generation != found->second.generation) {
        return Status{ErrorCode::Stale, "assessment names an older event generation"};
    }
    const auto existing = impl.state.assessments.find(assessment.event);
    if (existing != impl.state.assessments.end() &&
        assessment.generation < existing->second.generation) {
        return Status{ErrorCode::Stale, "assessment is older than the retained one"};
    }
    if (assessment.sites.size() > impl.state.limits.max_sites) {
        return Status{ErrorCode::Exhausted, "assessment covers too many sites"};
    }
    for (const SiteAssessment& site : assessment.sites) {
        if (impl.state.sites.find(site.site) == impl.state.sites.end()) {
            return Status{ErrorCode::NotFound, "assessment names an unregistered site"};
        }
        status = require_text(site.note, "assessment note", true);
        if (!status.ok()) {
            return status;
        }
    }
    status = impl.append_and_apply(journal::RecordType::EventAssessment,
                                   codec::encode_assessment(assessment));
    if (!status.ok()) {
        return status;
    }
    DisasterEvent event = found->second;
    if (event.phase == EventPhase::Conflicted) {
        // A newer assessment is the explicit way out of an evidence conflict.
        event.conflicting_sites.clear();
        event.phase = event.phase_before_interruption;
        event.status_detail = "conflict cleared by a newer assessment";
    }
    if (event.phase == EventPhase::Declared) {
        event.phase = EventPhase::Assessed;
        event.status_detail = "assessed";
    }
    event.updated_at = impl.now();
    event.updated_epoch = impl.state.epoch;
    status = impl.append_and_apply(journal::RecordType::EventTransition,
                                   codec::encode_event(event));
    if (!status.ok()) {
        return status;
    }
    status = impl.commit("assessment");
    if (!status.ok()) {
        return status;
    }
    lock.unlock();
    return impl.publish_notices();
}

Result<RecoveryPlanId> Coordinator::create_plan(const CreatePlanRequest& request) {
    if (impl_ == nullptr) {
        return Status{ErrorCode::Invalid, "coordinator is not open"};
    }
    Impl& impl = *impl_;
    std::unique_lock<std::shared_mutex> lock(impl.mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        impl.busy_rejections.fetch_add(1);
        return Status{ErrorCode::Busy, "another coordinator call is in progress"};
    }
    internal::LockOrderGuard order(internal::LockLevel::Engine, &impl.lock_order_violations);
    impl.stats.api_calls += 1;
    if (impl.shutting_down) {
        return Status{ErrorCode::Shutdown, "coordinator is shutting down"};
    }
    Status status = require_text(request.requested_by, "requested_by", false);
    if (!status.ok()) {
        return status;
    }
    const auto found = impl.state.events.find(request.event);
    if (found == impl.state.events.end()) {
        return Status{ErrorCode::NotFound, "plan request names an unknown event"};
    }
    if (found->second.disposition != EventDisposition::Active) {
        return Status{ErrorCode::Conflict, "plan request names an event that is no longer active"};
    }
    if (found->second.phase == EventPhase::Conflicted) {
        return Status{ErrorCode::Conflict,
                      "the event has contradictory evidence; resolve it before planning"};
    }
    if (found->second.phase == EventPhase::Closed) {
        return Status{ErrorCode::Conflict, "the event is closed"};
    }
    const auto plans = impl.state.plan_count.find(request.event);
    if (plans != impl.state.plan_count.end() &&
        plans->second >= impl.state.limits.max_plans_per_event) {
        return Status{ErrorCode::Exhausted, "event has reached its plan limit"};
    }
    const Generation generation = impl.next_generation_for(request.event);
    Result<RecoveryPlanId> built =
        impl.build_plan_for(request.event, generation, PlanKind::Recovery, request.requested_by,
                            std::vector<SiteId>{}, request.allow_partial_placement);
    if (!built.ok()) {
        lock.unlock();
        (void)impl.publish_notices();
        return built.status();
    }
    lock.unlock();
    const Status published = impl.publish_notices();
    if (!published.ok()) {
        return published;
    }
    return built.value();
}

Status Coordinator::begin_recovery(DisasterEventId event_id) {
    if (impl_ == nullptr) {
        return Status{ErrorCode::Invalid, "coordinator is not open"};
    }
    Impl& impl = *impl_;
    std::unique_lock<std::shared_mutex> lock(impl.mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        impl.busy_rejections.fetch_add(1);
        return Status{ErrorCode::Busy, "another coordinator call is in progress"};
    }
    internal::LockOrderGuard order(internal::LockLevel::Engine, &impl.lock_order_violations);
    impl.stats.api_calls += 1;
    if (impl.shutting_down) {
        return Status{ErrorCode::Shutdown, "coordinator is shutting down"};
    }
    const auto found = impl.state.events.find(event_id);
    if (found == impl.state.events.end()) {
        return Status{ErrorCode::NotFound, "event does not exist"};
    }
    const DisasterEvent& event = found->second;
    if (event.disposition != EventDisposition::Active) {
        return Status{ErrorCode::Conflict, "event is no longer active"};
    }
    if (event.phase == EventPhase::Evacuating || event.phase == EventPhase::FailingOver) {
        return ok_status();
    }
    if (event.phase == EventPhase::Conflicted) {
        return Status{ErrorCode::Conflict, "event has contradictory evidence"};
    }
    if (event.phase == EventPhase::Blocked) {
        return Status{ErrorCode::NotReady, "event is blocked; resolve the blocking step first"};
    }
    if (event.phase != EventPhase::PlanReady && event.phase != EventPhase::Declared &&
        event.phase != EventPhase::Assessed) {
        return Status{ErrorCode::PolicyViolation, "recovery cannot start from this phase"};
    }
    if (!event.active_plan.valid()) {
        return Status{ErrorCode::NotReady, "event has no active plan"};
    }
    const auto plan = impl.state.plans.find(event.active_plan);
    if (plan == impl.state.plans.end() || plan->second.state != PlanState::Active) {
        return Status{ErrorCode::NotReady, "event has no active plan generation"};
    }
    bool has_evacuation = false;
    for (const RecoveryStepId step_id : plan->second.order) {
        if (plan->second.steps.at(step_id).kind == StepKind::EvacuateSource) {
            has_evacuation = true;
            break;
        }
    }
    const Status status = impl.transition_event(
        event_id, has_evacuation ? EventPhase::Evacuating : EventPhase::FailingOver,
        "recovery started");
    if (!status.ok()) {
        return status;
    }
    const Status committed = impl.commit("recovery started");
    if (!committed.ok()) {
        return committed;
    }
    lock.unlock();
    return impl.publish_notices();
}

Status Coordinator::begin_restoration(const RestorationRequest& request) {
    if (impl_ == nullptr) {
        return Status{ErrorCode::Invalid, "coordinator is not open"};
    }
    Impl& impl = *impl_;
    std::unique_lock<std::shared_mutex> lock(impl.mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        impl.busy_rejections.fetch_add(1);
        return Status{ErrorCode::Busy, "another coordinator call is in progress"};
    }
    internal::LockOrderGuard order(internal::LockLevel::Engine, &impl.lock_order_violations);
    impl.stats.api_calls += 1;
    if (impl.shutting_down) {
        return Status{ErrorCode::Shutdown, "coordinator is shutting down"};
    }
    Status status = require_text(request.requested_by, "requested_by", false);
    if (!status.ok()) {
        return status;
    }
    const auto found = impl.state.events.find(request.event);
    if (found == impl.state.events.end()) {
        return Status{ErrorCode::NotFound, "event does not exist"};
    }
    if (found->second.disposition != EventDisposition::Active) {
        return Status{ErrorCode::Conflict, "event is no longer active"};
    }
    if (found->second.phase != EventPhase::Stabilized) {
        return Status{ErrorCode::NotReady,
                      "restoration requires the event to be stabilized first"};
    }
    const Generation generation = impl.next_generation_for(request.event);
    std::vector<SiteId> targets = found->second.affected_sites;
    Result<RecoveryPlanId> built =
        impl.build_plan_for(request.event, generation, PlanKind::Restoration, request.requested_by,
                            targets, false);
    if (!built.ok()) {
        lock.unlock();
        (void)impl.publish_notices();
        return built.status();
    }
    status = impl.transition_event(request.event, EventPhase::Restoring, "restoration started");
    if (!status.ok()) {
        return status;
    }
    status = impl.commit("restoration");
    if (!status.ok()) {
        return status;
    }
    lock.unlock();
    return impl.publish_notices();
}

Result<RecoveryPlanId> Coordinator::authorize_failback(const FailbackRequest& request) {
    if (impl_ == nullptr) {
        return Status{ErrorCode::Invalid, "coordinator is not open"};
    }
    Impl& impl = *impl_;
    std::unique_lock<std::shared_mutex> lock(impl.mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        impl.busy_rejections.fetch_add(1);
        return Status{ErrorCode::Busy, "another coordinator call is in progress"};
    }
    internal::LockOrderGuard order(internal::LockLevel::Engine, &impl.lock_order_violations);
    impl.stats.api_calls += 1;
    if (impl.shutting_down) {
        return Status{ErrorCode::Shutdown, "coordinator is shutting down"};
    }
    Status status = require_text(request.authorized_by, "authorized_by", false);
    if (!status.ok()) {
        return status;
    }
    status = require_text(request.justification, "justification", false);
    if (!status.ok()) {
        return status;
    }
    const auto found = impl.state.events.find(request.event);
    if (found == impl.state.events.end()) {
        return Status{ErrorCode::NotFound, "event does not exist"};
    }
    const DisasterEvent& event = found->second;
    if (event.disposition != EventDisposition::Active) {
        return Status{ErrorCode::Conflict, "event is no longer active"};
    }
    if (event.phase != EventPhase::Restoring && event.phase != EventPhase::Validating &&
        event.phase != EventPhase::Returning) {
        return Status{ErrorCode::NotReady,
                      "failback is a separate decision and requires a restoring or returning event"};
    }
    if (!contains_site(event.affected_sites, request.target_site)) {
        return Status{ErrorCode::Invalid, "failback target is not an affected site of this event"};
    }
    if (!contains_site(event.returned_sites, request.target_site)) {
        return Status{ErrorCode::GateUnmet,
                      "the site has not been returned to service, so it cannot take work back"};
    }
    Result<Freshness> freshness = impl.readiness_freshness_for(request.target_site);
    if (!freshness.ok()) {
        return freshness.status();
    }
    if (!is_fresh(freshness.value())) {
        std::string message = "failback requires fresh readiness evidence; the observation is ";
        message.append(to_string(freshness.value()));
        return Status{ErrorCode::GateUnmet, std::move(message)};
    }
    const auto evidence = impl.state.readiness.find(request.target_site);
    if (evidence == impl.state.readiness.end() ||
        !readiness_passes_checks(evidence->second, impl.state.policy.required_readiness_checks)) {
        return Status{ErrorCode::GateUnmet, "failback requires a passing readiness observation"};
    }

    const Generation generation = impl.next_generation_for(request.event);
    const std::vector<SiteId> targets{request.target_site};
    Result<RecoveryPlanId> built =
        impl.build_plan_for(request.event, generation, PlanKind::Failback, request.authorized_by,
                            targets, false);
    if (!built.ok()) {
        lock.unlock();
        (void)impl.publish_notices();
        return built.status();
    }
    codec::FailbackRecord record;
    record.event = request.event;
    record.plan = built.value();
    record.generation = generation;
    record.target_site = request.target_site;
    record.epoch = impl.state.epoch;
    record.at = impl.now();
    record.authorized_by = request.authorized_by;
    record.justification = request.justification;
    status = impl.append_and_apply(journal::RecordType::FailbackAuthorization,
                                   codec::encode_failback_record(record));
    if (!status.ok()) {
        return status;
    }
    const DisasterEvent& current = impl.state.events.at(request.event);
    DisasterEvent updated = current;
    updated.status_detail = "failback authorized for site " + request.target_site.to_string();
    updated.updated_at = impl.now();
    updated.updated_epoch = impl.state.epoch;
    status = impl.append_and_apply(journal::RecordType::EventTransition,
                                   codec::encode_event(updated));
    if (!status.ok()) {
        return status;
    }
    status = impl.commit("failback authorized");
    if (!status.ok()) {
        return status;
    }
    const RecoveryPlanId plan_id = built.value();
    lock.unlock();
    const Status published = impl.publish_notices();
    if (!published.ok()) {
        return published;
    }
    return plan_id;
}

Status Coordinator::return_site_to_service(const ReturnToServiceRequest& request) {
    if (impl_ == nullptr) {
        return Status{ErrorCode::Invalid, "coordinator is not open"};
    }
    Impl& impl = *impl_;
    std::unique_lock<std::shared_mutex> lock(impl.mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        impl.busy_rejections.fetch_add(1);
        return Status{ErrorCode::Busy, "another coordinator call is in progress"};
    }
    internal::LockOrderGuard order(internal::LockLevel::Engine, &impl.lock_order_violations);
    impl.stats.api_calls += 1;
    if (impl.shutting_down) {
        return Status{ErrorCode::Shutdown, "coordinator is shutting down"};
    }
    Status status = require_text(request.authorized_by, "authorized_by", false);
    if (!status.ok()) {
        return status;
    }
    status = require_text(request.justification, "justification", true);
    if (!status.ok()) {
        return status;
    }
    const auto found = impl.state.events.find(request.event);
    if (found == impl.state.events.end()) {
        return Status{ErrorCode::NotFound, "event does not exist"};
    }
    const DisasterEvent& event = found->second;
    if (event.disposition != EventDisposition::Active) {
        return Status{ErrorCode::Conflict, "event is no longer active"};
    }
    if (event.phase != EventPhase::Validating && event.phase != EventPhase::Returning) {
        return Status{ErrorCode::NotReady,
                      "return to service requires an event that is validating or returning"};
    }
    if (!contains_site(event.affected_sites, request.site)) {
        return Status{ErrorCode::Invalid, "site is not affected by this event"};
    }
    Result<Freshness> freshness = impl.readiness_freshness_for(request.site);
    if (!freshness.ok()) {
        return freshness.status();
    }
    if (!is_fresh(freshness.value())) {
        std::string message = "return to service requires fresh readiness evidence; the observation is ";
        message.append(to_string(freshness.value()));
        return Status{ErrorCode::GateUnmet, std::move(message)};
    }
    const auto evidence = impl.state.readiness.find(request.site);
    if (evidence == impl.state.readiness.end()) {
        return Status{ErrorCode::GateUnmet, "no readiness observation exists for this site"};
    }
    if (!readiness_passes_checks(evidence->second, impl.state.policy.required_readiness_checks)) {
        return Status{ErrorCode::GateUnmet,
                      "readiness observation does not satisfy the required checks"};
    }
    if (!event.active_plan.valid()) {
        return Status{ErrorCode::NotReady, "event has no active plan"};
    }
    const auto plan = impl.state.plans.find(event.active_plan);
    if (plan == impl.state.plans.end()) {
        return Status{ErrorCode::NotReady, "event has no active plan"};
    }
    RecoveryStepId target_step;
    for (const RecoveryStepId step_id : plan->second.order) {
        const RecoveryStep& step = plan->second.steps.at(step_id);
        if (step.kind == StepKind::ReturnToService && step.destination == request.site) {
            target_step = step_id;
            break;
        }
    }
    if (!target_step.valid()) {
        return Status{ErrorCode::NotFound,
                      "the active plan has no return-to-service step for this site"};
    }
    const RecoveryStep& step = plan->second.steps.at(target_step);
    if (step.state == StepState::Succeeded) {
        return ok_status();
    }
    if (is_terminal_step_state(step.state)) {
        return Status{ErrorCode::PolicyViolation,
                      "the return-to-service step for this site has already terminated"};
    }
    for (const RecoveryStepId dependency : step.depends_on) {
        const auto found_step = plan->second.steps.find(dependency);
        if (found_step == plan->second.steps.end()) {
            return Status{ErrorCode::NotFound, "return-to-service step has an unknown dependency"};
        }
        if (found_step->second.state != StepState::Succeeded) {
            return Status{ErrorCode::DependencyUnmet,
                          "readiness validation for this site has not succeeded"};
        }
    }
    status = impl.transition_step(plan->second.id, target_step, StepState::Ready,
                                  "return authorized by " + request.authorized_by, true);
    if (!status.ok()) {
        return status;
    }
    if (event.phase == EventPhase::Validating) {
        status = impl.transition_event(request.event, EventPhase::Returning,
                                       "returning sites to service");
        if (!status.ok()) {
            return status;
        }
    }
    status = impl.commit("return authorized");
    if (!status.ok()) {
        return status;
    }
    lock.unlock();
    return impl.publish_notices();
}

Status Coordinator::resolve_blocked(const BlockedResolution& resolution) {
    if (impl_ == nullptr) {
        return Status{ErrorCode::Invalid, "coordinator is not open"};
    }
    Impl& impl = *impl_;
    std::unique_lock<std::shared_mutex> lock(impl.mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        impl.busy_rejections.fetch_add(1);
        return Status{ErrorCode::Busy, "another coordinator call is in progress"};
    }
    internal::LockOrderGuard order(internal::LockLevel::Engine, &impl.lock_order_violations);
    impl.stats.api_calls += 1;
    if (impl.shutting_down) {
        return Status{ErrorCode::Shutdown, "coordinator is shutting down"};
    }
    Status status = require_text(resolution.resolved_by, "resolved_by", false);
    if (!status.ok()) {
        return status;
    }
    status = require_text(resolution.justification, "justification", false);
    if (!status.ok()) {
        return status;
    }
    const auto found = impl.state.events.find(resolution.event);
    if (found == impl.state.events.end()) {
        return Status{ErrorCode::NotFound, "event does not exist"};
    }
    if (found->second.phase != EventPhase::Blocked) {
        return Status{ErrorCode::NotReady, "the event is not blocked"};
    }
    if (!found->second.active_plan.valid()) {
        return Status{ErrorCode::NotReady, "the event has no active plan"};
    }
    const auto plan = impl.state.plans.find(found->second.active_plan);
    if (plan == impl.state.plans.end()) {
        return Status{ErrorCode::NotReady, "the event has no active plan"};
    }
    const auto step = plan->second.steps.find(resolution.step);
    if (step == plan->second.steps.end()) {
        return Status{ErrorCode::NotFound, "step does not belong to the active plan"};
    }
    const bool safety_critical = step->second.safety_critical;
    Status recorded = ok_status();
    if (resolution.retry) {
        recorded = impl.transition_step(plan->second.id, resolution.step, StepState::Ready,
                                        "retry authorized by " + resolution.resolved_by + ": " +
                                            resolution.justification);
    } else {
        recorded = impl.transition_step(plan->second.id, resolution.step, StepState::Cancelled,
                                        "abandoned by " + resolution.resolved_by + ": " +
                                            resolution.justification);
    }
    if (!recorded.ok()) {
        return recorded;
    }
    if (!resolution.retry && safety_critical) {
        DisasterEvent event = impl.state.events.at(resolution.event);
        event.failed_safety_critical_steps += 1;
        event.status_detail = "a safety-critical step was abandoned";
        event.updated_at = impl.now();
        event.updated_epoch = impl.state.epoch;
        const Status noted = impl.append_and_apply(journal::RecordType::EventTransition,
                                                   codec::encode_event(event));
        if (!noted.ok()) {
            return noted;
        }
    }
    status = impl.settle_dependents(resolution.event, nullptr);
    if (!status.ok()) {
        return status;
    }
    DisasterEvent event = impl.state.events.at(resolution.event);
    event.phase = event.phase_before_interruption;
    event.status_detail = resolution.retry ? "blocking step authorized to retry"
                                           : "blocking step resolved by abandonment";
    event.blocked_steps = 0;
    event.updated_at = impl.now();
    event.updated_epoch = impl.state.epoch;
    status = impl.append_and_apply(journal::RecordType::EventTransition, codec::encode_event(event));
    if (!status.ok()) {
        return status;
    }
    status = impl.commit("blocked step resolved");
    if (!status.ok()) {
        return status;
    }
    lock.unlock();
    return impl.publish_notices();
}

Status Coordinator::close_event(const EventClosure& closure) {
    if (impl_ == nullptr) {
        return Status{ErrorCode::Invalid, "coordinator is not open"};
    }
    Impl& impl = *impl_;
    std::unique_lock<std::shared_mutex> lock(impl.mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        impl.busy_rejections.fetch_add(1);
        return Status{ErrorCode::Busy, "another coordinator call is in progress"};
    }
    internal::LockOrderGuard order(internal::LockLevel::Engine, &impl.lock_order_violations);
    impl.stats.api_calls += 1;
    if (impl.shutting_down) {
        return Status{ErrorCode::Shutdown, "coordinator is shutting down"};
    }
    Status status = require_text(closure.closed_by, "closed_by", false);
    if (!status.ok()) {
        return status;
    }
    status = require_text(closure.summary, "summary", false);
    if (!status.ok()) {
        return status;
    }
    const auto found = impl.state.events.find(closure.event);
    if (found == impl.state.events.end()) {
        return Status{ErrorCode::NotFound, "event does not exist"};
    }
    const DisasterEvent& event = found->second;
    if (event.phase == EventPhase::Closed) {
        return ok_status();
    }
    if (event.phase != EventPhase::Returning) {
        return Status{ErrorCode::NotReady, "only a returning event can be closed"};
    }
    if (event.failed_safety_critical_steps > 0) {
        return Status{ErrorCode::GateUnmet,
                      "a safety-critical step failed; the event cannot be closed"};
    }
    DisasterEvent closed = event;
    closed.phase = EventPhase::Closed;
    closed.disposition = EventDisposition::Closed;
    std::string detail = closure.summary;
    detail.append(" | sites returned ");
    detail.append(std::to_string(closed.returned_sites.size()));
    detail.append(" of ");
    detail.append(std::to_string(closed.affected_sites.size()));
    closed.status_detail = std::move(detail);
    closed.updated_at = impl.now();
    closed.updated_epoch = impl.state.epoch;
    status = impl.append_and_apply(journal::RecordType::EventTransition, codec::encode_event(closed));
    if (!status.ok()) {
        return status;
    }
    status = impl.commit("event closed");
    if (!status.ok()) {
        return status;
    }
    lock.unlock();
    return impl.publish_notices();
}

// ---------------------------------------------------------------------------
// Durability
// ---------------------------------------------------------------------------

Status Coordinator::checkpoint() {
    if (impl_ == nullptr) {
        return Status{ErrorCode::Invalid, "coordinator is not open"};
    }
    Impl& impl = *impl_;
    std::unique_lock<std::shared_mutex> lock(impl.mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        impl.busy_rejections.fetch_add(1);
        return Status{ErrorCode::Busy, "another coordinator call is in progress"};
    }
    internal::LockOrderGuard order(internal::LockLevel::Engine, &impl.lock_order_violations);
    impl.stats.api_calls += 1;
    if (impl.shutting_down) {
        return Status{ErrorCode::Shutdown, "coordinator is shutting down"};
    }
    const bool compact_after =
        impl.options.auto_compact &&
        impl.state.records_since_snapshot >= impl.state.limits.compaction_record_threshold;
    const Status status = impl.write_snapshot(compact_after);
    if (!status.ok()) {
        return status;
    }
    lock.unlock();
    return impl.publish_notices();
}

Status Coordinator::compact() {
    if (impl_ == nullptr) {
        return Status{ErrorCode::Invalid, "coordinator is not open"};
    }
    Impl& impl = *impl_;
    std::unique_lock<std::shared_mutex> lock(impl.mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        impl.busy_rejections.fetch_add(1);
        return Status{ErrorCode::Busy, "another coordinator call is in progress"};
    }
    internal::LockOrderGuard order(internal::LockLevel::Engine, &impl.lock_order_violations);
    impl.stats.api_calls += 1;
    if (impl.shutting_down) {
        return Status{ErrorCode::Shutdown, "coordinator is shutting down"};
    }
    const Status status = impl.write_snapshot(true);
    if (!status.ok()) {
        return status;
    }
    lock.unlock();
    return impl.publish_notices();
}

Result<JournalVerification> Coordinator::verify_journal() const {
    if (impl_ == nullptr) {
        return Status{ErrorCode::Invalid, "coordinator is not open"};
    }
    const Impl& impl = *impl_;
    std::shared_lock<std::shared_mutex> lock(impl.mutex);
    JournalVerification verification;
    Result<bool> present = fileio::exists(impl.journal_path);
    if (!present.ok()) {
        return present.status();
    }
    if (!present.value()) {
        verification.ok = true;
        verification.detail = "journal does not exist yet";
        return verification;
    }
    Result<journal::Reader> reader =
        journal::Reader::open(impl.journal_path, impl.state.limits.max_journal_bytes);
    if (!reader.ok()) {
        return reader.status();
    }
    const journal::ScanReport& report = reader.value().report();
    verification.ok = !report.interior_corruption;
    verification.torn_tail = report.torn_tail;
    verification.interior_corruption = report.interior_corruption;
    verification.file_bytes = report.file_bytes;
    verification.committed_bytes = report.committed_bytes;
    verification.discarded_tail_bytes = report.discarded_tail_bytes;
    verification.record_count = report.record_count;
    verification.last_committed_sequence = report.last_committed_sequence;
    verification.chain = report.chain;
    verification.detail = report.detail;
    return verification;
}

Status Coordinator::shutdown() {
    if (impl_ == nullptr) {
        return Status{ErrorCode::Invalid, "coordinator is not open"};
    }
    Impl& impl = *impl_;
    std::unique_lock<std::shared_mutex> lock(impl.mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        impl.busy_rejections.fetch_add(1);
        return Status{ErrorCode::Busy, "another coordinator call is in progress"};
    }
    internal::LockOrderGuard order(internal::LockLevel::Engine, &impl.lock_order_violations);
    if (impl.shutting_down) {
        return ok_status();
    }
    impl.shutting_down = true;
    // Order matters: stop accepting receipts, unblock participants that are
    // holding a worker in a read, then join the workers. Nothing that arrives
    // after this point can be published as success.
    impl.mailbox->close();
    (void)impl.shutdown_endpoints();
    impl.pool->stop();
    const std::vector<internal::MailboxEntry> discarded = impl.mailbox->drain();
    // Anything still in the mailbox when the coordinator stops is dropped: work
    // that was cancelled by shutdown may never be published as success.
    impl.stats.receipts_ignored += discarded.size();
    impl.pending_exchanges = 0;
    if (impl.writer != nullptr) {
        const std::string note = "coordinator epoch " + impl.state.epoch.to_string() +
                                 " closed; discarded " + std::to_string(discarded.size()) +
                                 " receipts after shutdown";
        (void)impl.append_and_apply(journal::RecordType::Note, codec::encode_note(note));
        (void)impl.commit("shutdown");
        (void)impl.writer->close();
        impl.writer.reset();
    }
    if (impl.lock) {
        (void)impl.lock->release();
        impl.lock.reset();
    }
    lock.unlock();
    return ok_status();
}

Coordinator::~Coordinator() {
    if (impl_ != nullptr) {
        (void)shutdown();
    }
}

Status Coordinator::set_observer(Observer observer) {
    if (impl_ == nullptr) {
        return Status{ErrorCode::Invalid, "coordinator is not open"};
    }
    Impl& impl = *impl_;
    std::unique_lock<std::shared_mutex> lock(impl.mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        impl.busy_rejections.fetch_add(1);
        return Status{ErrorCode::Busy, "another coordinator call is in progress"};
    }
    impl.observer = std::move(observer);
    return ok_status();
}

// ---------------------------------------------------------------------------
// Read-only views
// ---------------------------------------------------------------------------

Epoch Coordinator::epoch() const noexcept {
    return impl_ == nullptr ? Epoch{} : impl_->state.epoch;
}

const Limits& Coordinator::limits() const noexcept {
    static const Limits kFallback{};
    return impl_ == nullptr ? kFallback : impl_->state.limits;
}

const std::string& Coordinator::directory() const noexcept {
    static const std::string kEmpty;
    return impl_ == nullptr ? kEmpty : impl_->options.directory;
}

Sequence Coordinator::last_committed_sequence() const {
    if (impl_ == nullptr) {
        return Sequence{};
    }
    Impl& impl = *impl_;
    std::shared_lock<std::shared_mutex> lock(impl.mutex);
    return impl.state.last_sequence;
}

Digest Coordinator::state_digest() const {
    if (impl_ == nullptr) {
        return Digest{};
    }
    Impl& impl = *impl_;
    std::shared_lock<std::shared_mutex> lock(impl.mutex);
    return internal::state_digest(impl.state);
}

CoordinatorStats Coordinator::stats() const {
    if (impl_ == nullptr) {
        return CoordinatorStats{};
    }
    Impl& impl = *impl_;
    std::shared_lock<std::shared_mutex> lock(impl.mutex);
    CoordinatorStats copy = impl.stats;
    copy.busy_rejections = impl.busy_rejections.load();
    copy.lock_order_violations = impl.lock_order_violations.load();
    if (impl.pool != nullptr) {
        copy.transport_failures = impl.pool->transport_failures();
        copy.unanswered_exchanges = impl.pool->unanswered();
    }
    return copy;
}

Result<RecoveryPolicy> Coordinator::policy() const {
    if (impl_ == nullptr) {
        return Status{ErrorCode::Invalid, "coordinator is not open"};
    }
    Impl& impl = *impl_;
    std::shared_lock<std::shared_mutex> lock(impl.mutex);
    return impl.state.policy;
}

Result<std::vector<FailureDomainId>> Coordinator::failure_domains() const {
    if (impl_ == nullptr) {
        return Status{ErrorCode::Invalid, "coordinator is not open"};
    }
    Impl& impl = *impl_;
    std::shared_lock<std::shared_mutex> lock(impl.mutex);
    std::vector<FailureDomainId> out;
    out.reserve(impl.state.domains.size());
    for (const auto& entry : impl.state.domains) {
        out.push_back(entry.first);
    }
    return out;
}

Result<FailureDomainRecord> Coordinator::failure_domain(FailureDomainId id) const {
    if (impl_ == nullptr) {
        return Status{ErrorCode::Invalid, "coordinator is not open"};
    }
    Impl& impl = *impl_;
    std::shared_lock<std::shared_mutex> lock(impl.mutex);
    const auto found = impl.state.domains.find(id);
    if (found == impl.state.domains.end()) {
        return Status{ErrorCode::NotFound, "failure domain does not exist"};
    }
    return found->second;
}

Result<std::vector<SiteId>> Coordinator::sites() const {
    if (impl_ == nullptr) {
        return Status{ErrorCode::Invalid, "coordinator is not open"};
    }
    Impl& impl = *impl_;
    std::shared_lock<std::shared_mutex> lock(impl.mutex);
    std::vector<SiteId> out;
    out.reserve(impl.state.sites.size());
    for (const auto& entry : impl.state.sites) {
        out.push_back(entry.first);
    }
    return out;
}

Result<SiteRecord> Coordinator::site(SiteId id) const {
    if (impl_ == nullptr) {
        return Status{ErrorCode::Invalid, "coordinator is not open"};
    }
    Impl& impl = *impl_;
    std::shared_lock<std::shared_mutex> lock(impl.mutex);
    const auto found = impl.state.sites.find(id);
    if (found == impl.state.sites.end()) {
        return Status{ErrorCode::NotFound, "site does not exist"};
    }
    return found->second;
}

Result<std::vector<ObligationId>> Coordinator::obligations() const {
    if (impl_ == nullptr) {
        return Status{ErrorCode::Invalid, "coordinator is not open"};
    }
    Impl& impl = *impl_;
    std::shared_lock<std::shared_mutex> lock(impl.mutex);
    std::vector<ObligationId> out;
    out.reserve(impl.state.obligations.size());
    for (const auto& entry : impl.state.obligations) {
        out.push_back(entry.first);
    }
    return out;
}

Result<ProtectedObligation> Coordinator::obligation(ObligationId id) const {
    if (impl_ == nullptr) {
        return Status{ErrorCode::Invalid, "coordinator is not open"};
    }
    Impl& impl = *impl_;
    std::shared_lock<std::shared_mutex> lock(impl.mutex);
    const auto found = impl.state.obligations.find(id);
    if (found == impl.state.obligations.end()) {
        return Status{ErrorCode::NotFound, "obligation does not exist"};
    }
    return found->second;
}

Result<DestinationCapability> Coordinator::capability(SiteId id) const {
    if (impl_ == nullptr) {
        return Status{ErrorCode::Invalid, "coordinator is not open"};
    }
    Impl& impl = *impl_;
    std::shared_lock<std::shared_mutex> lock(impl.mutex);
    const auto found = impl.state.capabilities.find(id);
    if (found == impl.state.capabilities.end()) {
        return Status{ErrorCode::Unknown, "no capability evidence has been recorded for this site"};
    }
    return found->second;
}

Result<SiteReadinessEvidence> Coordinator::readiness(SiteId id) const {
    if (impl_ == nullptr) {
        return Status{ErrorCode::Invalid, "coordinator is not open"};
    }
    Impl& impl = *impl_;
    std::shared_lock<std::shared_mutex> lock(impl.mutex);
    const auto found = impl.state.readiness.find(id);
    if (found == impl.state.readiness.end()) {
        return Status{ErrorCode::Unknown, "no readiness evidence has been recorded for this site"};
    }
    return found->second;
}

Result<Freshness> Coordinator::readiness_freshness(SiteId id) const {
    if (impl_ == nullptr) {
        return Status{ErrorCode::Invalid, "coordinator is not open"};
    }
    Impl& impl = *impl_;
    std::shared_lock<std::shared_mutex> lock(impl.mutex);
    return impl.readiness_freshness_for(id);
}

Result<FederationState> Coordinator::federation_state() const {
    if (impl_ == nullptr) {
        return Status{ErrorCode::Invalid, "coordinator is not open"};
    }
    Impl& impl = *impl_;
    std::shared_lock<std::shared_mutex> lock(impl.mutex);
    return impl.state.federation;
}

Result<std::vector<DisasterEventId>> Coordinator::events() const {
    if (impl_ == nullptr) {
        return Status{ErrorCode::Invalid, "coordinator is not open"};
    }
    Impl& impl = *impl_;
    std::shared_lock<std::shared_mutex> lock(impl.mutex);
    std::vector<DisasterEventId> out;
    out.reserve(impl.state.events.size());
    for (const auto& entry : impl.state.events) {
        out.push_back(entry.first);
    }
    return out;
}

Result<DisasterEvent> Coordinator::event(DisasterEventId id) const {
    if (impl_ == nullptr) {
        return Status{ErrorCode::Invalid, "coordinator is not open"};
    }
    Impl& impl = *impl_;
    std::shared_lock<std::shared_mutex> lock(impl.mutex);
    const auto found = impl.state.events.find(id);
    if (found == impl.state.events.end()) {
        return Status{ErrorCode::NotFound, "event does not exist"};
    }
    return found->second;
}

Result<std::vector<RecoveryPlanId>> Coordinator::plans() const {
    if (impl_ == nullptr) {
        return Status{ErrorCode::Invalid, "coordinator is not open"};
    }
    Impl& impl = *impl_;
    std::shared_lock<std::shared_mutex> lock(impl.mutex);
    std::vector<RecoveryPlanId> out;
    out.reserve(impl.state.plans.size());
    for (const auto& entry : impl.state.plans) {
        out.push_back(entry.first);
    }
    return out;
}

Result<RecoveryPlan> Coordinator::plan(RecoveryPlanId id) const {
    if (impl_ == nullptr) {
        return Status{ErrorCode::Invalid, "coordinator is not open"};
    }
    Impl& impl = *impl_;
    std::shared_lock<std::shared_mutex> lock(impl.mutex);
    const auto found = impl.state.plans.find(id);
    if (found == impl.state.plans.end()) {
        return Status{ErrorCode::NotFound, "plan does not exist"};
    }
    return found->second;
}

Result<std::vector<RecoveryPlanId>> Coordinator::plans_for_event(DisasterEventId id) const {
    if (impl_ == nullptr) {
        return Status{ErrorCode::Invalid, "coordinator is not open"};
    }
    Impl& impl = *impl_;
    std::shared_lock<std::shared_mutex> lock(impl.mutex);
    std::vector<RecoveryPlanId> out;
    for (const auto& entry : impl.state.plans) {
        if (entry.second.event == id) {
            out.push_back(entry.first);
        }
    }
    return out;
}

Result<RecoveryPlanId> Coordinator::active_plan(DisasterEventId id) const {
    if (impl_ == nullptr) {
        return Status{ErrorCode::Invalid, "coordinator is not open"};
    }
    Impl& impl = *impl_;
    std::shared_lock<std::shared_mutex> lock(impl.mutex);
    const auto found = impl.state.active_plan.find(id);
    if (found == impl.state.active_plan.end()) {
        return Status{ErrorCode::NotFound, "event has no active plan"};
    }
    return found->second;
}

Result<std::vector<RejectedEvidence>> Coordinator::rejected_evidence() const {
    if (impl_ == nullptr) {
        return Status{ErrorCode::Invalid, "coordinator is not open"};
    }
    Impl& impl = *impl_;
    std::shared_lock<std::shared_mutex> lock(impl.mutex);
    return impl.state.rejected_evidence;
}

Result<std::vector<EffectReceipt>> Coordinator::receipts() const {
    if (impl_ == nullptr) {
        return Status{ErrorCode::Invalid, "coordinator is not open"};
    }
    Impl& impl = *impl_;
    std::shared_lock<std::shared_mutex> lock(impl.mutex);
    std::vector<EffectReceipt> out;
    out.reserve(impl.state.receipts.size());
    for (const auto& entry : impl.state.receipts) {
        out.push_back(entry.second);
    }
    return out;
}

Result<std::vector<EffectRequest>> Coordinator::outstanding_requests() const {
    if (impl_ == nullptr) {
        return Status{ErrorCode::Invalid, "coordinator is not open"};
    }
    Impl& impl = *impl_;
    std::shared_lock<std::shared_mutex> lock(impl.mutex);
    std::vector<EffectRequest> out;
    out.reserve(impl.state.outstanding.size());
    for (const auto& entry : impl.state.outstanding) {
        out.push_back(entry.second);
    }
    return out;
}

Result<std::vector<EndpointDescriptor>> Coordinator::endpoints() const {
    if (impl_ == nullptr) {
        return Status{ErrorCode::Invalid, "coordinator is not open"};
    }
    Impl& impl = *impl_;
    std::shared_lock<std::shared_mutex> lock(impl.mutex);
    std::vector<EndpointDescriptor> out;
    out.reserve(impl.state.endpoints.size());
    for (const auto& entry : impl.state.endpoints) {
        out.push_back(entry.second);
    }
    return out;
}

Result<std::unique_ptr<Coordinator>> Coordinator::open(CoordinatorOptions options) {
    if (options.directory.empty()) {
        return Status{ErrorCode::Invalid, "coordinator directory must not be empty"};
    }
    if (options.journal_name.empty() || options.snapshot_name.empty() || options.lock_name.empty()) {
        return Status{ErrorCode::Invalid, "journal, snapshot, and lock names must not be empty"};
    }
    Status status = fileio::validate_leaf_name(options.journal_name);
    if (!status.ok()) {
        return status;
    }
    status = fileio::validate_leaf_name(options.snapshot_name);
    if (!status.ok()) {
        return status;
    }
    status = fileio::validate_leaf_name(options.lock_name);
    if (!status.ok()) {
        return status;
    }
    if (options.worker_threads == 0) {
        options.worker_threads = 1;
    }
    if (options.worker_threads > options.limits.max_worker_threads) {
        return Status{ErrorCode::OutOfRange, "worker thread count exceeds the configured limit"};
    }
    if (!options.clock) {
        options.clock = std::make_shared<SystemClock>();
    }

    auto coordinator = std::unique_ptr<Coordinator>(new Coordinator());
    coordinator->impl_ = std::make_unique<Impl>();
    Impl& impl = *coordinator->impl_;
    impl.options = std::move(options);
    impl.state.limits = impl.options.limits;
    impl.state.policy = impl.options.policy;
    if (!impl.state.policy.generation.valid()) {
        impl.state.policy.generation = Generation{1};
    }
    impl.journal_path = fileio::join(impl.options.directory, impl.options.journal_name);
    impl.snapshot_path = fileio::join(impl.options.directory, impl.options.snapshot_name);
    impl.lock_path = fileio::join(impl.options.directory, impl.options.lock_name);
    impl.mailbox = std::make_unique<internal::Mailbox>(impl.state.limits.max_pending_receipts);
    impl.pool = std::make_unique<internal::WorkerPool>(
        impl.options.worker_threads, *impl.mailbox, impl.state.limits.max_outstanding_requests);

    status = impl.recover();
    if (!status.ok()) {
        impl.mailbox->close();
        impl.pool->stop();
        (void)impl.shutdown_endpoints();
        if (impl.lock) {
            (void)impl.lock->release();
        }
        return status;
    }
    impl.initialized = true;
    return coordinator;
}

}  // namespace drc
