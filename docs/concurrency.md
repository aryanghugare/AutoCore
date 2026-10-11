# Concurrent execution

`ConcurrentSimulation` runs Engine, Brake, Dashboard, and diagnostics on four
fixed `std::thread` workers. It owns their bus, inboxes, behaviors, store, and
threads. No worker is detached. Existing ECU APIs remain deterministic when
called directly with injected timestamps; the default CLI still uses that mode.
Run `./build/vaecu_sim --concurrent` for the real-clock demonstration.

## Ownership and synchronization

| State | Owner / synchronization |
| --- | --- |
| Engine behavior and inputs | Engine worker only; initial inputs copied from config |
| Brake behavior and events | Brake worker only |
| Dashboard behavior | Dashboard worker only |
| Diagnostic detection and its duplicate cache | Diagnostic worker only |
| Bus registration, send order, closure | Bus mutex |
| Inbox ring, pending count, closure | One mutex and condition variable per inbox |
| Diagnostic records and event FIFO | Store mutex; condition variable for event readers |
| Published Dashboard snapshot | Runtime snapshot mutex and condition variable |
| Lifecycle and thread creation/join | Runtime lifecycle mutex; workers never acquire it |
| Startup readiness and release | Runtime startup mutex and condition variable |
| First worker failure | Runtime failure mutex, copied `exception_ptr` |
| Cancellation and producer enable flags | Atomic booleans |

An ECU does not need internal locks: only its assigned worker calls its
lifecycle, tick, message, and state methods. Other threads cannot access those
ECU instances through the runtime API. `dashboard_snapshot()` returns an owned
copy, and `wait_for_dashboard(after_revision, deadline)` waits for a newer copy.
This is a one-value mailbox: slow readers may skip intermediate revisions.
Each reader keeps its own revision. No reference into mutable state escapes.

`fault(id)` returns an owned record. `take_event()` and `wait_event_until()`
consume event values from a shared FIFO; multiple reporters compete for events,
not broadcast copies. Readers may call these while workers run or stop. The
store's thread safety does not remove the monitor's sole-writer requirement:
do not manually change faults behind an active detector.

The bus always locks bus-before-inbox. Inbox operations never lock the bus.
Admission checks filtering, closure, capacity, and insertion under one inbox
lock, so a concurrently closed receiver is excluded rather than misreported as
overflow. Bus sends hold the bus mutex through fan-out, establishing a common
admission order for all receivers. The order of competing producers is lock
acquisition order, not deterministic scheduling or numeric CAN priority.
No bus or inbox lock is held while executing ECU callbacks. A snapshot/store
read cannot wait for a worker to acquire the lifecycle mutex, so `stop()` can
join under that mutex without forming a worker-to-owner lock cycle.

## Queues, backpressure, and ordering

- Runtime registration is fixed at four inboxes. Engine's empty filter gives
  its periodic worker the same interruptible wait mechanism as other workers.
- `ConcurrentConfig::inbox_capacity` defaults to 64 per inbox. Positive values
  allocate a ring once; no queue allocations occur during sends/receives.
- Inboxes preserve FIFO admission order and reject the newest copy when full.
  Producers do not wait for capacity. They may briefly wait for mutex ownership.
  `SendResult` exposes delivered and overflow-dropped copy counts; admission is
  not evidence of callback execution. Do not retry blindly or spin on overflow.
- The diagnostic event FIFO defaults to 64 slots (`event_capacity`). Overflow
  overwrites the oldest event, retains current records, and increments the
  saturating discard count. Slow reporters must check `discarded_events()`.
- Telemetry reporting stores one latest Dashboard snapshot rather than an
  unbounded history. Snapshot revisions saturate at `uint64_t` maximum.
- Inbox closure discards pending frames and wakes every receiver. A frame
  already removed by a worker may finish processing; it is not forcibly revoked.
- Diagnostic-feed closure retains pending events for draining and leaves fault
  records unchanged. Subsequent store updates throw `logic_error`. An empty
  closed feed returns no event immediately; `events_closed()` distinguishes
  this from a deadline in the standalone store.

`CanReceiver::receive_until(deadline)` is a concrete addition, preserving the
mockable nonblocking `ICanReceiver` interface. It returns an owned frame, `Empty`
on timeout, or `Closed`. Closure takes precedence over frames; otherwise queued
data wins over an elapsed deadline. Predicate waits handle spurious wakeups.
Already-expired deadlines are checked before waiting, including extreme past
synthetic timestamps. Fixed capacity bounds memory, not execution latency.

## Startup, cancellation, and cleanup

1. Construction validates configuration and registers all inboxes. It creates
   no threads. Tick periods must be 1..60000 ms; capacities must be positive.
2. `start()` creates four workers. Each starts its behavior with the common
   steady-clock timestamp, signals readiness, then waits at a startup barrier.
   After all four are ready, the caller releases the barrier and returns.
3. Each worker waits for an inbox frame or its next tick deadline. It samples
   `steady_clock` after reception, dispatches at most one frame, then checks the
   tick deadline. Continuous traffic therefore cannot starve ticks. Late ticks
   skip missed periods instead of emitting a catch-up burst. Silence still
   triggers Dashboard/diagnostic ticks, even when a producer is disabled.
4. `request_stop()` sets cancellation, closes inboxes, and wakes the startup and
   snapshot waits. It does not join. Flag changes participate in each wait's
   mutex handshake so cancellation notifications cannot be lost. Cancellation
   and barrier release are serialized by the startup mutex. If the barrier has
   not been released, cancellation also closes the diagnostic event feed
   immediately, including for an instance that has never started. Behavior
   startup does not update the store; only released workers can do so. After
   release, the diagnostic worker closes the feed after its last update.
5. Workers leave their loops, stop their own behaviors, and exit. Dashboard
   publishes its stopped state; diagnostics closes its event feed after its
   last possible update. `stop()` joins every created thread and closes the
   feed even when no diagnostic worker was created. It is idempotent, and the
   destructor calls it. After `stop()` returns, no worker can access the runtime.

Start is permitted once; stopped/cancelled instances cannot restart. A failure
during thread creation or behavior startup cancels and joins all threads that
were created before the exception reaches the caller. A worker exception is
captured, cancels its peers, and never escapes a thread entry point. Call
`rethrow_if_failed()` to report the first failure, including after `stop()`.
Cleanup does not manufacture diagnostic recovery.

External callers must preserve the runtime lifetime. Stop wakes reporting
waits, but callers must join their own reporting threads before destroying the
runtime. Cancellation is cooperative: it cannot interrupt an in-flight C++
callback. Current concrete callbacks contain no blocking I/O or unbounded work.
OS failures in nonthrowing mutex/cleanup operations are not recoverable here.

## Public controls and time

`send(frame)` admits external simulation traffic only while running and returns
`Closed` otherwise. Publication timestamps must use the same steady-clock epoch
as the runtime. Existing codecs still reject invalid payloads at the consumers.
`set_engine_publishing(bool)` and `set_brake_publishing(bool)` enable reproducible
silence/recovery scenarios; they take effect at the next worker tick. One
in-flight publication may finish after disabling. Initial enable flags can be
configured before start to test never-received streams.

Initial Engine inputs and timeout policies are constructor configuration. Live
raw-sensor updates are not added to this runtime: their existing observation
hook remains available in deterministic scenarios. No unsynchronized setter
on a worker-owned ECU is exposed.

Real-clock scheduling is intentionally nondeterministic. Timeout occurrence
time is when the observer notices a condition. Startup grace includes time
spent creating workers. Dropped/delayed copies may resemble ECU silence. The
previous per-callback boundary ordering still applies; an inbox drain is not
an atomic batch. This is not a real-time scheduler or physical CAN arbitration.
Deterministic boundary tests continue to call the detector directly with
injected time, without workers or sleeps.

## Tests and sanitizer commands

The new tests use bounded deadlines, futures for synchronization, owned result
values, and joined threads. They cover concurrent capacity enforcement, common
fan-out order, copied frames, deadline behavior, cancellation with queued data,
consumer/admission/discard accounting, concurrent receiver closure, diagnostic
readers, retained events, startup silence, communication recovery, long-wait
interruption, concurrent stops/reporters, RAII destruction, and startup races.
Thread creation resource exhaustion is not deliberately induced by tests; the
partial-start cleanup path is reviewed but that failure mechanism is unverified.

Use a separate build for ThreadSanitizer; do not combine it with AddressSanitizer.
On the current macOS arm64 environment, the exact commands are:

```sh
cmake -S . -B build-tsan -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=/usr/bin/clang++ -DCMAKE_OSX_ARCHITECTURES=arm64 -DBUILD_TESTING=ON -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/Users/apple/Desktop/Embedded/build/_deps/googletest-src -DCMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE=PRE_TEST -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
cmake --build build-tsan --parallel 2
TSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-tsan --output-on-failure --no-tests=error
TSAN_OPTIONS=halt_on_error=1 ./build-tsan/vaecu_sim --concurrent
```

The source override reuses the existing pinned GoogleTest source; substitute
your own trusted local path. `PRE_TEST` defers discovery to CTest. TSan is
toolchain/platform dependent; no claim is made that MSVC supports these flags.
If unavailable elsewhere, use the concurrency stress tests plus ASan/UBSan and
manual lock/ownership review, and record that they do not replace a race detector.

## Executed verification: 2026-10-11

Verified on macOS arm64 with Apple Clang 21 and CMake 4.2.3. Before changes,
the existing Debug build passed all 133 tests in 1.23 seconds. After this phase,
all 133 existing tests remain unchanged and 21 concurrency tests are added.

The normal configuration and final build/test commands were:

```sh
cmake -S . -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=/usr/bin/clang++ -DCMAKE_OSX_ARCHITECTURES=arm64 -DBUILD_TESTING=ON
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure --no-tests=error
./build/vaecu_sim
./build/vaecu_sim --concurrent
```

All exited zero. The deterministic output was preserved; the concurrent CLI
printed the five lines shown in the README and joined its workers. The separate
ThreadSanitizer configuration, build, full CTest run, and concurrent CLI commands
listed above also exited zero. TSan is supported and usable in this environment.

ASan/UBSan configuration and execution were:

```sh
cmake -S . -B build-sanitized -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=/usr/bin/clang++ -DCMAKE_OSX_ARCHITECTURES=arm64 -DBUILD_TESTING=ON -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/Users/apple/Desktop/Embedded/build/_deps/googletest-src -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-sanitized --parallel 2
ctest --test-dir build-sanitized --output-on-failure --no-tests=error
./build-sanitized/vaecu_sim --concurrent
```

All exited zero. Final complete suite results (test-run durations, not performance
benchmarks):

| Build | Result | CTest elapsed time |
| --- | --- | --- |
| Debug | 154/154 passed | 1.79 s |
| AddressSanitizer + UndefinedBehaviorSanitizer | 154/154 passed | 13.74 s |
| ThreadSanitizer | 154/154 passed | 67.87 s |

An additional TSan run sampled more interleavings by repeating the concurrency
cases in one process:

```sh
TSAN_OPTIONS=halt_on_error=1 ./build-tsan/vaecu_tests --gtest_filter='Concurrent*' --gtest_repeat=25 --gtest_break_on_failure > /tmp/vaecu-tsan-stress.log 2>&1
git diff --check
```

Both commands exited zero. The stress log contains 25 successful repetitions
of all 21 concurrency cases (525 case executions). Complete CTest logs and the
stress log contain no sanitizer errors/race reports. Builds reported no compiler
warnings. These checks sample schedules; they do not prove absence of all races
or guarantee timing. Windows, Linux, Intel macOS, CI, and deliberate OS resource
exhaustion remain unverified. No hardware or automotive certification is implied.

## Review corrections: 2026-10-11

The review first configured and built the existing Debug project and ran CTest:
154/154 passed in 1.66 seconds. It found one runtime cancellation defect and
two test weaknesses:

- Cancelling before startup left the diagnostic feed open. No worker would
  close it, so diagnostic readers waited until their deadline (potentially
  indefinitely). The new bounded regression failed before the fix, exiting 1.
  `request_stop()` now uses the existing startup mutex and barrier state to
  close the feed when no released writer can still use it. During normal
  execution the writer retains responsibility for closing the feed on exit;
  this avoids closing underneath an in-flight diagnostic update. No new
  synchronization object or interface was introduced.
- The startup-race test accepted every `runtime_error`, which includes unrelated
  worker failures and thread-creation `system_error`s. It now checks the expected
  cancellation messages and verifies that no worker failure was captured.
- A telemetry-reader test asserted before stopping the workers. If that fatal
  assertion failed, its reader futures would be destroyed before the runtime
  and could block while live workers kept generating snapshots. The same
  assertion now runs after unconditional stop, preserving the check and making
  its failure cleanup safe.

The reviewed ownership, bus-before-inbox lock order, fixed-capacity rings, owned
frame/snapshot copies, fault transition suppression, and deterministic callback
mode remain intact. No additional memory-safety or data-race defect was confirmed
by this review. Thread-creation resource failures remain untested; supported
platforms beyond the current macOS arm64 toolchain remain unverified. The runtime
does not currently expose each ECU's internal send/drop counters to reporters;
queue loss can still appear as stale data or communication silence.

All three configure commands recorded above were re-executed for this review,
using the existing pinned GoogleTest source. All configurations and builds
exited zero, with no reported compiler warnings. The build and final test
commands actually executed were:

```sh
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure --no-tests=error
cmake --build build-sanitized --parallel 2
ctest --test-dir build-sanitized --output-on-failure --no-tests=error
cmake --build build-tsan --parallel 2
TSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-tsan --output-on-failure --no-tests=error
```

| Review build | Actual complete-suite result | CTest elapsed time |
| --- | --- | --- |
| Debug | 155/155 passed | 1.64 s |
| AddressSanitizer + UndefinedBehaviorSanitizer | 155/155 passed | 13.49 s |
| ThreadSanitizer | 155/155 passed | 68.23 s |

The failed-before-fix regression command was:

```sh
./build/vaecu_tests --gtest_filter=ConcurrentSimulationTest.CancellationBeforeStartWakesDiagnosticReadersWithoutJoining
```

It exited 1 because the reader was not ready after cancellation. After the fix,
the following targeted repetitions and CLI checks all exited zero:

```sh
./build/vaecu_tests --gtest_filter='ConcurrentSimulationTest.Cancellation*' --gtest_repeat=25 --gtest_break_on_failure
TSAN_OPTIONS=halt_on_error=1 ./build-tsan/vaecu_tests --gtest_filter='ConcurrentSimulationTest.Cancellation*' --gtest_repeat=25 --gtest_break_on_failure > /tmp/vaecu-review-tsan-cancellation.log 2>&1
./build/vaecu_sim
./build/vaecu_sim --concurrent
./build-sanitized/vaecu_sim --concurrent
TSAN_OPTIONS=halt_on_error=1 ./build-tsan/vaecu_sim --concurrent
```

Each repetition run passed both cancellation cases 25 times (50 case
executions per run). The deterministic CLI printed its 16 scenario lines; each
concurrent CLI printed the five expected scenario/shutdown lines. Complete
CTest logs and the targeted TSan log contain no reported sanitizer errors.
These results apply only to this macOS arm64 environment and sampled schedules.
