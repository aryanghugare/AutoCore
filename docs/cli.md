# Command-line demonstration

Build and test from the repository root on macOS Apple Silicon:

```sh
cmake -S . -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=/usr/bin/clang++ -DCMAKE_OSX_ARCHITECTURES=arm64 -DBUILD_TESTING=ON
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure --no-tests=error
```

## Commands and scenarios

```sh
./build/vaecu_sim --scenario normal
./build/vaecu_sim --scenario sensor-failure
./build/vaecu_sim --scenario timeout
./build/vaecu_sim --scenario recovery
./build/vaecu_sim --scenario recovery --summary-only
./build/vaecu_sim --help
./build/vaecu_sim --concurrent
```

No arguments selects `normal`. The previous combined deterministic demonstration
has been replaced by four selectable scenarios. `--summary-only` may appear
before or after `--scenario`. `--help` (or `-h`) and the retained `--concurrent`
mode must be used alone. Unknown names, extra arguments, missing values,
duplicate options, and conflicting modes are rejected before constructing ECUs.

| Scenario | Simulated duration | Behavior | CAN transmissions | Faults raised / recovered / still active |
| --- | --- | --- | --- | --- |
| `normal` | 500 ms | Healthy telemetry; brake apply at 250 ms, release at 500 ms | 8 | 0 / 0 / 0 |
| `sensor-failure` | 500 ms | Invalid Engine RPM sample at 250 ms; retain last valid input | 8 | 1 / 0 / 1 |
| `timeout` | 1500 ms | Pause Engine publications at 250 ms; stale at 500 ms, timeout at 1000 ms | 10 | 2 / 0 / 2 |
| `recovery` | 1500 ms | Sensor failure and Engine silence at 250 ms; restore both at 1250 ms | 12 | 3 / 3 / 0 |

All scenarios initialize Engine to 1500 RPM and 60 km/h, publish Brake status
every 250 ms, and exercise brake apply/release. The tick at the end of each
duration is included. Engine silence pauses publications while its behavior
object remains started; it simulates lost communication rather than a process
crash. On recovery, publication resumes without restarting the ECU.

Timestamps are relative simulation milliseconds starting at zero, not UTC or
wall-clock time. Scenarios run synchronously with injected timestamps and no
sleeps. Frames use the existing [message schema](messages.md). No electrical
CAN arbitration or full CAN protocol implementation is claimed. Routing is
FIFO publication order; this demo does not add priority arbitration. Diagnostics
are project-specific fault records, not UDS services.

## Expected output

Complete default / `normal` output:

```text
Virtual Automotive ECU Platform | scenario=normal | deterministic
Software CAN simulation | time=relative ms | routing=FIFO
[0000 ms] RUN started
[0000 ms] CAN tx=Engine id=0x100 len=4 data=[DC 05 3C 00] routed=2 dropped=0
[0000 ms] CAN tx=Brake id=0x200 len=1 data=[00] routed=2 dropped=0
[0000 ms] ECU Engine=ONLINE Brake=ONLINE Dashboard=ONLINE | rpm=1500 speed=60km/h brake=RELEASED | engine=fresh brake=fresh
[0250 ms] CAN tx=Scenario id=0x300 len=1 data=[01] routed=2 dropped=0
[0250 ms] CAN tx=Engine id=0x100 len=4 data=[DC 05 3C 00] routed=2 dropped=0
[0250 ms] CAN tx=Brake id=0x200 len=1 data=[01] routed=2 dropped=0
[0250 ms] ECU Engine=ONLINE Brake=ONLINE Dashboard=ONLINE | rpm=1500 speed=60km/h brake=APPLIED | engine=fresh brake=fresh
[0500 ms] CAN tx=Scenario id=0x300 len=1 data=[02] routed=2 dropped=0
[0500 ms] CAN tx=Engine id=0x100 len=4 data=[DC 05 3C 00] routed=2 dropped=0
[0500 ms] CAN tx=Brake id=0x200 len=1 data=[00] routed=2 dropped=0
[0500 ms] ECU Engine=ONLINE Brake=ONLINE Dashboard=ONLINE | rpm=1500 speed=60km/h brake=RELEASED | engine=fresh brake=fresh
[0500 ms] RUN stopped; resources released
[0500 ms] ECU Engine=OFFLINE Brake=OFFLINE Dashboard=OFFLINE | rpm=1500 speed=60km/h brake=RELEASED | engine=stale brake=stale
Summary: duration=500ms CAN=8 routed=16 processed=16 dropped=0
Diagnostics: raised=0 recovered=0 active=0 lost=0
Shutdown: all ECUs OFFLINE; pending_discarded=0
```

Sensor failure adds these lines (excerpt):

```text
[0250 ms] SCENARIO engine sensor invalid: rpm=-1; last valid input retained
[0250 ms] DIAG code=0x1001 ACTIVE engine speed sensor invalid
```

Communication failure and recovery include these lines (excerpt):

```text
[0500 ms] DIAG code=0x1102 ACTIVE engine message stale
[1000 ms] DIAG code=0x1104 ACTIVE engine communication timeout
[1000 ms] ECU Engine=OFFLINE Brake=ONLINE Dashboard=ONLINE | rpm=1500 speed=60km/h brake=RELEASED | engine=stale brake=fresh
[1250 ms] DIAG code=0x1001 RECOVERED engine speed sensor invalid
[1250 ms] DIAG code=0x1102 RECOVERED engine message stale
[1250 ms] DIAG code=0x1104 RECOVERED engine communication timeout
[1250 ms] ECU Engine=ONLINE Brake=ONLINE Dashboard=ONLINE | rpm=1500 speed=60km/h brake=RELEASED | engine=fresh brake=fresh
```

Exact `--scenario recovery --summary-only` output:

```text
Virtual Automotive ECU Platform | scenario=recovery | deterministic
Software CAN simulation | time=relative ms | routing=FIFO
Summary: duration=1500ms CAN=12 routed=24 processed=24 dropped=0
Diagnostics: raised=3 recovered=3 active=0 lost=0
Shutdown: all ECUs OFFLINE; pending_discarded=0
```

The summaries for `sensor-failure` and `timeout` respectively contain:

```text
Summary: duration=500ms CAN=8 routed=16 processed=16 dropped=0
Diagnostics: raised=1 recovered=0 active=1 lost=0
Shutdown: all ECUs OFFLINE; pending_discarded=0
```

```text
Summary: duration=1500ms CAN=10 routed=20 processed=20 dropped=0
Diagnostics: raised=2 recovered=0 active=2 lost=0
Shutdown: all ECUs OFFLINE; pending_discarded=0
```

`CAN` counts original transmissions. `routed` counts frame copies admitted to
receiver inboxes; `processed` counts consumed callbacks. Telemetry/status routes
to Dashboard and diagnostics; brake events route to Brake and diagnostics, so
each transmission admits two copies. `dropped` counts failed admissions, `lost`
counts discarded diagnostic events, and `pending_discarded` counts frame copies
discarded at shutdown. These fixed plans require successful delivery and report
failure if admission is incomplete. They do not silently print a successful run.

Engine/Brake `ONLINE` means a sample has been received and no communication
timeout is active. Data can be stale while the ECU is still online, until the
inclusive 1000 ms timeout boundary. Dashboard status is its local lifecycle
state. Sensor faults do not imply communication failure: the last valid Engine
inputs remain available with a separate active diagnostic. This demo has no
remote heartbeat or process liveness detection beyond those rules.

Shutdown marks all ECUs offline and retained display data stale. It does not
manufacture recovery events: unresolved faults remain active in the summary.

## Separation and lifecycle

`src/demo_scenario.cpp` orchestrates existing ECUs, bus, codecs, and diagnostics.
`run_demo_scenario(DemoScenario)` returns an owned `DemoReport` with action,
transmission, diagnostic, and status events plus summary counters. It creates a
fresh session per call. There is no terminal formatting, sleeping, or real-thread
execution in that runner. Invalid enum values throw before session construction.

`app/cli.cpp` parses arguments and formats the report. `app/main.cpp` only passes
arguments and streams to it. The core never depends on `vaecu_cli`; formatting
uses a local classic locale, ASCII text, and explicit hex widths. Tests normalize
Windows CRLF for process-output comparisons. The private recording sender
delegates to the real bus, so output represents real admissions, not invented
telemetry. FIFO draining occurs after publication, with brake commands consumed
before Brake publishes status. Diagnostic events are drained after each tick.

Each fixed scenario has at most seven ticks and fewer than 64 trace events;
receiver queues and the diagnostic event store retain their existing bounds.
All callbacks are serialized in this mode. Cleanup stops components and closes
queues/events on both success and exceptions through an owned RAII session.
The trace is formatted after cleanup, so a failed output stream cannot leave a
simulation running. This is a finite demonstration, not a continuous logger.

The existing `--concurrent` demo remains available with its five status/shutdown
lines. It uses real threads and steady-clock deadlines, blocks on condition
variables, and joins workers on shutdown. Its scheduling is intentionally not
reproducible. See [the concurrent runtime contracts](concurrency.md).

Exit codes: `0` for success/help, `2` for argument errors (usage on stderr, no
simulation output), `1` for simulation or output errors. For example,
`./build/vaecu_sim --scenario unknown` reports `Error: Unknown scenario: unknown`
and usage on stderr and exits `2`.
Successful scenario, help, and concurrent commands flush stdout before returning;
buffered write/flush failures return `1`. This check occurs after simulation
cleanup. Report text uses unformatted writes so a caller's field width cannot
add padding or be consumed by report formatting.

## Automated verification

`tests/demo_scenario_test.cpp` checks exact plans/counts, raw sensor rejection,
retained valid data, stale/timeout boundaries, online/offline/recovery behavior,
brake command flow, monotonic trace timestamps, shutdown, and fresh sessions.
`tests/cli_test.cpp` checks parsing, invalid/conflicting options, help, exact
summary text, repeatability of each scenario, stream formatting, and output
failure. Eight CTest process cases execute the real binary and verify stdout,
stderr, exit codes, and concurrent shutdown. Expected fixtures are maintained
independently in `tests/cli_expected/`; assertion expectations were not generated
from a run of the implementation.

The initial CLI phase retained 162 existing cases and added 14 GoogleTest cases
and eight process cases, reaching 184 CTest cases. Its review adds two regression
cases for field width and buffered output failure, reaching 186 cases. This is
a test count, not a coverage metric.

## Executed verification: 2026-10-11, CLI phase

Environment: macOS arm64, Apple Clang 21.0.0, CMake 4.2.3. The baseline build
and CTest run passed all 162 existing cases before edits. After the new scenario,
CLI, and process tests were added, the final builds and complete suites passed:

| Configuration | Executed result | CTest elapsed time |
| --- | --- | --- |
| Debug | 184/184 passed | 4.90 s |
| ASan + UBSan | 184/184 passed | 20.35 s |
| TSan, separate build | 184/184 passed | 89.16 s |

The Debug configure/build/test commands at the top of this guide were executed.
The exact sanitizer commands executed were:

```sh
cmake -S . -B build-quality-asan -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=/usr/bin/clang++ -DCMAKE_OSX_ARCHITECTURES=arm64 -DBUILD_TESTING=ON -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/Users/apple/Desktop/Embedded/build/_deps/googletest-src -DVAECU_ENABLE_ASAN=ON -DVAECU_ENABLE_UBSAN=ON
cmake --build build-quality-asan --parallel 2
ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-quality-asan --output-on-failure --no-tests=error

cmake -S . -B build-quality-tsan -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=/usr/bin/clang++ -DCMAKE_OSX_ARCHITECTURES=arm64 -DBUILD_TESTING=ON -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/Users/apple/Desktop/Embedded/build/_deps/googletest-src -DVAECU_ENABLE_TSAN=ON
cmake --build build-quality-tsan --parallel 2
TSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-quality-tsan --output-on-failure --no-tests=error
```

All exited zero. The source override reused the existing trusted pinned
GoogleTest extraction; see the [dependency strategy](../README.md#dependency-strategy).
No compiler warnings or sanitizer errors were reported. No existing tests or
assertions were weakened. Successful sanitizer runs cover executed paths and
schedules, not every possible input or interleaving.

Each scenario was also run directly with full logs:

```sh
./build/vaecu_sim --scenario normal
./build/vaecu_sim --scenario sensor-failure
./build/vaecu_sim --scenario timeout
./build/vaecu_sim --scenario recovery
./build/vaecu_sim --scenario recovery --summary-only
```

All five commands exited zero and produced the counts and events documented
above. Executable-level tests additionally verified default output, help,
invalid-name exit code `2`, and the retained concurrent lifecycle/shutdown output.

An application-only build and smoke run were executed without GoogleTest:

```sh
cmake -S . -B build-app -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=/usr/bin/clang++ -DCMAKE_OSX_ARCHITECTURES=arm64 -DBUILD_TESTING=OFF
cmake --build build-app --parallel 2
./build-app/vaecu_sim --scenario recovery --summary-only
```

All three commands exited zero. This is an application smoke check, not an
additional test suite. Windows, Linux, and Intel macOS builds were not executed.
No React frontend or hardware dependency was added.

## CLI review: findings and corrections

The review started with the previous phase's working-tree changes present;
those files were preserved. The baseline configure/build succeeded and all
184 tests passed in 3.42 seconds. Source review covered frame/codecs, ECU
lifecycle, FIFO routing, diagnostics, deterministic scenario cleanup, CLI
formatting, and concurrent startup, cancellation, lock order, and joining.

Two correctness defects were reproduced by new regression tests before fixes:

- Buffered destinations could accept every write and fail only on flush.
  Scenario, help, and concurrent modes returned success without flushing,
  so output loss could go unreported. `cli::run` now flushes after cleanup and
  returns `1` on either a failed stream state or a thrown I/O exception.
- `print_report` rendered text with a private classic-locale stream but then
  inserted the final string with formatted `operator<<`. A caller's field
  width added padding and reset that width to zero. An unformatted `write`
  now preserves the caller's formatting settings and writes the exact text.

The regression buffer accepts writes and deliberately fails synchronization;
the test exercises normal, help, and concurrent modes with both throwing and
nonthrowing streams. The field-width test checks exact bytes and retained
width/fill. Both new cases failed against the original implementation (exit
`1`) and passed after the correction (exit `0`):

```sh
./build/vaecu_tests --gtest_filter='CliTest.ReportIgnoresAndPreservesCallerFieldWidth:CliTest.BufferedOutputFailuresAreReportedInEveryMode'
```

This review changes only `app/cli.cpp`, `tests/cli_test.cpp`, this guide, and the
README's expected test count. Existing assertions and output fixtures remain
intact. The fixes add no shared state, worker callbacks, or core/UI dependency.
Simulation cleanup completes before final output synchronization, so an I/O
failure cannot leave a session or its worker threads running.

No additional ownership, data-race, or deadlock defect was confirmed. The
current fixed plans bound trace size through their finite schedule; adding
arbitrary duration/history would require a new explicit bound. Concurrent
diagnostic events and Dashboard snapshots are independently synchronized
observations, not an atomic snapshot across ECUs. The runtime already
documents cooperative cancellation and caller lifetime requirements.
Cross-platform execution and deliberate partial thread-creation failure remain
unverified; successful sampled runs do not establish those properties.

Final review verification on the same macOS arm64/Apple Clang environment:

| Configuration | Executed result | CTest elapsed time |
| --- | --- | --- |
| Debug | 186/186 passed | 6.08 s |
| ASan + UBSan | 186/186 passed | 21.84 s |
| TSan, separate build | 186/186 passed | 86.96 s |

The Debug configure command at the top and both sanitizer configure commands
in the CLI-phase section were rerun successfully. These build/test commands
were executed after the fixes, all exiting `0`:

```sh
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure --no-tests=error
cmake --build build-quality-asan --parallel 2
ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-quality-asan --output-on-failure --no-tests=error
cmake --build build-quality-tsan --parallel 2
TSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-quality-tsan --output-on-failure --no-tests=error
./build/vaecu_sim --scenario recovery --summary-only
git diff --check
```

The targeted regression command also exited `0` after rebuilding the fix.
The recovery smoke run retained its exact five-line summary. No compiler
warnings or sanitizer errors were reported. Only the two intentionally added
regression cases failed before the fix; there are no unresolved build/test
blockers for this phase. The review did not advance implementation to another
phase.
