#pragma once

// Engine internals shared by engine.cpp, advance.cpp, and recovery.cpp. Not
// installed: the public surface is include/drc/engine.hpp.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

#include "drc/engine.hpp"
#include "drc/journal.hpp"
#include "drc/lockfile.hpp"
#include "lockorder.hpp"
#include "state.hpp"

namespace drc::internal {

// What a worker reports when an exchange finishes without an applicable
// receipt: the participant closed the channel, or the transport failed. A
// silent participant is not a success and not a failure, it is an unknown
// outcome, and the engine needs to be told that one exchange is over.
struct Completion {
    EffectRequestId request;
    Status transport;
    bool delivered_any = false;
};

struct MailboxEntry {
    EffectReceipt receipt;
    Completion completion;
    bool is_completion = false;
};

// Bounded mailbox. Workers are producers, the engine thread is the only
// consumer. A closed mailbox drops entries and counts them so that work
// cancelled by shutdown can never be published as success afterwards.
class Mailbox {
public:
    explicit Mailbox(std::size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {}

    bool push(const EffectReceipt& receipt, std::uint64_t* dropped);
    bool push(const Completion& completion, std::uint64_t* dropped);
    [[nodiscard]] std::vector<MailboxEntry> drain();
    // Blocks until an entry is available. Returns false when the mailbox is
    // closed. There is no timeout: an exchange always ends, because a
    // participant either answers, closes its channel, or is terminated.
    bool wait_for_entry();
    void close();
    [[nodiscard]] std::size_t size() const;

private:
    bool push_entry(MailboxEntry entry, std::uint64_t* dropped);

    mutable std::mutex mutex_;
    std::condition_variable space_;
    std::condition_variable filled_;
    std::deque<MailboxEntry> queue_;
    std::size_t capacity_;
    bool closed_ = false;
};

struct Job {
    EffectRequest request;
    std::shared_ptr<EffectEndpoint> endpoint;
};

// Fixed-size worker pool. Workers never touch coordinator state: they perform
// one blocking request/response exchange with a participant and publish
// receipts into the mailbox.
class WorkerPool {
public:
    WorkerPool(std::uint32_t threads, Mailbox& mailbox, std::size_t queue_capacity);
    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;
    ~WorkerPool();

    [[nodiscard]] Status submit(std::shared_ptr<EffectEndpoint> endpoint, EffectRequest request);
    void stop();
    [[nodiscard]] std::size_t queued() const;
    [[nodiscard]] std::uint64_t dispatches() const;
    [[nodiscard]] std::uint64_t transport_failures() const;
    [[nodiscard]] std::uint64_t unanswered() const;

private:
    void run();

    mutable std::mutex mutex_;
    std::condition_variable work_;
    std::condition_variable space_;
    std::deque<Job> queue_;
    std::size_t capacity_;
    bool closed_ = false;
    Mailbox* mailbox_;
    std::vector<std::thread> threads_;
    std::atomic<std::uint64_t> dispatches_{0};
    std::atomic<std::uint64_t> transport_failures_{0};
    std::atomic<std::uint64_t> unanswered_{0};
};

}  // namespace drc::internal

namespace drc {

struct Coordinator::Impl {
    CoordinatorOptions options;
    internal::CoordinatorState state;
    std::unique_ptr<lockfile::Lock> lock;
    std::unique_ptr<journal::Writer> writer;
    std::map<EffectDomain, std::shared_ptr<EffectEndpoint>> endpoints;
    std::unique_ptr<internal::Mailbox> mailbox;
    std::unique_ptr<internal::WorkerPool> pool;
    Observer observer;

    mutable std::shared_mutex mutex;
    std::atomic<std::uint64_t> lock_order_violations{0};
    // Counted without the state lock, so it must not live in the statistics
    // struct that is only written under that lock.
    std::atomic<std::uint64_t> busy_rejections{0};
    CoordinatorStats stats;

    bool initialized = false;
    bool shutting_down = false;
    std::uint64_t fault_dispatches = 0;
    // The most recent reason a participant could not be started, surfaced in the
    // advance report so a silent configuration failure is never silent.
    std::string last_endpoint_failure;
    // Exchanges the engine has dispatched and not yet heard back about, so
    // "nothing left to do" cannot be declared while work is still in flight.
    std::uint64_t pending_exchanges = 0;
    std::string journal_path;
    std::string snapshot_path;
    std::string lock_path;
    // Records applied since the last snapshot, kept so compaction can rebuild
    // the journal without re-deriving anything.
    std::vector<journal::Record> retained;
    std::vector<TransitionNotice> notices;

    [[nodiscard]] UnixNanos now() const;
    [[nodiscard]] Generation next_generation_for(DisasterEventId event) const;

    // Appends one record and applies it. Every authoritative mutation goes
    // through here, which is what makes replay an exact reproduction.
    [[nodiscard]] Status append_and_apply(journal::RecordType type,
                                          std::vector<std::uint8_t> payload);
    [[nodiscard]] Status commit(const char* reason);
    [[nodiscard]] Status apply_commit_record();
    [[nodiscard]] Status publish_notices();
    [[nodiscard]] Status recover();
    [[nodiscard]] Status write_snapshot(bool compact_after);
    [[nodiscard]] Status compact_journal();
    [[nodiscard]] Result<AdvanceReport> advance(const AdvanceRequest& request);
    [[nodiscard]] Result<RecoveryPlanId> build_plan_for(DisasterEventId event,
                                                        Generation generation,
                                                        PlanKind kind,
                                                        const std::string& requested_by,
                                                        const std::vector<SiteId>& targets,
                                                        bool allow_partial);
    [[nodiscard]] std::shared_ptr<EffectEndpoint> endpoint_for(EffectDomain domain);
    [[nodiscard]] Status transition_event(DisasterEventId event, EventPhase phase, std::string detail);
    [[nodiscard]] Status transition_step(RecoveryPlanId plan,
                                         RecoveryStepId step,
                                         StepState state,
                                         std::string detail,
                                         bool authorize = false);
    [[nodiscard]] Status mark_event_blocked(DisasterEventId event, std::string detail);
    void reset_inflight_after_recovery();
    [[nodiscard]] Status shutdown_endpoints();
    [[nodiscard]] Result<Freshness> readiness_freshness_for(SiteId site) const;
    [[nodiscard]] Status dispatch_ready_steps(const AdvanceRequest& request, AdvanceReport& report);
    // Could this step be dispatched right now, ignoring the per-call budget? It
    // is what makes "nothing left to do" an honest statement rather than "we
    // stopped dispatching".
    [[nodiscard]] bool step_is_dispatchable(const RecoveryPlan& plan, const RecoveryStep& step) const;
    [[nodiscard]] Status apply_receipt(const EffectReceipt& receipt, ReceiptDisposition& disposition);
    [[nodiscard]] Status apply_completion(const internal::Completion& completion);
    [[nodiscard]] ReceiptDisposition judge_receipt(const EffectReceipt& receipt) const;
    [[nodiscard]] Status promote_phases(DisasterEventId event, AdvanceReport& report);
    // Cancels steps that can never run because something they depend on did not
    // succeed. Cancellation is recorded, never silent.
    [[nodiscard]] Status settle_dependents(DisasterEventId event, AdvanceReport* report);
    // Steps that ran out of attempts without a successful outcome are failures,
    // not an indefinite pause: the event stops and waits for a decision.
    [[nodiscard]] Status settle_exhausted(DisasterEventId event, AdvanceReport* report);
};

}  // namespace drc
