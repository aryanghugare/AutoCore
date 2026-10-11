# Diagnostic subsystem

This is an in-memory, software-only diagnostic model with project-local fault
identifiers. It implements no UDS services, standardized OBD code encoding,
vehicle protocol, persistence, or safety certification.

## Responsibilities and integration

- `DiagnosticMonitor` detects conditions. It implements the existing `IEcu`
  callbacks and receives copied frames through a separate virtual-bus inbox.
  It depends on the message codecs and an injected `IDiagnosticSink`, not on
  Engine, Brake, Dashboard, or the concrete bus.
- `DiagnosticStore` implements that sink. It stores fault records and bounded
  transition events, suppressing duplicate state updates defensively.
- Reporting reads owned `record(id)` snapshots and consumes `take_event()`
  values. The CLI formats these values; detection and storage perform no I/O.

Register the observer inbox for the engine telemetry, brake status, and brake
event IDs from `messages.hpp`. Dispatch its frames to `on_message(frame, now)`
using the existing `ICanReceiver` interface, then call `on_tick(now)` regularly,
including during silence. The caller determines callback order. The demo
dispatches queued messages before ticking at the same simulation time. Every
message or raw-sample callback also evaluates both periodic streams; dispatching
an inbox is not an atomic batch. A frame received exactly at its own timeout
boundary updates its stream before evaluation. The other stream may still raise
a fault before its queued frame is processed, then recover at the same timestamp.
For example, with both streams last observed at 0 ms, processing Engine and then
Brake at 1000 ms raises and recovers Brake stale/timeout faults at 1000 ms, even
when both frames were already queued. These are ordered observation events;
they do not prove that the producer or bus was inactive. Calling a tick first
can similarly raise faults followed by same-time recovery. Publish and dispatch
before deadlines when a scenario is meant to demonstrate uninterrupted healthy
traffic. No autonomous scheduler, worker thread, or batch delivery is introduced.

The observer monitors Engine and Brake publication streams. Dashboard sends no
heartbeat, so this subsystem does not claim to detect Dashboard's own liveness.
Diagnostic values are not new CAN message identifiers and are not transmitted
as CAN frames.

## Fault catalog

The corresponding `FaultId` and `fault_definitions` catalog are declared in
`include/vaecu/diagnostics.hpp`.

| Local identifier | Fault | Recovery evidence |
| --- | --- | --- |
| `0x1001` | Engine-speed sensor sample outside 0–9000 rpm | Valid raw engine-speed sample |
| `0x1002` | Vehicle-speed sensor sample outside 0–300 km/h | Valid raw vehicle-speed sample |
| `0x1101` | Engine telemetry never accepted after startup grace | First accepted telemetry frame |
| `0x1102` | Accepted engine telemetry is stale | Accepted publication within the freshness window |
| `0x1103` | Invalid engine telemetry payload | Accepted, valid engine payload |
| `0x1104` | Engine communication timeout | Matching frame delivered to the observer |
| `0x1201` | Brake status never accepted after startup grace | First accepted status frame |
| `0x1202` | Accepted brake status is stale | Accepted publication within the freshness window |
| `0x1203` | Invalid brake status payload | Accepted, valid brake status payload |
| `0x1204` | Brake communication timeout | Matching status frame delivered to the observer |
| `0x1205` | Invalid brake event payload | Accepted, valid brake event payload |

Sensor bounds come from the existing message schema, not manufacturer data.
The scenario calls `observe_engine_inputs(signed_rpm, signed_speed, now)` before
attempting the existing Engine input update. Signed 64-bit samples let tests
inject negative and oversized values without narrowing into valid integers.
Invalid samples are diagnosed without changing Engine's last valid inputs.
Valid telemetry does not clear a raw-sensor fault; only a new valid raw sample
does. Samples are independent, so one sensor can recover while the other stays
faulty. This explicit observation hook avoids putting DTC storage into ECU
business logic or changing its input-validation APIs.

## Timing and validation policy

Every callback receives `Timestamp now` explicitly, using the same steady-clock
simulation epoch as message timestamps. No method reads a clock or sleeps.
The monitor enforces nondecreasing processing time from `start(now)`; the store
also checks observation order. Negative epoch offsets are valid. Deadline
arithmetic guards overflow for extreme synthetic time points.

`DiagnosticConfig` contains three independently configurable positive durations:

| Setting | Default | Condition |
| --- | --- | --- |
| `startup_grace` | 500 ms | No accepted publication by `start + grace` |
| `stale_after` | 500 ms | `now >= last accepted publication + stale_after` |
| `communication_timeout` | 1000 ms | `now >= last delivery callback + timeout`, or `start + timeout` before any delivery |

Thresholds are inclusive. These settings apply to both periodic streams.
Missing and stale are mutually exclusive for a stream: missing means no valid
sample has ever been accepted; stale applies to the last accepted sample.
Missing or stale may coexist with a communication timeout. Fault occurrence
time is the observation time when the monitor first detects the condition,
not an inferred earlier deadline if ticks skipped over it.

Communication activity and usable data are deliberately separate. A matching
frame counts as activity even with a bad payload or timestamp, and may clear a
communication-timeout fault while stale/payload faults remain active. Delivery
time means the observer's callback time, not hardware receive time or evidence
of when an ECU physically transmitted. A delayed queued frame can therefore
restore observed activity while still being stale. Unknown IDs do not count
as activity. A brake event is aperiodic and does not refresh the brake status
stream's timers; absence of brake events is not a fault.

Payload validity uses the existing codecs: exact ID, exact length, and signal
ranges. Invalid recognized payloads raise their payload fault. Valid data is
accepted only if its publication is not in the future and not older than the
last accepted publication of that stream. Future/out-of-order frames cannot
clear a payload fault or refresh usable data. Equal publication timestamps
follow dispatch order but do not extend freshness. A valid delayed publication
can recover missing/payload faults while immediately producing a stale fault.
Envelope errors rejected before bus admission never reach this observer; the
caller still gets the existing frame exception or `SendStatus::InvalidFrame`.

## Records, transitions, and bounded storage

`DiagnosticRecord` retains the fault ID, `Active`/`Cleared` status, first and
latest occurrence time, most recent recovery time, and episode count. A fault
episode begins only on an inactive-to-active transition. Repeated cycles do not
change occurrence times or count. Clearing an unseen fault creates no record.
Cleared records remain available; reactivation increments the episode count,
updates the latest occurrence, and preserves previous recovery information.

`DiagnosticEvent` stores the fault ID, `Raised`/`Recovered` transition, and
observation timestamp. There is one record slot per catalog entry (11 slots).
The event FIFO defaults to 64 slots and allocates once at construction. At
capacity it overwrites the oldest unconsumed event, preserving the newest, and
increments `discarded_events()`. Current records are unaffected by event loss.
Both episode and discard counters saturate at `std::size_t` maximum. This is a
bounded recent-event feed, not a complete durable history. Consumers should
check the discard count when history completeness matters.

The monitor reports only state changes; the store also suppresses duplicate
states so repeated direct sink updates do not create duplicate records/events.
Monitor callbacks require serialization; `ConcurrentSimulation` gives them one
owner thread. Store records and event operations are now mutex-protected, so
reporting may run concurrently. A monitor's assigned sink faults must
initially be inactive, and it must be their sole writer during observation.
Use a fresh store for a fresh independent session; do not manually clear faults
behind an active monitor. The sink reference is non-owning and must outlive the
monitor. Record/event results are owned values with no reference into storage.

Lifecycle matches the existing ECUs: start once, tick/message/sample calls only
while running, stop idempotently (also before start), and no restart afterward.
Stopping retains active faults; it is not evidence of recovery. The sink may
throw, and such exceptions propagate. Successful earlier transitions in a batch
are not rolled back. A failed transition is not committed to the monitor's
duplicate cache: periodic conditions can be retried by ticking again; sensor
or payload observations must be resubmitted. Processing time does not roll back.
The supplied store does not allocate during fault updates.

The concurrent phase adds `wait_event_until(deadline)`, `close_events()`, and
`events_closed()` to the store. Closure retains queued events for draining,
wakes reporters, and rejects further fault updates without changing records.
See [concurrency.md](concurrency.md) for current ownership and verification;
the dated results below record earlier diagnostic-phase runs.

## Tests and demonstration

GoogleMock tests isolate the detector from storage and verify transition-only
calls, threshold boundaries, sensor recovery, independent payload faults, and
sink-failure retry. Store tests cover record episodes, duplicate suppression,
recovery events, owned snapshots, unknown identifiers, monotonic time, and FIFO
capacity/overflow. Integration tests cover actual bus fan-out, ECU communication
loss/recovery, bad inputs preserving Engine state, and malformed application
payloads passing envelope validation but failing the codec.

The CLI retains its normal/apply/stale/recovery state demonstration and adds a
bad sensor sample, malformed telemetry payload, communication loss, and recovery
events. Missing-message behavior is tested with deterministic startup silence.
No hardware, frontend, real-time waits, or UDS implementation is included.

## Initial implementation verification

Verified on 2026-10-11 on macOS arm64 with Apple Clang 21 and CMake 4.2.3,
from the repository root. The existing GoogleTest 1.17.0 source was available
locally. Normal configuration, compilation, tests, and demonstration:

```sh
cmake -S . -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=/usr/bin/clang++ -DCMAKE_OSX_ARCHITECTURES=arm64 -DBUILD_TESTING=ON
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure --no-tests=error
./build/vaecu_sim
```

All commands exited with status 0. The final normal CTest run passed 132/132
tests in 1.46 seconds: 98 existing tests and 34 new diagnostic tests. The build
produced no project compiler warnings. The CLI output matches the README.

AddressSanitizer and UndefinedBehaviorSanitizer were enabled for a separate
build, with sanitizer error recovery disabled. This reuses the existing local
GoogleTest source instead of downloading another copy:

```sh
cmake -S . -B build-sanitized -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=/usr/bin/clang++ -DCMAKE_OSX_ARCHITECTURES=arm64 -DBUILD_TESTING=ON -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/Users/apple/Desktop/Embedded/build/_deps/googletest-src -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-sanitized --parallel 2
ctest --test-dir build-sanitized --output-on-failure --no-tests=error
./build-sanitized/vaecu_sim
```

All commands exited with status 0. The sanitized CTest run passed 132/132 tests
in 8.46 seconds. The sanitized CLI also completed with the expected output;
neither run reported sanitizer errors. These results verify this macOS arm64
environment only. Windows, Linux, Intel macOS, and concurrent execution have
not been verified by this phase.

## Review and subsequent verification

The review on 2026-10-11 found an inaccurate description of dispatch-before-tick
ordering. `on_message()` evaluates both streams after observing one frame;
draining multiple frames is not atomic. The integration instructions above now
describe same-time raise/recovery events accurately. A new GoogleMock test,
`BoundaryDeliveryEvaluatesOtherStreamsBeforeTheirNextCallback`, checks exact
transition order at 1000 ms, no Engine fault after its timely publication, Brake
recovery after its callback, and no duplicate transitions on following ticks.
No existing tests were removed or weakened, and no production implementation
was changed by this review.

The review inspected frame/codecs, bounded queues, ECU lifecycles, diagnostic
records, detector/store interaction, the CLI, and tests. No confirmed memory
safety or correctness defect was found within the serialized-callback contract.
Important limits remain: the monitor must be the sole writer for its assigned
faults, raw sensor observation is a scenario responsibility, ticks must continue
during silence, and a diagnostic inbox's dropped messages may look like missing
data. The observer reports what it receives; it cannot distinguish producer
silence from bus-queue loss. Concurrent calls require future synchronization;
the current implementation provides none. Dashboard has no heartbeat, and
event overflow provides recent transitions rather than complete history.

Both configurations were regenerated using the exact configure commands in
the preceding section. The following commands were executed:

```sh
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure --no-tests=error
./build/vaecu_sim
cmake --build build-sanitized --parallel 2
ctest --test-dir build-sanitized --output-on-failure --no-tests=error
./build-sanitized/vaecu_sim
git diff --check
```

All exited with status 0. Before the review changes, the baseline passed 132/132
tests in 1.24 seconds. After adding the boundary test, the complete normal suite
passed 133/133 in 1.28 seconds, and the complete sanitizer suite passed 133/133
in 8.65 seconds. Builds reported no warnings, both demonstrations matched the
README output, and the sanitizer runs reported no errors. This review changes
only this document, the README, and `tests/diagnostics_test.cpp`. Existing
uncommitted diagnostic-phase files were preserved. No next-phase work was done.
