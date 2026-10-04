# Disaster Recovery Coordinator

`Disaster Recovery Coordinator` owns **disaster-recovery
orchestration state and sequencing across complete facilities**: which sites an
event covers, which protected obligations must survive, in what order recovery
may proceed, which delegated effects were requested from neighbouring
authorities, and what those authorities answered.

Version 1.0.1. C++20, CMake, no third-party runtime dependency, Apache-2.0.

## What it owns, and what it does not

| Owned here | Owned elsewhere (consumed as evidence, identifiers, or receipts) |
| --- | --- |
| disaster events, their phase and disposition | local incident truth and local site recovery internals |
| recovery plans, step DAGs, plan generations | capacity truth, placement decisions, reservation internals |
| protected obligations and their recovery state | accelerator execution recovery (ASI) |
| delegated effect requests and the receipts for them | network topology, paths, and network recovery (DFI) |
| the durable journal, snapshots, and recovery behaviour | whether a site is *really* ready: that is the site control plane's claim |

The architectural rule is that this runtime owns one thing exactly, consumes
neighbouring truth explicitly, never infers another runtime's authority, and
never absorbs another boundary merely because integration exists. Every
interaction with a neighbour is a typed request, a typed piece of evidence, or an
opaque identifier. There is no code path that reads or writes a neighbour's
state, and no neighbour is required to be any particular implementation.

## Quick start

```powershell
# Build (MSVC 2022 Build Tools + Ninja, from a developer prompt)
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release

# Run the suites
ctest --test-dir build/release --output-on-failure

# Drive an event with the console (see docs/OPERATIONS.md for the full runbook)
build/release/tools/drcctl.exe --dir C:\drc\prod domain --id 1 --name dc-a
build/release/tools/drcctl.exe --dir C:\drc\prod domain --id 2 --name dc-b
build/release/tools/drcctl.exe --dir C:\drc\prod site --id 1 --name a1 --domain 1 --capacity 100
build/release/tools/drcctl.exe --dir C:\drc\prod site --id 2 --name b1 --domain 2 --capacity 900
build/release/tools/drcctl.exe --dir C:\drc\prod obligation --id 10 --name auth --class safety_critical --home 1 --capacity 10
build/release/tools/drcctl.exe --dir C:\drc\prod capability --site 2 --generation 1 --capacity 500 --source capacity-authority
build/release/tools/drcctl.exe --dir C:\drc\prod endpoint --domain site_control_plane --id 1 --name synthetic
build/release/tools/drcctl.exe --dir C:\drc\prod declare --by oncall --reason "power loss" --sites 1
build/release/tools/drcctl.exe --dir C:\drc\prod plan --event 1 --by oncall
build/release/tools/drcctl.exe --dir C:\drc\prod begin --event 1
build/release/tools/drcctl.exe --dir C:\drc\prod advance --event 1 --repeat 8
build/release/tools/drcctl.exe --dir C:\drc\prod status --event 1
```

A library consumer does the same thing through the API:

```cpp
#include "drc/engine.hpp"

drc::CoordinatorOptions options;
options.directory = "C:/drc/prod";
options.clock = std::make_shared<drc::ManualClock>();  // or SystemClock
drc::Result<std::unique_ptr<drc::Coordinator>> opened = drc::Coordinator::open(options);
if (!opened.ok()) {
    return opened.status().to_string();   // typed: corrupt, locked, io, ...
}
std::unique_ptr<drc::Coordinator> coordinator = std::move(opened).value();

// One call is one durable transition: it is journaled and committed before it
// returns, and it is idempotent for identical content.
drc::Status status = coordinator->define_site(drc::SiteRecord{
    drc::SiteId{1}, "a1", drc::FailureDomainId{1}, 100});

// Advance is bounded by work, never by time: it returns with a report that says
// what was dispatched, what came back, and what is still outstanding.
drc::AdvanceRequest request;
request.event = event_id;
request.max_rounds = 8;
drc::Result<drc::AdvanceReport> report = coordinator->advance(request);
```

## Architecture

The runtime is a deterministic engine around a durable log:

    drcctl / embedding process
        |  public API (include/drc/*.hpp), Result<T>, no exceptions
        v
    +-----------------------------+        +----------------------------+
    | Coordinator (engine thread) |        | worker pool (fixed size)   |
    |  state + journal + snapshot |<------>|  one exchange at a time    |
    |  one shared_mutex           | mailbox|  never touches state      |
    +-----------------------------+        +-------------+--------------+
                 |                                      |
                 | typed EffectRequest                  | pipes
                 v                                      v
    site control plane / placement+reservation+capacity / ASI / DFI participants

Details, including the durable byte layout, the recovery algorithm, the effect
vocabulary, and the lock order, are in [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).
Operating procedures are in [`docs/OPERATIONS.md`](docs/OPERATIONS.md).

## Data model and invariants

* `DisasterEventId` — a declared disaster with a generation, a phase, and a
  disposition. Declaration is explicit; missing telemetry is never enough.
* `RecoveryPlanId` — one plan generation for an event, containing a step DAG, a
  canonical order, placements, and diagnostics.
* `RecoveryStepId` — one delegated effect: evacuate, reserve, place, restore,
  recover execution, recover network, verify, validate readiness, return to
  service, or fail back.
* `ObligationId` — a protected obligation with a recovery class and dependencies.
* `SiteId`, `FailureDomainId` — identity and failure-domain membership only.
* `DestinationCapability`, `SiteReadinessEvidence`, `FederationState` — explicit
  neighbour evidence with generations and observation epochs.
* `EffectRequest` / `EffectReceipt` — the typed request and the neighbour's own
  answer, each carrying the generations and epoch they are valid under.

Invariants the tests defend:

1. Identities are non-zero, never reused, allocated from durable watermarks.
2. A plan is a DAG: cycles are rejected at registration and at plan build; the
   canonical order is deterministic.
3. Capacity is never overcommitted against the evidence supplied, with checked
   arithmetic.
4. A protected obligation is never given an invented destination: with no fresh
   capability evidence the call fails with `unsupported` and names it, or the
   obligation is reported as unplaced with a reason.
5. Nothing is silently dropped: in-scope obligations are placed, unplaced, or
   deferred, each with a durable reason.
6. Protected obligations keep priority in every ordering decision.
7. Dependency order is enforced before a step is dispatched.
8. A stale plan generation cannot execute: its steps are `superseded` and its
   receipts are refused.
9. Duplicate effects are idempotent or explicitly rejected; duplicate receipts
   are ignored and counted.
10. Partial completion is durable: every dispatch is committed before it is sent.
11. A safety-critical step that failed or was abandoned blocks closure, always.
12. Return to service requires fresh, passing readiness evidence.
13. Recovered evidence is not automatically fresh: the epoch is bumped on open.
14. Failback is separately governed, never an automatic reversal.
15. Simultaneous multi-site and domain-wide failures are supported.
16. Behaviour under federation partition is explicit: unreachable destinations
    defer, and unknown reachability is not assumed unless policy says so.

## Authority, generations, and epochs

Three counters fence everything that can go stale. A plan generation fences
steps and receipts; an event generation names the authority a step was issued
under; a coordinator **epoch** is bumped on every successful open, so nothing
issued by a previous incarnation can change state and no evidence from a
previous incarnation is fresh. Evidence generations fence observations: older is
`stale`, equal-but-different is `conflict` (the event becomes `conflicted` and
dispatch stops until a newer observation arrives).

## Lifecycle

    declared -> assessed -> plan_ready -> evacuating -> failing_over
      -> stabilized -> restoring -> validating -> returning -> closed

with `blocked` and `conflicted` preserving the phase they interrupted. Blocked
events dispatch nothing until `resolve_blocked` records an operator decision;
conflicted events dispatch nothing until newer evidence arrives. Failback is a
separate authorization that requires the site to have been returned already.

## Persistence and recovery

Durable state is an append-only journal plus a snapshot. Records are
chain-hashed and checksummed; the durability boundary is a commit record, and
every state-changing call ends at one. Recovery distinguishes a recoverable
uncommitted tail (discarded, with the byte count reported) from interior
corruption (refused, with the offset reported, file left byte-identical), then
verifies the **checksum fixed point**: the digest of the replayed state must
equal the digest the last commit recorded. In-flight effects become
`indeterminate` after a restart, never successful.

See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) section 7 for the byte layout
and [`docs/OPERATIONS.md`](docs/OPERATIONS.md) section 7 for what to do when a
file is refused.

## Build, test, install

```powershell
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
ctest --test-dir build/release --output-on-failure

# install and validate an independent downstream consumer
cmake --install build/release --prefix _install
pwsh -File scripts/verify_install.ps1 -BuildDir build/release -Prefix _install -SkipBuild
```

```bash
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
ctest --test-dir build/release --output-on-failure
bash scripts/verify_install.sh build/release _install Release
```

Options: `DRC_BUILD_TESTS`, `DRC_BUILD_TOOLS`, `DRC_BUILD_BENCHMARKS`,
`DRC_WARNINGS_AS_ERRORS` (default ON), `DRC_ENABLE_SANITIZERS` (GCC/Clang).
First-party warnings are errors: `-Wall -Wextra -Wpedantic -Wshadow
-Wconversion -Wsign-conversion -Werror` on GCC/Clang, `/W4 /WX /permissive-
/utf-8 /Zc:__cplusplus` on MSVC.

### Continuous integration

`.github/workflows/ci.yml` is the same build, test and install path on eight
jobs: MSVC Debug and Release on Windows, GCC Debug and Release, clang Debug and
Release, clang with AddressSanitizer and UndefinedBehaviorSanitizer, and a fresh
clone of the commit built and tested in a clean directory. Every job also runs
the suite as ten CTest areas, so a failure names the area it came from.

The Linux jobs are the only place the POSIX half of the process layer, the file
layer and the harness is compiled and executed. They found five defects there on
their first run, and two more in the harness itself; all seven are listed under
"Validation performed" with what caused them.

### Testing rules

There is no timeout, watchdog, or kill-as-pass anywhere in this repository: not
in CTest, not in the workflows, not in the scripts. A hanging test is a defect
to diagnose, and the suites are expected to complete naturally. The same rule
applies to the tests themselves: their loops are bounded by work (attempts,
rounds, dispatch budgets), never by wall-clock time.

### Consuming the installed package

```cmake
find_package(DisasterRecoveryCoordinator 1.0 REQUIRED)
target_link_libraries(your_target PRIVATE
    DisasterRecoveryCoordinator::disaster_recovery_coordinator)
```

`tests/downstream/` is a standalone project that does exactly this and is built
only against an installed prefix, never against the build tree; the scripts
above run it.

## Command line

`drcctl` is the coordinator console and uses only the public API:

| Command | Purpose |
| --- | --- |
| `domain`, `site`, `obligation`, `policy` | register authoritative objects |
| `capability`, `readiness`, `federation` | record neighbour evidence |
| `endpoint` | register a participant, in-process or as a child process |
| `declare`, `assess`, `plan`, `begin` | drive an event to recovery |
| `advance` | bounded dispatch and receipt collection, with a report |
| `restore`, `return`, `failback`, `close` | stabilized, validating, returning, closed |
| `resolve` | resolve a blocked event (retry or abandon, with justification) |
| `checkpoint`, `compact`, `verify`, `dump`, `status` | durability and inspection |
| `script` / `--script FILE` | run a deterministic command script |

`drc_endpoint` is the reference participant process: it reads typed requests on
stdin, answers with receipts on stdout, and can be told to reject, defer,
answer unknown, stay silent, duplicate, reorder, answer with a stale generation
or a foreign epoch, delay, or die mid-stream. Every one of those behaviours is
exercised by the suites.

## Platform support and limitations

| Platform | Status |
| --- | --- |
| Windows x64, MSVC 19.4x | built and tested, Debug and Release, plus AddressSanitizer |
| Windows x64, MinGW GCC 14 and MinGW clang 19 | built and tested |
| Linux x64, GCC 13 | built and tested in CI, Debug and Release |
| Linux x64, clang 18 | built and tested in CI, Debug and Release |
| Linux x64, clang with ASan + UBSan | built and tested in CI, including leak detection |

Honest limitations, stated rather than hidden:

* **No wall-clock guarantee.** The coordinator sequences recovery and records
  objectives; it does not promise an RTO. Nothing here claims a completion time
  it did not measure.
* **The participants are yours to provide.** `drc_endpoint` is a reference
  implementation of the wire protocol used by the tests, not a facility
  controller. A real deployment speaks to its own site control plane,
  placement/capacity authority, ASI, and DFI.
* **At-least-once effects.** Delivery is at-least-once per attempt: a
  participant must treat repeated requests for the same step and plan generation
  as idempotent. The reference participant does; a participant that does not
  can double-apply an effect.
* **One coordinator per journal.** The lock file enforces it. Two coordinators
  cannot share a directory, and that is a feature: two would both believe they
  hold authority.
* **Federation partition behaviour is a policy choice.** With no federation
  observation the default assumes no partition; deployments that must not assume
  reachability set `require_federation_evidence` and every remote effect waits
  for an explicit observation.
* **Windows directory flushing.** `atomic_replace` uses a write-through rename,
  which is the documented durability boundary on NTFS; POSIX also fsyncs the
  directory. The difference is stated in the source where it matters.
* **UUID-free identities.** Identities are 64-bit counters, not random; they are
  unique within a journal directory, which is the only scope they are used in.
* **SIGPIPE is ignored on POSIX.** The first participant spawn replaces the
  *default* disposition of `SIGPIPE` with "ignore", so writing to a participant
  that has already exited returns `EPIPE` and becomes an unknown result rather
  than killing the process. A host that installed its own handler keeps it; the
  change is stated in `include/drc/process.hpp` where it happens.
* **Mutating calls do not block.** A call that finds another call in progress
  answers `busy` instead of waiting, and a caller that must make progress
  retries it. This is deliberate — it keeps one slow caller from freezing the
  console — and the test harness retries it the same way an operator script
  would.
* **One toolchain caveat, reproduced and stated.** MinGW clang 19 (WinLibs)
  cannot run `thread_local` storage at all: a ten-line program that writes a
  `thread_local int` dies with an access violation, with and without
  `-fno-emulated-tls`, while MSVC and MinGW GCC run the same program correctly.
  Per-thread nesting depth is exactly what `thread_local` is for, so on that one
  toolchain the lock-order tracker keeps no per-thread state and reports no
  violations instead of crashing; the lock order itself is unchanged, and the
  tracker is exercised for real by MSVC and GCC on Windows and by GCC and Clang
  on Linux (including under ASan and UBSan).
* **Long paths.** Windows extended-length paths are supported through the
  library's own file helpers, which apply the `\\?\` prefix for the whole
  operation. The standard filesystem library is not used for directory creation
  or tree removal, because beyond MAX_PATH it is unreliable — with MinGW
  libstdc++ a `remove_all` on such a path does not terminate.

## Relationship to adjacent boundaries

* **Site control plane** — the coordinator asks it to evacuate, restore, verify,
  validate readiness, return a site to service, and fail work back. It never
  performs any of those itself and never reads the site's internal state.
* **Placement, reservation, and capacity** — the coordinator consumes capacity
  observations, asks for reservations and placements, and stores the references
  that come back as opaque identifiers. It never decides placement itself and
  never edits a reservation.
* **ASI (accelerator execution recovery)** — execution recovery after a move is
  a request; the coordinator records the step and its receipt.
* **DFI (network recovery)** — recovering the path to a destination is a request,
  and the federation state that says which failure domains are reachable is
  evidence.
* **Earlier DCCP boundaries** (facility identity, topology, assets, capacity,
  power, cooling, lifecycle, policy, tenancy, failure/recovery, observability,
  economics) are consumed the same way: as identifiers, evidence, and receipts.

## Repository layout

    include/drc/    stable public API (installed)
    src/            implementation and the internal durable codecs
    tools/          drcctl (console) and drc_endpoint (reference participant)
    tests/          suites by area, a shared harness, and a downstream consumer
    benchmarks/     measured cost of completed operations
    docs/           ARCHITECTURE.md, OPERATIONS.md
    scripts/        install/downstream and fresh-clone validation
    cmake/          package config for find_package
    .github/        cross-platform CI (Windows/MSVC, Ubuntu/GCC, Ubuntu/Clang, sanitizers)

## Validation performed

Everything below was run on the machine that produced this repository, with the
exact commands shown, and the numbers are the ones the tools printed. Nothing in
this section is extrapolated.

Every job in `.github/workflows/ci.yml` is required to pass on the commit this
release points at. These are the results of that run, followed by the local runs
used while developing:

| Configuration | How it runs | Result |
| --- | --- | --- |
| Ubuntu 24.04, GCC 13.3, Debug and Release | CI: `ubuntu / gcc / <config>` | **82 passed, 0 failed, 11,330 checks** per job |
| Ubuntu 24.04, clang 18, Debug and Release | CI: `ubuntu / clang / <config>` | **82 passed, 0 failed** per job |
| Ubuntu 24.04, clang, AddressSanitizer + UBSan | CI: `-DDRC_ENABLE_SANITIZERS=ON`, `detect_leaks=1`, `halt_on_error=1` | **82 passed, 0 failed**, no sanitizer report |
| Ubuntu 24.04, fresh clone of the commit | CI: `ubuntu / fresh clone from the commit` | **82 passed, 0 failed** inside the clone |
| Ubuntu 24.04, package install + downstream consumer | CI: `bash scripts/verify_install.sh build _install Release` | configure, build, link and run against `_install`: `final phase: stabilized, steps: 7` … `consumer ok`, exit 0 |
| Windows, MSVC 19.44, Debug and Release | CI: `windows / msvc / <config>` | **82 passed, 0 failed** per job, plus the PowerShell install check |
| Windows x64, MSVC 19.44, Release | local: `build\msvc\tests\drc_tests.exe` | **82 passed, 0 failed, 11,329 checks**, 31 s |
| Windows x64, MSVC 19.44, AddressSanitizer | local: `-DDRC_ENABLE_MSVC_ASAN=ON`, `ASAN_OPTIONS=halt_on_error=1` | **82 passed, 0 failed**, no sanitizer report |
| Windows x64, MinGW GCC 14.2 and MinGW clang 19.1, Release | local: `-DCMAKE_CXX_COMPILER=g++` / `clang++` | **82 passed, 0 failed, 11,329 checks** on both |
| CTest, all ten areas | local: `ctest --test-dir build/msvc --output-on-failure` | **10/10 passed** |
| Fresh clone on Windows | local: `scripts/fresh_clone_check.ps1 -Ref HEAD` | **82 passed, 0 failed** in the clone |

The Linux jobs are not decoration: they are the only place the POSIX half of the
process layer, the file layer and the test harness is compiled and executed, and
the first run of this workflow found five defects in that layer and two in the
test harness itself (see below). The Windows jobs caught the same classes of
problem on their side, including one that MSVC's checked iterators reported and
nothing else did.

Tests are plain runs with no timeout, no watchdog, and no kill-as-pass rule,
anywhere: not in CTest, not in the scripts, not in the workflows. The suites
assert on observable values (typed error codes, byte offsets, digests, phases,
step states, receipt dispositions) and print the values that broke an invariant.

### Defects this work found and fixed

These are the real ones, kept here because the fixes are the interesting part:

1. **SHA-256 buffer overflow.** The block loop could leave the 64-byte buffer
   full without compressing, so a following byte wrote out of bounds. Found by
   the one-million-character test vector.
2. **Windows participant spawn closed the transport.** `Child::spawn` called the
   full pipe cleanup after a successful `CreateProcessW`, nulling the parent's
   handles before storing them, so every spawned participant was unreachable and
   every delegated step went indeterminate. Found by the multiprocess suite.
3. **Journal sequence gap after truncating an uncommitted tail.** A journal whose
   only committed content was its header resumed numbering from the record it
   had just discarded, so the next open reported interior corruption and the
   directory became permanently unusable. Found by the crash-after-append matrix.
4. **Duplicate receipts were unreachable.** The outstanding-request set was
   consulted before the receipt index, so a second answer for an already
   answered request was classified as an unknown request. Found by the fencing
   suite.
5. **Long paths were refused, then mis-created.** Extended-length paths walked
   the `\\?\` prefix as if it were a directory. Replaced with a recursive
   create that names the path in its errors.
6. **Two console defects.** An unknown command exited without printing anything,
   and every flag after the command word was swallowed by the global parse, so
   documented commands with options could not be used from the command line.
7. **Quiescence was reported while work remained.** A bounded `advance` could say
   "nothing left to do" when the only reason nothing happened was its own
   dispatch budget.
8. **Out-of-bounds read in the truncation sweep.** One sample was the byte after
   the last frame; the release build read past the buffer. Found by MSVC's
   checked iterators in the Debug build.
9. **Silence was treated as a hang risk.** A participant that neither answered
   nor closed left the engine waiting forever. Exchanges now have an explicit
   operational budget, an unanswered exchange becomes an unknown outcome, and a
   half-read frame quarantines that participant instead of desynchronising the
   stream.
10. **POSIX truncation left a hole in the journal.** `ftruncate` does not move the
    file offset, so the append that follows a repair started at the old end of
    file and the kernel filled the gap with zeros: every repaired journal came
    back as interior corruption on Linux. Windows positions explicitly before
    `SetEndOfFile`, which is why only one platform was affected. The POSIX path
    now seeks to the truncated size. Found by the Linux jobs.
11. **The POSIX process layer did not compile, and could not have been caught
    locally.** A reaping helper was defined after its use, and `running()`
    assigned to members from a `const` method. Neither Windows build compiles
    that branch at all. Found by the Linux jobs.
12. **The liveness query consumed the child's exit status.** `running()` called
    `waitpid(WNOHANG)`, which reaps: the status was discarded, so a later `wait()`
    failed with `ECHILD` and the outcome of the process was lost forever. It is a
    read-only existence check now, exactly like `GetExitCodeProcess`.
13. **Diagnostics could block, and killed children stayed zombies.**
    `read_stderr` waited on an empty pipe where Windows peeks, and the destructor
    killed a child without reaping it. Both fixed; descriptors are also opened
    with `O_CLOEXEC` so a participant never inherits the coordinator's journal.
14. **Nothing ran on Linux at all.** The console and the participants were driven
    through `cmd.exe` and `.cmd` wrappers, so on Linux every one of those tests
    failed with exit code 127 before it tested anything. Script creation, quoting
    and the shell invocation now live in the shared harness and work on both
    platforms.
15. **A crash had no author.** The test runner flushed its log after a test
    finished, so a test that aborted the process left no record of which test it
    was. The `RUN` line is flushed before the test starts.
16. **A concurrent `Busy` was treated as a failure, and a failing requirement
    terminated the process.** `Busy` is the documented answer when another caller
    holds the engine lock, so the harness retries it with a bound on attempts
    rather than on wall-clock time; and threads now live in a group that joins
    them on scope exit, so a failing requirement is reported instead of unwinding
    into `std::terminate` and hiding the failure that caused it.
17. **A participant that died mid-exchange killed the coordinator.** Writing to a
    process that had already exited raises `SIGPIPE`, whose default action
    terminates the program: a transport failure became a crash. The default
    disposition is now replaced with "ignore" once, so the write reports
    `EPIPE` and the exchange becomes an unknown outcome that fences the step.
    A host that installed its own handler keeps it.
18. **Four readers could starve the writer.** The engine's shared lock is
    reader-preferring, so a single mutating call under continuous read load kept
    meeting its own documented `Busy` answer until the retry bound ran out. The
    retry loop yields so readers can finish their critical sections, and the
    stress readers yield as well; the bound stays on attempts, never on
    wall-clock time.

### What is *not* claimed

* No RTO/RPO guarantee, and no claim about recovery time under real load.
* The participants in this repository are reference implementations of the wire
  protocol, not facility controllers; a real deployment supplies its own.
* Effects are at-least-once per attempt. A participant that is not idempotent
  for a repeated request can apply an effect twice.
* **The transport is pipes, not sockets.** Participants speak a framed protocol
  over stdin and stdout, which is what the boundary needs and what the tests
  exercise on both platforms. There is no socket code in this repository, and no
  claim is made about one.
* **Benchmark numbers come from one Windows machine**, and every durable figure
  is dominated by the storage flush; they are throughput indicators, not service
  levels.

## Benchmarks

`benchmarks/benchmark_main.cpp` measures completed operations with
`std::chrono::steady_clock`. Every number below is a measurement, no estimate:
the fixture is 2 failure domains, 8 sites (4 affected, 4 surviving), 64
protected obligations with 15 dependency edges, 4 in-process synthetic
endpoints, and a produced plan of **388 steps** with 64 placements, 0 unplaced
and 0 deferred — read back from the stored plan, not assumed.

Windows x64, MSVC 19.44, Release, 16 hardware threads, 4 worker threads:

| Benchmark | Iterations | Per operation | What it measures |
| --- | --- | --- | --- |
| `create_plan` | 16 | **3.71 ms** | one plan build for 388 steps plus its plan record and commit |
| `plan_step_advance` | 1,164 steps | **2.98 ms/step** | time inside `advance()` per step that reached succeeded |
| `plan_completion` | 3 runs | **1.156 s** | one whole 388-step plan, including waiting for the asynchronous workers |
| `checkpoint` | 16 | **21.4 ms** | one snapshot write over the full state plus its reference record and commit |
| `durable_commit` | 256 | **1.76 ms** | one state-changing call: one journal append plus one commit flushed to storage |

Run it with `build/msvc/benchmarks/drc_benchmarks.exe` (`--quick` for the short
form used by CTest, `--iterations N` to bound a run).

Honest notes about these numbers:

* Every durable figure is dominated by the storage flush. One plan run performs
  554 commits, which is why `plan_completion` is more than a second on this
  machine, and why `plan_step_advance` is a durable-throughput number rather
  than a CPU number.
* `plan_completion` includes real waiting for the participant exchanges, so it is
  end-to-end wall clock, not processing time.
* Run-to-run variance on a shared Windows box is large for `checkpoint`
  (13-30 ms for identical state in consecutive runs); treat single samples as
  indicative and repeat before drawing conclusions.
* Duplicate and reordered replies, silence, and participant death are measured
  by the test suites, not by these benchmarks.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
