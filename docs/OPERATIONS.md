# Operations

This is the runbook for operating the coordinator: what to run, what the output
means, and what to do when something is wrong. Every command here is exercised
by the test suite.

## 1. Processes

A deployment has one coordinator process per journal directory and one
participant process per neighbouring boundary:

    drcctl        the coordinator console (and the coordinator itself)
    drc_endpoint  a reference participant: site control plane, placement/
                  reservation/capacity, ASI execution recovery, or DFI network
                  recovery

Participants are ordinary programs that speak the framed pipe protocol in
`include/drc/process.hpp` (a 32-bit little-endian length followed by a canonical
payload). A real neighbour can be anything that speaks it; a deployment that
does not want child processes registers everything in-process and uses the
synthetic participant instead.

## 2. Directories and files

    <dir>/coordinator.journal    append-only durable history
    <dir>/coordinator.snapshot   the last checkpoint (regenerable)
    <dir>/coordinator.lock       single-writer lock, owner pid + epoch

Back up the whole directory. The snapshot alone is not a backup: the journal
that follows it is what makes the state current. Losing the snapshot is
survivable (the journal still carries the history until it is compacted);
losing the journal is not.

## 3. Day-one setup

    drcctl --dir C:\drc\prod domain  --id 1 --name dc-a
    drcctl --dir C:\drc\prod domain  --id 2 --name dc-b
    drcctl --dir C:\drc\prod site    --id 1 --name a1 --domain 1 --capacity 400
    drcctl --dir C:\drc\prod site    --id 2 --name b1 --domain 2 --capacity 4000
    drcctl --dir C:\drc\prod obligation --id 10 --name auth \
        --class safety_critical --home 1 --capacity 40
    drcctl --dir C:\drc\prod policy --freshness-ns 300000000000 --attempts 3 \
        --in-flight 8 --required-check power --required-check network
    drcctl --dir C:\drc\prod endpoint --domain site_control_plane --id 1 \
        --name scp --exec C:\drc\bin\site-control-plane.exe

Registration is idempotent: repeating a command with identical content is a
no-op, and repeating it with different content is refused with `conflict`
rather than silently overwriting an authoritative object.

## 4. Evidence

Capacity and readiness are observations, not guesses:

    drcctl --dir C:\drc\prod capability --site 2 --generation 17 --capacity 900 \
        --classes safety_critical,protected --source capacity-authority
    drcctl --dir C:\drc\prod readiness  --site 1 --generation 9 --source scp \
        --check power=pass --check network=pass
    drcctl --dir C:\drc\prod federation --generation 4 --source dfi --partition \
        --unreachable 2

Rules that matter in an incident:

* a lower generation is refused as `stale` and recorded in `rejected_evidence`;
* the same generation with different content is a `conflict`, and the event
  becomes `conflicted` until a newer observation arrives;
* evidence observed by a previous incarnation of the coordinator (a previous
  epoch) is never fresh, so after a restart every neighbour must report again;
* `unknown` is not `fresh`: missing evidence never silently satisfies a gate.

## 5. Running an event

    drcctl --dir C:\drc\prod declare --by oncall --reason "domestic power loss" --sites 1
    drcctl --dir C:\drc\prod assess  --event 1 --generation 1 --source observatory \
        --site 1:unavailable:present
    drcctl --dir C:\drc\prod plan    --event 1 --by oncall
    drcctl --dir C:\drc\prod begin   --event 1
    drcctl --dir C:\drc\prod advance --event 1 --rounds 8 --repeat 8
    drcctl --dir C:\drc\prod status  --event 1

`advance` prints one line per call: phase, dispatches, receipts applied and
ignored, per-state step counts, and whether the engine is quiescent. It is
bounded by work, never by time: `--rounds` and `--dispatches` cap what one call
may do, and `quiescent=1` means there is nothing left to do until a neighbour
answers or an operator acts.

Returning sites to service is deliberate and per site:

    drcctl --dir C:\drc\prod restore --event 1 --by oncall
    drcctl --dir C:\drc\prod advance --event 1 --repeat 4
    drcctl --dir C:\drc\prod return  --event 1 --site 1 --by oncall \
        --justification "readiness verified by the site control plane"
    drcctl --dir C:\drc\prod advance --event 1 --repeat 4
    drcctl --dir C:\drc\prod close   --event 1 --by oncall --summary "site 1 back in service"

Failback is a separate decision, never an automatic reversal:

    drcctl --dir C:\drc\prod failback --event 1 --site 1 --by oncall \
        --justification "site 1 confirmed stable for 24h"

## 6. When an event is blocked

`status` prints `phase=blocked` and the step list names what failed. A
safety-critical failure cannot be skipped and cannot be closed over.

    drcctl --dir C:\drc\prod status --event 1
    # retry the step once the neighbour is healthy again
    drcctl --dir C:\drc\prod resolve --event 1 --step 42 --retry --by oncall \
        --justification "placement authority recovered"
    # or abandon it, with a recorded justification (this permanently blocks closure
    # if the step is safety critical)
    drcctl --dir C:\drc\prod resolve --event 1 --step 42 --by oncall \
        --justification "obligation retired by the business"

`phase=conflicted` means two authoritative claims disagree. Nothing is
dispatched while conflicted. Publish a newer observation for the contested site,
or record a newer assessment, and the event returns to the phase it interrupted.

## 7. Inspecting and repairing durable state

    drcctl --dir C:\drc\prod verify      # read-only scan of the journal
    drcctl --dir C:\drc\prod checkpoint  # write a snapshot
    drcctl --dir C:\drc\prod compact     # checkpoint plus journal rewrite
    drcctl --dir C:\drc\prod dump        # canonical state, one line per object

`verify` prints the file size, the committed size, how many bytes would be
discarded as an uncommitted tail, the record count, the last committed sequence,
and the chain digest. It never modifies the file.

Interpreting a refusal to open:

* `corrupt ... offset N ...` — interior corruption. The file is left untouched.
  Restore the directory from backup, or accept the loss of everything after the
  last snapshot and rebuild the journal by hand; do not truncate through the
  damaged record.
* `locked ...` — another process holds the journal. `verify` and `dump` work
  without the lock; a second coordinator does not, by design. If the recorded
  owner process is gone, the next `open` takes the lock over automatically and
  says so; if it is alive, stop it first.
* `format_unsupported` — the file was written by a newer format version. Use a
  coordinator that understands it; do not edit the file.
* A torn tail is not an error: it is reported as discarded bytes and the
  committed prefix is used.

## 8. Restarts and upgrades

* Restarting is safe at any point: state is rebuilt from the snapshot plus the
  committed journal, the checksum fixed point is verified, the epoch is bumped,
  and effects that were in flight become `indeterminate` so they are retried
  rather than assumed.
* Effects that were in flight are never reported as complete after a restart,
  and answers from the previous incarnation are refused as `foreign_epoch`.
* Upgrading the binary is an upgrade of a format-versioned artifact. The
  version is checked before anything is written; a mismatch is refused, not
  guessed.
* There is no daemon, no scheduler, and no timer: `advance` is driven by the
  operator, a supervisor, or a script. Nothing happens while nobody calls.

## 9. Scripting

`drcctl --script FILE` executes one command per line (`#` starts a comment) and
prints a deterministic line per command, which makes it usable from a
supervisor and assertable from a test:

    #!/usr/bin/env drcctl --script
    declare --by oncall --reason "maintenance window" --sites 1,2
    assess --event 1 --generation 1 --source observatory --site 1:degraded:present
    plan --event 1 --by oncall
    begin --event 1
    advance --event 1 --rounds 8 --repeat 8
    checkpoint
    verify

Exit code 0 means every command succeeded; a failing command prints `error
<code> <message>` and stops the script unless `--continue-on-error` is given.

## 10. Nobody is watching: how silence behaves

* Each exchange with a participant is bounded by
  `--exchange-budget-ms` (30 seconds by default). Silence becomes an unknown
  outcome, and the step stays retryable until its attempt budget is spent.
* Attempts are bounded by `policy.max_step_attempts`. Running out is a failure
  that blocks the event; it is never an indefinite pause and never a success.
* Shutdown cancels outstanding work: nothing that had not completed is reported
  as complete, and receivers of a cancelled effect see no success receipt.
