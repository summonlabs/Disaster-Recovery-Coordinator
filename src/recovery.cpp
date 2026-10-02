#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "codec.hpp"
#include "drc/canonical.hpp"
#include "drc/checkpoint.hpp"
#include "drc/fileio.hpp"
#include "engine_internal.hpp"

namespace drc {

void Coordinator::Impl::reset_inflight_after_recovery() {
    // One: an outstanding request from a previous incarnation can never be
    // answered. Recording that fact durably keeps replay exact and stops the
    // outstanding set from growing across restarts.
    std::vector<EffectRequest> unanswerable;
    for (const auto& entry : state.outstanding) {
        if (entry.second.issuing_epoch != state.epoch) {
            unanswerable.push_back(entry.second);
        }
    }
    for (const EffectRequest& request : unanswerable) {
        EffectReceipt receipt;
        receipt.id = EffectReceiptId{state.next_receipt_id};
        receipt.request = request.id;
        receipt.domain = request.domain;
        receipt.endpoint = EndpointId{};
        receipt.outcome = EffectOutcome::Unknown;
        receipt.detail = ErrorCode::Stale;
        receipt.event = request.event;
        receipt.plan = request.plan;
        receipt.step = request.step;
        receipt.event_generation = request.event_generation;
        receipt.plan_generation = request.plan_generation;
        receipt.observed_epoch = request.issuing_epoch;
        receipt.observed_at = now();
        receipt.endpoint_sequence = 0;
        receipt.message = "declared unanswerable by recovery: the issuing epoch is over";
        const std::vector<std::uint8_t> encoded = encode_effect_receipt(receipt);
        if (encoded.empty()) {
            continue;
        }
        (void)append_and_apply(journal::RecordType::Receipt, encoded);
    }

    // Two: a step that was in flight when the coordinator stopped has an
    // outcome nobody knows. It becomes indeterminate and retryable, never
    // successful by assumption.
    std::vector<RecoveryStep> stranded;
    for (const auto& plan_entry : state.plans) {
        for (const auto& step_entry : plan_entry.second.steps) {
            if (step_entry.second.state == StepState::InFlight) {
                stranded.push_back(step_entry.second);
            }
        }
    }
    for (const RecoveryStep& step : stranded) {
        RecoveryStep updated = step;
        updated.state = StepState::Indeterminate;
        updated.detail = "the coordinator restarted while this effect was in flight";
        const std::vector<std::uint8_t> encoded = encode_step_transition(updated);
        if (encoded.empty()) {
            continue;
        }
        (void)append_and_apply(journal::RecordType::StepTransition, encoded);
    }
}

Status Coordinator::Impl::recover() {
    Status status = fileio::ensure_directory(options.directory);
    if (!status.ok()) {
        return status;
    }

    // Take the single-writer lock before touching the journal. A live owner is
    // never displaced; a lock whose owner is gone may be taken over, and that
    // fact is reported.
    Epoch lock_epoch{1};
    Result<bool> lock_present = fileio::exists(lock_path);
    if (!lock_present.ok()) {
        return lock_present.status();
    }
    if (lock_present.value()) {
        Result<lockfile::Contents> previous = lockfile::read(lock_path);
        if (!previous.ok()) {
            return Status{ErrorCode::Locked,
                          "lock file exists but is not a valid coordinator lock: " +
                              previous.status().to_string()};
        }
        if (previous.value().epoch.valid()) {
            Result<Epoch> bumped = previous.value().epoch.next();
            if (bumped.ok()) {
                lock_epoch = bumped.value();
            }
        }
    }
    lockfile::Lock::Options lock_options;
    lock_options.path = lock_path;
    lock_options.owner = options.owner;
    lock_options.epoch = lock_epoch;
    lock_options.now_unix_seconds = now() / kNanosPerSecond;
    lock_options.allow_takeover = options.allow_lock_takeover;
    Result<lockfile::Lock> acquired = lockfile::Lock::acquire(lock_options);
    if (!acquired.ok()) {
        return acquired.status();
    }
    lock = std::make_unique<lockfile::Lock>(std::move(acquired).value());
    if (lock->takeover()) {
        notices.push_back(TransitionNotice{
            "lock_takeover", DisasterEventId{}, RecoveryPlanId{}, RecoveryStepId{},
            "took over the journal lock from pid " + std::to_string(lock->previous().pid)});
    }

    Result<bool> journal_present = fileio::exists(journal_path);
    if (!journal_present.ok()) {
        return journal_present.status();
    }
    journal::ScanReport scan;
    const bool has_journal_file = journal_present.value();
    if (has_journal_file) {
        Result<journal::Reader> reader =
            journal::Reader::open(journal_path, state.limits.max_journal_bytes);
        if (!reader.ok()) {
            return reader.status();
        }
        scan = reader.value().report();
        if (scan.interior_corruption) {
            std::string message = "journal has interior corruption at offset ";
            message.append(std::to_string(scan.corruption_offset));
            message.append(": ");
            message.append(scan.detail);
            message.append("; recovery refuses to truncate through it");
            return Status{ErrorCode::Corrupt, std::move(message)};
        }
        if (scan.torn_tail) {
            stats.torn_tail_recoveries += 1;
            std::string note = "recovering an uncommitted journal tail of ";
            note.append(std::to_string(scan.discarded_tail_bytes));
            note.append(" bytes: ");
            note.append(scan.detail);
            notices.push_back(TransitionNotice{"torn_tail", DisasterEventId{}, RecoveryPlanId{},
                                               RecoveryStepId{}, note});
        }
        stats.recovered_records = scan.committed_record_count;
    }

    bool snapshot_loaded = false;
    Sequence replay_from;
    if (fileio::exists(snapshot_path).value()) {
        Result<checkpoint::Contents> contents =
            checkpoint::read_file(snapshot_path, state.limits.max_journal_bytes);
        if (contents.ok()) {
            Result<internal::CoordinatorState> decoded =
                internal::decode_state(std::span<const std::uint8_t>(contents.value().payload));
            if (decoded.ok()) {
                if (internal::state_digest(decoded.value()) != contents.value().header.state_digest) {
                    return Status{ErrorCode::Corrupt,
                                  "snapshot content does not match the digest in its header"};
                }
                state = std::move(decoded).value();
                snapshot_loaded = true;
                replay_from = contents.value().header.covered_through;
                if (contents.value().header.epoch.valid() &&
                    contents.value().header.epoch > state.epoch) {
                    state.epoch = contents.value().header.epoch;
                }
            } else {
                notices.push_back(TransitionNotice{"snapshot_unusable", DisasterEventId{},
                                                   RecoveryPlanId{}, RecoveryStepId{},
                                                   decoded.status().to_string()});
            }
        } else {
            notices.push_back(TransitionNotice{"snapshot_unusable", DisasterEventId{},
                                               RecoveryPlanId{}, RecoveryStepId{},
                                               contents.status().to_string()});
        }
        if (!snapshot_loaded && !has_journal_file) {
            return Status{ErrorCode::Corrupt,
                          "the snapshot is unusable and no journal exists to replay"};
        }
    }

    bool have_commit = false;
    journal::CommitPayload last_commit;
    if (has_journal_file) {
        Result<journal::Reader> reader =
            journal::Reader::open(journal_path, state.limits.max_journal_bytes);
        if (!reader.ok()) {
            return reader.status();
        }
        const Status replayed = reader.value().for_each_committed(
            [&](const journal::Record& record) -> Status {
                if (record.sequence <= replay_from) {
                    return ok_status();
                }
                if (record.type == journal::RecordType::Commit) {
                    Result<journal::CommitPayload> payload = journal::decode_commit(record.payload);
                    if (!payload.ok()) {
                        return payload.status();
                    }
                    last_commit = payload.value();
                    have_commit = true;
                    state.last_sequence = record.sequence;
                    state.record_count = payload.value().record_count;
                    // The epoch of the last committed record is what the next
                    // incarnation must exceed, so the fence always climbs.
                    if (payload.value().epoch.valid() &&
                        payload.value().epoch > state.epoch) {
                        state.epoch = payload.value().epoch;
                    }
                    return ok_status();
                }
                std::vector<TransitionNotice> local;
                const Status applied = internal::apply_record(state, record, local);
                if (!applied.ok()) {
                    return applied;
                }
                for (TransitionNotice& notice : local) {
                    notices.push_back(std::move(notice));
                }
                if (record.type != journal::RecordType::Header &&
                    record.type != journal::RecordType::SnapshotRef) {
                    retained.push_back(record);
                }
                return ok_status();
            });
        if (!replayed.ok()) {
            return replayed;
        }
    }

    // The checksum fixed point: a replay of the committed records must
    // reproduce exactly the state digest the commit recorded. If it does not,
    // the durable history is not trustworthy and recovery refuses it.
    if (have_commit) {
        const Digest actual = internal::state_digest(state);
        if (actual != last_commit.state_digest) {
            return Status{ErrorCode::Corrupt,
                          "replayed state digest " + actual.to_hex() +
                              " does not match the committed digest " +
                              last_commit.state_digest.to_hex()};
        }
    }

    // Fence everything the previous incarnation produced.
    std::uint64_t epoch_value = 1;
    if (state.epoch.valid() && state.epoch.value() + 1 > epoch_value) {
        epoch_value = state.epoch.value() + 1;
    }
    if (scan.recorded_epoch.valid() && scan.recorded_epoch.value() + 1 > epoch_value) {
        epoch_value = scan.recorded_epoch.value() + 1;
    }
    if (lock_epoch.valid() && lock_epoch.value() > epoch_value) {
        epoch_value = lock_epoch.value();
    }
    state.epoch = Epoch{epoch_value};

    journal::Writer::Options writer_options;
    writer_options.path = journal_path;
    writer_options.max_bytes = state.limits.max_journal_bytes;
    writer_options.truncate_to_committed = true;
    Result<journal::Writer> opened = journal::Writer::open(writer_options);
    if (!opened.ok()) {
        return opened.status();
    }
    writer = std::make_unique<journal::Writer>(std::move(opened).value());
    state.last_sequence = writer->last_sequence();
    state.records_since_snapshot = retained.size();

    const bool fresh_journal = !has_journal_file || scan.empty;
    if (fresh_journal) {
        codec::JournalHeader header;
        header.format_version = journal::kFormatVersion;
        header.producer = "disaster-recovery-coordinator";
        header.created_at = now();
        header.epoch = state.epoch;
        status = append_and_apply(journal::RecordType::Header,
                                  codec::encode_journal_header(header));
        if (!status.ok()) {
            return status;
        }
        // The policy is authority, so it is journaled, not assumed from the
        // options that happen to be in force at open time.
        status = append_and_apply(journal::RecordType::Policy,
                                  codec::encode_policy(state.policy));
        if (!status.ok()) {
            return status;
        }
    }

    reset_inflight_after_recovery();

    const std::string note = "epoch " + state.epoch.to_string() + " opened by pid " +
                             std::to_string(lockfile::current_process_id());
    status = append_and_apply(journal::RecordType::Note, codec::encode_note(note));
    if (!status.ok()) {
        return status;
    }
    status = commit("open");
    if (!status.ok()) {
        return status;
    }

    if (state.records_since_snapshot >= state.limits.compaction_record_threshold) {
        // Compaction never runs without first writing a snapshot: the
        // rewritten journal must be readable on its own.
        const Status compacted = write_snapshot(true);
        if (!compacted.ok()) {
            return compacted;
        }
    }
    return ok_status();
}

}  // namespace drc
