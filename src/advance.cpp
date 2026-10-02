#include <algorithm>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "codec.hpp"
#include "engine_internal.hpp"

namespace drc {

ReceiptDisposition Coordinator::Impl::judge_receipt(const EffectReceipt& receipt) const {
    if (!receipt.request.valid()) {
        return ReceiptDisposition::UnknownRequest;
    }
    // Checked before the outstanding set: applying the first answer retires the
    // request, so a second answer for it would otherwise look unknown rather
    // than duplicate.
    if (state.receipt_by_request.find(receipt.request) != state.receipt_by_request.end()) {
        return ReceiptDisposition::Duplicate;
    }
    const auto request = state.outstanding.find(receipt.request);
    if (request == state.outstanding.end()) {
        return ReceiptDisposition::UnknownRequest;
    }
    if (request->second.issuing_epoch != state.epoch) {
        // The request was issued by an earlier incarnation of this coordinator.
        // Its answer can no longer change anything.
        return ReceiptDisposition::ForeignEpoch;
    }
    if (receipt.observed_epoch != request->second.issuing_epoch) {
        return ReceiptDisposition::ForeignEpoch;
    }
    const auto plan = state.plans.find(request->second.plan);
    if (plan == state.plans.end()) {
        return ReceiptDisposition::StaleGeneration;
    }
    if (plan->second.generation != receipt.plan_generation ||
        plan->second.event_generation != receipt.event_generation) {
        return ReceiptDisposition::StaleGeneration;
    }
    if (plan->second.state != PlanState::Active) {
        return ReceiptDisposition::StaleGeneration;
    }
    if (receipt.outcome == EffectOutcome::Superseded) {
        return ReceiptDisposition::StaleGeneration;
    }
    const auto step = plan->second.steps.find(receipt.step);
    if (step == plan->second.steps.end()) {
        return ReceiptDisposition::UnknownRequest;
    }
    if (step->second.state != StepState::InFlight) {
        // The step already reached a terminal state, or was never dispatched.
        // A late answer must not reopen it.
        return ReceiptDisposition::NotApplicable;
    }
    if (step->second.last_request.valid() && step->second.last_request != receipt.request) {
        return ReceiptDisposition::NotApplicable;
    }
    return ReceiptDisposition::Applies;
}

Status Coordinator::Impl::apply_receipt(const EffectReceipt& receipt,
                                        ReceiptDisposition& disposition) {
    disposition = judge_receipt(receipt);
    if (disposition != ReceiptDisposition::Applies) {
        stats.receipts_ignored += 1;
        switch (disposition) {
            case ReceiptDisposition::Duplicate:
                stats.duplicate_receipts += 1;
                break;
            case ReceiptDisposition::StaleGeneration:
                stats.stale_receipts += 1;
                break;
            case ReceiptDisposition::ForeignEpoch:
                stats.foreign_epoch_receipts += 1;
                break;
            default:
                break;
        }
        return ok_status();
    }
    const auto request = state.outstanding.find(receipt.request);
    const RecoveryPlanId plan_id = request->second.plan;
    const RecoveryStepId step_id = request->second.step;
    const DisasterEventId event_id = request->second.event;

    EffectReceipt durable = receipt;
    // Receipt identities are allocated by the coordinator, not by the
    // participant, so two endpoints can never collide.
    durable.id = EffectReceiptId{state.next_receipt_id};
    durable.domain = plan_id.valid() ? durable.domain : durable.domain;
    const std::vector<std::uint8_t> encoded = encode_effect_receipt(durable);
    if (encoded.empty()) {
        return Status{ErrorCode::OutOfRange, "receipt could not be encoded"};
    }
    Status status = append_and_apply(journal::RecordType::Receipt, encoded);
    if (!status.ok()) {
        return status;
    }

    const auto plan = state.plans.find(plan_id);
    if (plan == state.plans.end()) {
        return ok_status();
    }
    const auto step = plan->second.steps.find(step_id);
    if (step == plan->second.steps.end()) {
        return ok_status();
    }
    const RecoveryStep current = step->second;

    switch (receipt.outcome) {
        case EffectOutcome::Completed: {
            if ((current.kind == StepKind::ReserveDestination ||
                 current.kind == StepKind::PlaceObligation) &&
                (receipt.reservation.valid() || receipt.placement.valid())) {
                RecoveryPlan updated = plan->second;
                for (PlacementAssignment& assignment : updated.assignments) {
                    if (assignment.obligation != current.obligation) {
                        continue;
                    }
                    if (current.kind == StepKind::ReserveDestination &&
                        receipt.reservation.valid()) {
                        assignment.reservation = receipt.reservation;
                    }
                    if (current.kind == StepKind::PlaceObligation && receipt.placement.valid()) {
                        assignment.placement = receipt.placement;
                    }
                }
                const std::vector<std::uint8_t> encoded_plan = encode_plan(updated);
                if (encoded_plan.empty()) {
                    return Status{ErrorCode::OutOfRange, "plan could not be encoded"};
                }
                status = append_and_apply(journal::RecordType::Plan, encoded_plan);
                if (!status.ok()) {
                    return status;
                }
            }
            status = transition_step(plan_id, step_id, StepState::Succeeded,
                                     "completed with outcome completed");
            if (!status.ok()) {
                return status;
            }
            if (current.kind == StepKind::ReturnToService) {
                codec::ReturnRecord record;
                record.event = event_id;
                record.site = current.destination;
                record.epoch = state.epoch;
                record.at = now();
                record.authorized_by = "site control plane";
                record.justification = "return-to-service effect completed";
                status = append_and_apply(journal::RecordType::ReturnToService,
                                          codec::encode_return_record(record));
                if (!status.ok()) {
                    return status;
                }
            }
            break;
        }
        case EffectOutcome::Accepted: {
            // Accepted is not completion. The outcome is genuinely unknown
            // until the participant reports it, so the step stays retryable
            // and never counts as success.
            status = transition_step(plan_id, step_id, StepState::Indeterminate,
                                     "accepted by the participant; completion not reported");
            break;
        }
        case EffectOutcome::Deferred: {
            status = transition_step(plan_id, step_id, StepState::Deferred,
                                     "the participant deferred the request");
            break;
        }
        case EffectOutcome::Unknown: {
            status = transition_step(plan_id, step_id, StepState::Indeterminate,
                                     "the participant did not report an outcome");
            break;
        }
        case EffectOutcome::Superseded: {
            status = transition_step(plan_id, step_id, StepState::Superseded,
                                     "the participant reported the request as superseded");
            break;
        }
        case EffectOutcome::Rejected:
        case EffectOutcome::Unsupported: {
            std::string detail = "the participant rejected the request";
            if (receipt.detail != ErrorCode::Ok) {
                detail.append(": ");
                detail.append(to_string(receipt.detail));
            }
            status = transition_step(plan_id, step_id, StepState::Failed, std::move(detail));
            if (!status.ok()) {
                return status;
            }
            if (current.safety_critical) {
                DisasterEvent event = state.events.at(event_id);
                event.failed_safety_critical_steps += 1;
                const std::vector<std::uint8_t> encoded_event = codec::encode_event(event);
                status = append_and_apply(journal::RecordType::EventTransition, encoded_event);
                if (!status.ok()) {
                    return status;
                }
            }
            status = mark_event_blocked(event_id, "step " + step_id.to_string() +
                                                      " failed at " +
                                                      std::string{to_string(current.kind)});
            break;
        }
    }
    if (!status.ok()) {
        return status;
    }

    // Exhausted attempts are settled by settle_exhausted, which also covers
    // exchanges that ended without a receipt at all.
    stats.receipts_applied += 1;
    return ok_status();
}

Status Coordinator::Impl::apply_completion(const internal::Completion& completion) {
    if (state.receipt_by_request.find(completion.request) != state.receipt_by_request.end()) {
        // A receipt already settled this request; the exchange end adds nothing.
        return ok_status();
    }
    const auto request = state.outstanding.find(completion.request);
    if (request == state.outstanding.end()) {
        return ok_status();
    }
    if (request->second.issuing_epoch != state.epoch) {
        return ok_status();
    }
    const RecoveryPlanId plan_id = request->second.plan;
    const RecoveryStepId step_id = request->second.step;
    const auto plan = state.plans.find(plan_id);
    if (plan == state.plans.end()) {
        return ok_status();
    }
    const auto step = plan->second.steps.find(step_id);
    if (step == plan->second.steps.end() || step->second.state != StepState::InFlight) {
        return ok_status();
    }
    // Silence is not success and not failure: the outcome is unknown, and the
    // step stays retryable until its attempt budget runs out.
    std::string detail;
    if (completion.transport.ok()) {
        detail = "the participant ended the exchange without reporting an outcome";
    } else {
        detail = "the exchange failed: ";
        detail.append(completion.transport.to_string());
    }
    return transition_step(plan_id, step_id, StepState::Indeterminate, std::move(detail));
}

Status Coordinator::Impl::settle_dependents(DisasterEventId event_id, AdvanceReport* report) {
    const auto event = state.events.find(event_id);
    if (event == state.events.end() || !event->second.active_plan.valid()) {
        return ok_status();
    }
    const RecoveryPlanId plan_id = event->second.active_plan;
    if (state.plans.find(plan_id) == state.plans.end()) {
        return ok_status();
    }
    bool changed = true;
    std::size_t guard = 0;
    while (changed && guard < 64) {
        changed = false;
        guard += 1;
        const std::vector<RecoveryStepId> order = state.plans.at(plan_id).order;
        for (const RecoveryStepId step_id : order) {
            const auto step = state.plans.at(plan_id).steps.find(step_id);
            if (step == state.plans.at(plan_id).steps.end()) {
                continue;
            }
            const RecoveryStep current = step->second;
            if (is_terminal_step_state(current.state) || current.state == StepState::InFlight) {
                continue;
            }
            for (const RecoveryStepId dependency : current.depends_on) {
                const auto found = state.plans.at(plan_id).steps.find(dependency);
                if (found == state.plans.at(plan_id).steps.end()) {
                    continue;
                }
                const StepState dependency_state = found->second.state;
                if (dependency_state != StepState::Cancelled &&
                    dependency_state != StepState::Superseded &&
                    dependency_state != StepState::Failed) {
                    continue;
                }
                std::string detail = "dependency ";
                detail.append(dependency.to_string());
                detail.append(" ended as ");
                detail.append(to_string(dependency_state));
                const Status status =
                    transition_step(plan_id, step_id, StepState::Cancelled, detail);
                if (!status.ok()) {
                    return status;
                }
                if (current.safety_critical) {
                    DisasterEvent updated = state.events.at(event_id);
                    updated.failed_safety_critical_steps += 1;
                    const Status noted = append_and_apply(journal::RecordType::EventTransition,
                                                          codec::encode_event(updated));
                    if (!noted.ok()) {
                        return noted;
                    }
                }
                if (report != nullptr) {
                    report->steps_failed += 1;
                }
                changed = true;
                break;
            }
        }
    }
    return ok_status();
}

Status Coordinator::Impl::settle_exhausted(DisasterEventId event_id, AdvanceReport* report) {
    const auto event = state.events.find(event_id);
    if (event == state.events.end() || !event->second.active_plan.valid()) {
        return ok_status();
    }
    const RecoveryPlanId plan_id = event->second.active_plan;
    if (state.plans.find(plan_id) == state.plans.end()) {
        return ok_status();
    }
    const std::vector<RecoveryStepId> order = state.plans.at(plan_id).order;
    for (const RecoveryStepId step_id : order) {
        const auto step = state.plans.at(plan_id).steps.find(step_id);
        if (step == state.plans.at(plan_id).steps.end()) {
            continue;
        }
        const RecoveryStep current = step->second;
        const bool retryable = current.state == StepState::Indeterminate ||
                               current.state == StepState::Deferred;
        if (!retryable || current.attempts < state.policy.max_step_attempts) {
            continue;
        }
        std::string detail = "no successful outcome after ";
        detail.append(std::to_string(current.attempts));
        detail.append(" attempts");
        const Status failed = transition_step(plan_id, step_id, StepState::Failed, detail);
        if (!failed.ok()) {
            return failed;
        }
        if (current.safety_critical) {
            DisasterEvent updated = state.events.at(event_id);
            updated.failed_safety_critical_steps += 1;
            const Status noted = append_and_apply(journal::RecordType::EventTransition,
                                                  codec::encode_event(updated));
            if (!noted.ok()) {
                return noted;
            }
        }
        if (report != nullptr) {
            report->steps_failed += 1;
        }
        const Status blocked = mark_event_blocked(event_id, "step " + step_id.to_string() +
                                                                   " exhausted its attempts");
        if (!blocked.ok()) {
            return blocked;
        }
    }
    return ok_status();
}

Status Coordinator::Impl::promote_phases(DisasterEventId event_id, AdvanceReport& report) {
    const auto found = state.events.find(event_id);
    if (found == state.events.end()) {
        return ok_status();
    }
    DisasterEvent event = found->second;
    if (event.disposition != EventDisposition::Active || !event.active_plan.valid()) {
        return ok_status();
    }
    const auto plan = state.plans.find(event.active_plan);
    if (plan == state.plans.end() || plan->second.state != PlanState::Active) {
        return ok_status();
    }
    std::size_t total = 0;
    std::size_t succeeded = 0;
    std::size_t evacuation_total = 0;
    std::size_t evacuation_succeeded = 0;
    std::size_t readiness_total = 0;
    std::size_t readiness_succeeded = 0;
    std::size_t failed = 0;
    for (const RecoveryStepId step_id : plan->second.order) {
        const RecoveryStep& step = plan->second.steps.at(step_id);
        total += 1;
        if (step.state == StepState::Succeeded) {
            succeeded += 1;
        }
        if (step.state == StepState::Failed || step.state == StepState::Cancelled) {
            failed += 1;
        }
        if (step.kind == StepKind::EvacuateSource) {
            evacuation_total += 1;
            if (step.state == StepState::Succeeded) {
                evacuation_succeeded += 1;
            }
        }
        if (step.kind == StepKind::ValidateReadiness) {
            readiness_total += 1;
            if (step.state == StepState::Succeeded) {
                readiness_succeeded += 1;
            }
        }
    }

    EventPhase next = event.phase;
    if (plan->second.kind == PlanKind::Recovery) {
        if (event.phase == EventPhase::Evacuating && evacuation_total > 0 &&
            evacuation_succeeded == evacuation_total) {
            next = EventPhase::FailingOver;
        }
        if ((event.phase == EventPhase::Evacuating || event.phase == EventPhase::FailingOver) &&
            failed == 0 && succeeded == total && total > 0) {
            next = EventPhase::Stabilized;
        }
    } else if (plan->second.kind == PlanKind::Restoration) {
        if (event.phase == EventPhase::Restoring && readiness_total > 0 &&
            readiness_succeeded == readiness_total) {
            next = EventPhase::Validating;
        }
        if (event.phase == EventPhase::Validating && !event.returned_sites.empty()) {
            next = EventPhase::Returning;
        }
    }
    if (next != event.phase) {
        const Status status = transition_event(
            event_id, next,
            std::string{"phase advanced to "} + std::string{to_string(next)});
        if (!status.ok()) {
            return status;
        }
        report.notes.push_back(std::string{"phase advanced to "} + std::string{to_string(next)});
    }
    return ok_status();
}

bool Coordinator::Impl::step_is_dispatchable(const RecoveryPlan& plan,
                                            const RecoveryStep& step) const {
    const bool retryable = step.state == StepState::Pending || step.state == StepState::Ready ||
                           step.state == StepState::Deferred ||
                           step.state == StepState::Indeterminate;
    if (!retryable || !step.authorized || step.attempts >= state.policy.max_step_attempts) {
        return false;
    }
    for (const RecoveryStepId dependency : step.depends_on) {
        const auto found = plan.steps.find(dependency);
        if (found == plan.steps.end() || found->second.state != StepState::Succeeded) {
            return false;
        }
    }
    FailureDomainId domain = step.destination_domain;
    if (!domain.valid()) {
        const SiteId site = step.destination.valid() ? step.destination : step.source;
        const auto record = state.sites.find(site);
        if (record != state.sites.end()) {
            domain = record->second.domain;
        }
    }
    if (domain.valid()) {
        const bool reachable = state.federation.known
                                   ? federation_reaches(state.federation, domain)
                                   : !state.policy.require_federation_evidence;
        if (!reachable) {
            return false;
        }
    }
    return state.endpoints.find(effect_domain_for(step.kind)) != state.endpoints.end();
}

Status Coordinator::Impl::dispatch_ready_steps(const AdvanceRequest& request,
                                               AdvanceReport& report) {
    const auto found = state.events.find(request.event);
    if (found == state.events.end() || !found->second.active_plan.valid()) {
        return ok_status();
    }
    const RecoveryPlanId plan_id = found->second.active_plan;
    if (state.plans.find(plan_id) == state.plans.end()) {
        return ok_status();
    }
    if (state.plans.at(plan_id).state != PlanState::Active) {
        return ok_status();
    }
    std::uint32_t dispatched = 0;
    std::uint32_t in_flight = 0;
    for (const RecoveryStepId step_id : state.plans.at(plan_id).order) {
        if (state.plans.at(plan_id).steps.at(step_id).state == StepState::InFlight) {
            in_flight += 1;
        }
    }
    const std::vector<RecoveryStepId> order = state.plans.at(plan_id).order;
    for (const RecoveryStepId step_id : order) {
        if (dispatched >= request.max_dispatches) {
            report.notes.push_back("dispatch budget for this call is exhausted");
            break;
        }
        if (in_flight >= state.policy.max_in_flight_steps) {
            report.notes.push_back("in-flight bound reached");
            break;
        }
        const auto step = state.plans.at(plan_id).steps.find(step_id);
        if (step == state.plans.at(plan_id).steps.end()) {
            continue;
        }
        const RecoveryStep current = step->second;
        const bool retryable = current.state == StepState::Pending ||
                               current.state == StepState::Ready ||
                               current.state == StepState::Deferred ||
                               current.state == StepState::Indeterminate;
        if (!retryable || !current.authorized ||
            current.attempts >= state.policy.max_step_attempts) {
            continue;
        }
        bool dependencies_satisfied = true;
        for (const RecoveryStepId dependency : current.depends_on) {
            const auto found_step = state.plans.at(plan_id).steps.find(dependency);
            if (found_step == state.plans.at(plan_id).steps.end() ||
                found_step->second.state != StepState::Succeeded) {
                dependencies_satisfied = false;
                break;
            }
        }
        if (!dependencies_satisfied) {
            continue;
        }
        // Reachability and participant availability are checked before the
        // budget, so both paths agree on what "dispatchable" means.
        if (!step_is_dispatchable(state.plans.at(plan_id), current)) {
            FailureDomainId domain = current.destination_domain;
            if (!domain.valid()) {
                const SiteId site = current.destination.valid() ? current.destination : current.source;
                const auto record = state.sites.find(site);
                if (record != state.sites.end()) {
                    domain = record->second.domain;
                }
            }
            const bool reachable =
                !domain.valid() || (state.federation.known
                                        ? federation_reaches(state.federation, domain)
                                        : !state.policy.require_federation_evidence);
            if (!reachable) {
                if (current.state != StepState::Deferred) {
                    const Status deferred = transition_step(
                        plan_id, step_id, StepState::Deferred,
                        "destination failure domain " + domain.to_string() +
                            " is not reachable under the current federation state");
                    if (!deferred.ok()) {
                        return deferred;
                    }
                }
                report.steps_deferred += 1;
                stats.deferred_dispatches += 1;
            } else {
                const EffectDomain target = effect_domain_for(current.kind);
                if (current.state != StepState::Deferred) {
                    const Status deferred = transition_step(
                        plan_id, step_id, StepState::Deferred,
                        "no effect endpoint is registered for " +
                            std::string{to_string(target)});
                    if (!deferred.ok()) {
                        return deferred;
                    }
                }
                report.steps_deferred += 1;
                report.notes.push_back("no endpoint registered for " +
                                       std::string{to_string(target)});
            }
            continue;
        }

        const EffectDomain target_domain = effect_domain_for(current.kind);
        std::shared_ptr<EffectEndpoint> endpoint = endpoint_for(target_domain);
        if (endpoint == nullptr) {
            // Registered but unusable: say so rather than looking like a stall.
            report.notes.push_back("participant for " + std::string{to_string(target_domain)} +
                                   " could not be started");
            if (!last_endpoint_failure.empty()) {
                report.notes.push_back(last_endpoint_failure);
            }
            break;
        }
        FailureDomainId domain = current.destination_domain;
        if (!domain.valid()) {
            const SiteId site =
                current.destination.valid() ? current.destination : current.source;
            const auto record = state.sites.find(site);
            if (record != state.sites.end()) {
                domain = record->second.domain;
            }
        }
        if (pool == nullptr) {
            return Status{ErrorCode::Shutdown, "worker pool is not available"};
        }
        if (pool->queued() + 1 > state.limits.max_outstanding_requests) {
            report.notes.push_back("dispatch queue is full");
            break;
        }

        EffectRequest outbound;
        outbound.id = EffectRequestId{state.next_request_id};
        outbound.domain = target_domain;
        outbound.kind = effect_kind_for(current.kind);
        outbound.event = request.event;
        outbound.plan = plan_id;
        outbound.step = current.id;
        outbound.event_generation = current.event_generation;
        outbound.plan_generation = current.plan_generation;
        outbound.issuing_epoch = state.epoch;
        outbound.source = current.source;
        outbound.destination = current.destination;
        outbound.obligation = current.obligation;
        outbound.destination_domain = domain;
        outbound.recovery_class = current.recovery_class;
        outbound.required_capacity_units = current.required_capacity_units;
        outbound.attempt = current.attempts + 1;
        if (!outbound.id.valid()) {
            return Status{ErrorCode::Exhausted, "effect request identity space is exhausted"};
        }

        // The dispatch is durable before the effect is sent: a crash after the
        // participant acted must not lose the fact that we asked.
        RecoveryStep updated = current;
        updated.attempts = current.attempts + 1;
        updated.state = StepState::InFlight;
        updated.last_request = outbound.id;
        updated.detail = "dispatched attempt " + std::to_string(updated.attempts);
        const std::vector<std::uint8_t> encoded_step = encode_step_transition(updated);
        if (encoded_step.empty()) {
            return Status{ErrorCode::OutOfRange, "step could not be encoded"};
        }
        Status status = append_and_apply(journal::RecordType::StepTransition, encoded_step);
        if (!status.ok()) {
            return status;
        }
        const std::vector<std::uint8_t> encoded_request = encode_effect_request(outbound);
        if (encoded_request.empty()) {
            return Status{ErrorCode::OutOfRange, "effect request could not be encoded"};
        }
        status = append_and_apply(journal::RecordType::Dispatch, encoded_request);
        if (!status.ok()) {
            return status;
        }
        status = commit("dispatch");
        if (!status.ok()) {
            return status;
        }
        stats.dispatches += 1;
        pending_exchanges += 1;
        const Status submitted = pool->submit(endpoint, outbound);
        if (!submitted.ok()) {
            // The request is durable but was never sent. Recording the step as
            // deferred is honest: no participant has acted on it.
            if (pending_exchanges > 0) {
                pending_exchanges -= 1;
            }
            const Status reverted = transition_step(
                plan_id, step_id, StepState::Deferred,
                "dispatch could not be queued: " + submitted.to_string());
            if (!reverted.ok()) {
                return reverted;
            }
            report.notes.push_back("dispatch could not be queued");
            continue;
        }
        fault_dispatches += 1;
        if (options.fault.crash_after_dispatches != 0 &&
            fault_dispatches >= options.fault.crash_after_dispatches) {
            std::_Exit(9);
        }
        dispatched += 1;
        in_flight += 1;
        report.dispatched += 1;
    }
    return ok_status();
}

Result<AdvanceReport> Coordinator::Impl::advance(const AdvanceRequest& request) {
    AdvanceReport report;
    report.event = request.event;
    const auto found = state.events.find(request.event);
    if (found == state.events.end()) {
        return Status{ErrorCode::NotFound, "event does not exist"};
    }
    report.phase_before = found->second.phase;
    report.plan = found->second.active_plan;
    report.plan_generation = found->second.active_plan_generation;
    if (found->second.disposition != EventDisposition::Active) {
        report.notes.push_back("event is not active");
        report.phase_after = found->second.phase;
        report.quiescent = true;
        return report;
    }
    if (found->second.phase == EventPhase::Conflicted) {
        report.notes.push_back("event has contradictory evidence; nothing was dispatched");
        report.phase_after = found->second.phase;
        report.quiescent = true;
        return report;
    }
    if (found->second.phase == EventPhase::Blocked) {
        report.notes.push_back("event is blocked; resolve the blocking step");
        report.phase_after = found->second.phase;
        report.quiescent = true;
        report.blocked = true;
        return report;
    }

    const std::uint32_t rounds = request.max_rounds == 0 ? 1 : request.max_rounds;
    // The loop is bounded by work, never by wall-clock time: dispatch rounds,
    // plus one iteration per exchange that must report back, plus slack for the
    // settle and promote passes.
    const std::uint64_t budget = (static_cast<std::uint64_t>(rounds) * 2ull) + 16ull +
                                 (static_cast<std::uint64_t>(request.max_dispatches) * 2ull);
    std::uint32_t dispatch_rounds = 0;
    bool quiescent = false;
    for (std::uint64_t iteration = 0; iteration < budget; ++iteration) {
        const std::uint64_t records_before = stats.records_appended;
        const std::uint32_t dispatched_before = report.dispatched;
        std::vector<internal::MailboxEntry> entries = mailbox->drain();
        for (internal::MailboxEntry& entry : entries) {
            if (entry.is_completion) {
                if (pending_exchanges > 0) {
                    pending_exchanges -= 1;
                }
                const Status status = apply_completion(entry.completion);
                if (!status.ok()) {
                    return status;
                }
                continue;
            }
            ReceiptDisposition disposition = ReceiptDisposition::Applies;
            const Status status = apply_receipt(entry.receipt, disposition);
            if (!status.ok()) {
                return status;
            }
            if (disposition == ReceiptDisposition::Applies) {
                report.receipts_applied += 1;
            } else {
                report.receipts_ignored += 1;
            }
        }
        if (dispatch_rounds < rounds) {
            dispatch_rounds += 1;
            report.rounds += 1;
            const Status dispatched = dispatch_ready_steps(request, report);
            if (!dispatched.ok()) {
                return dispatched;
            }
        }
        const Status settled = settle_dependents(request.event, &report);
        if (!settled.ok()) {
            return settled;
        }
        const Status exhausted = settle_exhausted(request.event, &report);
        if (!exhausted.ok()) {
            return exhausted;
        }
        const Status promoted = promote_phases(request.event, report);
        if (!promoted.ok()) {
            return promoted;
        }
        if (stats.records_appended != records_before) {
            const Status committed = commit("advance");
            if (!committed.ok()) {
                return committed;
            }
        }
        const bool did_dispatch = report.dispatched > dispatched_before;
        if (!entries.empty() || did_dispatch) {
            continue;
        }
        if (pending_exchanges > 0) {
            // Outstanding exchanges must report in before the engine may claim
            // there is nothing left to do.
            if (!mailbox->wait_for_entry()) {
                quiescent = true;
                break;
            }
            continue;
        }
        // Nothing happened this iteration. Before claiming there is nothing
        // left to do, check whether the only reason nothing happened is the
        // per-call budget: a bounded call must not report quiescence while
        // steps are sitting ready.
        bool work_remains = false;
        const auto event_plan = state.events.find(request.event);
        if (event_plan != state.events.end() && event_plan->second.active_plan.valid()) {
            const auto plan = state.plans.find(event_plan->second.active_plan);
            if (plan != state.plans.end() && plan->second.state == PlanState::Active) {
                std::size_t in_flight = 0;
                for (const RecoveryStepId step_id : plan->second.order) {
                    if (plan->second.steps.at(step_id).state == StepState::InFlight) {
                        in_flight += 1;
                    }
                }
                if (in_flight < state.policy.max_in_flight_steps) {
                    for (const RecoveryStepId step_id : plan->second.order) {
                        if (step_is_dispatchable(plan->second, plan->second.steps.at(step_id))) {
                            work_remains = true;
                            break;
                        }
                    }
                }
            }
        }
        if (work_remains) {
            quiescent = false;
            report.notes.push_back("the dispatch budget for this call is exhausted");
            break;
        }
        quiescent = true;
        break;
    }
    report.quiescent = quiescent;

    const auto final_event = state.events.find(request.event);
    if (final_event != state.events.end()) {
        report.phase_after = final_event->second.phase;
        report.blocked = final_event->second.phase == EventPhase::Blocked;
    }
    if (state.plans.find(report.plan) != state.plans.end()) {
        const RecoveryPlan& plan = state.plans.at(report.plan);
        for (const RecoveryStepId step_id : plan.order) {
            const RecoveryStep& step = plan.steps.at(step_id);
            switch (step.state) {
                case StepState::Succeeded:
                    report.steps_succeeded += 1;
                    break;
                case StepState::Failed:
                    report.steps_failed += 1;
                    break;
                case StepState::Deferred:
                    report.steps_deferred += 1;
                    break;
                case StepState::Indeterminate:
                    report.steps_indeterminate += 1;
                    break;
                case StepState::InFlight:
                    report.steps_in_flight += 1;
                    break;
                default:
                    report.steps_pending += 1;
                    break;
            }
        }
    }
    const std::vector<internal::MailboxEntry> leftover = mailbox->drain();
    for (const internal::MailboxEntry& entry : leftover) {
        if (entry.is_completion && pending_exchanges > 0) {
            pending_exchanges -= 1;
        }
    }
    report.receipts_ignored += static_cast<std::uint32_t>(leftover.size());
    stats.receipts_ignored += leftover.size();
    return report;
}

Result<AdvanceReport> Coordinator::advance(const AdvanceRequest& request) {
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
    Result<AdvanceReport> report = impl.advance(request);
    if (!report.ok()) {
        lock.unlock();
        (void)impl.publish_notices();
        return report.status();
    }
    lock.unlock();
    const Status published = impl.publish_notices();
    if (!published.ok()) {
        return published;
    }
    return report;
}

}  // namespace drc
