# Simulation message schema and ECU contracts

This is the single documented schema location. The corresponding constants,
value types, encoders, and decoders live in `include/vaecu/messages.hpp` and
`src/messages.cpp`. These are project-specific software messages, not a vehicle
manufacturer's protocol, UDS, or a claim of full CAN compliance.

## Message encoding

All multibyte fields are unsigned integers in explicit little-endian order.
There is no struct serialization, pointer reinterpretation, or dependence on
host byte order. The active payload length must match exactly; unused frame
storage is zero-normalized by `CanFrame` and is not part of the schema.

| Identifier | Producer | Consumer | Length | Active payload |
| --- | --- | --- | --- | --- |
| `0x100` engine telemetry | Engine ECU | Dashboard ECU | 4 | Bytes 0–1: engine speed, rpm, 0–9000 inclusive. Bytes 2–3: vehicle speed, whole km/h, 0–300 inclusive. |
| `0x200` brake status | Brake ECU | Dashboard ECU | 1 | Byte 0: released = 0, applied = 1. Other values are invalid. |
| `0x300` brake event | Simulation scenario | Brake ECU | 1 | Byte 0: Apply = 1, Release = 2. Other values are invalid. |

The input limits are chosen for this demonstration; they are not manufacturer
specifications or a physical vehicle dynamics model. Engine and vehicle speed
are independent simulated inputs, not values derived from gear ratios.
For example, 3000 rpm and 88 km/h encode as `B8 0B 58 00`.

Encoding invalid engine values or an unsupported brake-event enum throws
`std::invalid_argument`. Decoders return `std::nullopt` for the wrong ID,
incorrect length, or out-of-range values. They validate schema data only.
The bus validates the envelope; it does not know these signal definitions.

`CanFrame::timestamp()` is explicit process-local simulation publication time,
not bytes in the payload. It must share the epoch and units of callback `now`.
Tests advance synthetic time; no ECU reads the system clock.

## ECU responsibilities and dependencies

All three components implement the existing `IEcu` lifecycle. No virtual method
signature changed. `MessageDisposition` was added as a shared value enum; the
concrete ECUs expose the outcome of their latest `on_message()` callback through
`last_message_result()`. It is not a DTC or persistent diagnostic history.

- `EngineEcu(ICanSender&, EngineTelemetry)` owns validated input values. Defaults
  are zero rpm and zero speed. `set_inputs()` can configure inputs before start
  or change them while running. An invalid update leaves both previous values
  intact. Each tick publishes exactly one telemetry frame stamped with `now`.
  All inbound IDs are unsupported; bus messages never modify these inputs.
- `BrakeEcu(ICanSender&, BrakeStatus)` owns brake state, initially released
  unless configured otherwise. Valid Apply/Release events set the state;
  repeated application of the same event is idempotent. Event callbacks do not
  transmit. Each tick publishes exactly one current status frame.
- `DashboardEcu(DashboardConfig)` consumes engine and brake status through
  `on_message()`. It owns optional values and separate `Missing`, `Fresh`, or
  `Stale` health states. `state()` returns an owned snapshot, with integer units
  and a boolean ready for a future UI. It has no sender or inbox dependency:
  reception and dispatch belong to the caller, as defined by `IEcu`.

Sender references are non-owning and must outlive the producer ECUs. The bus
does not call ECU methods or hold ECU pointers. An ECU does not reference any
other ECU. Each ECU's calls, snapshots, updates, and teardown require one
serialized owner. The [concurrent runtime](concurrency.md) assigns each ECU a
worker and publishes synchronized snapshots for external readers; direct calls
to an ECU's getter or setter from another thread remain unsupported.
There are no hardware, UI, logging-framework, scheduler, or worker dependencies.

`start()` is permitted once. Tick/message calls before start or after stop, and
repeated start, throw `std::logic_error`. `stop()` is nonthrowing, idempotent,
and valid before start. Stop before start permanently prevents starting that
instance. The owner stops active ECUs before destruction. Producers have no
owned transport resources to release. Dashboard stop retains the last values
but marks any previously received signal stale.

Producer `last_send_result()` is absent before the first completed send; it
retains the latest completed `SendResult`, including overflow counts or closure.
There are no automatic retries or unbounded pending publications. Sender
exceptions propagate without replacing the previous completed result; a future
runtime will provide its exception boundary. Tick frequency is chosen by the
caller. Producer callback timestamps follow the scenario's monotonic timeline
and are checked before transmission or message handling.

## Incoming-message and freshness policy

Unknown IDs set `UnsupportedId` without changing application values. Supported
IDs with invalid schema data set `InvalidPayload`. The brake and dashboard
reject future publication timestamps with `InvalidTimestamp` and publications
older than the last accepted timestamp for that stream with `OutOfOrder`.
Equal timestamps follow dispatch order. No sequence counter or replay protection
is implemented; publications from multiple producers for the same ID are not
authenticated. Brake events have no expiry deadline beyond those checks.

Processing time for all three ECUs must be nondecreasing from `start(now)`
across tick and message callbacks. A regression throws `std::invalid_argument`
before sending, changing application state, or changing the latest outcome.
Equal processing timestamps are allowed. A producer advances its processing
timeline before calling its injected sender; if that sender throws, time does
not roll back, while the previous completed send result remains available.

Engine and brake timeouts are independently configurable positive steady-clock
durations, each defaulting to 500 milliseconds. A never-received valid signal
is `Missing`, not a fabricated zero or a released brake. A received signal is
`Stale` at **publication time + timeout**, including the exact boundary.
Freshness arithmetic avoids overflow at extreme synthetic time points.

Each dashboard tick or message callback evaluates freshness at `now`, even if
the incoming message is invalid or unsupported. Delayed delivery does not
restart the timeout. Equal publication timestamps do not extend it. Rejected
messages do not replace data or accepted timestamps. Valid old-but-ordered
messages can be accepted while immediately classified stale. New valid traffic
recovers its own stream independently. Last valid values remain available
alongside health flags; consumers must check those flags before displaying
values as current. Calling `state()` alone does not advance time.

## Producer-to-consumer flow

1. The application registers a brake inbox for the event ID and a dashboard
   inbox for the two telemetry IDs.
2. Engine `on_tick(now)` encodes its inputs and calls its injected `ICanSender`.
3. `VirtualCanBus` copies the frame into each matching open inbox, in FIFO
   publication order. Per-inbox capacity and reject-newest overflow still apply.
4. The caller retrieves an owned frame through `ICanReceiver::try_receive()` and
   dispatches it to Dashboard `on_message(frame, now)`.
5. Dashboard validates the schema and timestamp and updates its state snapshot.

A brake event follows the same routing path to Brake `on_message()`, which
changes brake state. The next brake tick publishes status for the dashboard.
There is no callback recursion or direct Engine-to-Dashboard dependency.

`app/main.cpp` demonstrates normal communication, brake application, stale data,
and recovery using synthetic timestamps and no threads or sleeps. Unit tests
inject GoogleMock senders; a dashboard test dispatches frames from a mock
receiver. Additional integration tests exercise the real virtual bus, event
routing, independent recovery, and overflow without real-time waiting.

## Initial ECU-phase verification (before review)

On macOS arm64 with Apple Clang, the following commands were executed:

```sh
cmake -S . -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=/usr/bin/clang++ -DCMAKE_OSX_ARCHITECTURES=arm64 -DBUILD_TESTING=ON
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure --no-tests=error
./build/vaecu_sim
```

All four exited zero. Compilation emitted no warnings. CTest ran the 56
existing cases and 38 added cases (7 codec, 28 ECU/lifecycle, 3 integration),
reporting `100% tests passed, 0 tests failed out of 94` in 0.85 seconds. The
application produced the normal/apply/stale/recovery output shown in the README.
This verifies the current single-threaded ECU phase locally; it does not verify
Windows, Linux, threaded behavior, or automotive standards compliance.

## ECU-phase review and verification

The baseline configuration, build, and all 94 tests passed. Review found a
timing-contract gap: Engine and Brake documented a monotonic timeline but did
not enforce it, while Dashboard did. After applying a brake event at 20 ms,
Brake could publish the changed state with a callback timestamp of 10 ms.
Callbacks earlier than `start(now)` were also accepted by these producers.

Engine and Brake now retain the latest processing time and reject regressions
before observable work. Four new regression tests were added without changing
existing expectations. The four tests failed against the previous implementation
and passed with the guards. They also check that a sender failure preserves the
last completed result and does not allow subsequent time rewind. Message
formats, routing, lifecycle signatures, and valid scenarios are unchanged.
No shared runtime or new inheritance abstraction was introduced for these small
guards.

Review found no additional confirmed routing or ownership defect. Frames and
snapshots own their values; the sender reference has the documented non-owning
lifetime requirement. Queues and registry are bounded. The implementation
remains single-threaded: concurrent access to mutable bus/ECU state would race.
Threading and cross-platform CI are outstanding project phases, not verified
capabilities of this phase. No next-phase implementation was started.

The normal configure/build/CTest/application commands above were executed again.
Configuration, compilation, and application execution exited zero with no
compiler warnings; all **98 tests passed** in 0.80 seconds. The application
retained its documented normal/apply/stale/recovery output.

The failing pre-fix regression command was:

```sh
ctest --test-dir build --output-on-failure --no-tests=error -R 'RejectsBackwardCallbacks|FailedSendPreservesCompletedResult'
```

It exited eight with four failed tests. After the fix, the full suite included
those four cases and exited zero.

A separate sanitizer configuration reused the existing pinned GoogleTest source
from the normal build directory (no additional download). These exact commands
were also executed:

```sh
cmake -S . -B build-sanitized -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=/usr/bin/clang++ -DCMAKE_OSX_ARCHITECTURES=arm64 -DBUILD_TESTING=ON -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/Users/apple/Desktop/Embedded/build/_deps/googletest-src -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-sanitized --parallel 2
ctest --test-dir build-sanitized --output-on-failure --no-tests=error
./build-sanitized/vaecu_sim
```

All four exited zero. Compilation emitted no warnings. All **98 sanitizer tests
passed** in 6.51 seconds, and the captured CTest output log
(`build-sanitized/Testing/Temporary/LastTest.log`) contained no AddressSanitizer
or UndefinedBehaviorSanitizer reports. The sanitized application also ran with
the same output and no reports. These results cover the executed tests, not a
proof of memory safety for every possible program input. Current-phase build
and relevant test verification have no unresolved blocker on macOS arm64.
