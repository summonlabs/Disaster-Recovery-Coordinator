# Architecture

This document states what the Disaster Recovery Coordinator owns, how it is
built, and which invariants the test suites exist to defend. It describes the
implementation in this repository and nothing else.

## 1. Boundary

The coordinator owns **disaster-recovery orchestration state and sequencing
across complete facilities**: which sites an event covers, which obligations
must be preserved, in what order recovery may proceed, which delegated effects
were requested, and what the neighbours said about them.

It does not own, and never inspects:

* local incident truth (what actually failed inside a site),
* local site recovery internals,
* network topology, paths, or recovery (DFI),
* accelerator execution recovery (ASI),
* capacity truth, placement decisions, or reservation internals.

Those are consumed as three things only: **typed evidence** (a capability, a
readiness observation, a federation state), **identifiers** (placement and
reservation references minted elsewhere), and **receipts** for typed effect
requests. Nothing in this repository reaches into a neighbour's state, and no
neighbour is required to be this implementation: the effect port is an
interface, and a deployment supplies whatever speaks it.

The core question the boundary answers:

> When one or more sites become unavailable or materially degraded, what
> coordinated sequence can preserve protected obligations, move or restore them
> elsewhere, and return facilities to service without violating authority,
> dependencies, or recovery ordering?

If RTO/RPO-like targets are ever modelled they are inputs and reporting only.
This runtime makes no wall-clock guarantee: it records objectives and evidence,
and it refuses to claim an outcome it did not observe.

## 2. Layers

    include/drc/*.hpp        stable public API (installed)
    src/*.cpp                implementation, one internal codec set
    tools/drcctl.cpp         operating console (public API only)
    tools/drc_endpoint.cpp   reference participant process (wire protocol)
    tests/                   unit, integration, property, adversarial,
                             recovery, fencing, concurrency, multiprocess, CLI
    benchmarks/              measured cost of completed operations

Everything below the public API is deliberately boring: little-endian canonical
encodings, one SHA-256, one CRC-32C, explicit bounds, and no third-party runtime
dependency.

## 3. Model

| Object | Meaning | Owner |
| --- | --- | --- |
| `DisasterEventId` | one declared disaster, with a generation | coordinator |
| `RecoveryPlanId` | one plan generation for an event | coordinator |
| `RecoveryStepId` | one delegated effect in a plan | coordinator |
| `SiteId`, `FailureDomainId` | identity and failure-domain membership only | coordinator (identity), facility boundary (truth) |
| `ObligationId` | a protected obligation and its recovery state | coordinator |
| `DestinationCapability` | what a neighbour says a site can accept | neighbour |
| `SiteReadinessEvidence` | what a neighbour says about readiness | neighbour |
| `FederationState` | what the network authority says about reachability | neighbour |
| `PlacementRef`, `ReservationRef` | opaque tokens minted elsewhere | neighbour |
| `EffectRequest` / `EffectReceipt` | one delegated effect and its answer | coordinator issues, neighbour answers |

Recovery classes are ordered `safety_critical`, `protected`, `essential`,
`standard`, `deferrable`. The order is authoritative: it drives step ordering
and it decides which failures stop an event.

### Invariants

1. Identities are non-zero and never reused. Every authoritative object has a
   stable identity allocated from a durable watermark, so a restart cannot
   reissue one.
2. A plan is a DAG. Cycles are rejected at registration (obligations) and at
   plan build (steps); the canonical order is a Kahn walk with a deterministic
   priority `(recovery class, ordinal, identity)`.
3. Capacity is never overcommitted: the sum of the demands assigned to a
   destination never exceeds the capability evidence that destination
   supplied, using checked arithmetic.
4. A protected obligation is never given an invented destination. If no
   destination has fresh capability evidence, planning fails with
   `unsupported` and says which obligation. With explicit partial approval the
   plan is built and the obligation appears in `unplaced` with a diagnostic.
5. Nothing is silently dropped: every in-scope obligation is placed, unplaced,
   or deferred, with a durable reason in the plan's diagnostics.
6. Declaring a disaster is an explicit act. Missing telemetry is recorded as
   missing telemetry and never promoted into a declaration.

## 4. Authority, generations, and epochs

Three counters do the fencing:

* **Plan generation** — a new plan for an event supersedes the previous one.
  Its unfinished steps become `superseded`, and a receipt that names an older
  plan generation is refused (`stale_generation`).
* **Event generation** — moves with the active plan generation and names the
  authority a step was issued under.
* **Coordinator epoch** — bumped on every successful open. Every effect request
  carries the epoch it was issued under; every receipt must echo it. After a
  restart nothing from the previous incarnation can change state, and recovered
  evidence is `foreign_epoch` until a neighbour reports again.

Evidence carries its own generation. A lower generation is refused as `stale`
and recorded in `rejected_evidence`; the same generation with different content
is a `conflict`, which moves the affected event to `conflicted` instead of
picking a winner. A newer observation for the contested site (or a newer
assessment) is what clears it.

## 5. Lifecycle

    declared -> assessed -> plan_ready -> evacuating -> failing_over
      -> stabilized -> restoring -> validating -> returning -> closed

plus `blocked` and `conflicted`, which preserve the phase they interrupted.

* `declared` — `declare_disaster` with an explicit declarer, reason, and an
  affected site or failure domain. An overlapping declaration against a live
  event becomes `conflicted` unless it explicitly supersedes that event; a
  supersede closes the older event and cancels its unfinished steps.
* `assessed` — `record_assessment` records what the observatory reported,
  including the absence of telemetry.
* `plan_ready` — `create_plan` built and stored a plan generation.
* `evacuating` / `failing_over` — `begin_recovery` starts dispatch. The event
  moves to `failing_over` once every evacuation step succeeded, and to
  `stabilized` once every step in the recovery plan succeeded.
* `restoring` — `begin_restoration` builds a restoration plan of readiness
  validations. `validating` follows when they succeed.
* `returning` — returns are authorized one site at a time with
  `return_site_to_service`, which requires fresh, passing readiness evidence.
  The return-to-service step is created unauthorized and stays that way until
  that call: a plan is not a decision.
* `closed` — `close_event` requires the returning phase, a summary, and zero
  failed safety-critical steps. A recovery cannot be declared closed over a
  safety-critical step that did not succeed.
* `blocked` — a failed step, or attempts exhausted without a successful
  outcome. `advance` dispatches nothing while blocked and says so. Only
  `resolve_blocked` moves it: retry, or abandon with a recorded justification.
  Abandoning a safety-critical step is recorded and permanently blocks closure.
* `conflicted` — contradictory evidence. No dispatch happens while conflicted.

Failback is **not** an automatic reversal: `authorize_failback` is a separate
decision, requires the site to have been returned already and to have fresh
capability evidence, and produces its own plan generation with its own
`failback_to_source` steps.

## 6. Execution model and lock order

Authoritative state is single-threaded. One lock — a `std::shared_mutex` —
guards it. Mutating calls take it exclusively with `try_lock`, so a second
mutating call is refused with `busy` instead of racing; read-only views take it
shared.

Delegated effects are performed by a fixed worker pool. Workers never touch
coordinator state: they perform one blocking request/response exchange and
publish the result into a bounded mailbox that the engine thread drains. That
gives exactly three lock levels, always acquired in this order and never
released out of order:

    level 1  engine state      (std::shared_mutex)
    level 2  receipt mailbox   (std::mutex)
    level 3  participant pipe  (std::mutex, one per participant)

The order is **encoded**, not just documented: `internal::LockOrder` refuses
(and counts) any acquisition at a level that is not strictly greater than the
current one, and the counter is reported as
`CoordinatorStats::lock_order_violations`, which the test suite asserts stays
zero.

Consequences that the audit checklist asks about, and how they are answered:

* **read-lock to write-lock upgrade** — never attempted; a mutating call takes
  the exclusive lock from the start.
* **locks held across callbacks** — observers are invoked after the engine lock
  is released, with no lock held, so an observer may read state and may call
  back into read-only APIs. A throwing observer is caught and counted, never
  allowed to damage state.
* **emission while holding locks** — impossible: notices are published at the
  end of each public call, after `unlock`.
* **reversed lock ordering** — the tracker above; also, no path takes the
  mailbox lock and then the engine lock.
* **shutdown while holding locks workers need** — shutdown sets the flag, closes
  the mailbox (which wakes blocked producers), terminates participants (which
  unblocks blocked reads), then joins the workers. Nothing a worker needs is
  held by the joiner.
* **stale completion mutating a newer generation** — receipts are judged before
  they are applied: unknown request, foreign epoch, stale generation, duplicate,
  and not-applicable are all ignored and counted.
* **cancelled work publishing success** — receipts that arrive after shutdown
  are dropped into a closed mailbox and counted; nothing is applied.

## 7. Durable format

Two artifacts, both versioned, both checksummed, neither ever edited in place.

### Journal

An append-only file of frames. Each frame is an 80-byte header followed by the
payload:

    offset  0  u32  magic 'DRCJ'
    offset  4  u16  format version
    offset  6  u16  flags (must be zero)
    offset  8  u32  header size (80)
    offset 12  u32  payload length (bounded by 1 MiB)
    offset 16  u64  sequence (contiguous from 1)
    offset 24  u32  record type
    offset 28  u32  CRC-32C of the payload
    offset 32  u32  CRC-32C of the header with this field zeroed
    offset 36  u32  reserved (must be zero)
    offset 40  u64  reserved (must be zero)
    offset 48  32 x u8  chain value

The chain value is `SHA-256(previous chain || sequence || type || payload
length || payload)`, so a reordered, duplicated, or removed record breaks the
chain of everything after it.

The **durability boundary is the commit record**: appending buffers frames,
`commit` flushes them to the operating system and to stable storage and then
appends and flushes a `commit` record that carries the sequence it covers, the
state digest, the epoch, and the time. Everything before the last commit record
is authoritative; everything after it is not.

Record payloads are canonical encodings of the objects in section 3. The same
encoders produce the snapshot payload, so there is one decoder and one set of
bounds.

### Snapshot

A single file: a 112-byte header (magic `DDRC`, version, epoch, covered
sequence, payload length, state digest, payload digest, time, header CRC-32C)
followed by the canonical state encoding. It is written to a temporary file,
flushed, re-read and compared byte for byte, then atomically replaced, then the
directory is synced (POSIX) or the write-through rename is the boundary
(Windows). A failed snapshot leaves the previous one intact.

### Recovery

`Coordinator::open` performs, in order: take the single-writer lock (taking over
a lock whose owner process is gone, and never displacing a live owner); scan the
journal; load the snapshot if it is intact; replay committed records after the
snapshot's covered sequence; then **verify the checksum fixed point** — the
digest of the replayed state must equal the digest recorded in the last commit,
or the open fails with `corrupt`. Then the epoch is bumped (it must exceed the
last committed epoch), in-flight steps are marked `indeterminate`, effects that
can never be answered are recorded as unanswerable, and the coordinator is open
for business.

Damage classification:

* A frame that fails validation **and** is followed by no intact frame, and
  that begins at or after the last committed boundary, is an **uncommitted
  tail**: it is discarded, and the number of discarded bytes is reported in the
  scan report and in the statistics.
* Anything else — a damaged frame with intact data after it, a sequence gap, a
  broken chain, a failed checksum before the committed boundary — is **interior
  corruption**. Recovery refuses, names the offset, and leaves the file
  byte-identical. It never truncates through interior corruption and never
  reports a repaired state it did not verify.

### Compaction

`checkpoint` writes a snapshot, appends a snapshot reference, commits, and then
(when configured) rewrites the journal as header + snapshot reference + the
records appended after the snapshot. The rewrite goes to a temporary file that
is flushed, verified by replacement, and only then becomes the journal; the
committed boundary is never lowered and uncommitted records are never carried
into the new file.

Changing any of these layouts is a format version bump plus a migration test,
not an edit in place.

## 8. Planning

`build_plan` is a pure function of explicit inputs, which is why the ordering,
capacity, and dependency invariants can be tested without an engine, a clock,
or a filesystem. Determinism is a hard property: identical inputs produce
identical step identities, order, and digest.

1. Select obligations in scope (home site affected, or home failure domain
   affected) and sort them by recovery class, then identity.
2. Build candidate destinations from capability evidence: fresh under the
   active policy, outside the affected sites and failure domains, supporting
   the obligation's class, with capacity; ordered by most available capacity,
   then identity.
3. Place greedily with checked arithmetic, tracking remaining capacity per
   destination.
4. Emit steps per placed obligation: evacuate at the source, reserve and place
   at the destination, recover the destination's network path (one step per
   destination site), restore, recover execution (ASI), verify. A dependent
   obligation's restore depends on the verified recovery of what it depends on.
5. Unplaced obligations are reported with a reason. Everything that depends,
   directly or transitively, on an unplaced obligation is reported as deferred
   with a reason, and its steps are removed.
6. The step set is ordered by the canonical DAG walk and hashed into the plan
   digest.

With `require_protected_placement` (the default) a protected obligation with no
destination fails the whole call with `unsupported`. Callers that explicitly
accept a partial plan get the plan plus the unplaced set and its diagnostics.

## 9. Effects

A step becomes an `EffectRequest` addressed to exactly one neighbour: site
control plane, placement/reservation/capacity, ASI execution recovery, or DFI
network recovery. The request carries the event, plan, step, both generations,
the issuing epoch, the attempt number, and the capacity it needs.

The dispatch is **journaled and committed before it is sent**. A crash after a
neighbour acted can therefore never lose the fact that the request was made.
Delivery is at-least-once per attempt; repeated requests for the same step and
plan generation must be idempotent at the participant, and the reference
participant in `tools/drc_endpoint.cpp` is.

Receipts are judged before they are applied:

| Disposition | Meaning | Effect |
| --- | --- | --- |
| `applies` | names a live outstanding request for the current generation and epoch | applied |
| `duplicate` | a receipt for a request that already has one | ignored, counted |
| `unknown_request` | no outstanding request with that identity | ignored, counted |
| `stale_generation` | names an older plan generation, or the request's plan is no longer active | ignored, counted |
| `foreign_epoch` | issued by an earlier incarnation | ignored, counted |
| `not_applicable` | the step already settled | ignored, counted |

Outcomes map to step states honestly: `completed` succeeds a step; `rejected`,
`unsupported`, and exhausted attempts fail it and block the event; `deferred`
stays retryable; `accepted` and `unknown` mean the outcome is genuinely unknown
and the step stays retryable; `superseded` is treated as stale.

### Fractions and partitions

An exchange is bounded by `CoordinatorOptions::exchange_budget_nanos` (30
seconds by default). A participant that neither answers nor closes within that
budget yields an unknown outcome, not a hang and not a success; an exchange
that ended mid-frame marks that participant unusable so a desynchronised frame
boundary can never be read as data.

Logical partitions are explicit: `FederationState` says which failure domains
are unreachable. Steps whose destination lies in an unreachable domain are
`deferred` with that reason instead of being dispatched. When the federation
state is unknown, reachability is not assumed unless the policy explicitly says
to (`require_federation_evidence`).

## 10. What the tests defend

* unit: encodings, bounds, digests against published vectors, UTF-8, paths,
  counters, freshness, checked arithmetic.
* integration: lifecycle, dependency order, priority ordering, reopen at every
  phase, conflicts and supersedes, partitions, stale generations, fresh-
  evidence gates, separately governed failback, blocked resolution, multi-site
  and domain-wide failures.
* property: seeded generated topologies checked against every plan invariant,
  determinism, cycle rejection, generation fencing, repeated reopen stability.
* adversarial: truncation and bit-flip sweeps, interior corruption, damaged
  snapshots, hostile metadata, path traversal, reserved names, long paths.
* recovery: close/reopen digests, checkpoint contents, compaction, uncommitted
  tails, conflicted snapshots, the commit-digest fixed point, repeated cycles.
* fencing: evidence generations, epoch invalidation, policy generations, stale
  and foreign receipts, duplicate receipts, single-writer locking, uncommitted
  state invisibility.
* concurrency: observer re-entrancy, throwing observers, busy rejection, reader
  consistency during advancement, bounded silence, shutdown publication rules,
  lock-order violations.
* multiprocess: real participant processes, participant death, silent
  participants, duplicate and reordered replies, coordinator crash and restart
  at every transition, hard kill, lock takeover, two coordinators.
* cli: the console as a product — scripts, exit codes, verbatim output,
  determinism, restart through a real process.

No test uses a timeout, a watchdog, or kill-as-pass. A hang is a defect.

## 11. Platform support

Portable first-party C++20. Windows (MSVC and MinGW/clang) and Linux (GCC and
Clang) are built and tested; the durable-format code is endian-explicit and has
no platform-conditional behaviour. Platform-specific code is limited to file
I/O (durable flush, atomic replace, long-path prefixing), process creation, and
pipe reads, and each of those states its own durability boundary in the source.
