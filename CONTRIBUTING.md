# Contributing to Disaster Recovery Coordinator

## What this project is

Disaster Recovery Coordinator is a DCCP boundary implementation: it owns
disaster-recovery orchestration state and sequencing across complete facilities,
and nothing else. It does not own local incident truth, local site recovery
internals, network recovery, accelerator execution recovery, capacity
brokerage, placement planning, or reservation internals. It consumes explicit
capability/evidence and issues typed requests to those boundaries.

Before changing behaviour, read `docs/ARCHITECTURE.md`. It states the
invariants the tests exist to defend.

## Build and test

```powershell
# Configure and build (MSVC 2022 Build Tools + Ninja)
cmd /c '"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 && cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build/release'

# Run the whole suite
build/release/tests/drc_tests.exe

# Run one test by name fragment
build/release/tests/drc_tests.exe --filter <fragment>

# Run through CTest
ctest --test-dir build/release --output-on-failure
```

The build treats first-party warnings as errors. A change that introduces a
warning is not ready.

On a toolchain that supports it, configure with `-DDRC_ENABLE_SANITIZERS=ON`
to build the suites under AddressSanitizer and UndefinedBehaviorSanitizer.

Do not add a timeout, watchdog, or process-kill-as-pass rule to any test or
validation command. A hang in this project is a defect to diagnose.

## Registering a test

Add a file named `tests/suite_<topic>.cpp`. The CMake glob picks it up after a
reconfigure. Use the framework in `tests/test.hpp`:

```cpp
#include "test.hpp"

DRC_TEST(a_stale_plan_generation_cannot_dispatch) {
    DRC_REQUIRE_EQ(some_observation.state, StepState::Pending);
}
```

A test must assert an invariant and must print the values that broke it. A test
that cannot fail is not a test.

## The durable format is frozen

The journal framing, the record chain, the snapshot layout, and the canonical
encodings are a compatibility surface. Changing any of them requires:

1. a format version bump in `include/drc/journal.hpp`;
2. a written migration statement in `docs/ARCHITECTURE.md`;
3. a test that opens a journal written by the previous version.

## Style

* C++20, no exceptions in the public API: every fallible operation returns
  `Result<T>` and every error carries a stable machine-readable code.
* No undefined behaviour: no unchecked narrowing, no signed overflow, no
  uninitialised reads, no reliance on iterator invalidation.
* Deterministic output: canonical state must never depend on hash iteration
  order, the wall clock, thread timing, or a random device.
* Comments explain why. A comment that restates the code is noise.
* One lock order per process. If you add a second mutex, document the order in
  `docs/ARCHITECTURE.md` and prove there is no inversion.
* No AI attribution and no `Co-authored-by` trailers in commits.

## Review expectations

A change is ready when:

* the full suite passes from a clean build directory;
* a new or changed invariant has a test that fails without the change;
* the durable format is unchanged, or the migration rules above are satisfied;
* `README.md` and `docs/` still describe reality.

## License

Contributions are accepted under the Apache License 2.0. There is no CLA. By
submitting a change you agree that it may be distributed under those terms.
