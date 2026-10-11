# Virtual CAN bus

The bus implements synchronous message routing with bounded, thread-safe receive
queues. It creates no worker threads and never calls ECU behavior. The runtime
and lock order are documented in [concurrency.md](concurrency.md); concrete ECU
behavior is documented in [messages.md](messages.md). Earlier verification
sections below describe the original single-threaded bus phase.

## Routing and registration

`VirtualCanBus` implements `ICanSender`. Register named inboxes using
`register_receiver(name, accepted_ids, capacity)`. Names are local bookkeeping:
CAN-inspired frames have message IDs, not destination addresses. The bus copies
a publication to every open inbox whose exact-ID filter matches.

- The default registration limit is four, configurable at construction.
- Names must be nonempty and unique. Duplicate names never replace an inbox.
- Empty filters match nothing; duplicate IDs do not duplicate deliveries.
- Filters remain fixed for an inbox's lifetime.
- A new registration sees future publications only; there is no replay.
- Stopped registrations remain registered and cannot be silently replaced.
- `receiver(name)` returns an empty shared pointer for an unknown name.
- `stop_receiver(name)` returns false for an unknown name. For a known inbox,
  it closes it and returns true, including repeated stops.

Registration is bounded and performs the storage allocation. Invalid names or
zero queue capacities throw `std::invalid_argument`; a full registration table
throws `std::length_error`; a closed bus rejects registration with
`std::logic_error`. Allocation failures propagate without replacing existing
registrations. Configured limits must be positive.

The bus does not start or stop an `IEcu`. `ConcurrentSimulation` dispatches inbox
frames to `IEcu::on_message()` and closes the inboxes during cancellation.
There is no sender authentication or automatic sender exclusion: an inbox
receives any matching publication. Exclude a publisher's own IDs from its
filter when loopback is unwanted.

## Filtering and queueing

`CanReceiver` implements `ICanReceiver`. It owns an immutable 2048-bit filter
and a fixed-capacity ring of optional frame values. Matching and enqueueing are
separate private operations; only the bus can enqueue. No generic queue or
filter interface is introduced.

- Default capacity: 64 frames per inbox; any positive capacity is configurable.
- Storage is allocated once when the inbox is constructed.
- Successful publications require no additional queue allocation.
- FIFO order follows serialized `send()` admissions, independently per inbox.
  Concurrent callers are ordered by bus-mutex acquisition, with no fairness guarantee.
- Numeric identifiers and timestamps never reorder traffic.
- A full inbox rejects its newest incoming copy, preserving queued frames.
- Overflow at one inbox does not prevent delivery to another.
- Empty/filtered/stopped inboxes are not counted as overflow drops.
- `try_receive()` returns an owned frame, `Empty`, or `Closed` immediately.
- `receive_until(deadline)` waits on the inbox condition variable, returning an
  owned frame, `Empty` on deadline, or `Closed`; closure wakes all waiters.
- `pending()`, `capacity()`, and `is_closed()` expose queue state.

`SendResult` reports successful and overflow-dropped delivery copies.
`Routed` includes zero subscribers, partial delivery, and all matching inboxes
being full. Counts describe active matching recipients only. They describe
queue admission, not a callback being executed or a hardware acknowledgement.
Applications can accumulate these results if persistent counters are needed.

## Frame validation

`send()` rechecks the identifier range and active payload length before routing.
Ordinary `CanFrame` values already enforce those invariants at construction.

`send_raw(identifier, payload_array, payload_size, timestamp)` is the explicit
untrusted-envelope boundary. It rejects negative IDs, IDs above `0x7FF`, and
lengths above eight with `InvalidFrame` and zero delivery/drop counts. It checks
before narrowing, constructing a frame, indexing, or changing any inbox.
Tests use this boundary rather than corrupting private frame storage.

Arbitrary payload bytes in a valid envelope are transported unchanged; signal
schema validation belongs to the [message codecs](messages.md). The frame's explicit
timestamp is preserved, so this phase is deterministic and does not query a
clock. It does not establish an authoritative receive timestamp or freshness
policy.

## Closure and ownership

Inbox `close()` discards pending frames and returns the number discarded.
Repeated close returns zero. Closed inboxes always receive `Closed` and are
excluded from later fan-out.

Bus `close()` closes every inbox, returns the total pending delivery copies
discarded by that call, and rejects future sends with `Closed`. Closure takes
precedence over raw-input validation. Repeated bus close returns zero.

Registration returns a shared inbox handle. The bus also owns that inbox, so
dropping a caller handle does not unregister it. The bus destructor closes all
inboxes; handles held after destruction remain valid and return `Closed`.
There is no inbox-to-bus owning reference or ownership cycle.
Shared ownership protects the inbox's lifetime; its mutex protects mutable
state. Concurrent bus/inbox methods are now supported by the concrete classes,
though the abstract transport interfaces impose no such guarantee on other
implementations. External callers must finish using the bus before destroying
it. `ConcurrentSimulation` owns that lifetime and joins workers first.

## Changes to foundation interfaces

- `ICanReceiver::receive_until(deadline)` is replaced with `try_receive()`.
- `ReceiveStatus::Timeout` becomes `ReceiveStatus::Empty`.
- `SendStatus::InvalidFrame` is added for raw-envelope rejection.
- Existing frame, identifier, sender signature, and ECU interfaces are unchanged.

The nonblocking interface remains usable and testable without OS waits or a
clock. Concrete `CanReceiver` now also provides interruptible timed reception;
existing transport mocks and nonblocking tests retain their interfaces.

## CAN scope

This is FIFO software fan-out. Priority arbitration is not simulated. In
particular, a lower numerical identifier does not overtake queued traffic.
There is no electrical arbitration, bit timing, ACK/error frames, bus-off,
CAN FD, ISO-TP, or claim of full CAN standards compliance.

## Tests

GoogleTest cases cover registration limits and errors, fan-out, exact filters,
empty/duplicate filters, unknown names and unmatched IDs, FIFO independent of
IDs/timestamps, copied payloads, default-capacity boundaries, partial/all-full
overflow, ring wraparound, malformed raw envelopes, closure, and handles that
outlive the bus. These cases use no threads or sleeps.

## Earlier local verification (before the successful GoogleTest run)

On macOS arm64 using Apple Clang 21.0.0 and CMake 4.2.3:

- The application-only CMake configuration built `vaecu_core` and `vaecu_sim`
  successfully with the project warnings. The executable ran successfully.
- A supplementary dependency-free checker executed 5,303 assertions covering
  routing, filtering, malformed input, overflow, closure, retained handles, and
  a fixed-seed 1,000-operation comparison against a `std::deque` queue model.
- The same checker was compiled together with all core sources using
  `-fsanitize=address,undefined` and ran successfully, with no sanitizer reports.
- The test-enabled CMake configuration failed downloading pinned GoogleTest
  because the shell could not resolve `github.com`. CTest then reported no tests
  found. At that point, none of the 56 GoogleTest/GoogleMock cases (34 new bus
  cases) had been compiled or executed. The supplementary checks are not GoogleTest.
- `git diff --check` reported no whitespace errors.

Use the README's test-enabled commands with network access, or add
`-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/absolute/path/to/googletest-1.17.0` for a
trusted local extraction of the pinned dependency, to complete GoogleTest
verification. Windows, Linux, and threaded behavior are not verified here.

## Current-phase review: 2026-10-10

The review found no confirmed routing, queue-indexing, or ownership defect in
the current synchronous implementation. At the end of that review, the
GoogleTest/GoogleMock sources had not been compiled or executed.

The payload-size rejection test now starts with a full inbox and verifies that
the existing frame survives invalid input. The default-capacity test now drains
all 64 admitted copies and verifies an empty queue. Existing expectations remain
intact. Transport-interface comments clarify that shared ownership does not
provide synchronization; concurrent bus/inbox access would cause data races.
No production behavior or public signatures changed during this review.

The following verification commands were actually executed from the repository
root using Apple Clang on macOS arm64:

```sh
cmake -S . -B build-app -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_OSX_ARCHITECTURES=arm64 -DBUILD_TESTING=OFF
cmake --build build-app --parallel 2
./build-app/vaecu_sim

cmake -S . -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_OSX_ARCHITECTURES=arm64 -DBUILD_TESTING=ON
ctest --test-dir build --output-on-failure --no-tests=error

clang++ -std=c++17 -arch arm64 -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -I include /tmp/vaecu-review-checks.cpp build-app/libvaecu_core.a -o /tmp/vaecu-review-checks
/tmp/vaecu-review-checks
clang++ -std=c++17 -arch arm64 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -I include /tmp/vaecu-review-checks.cpp src/can_id.cpp src/can_frame.cpp src/virtual_can_bus.cpp -o /tmp/vaecu-review-checks-sanitized
/tmp/vaecu-review-checks-sanitized

git diff --check
```

- Application configuration, build, and execution exited zero; compilation
  emitted no warnings. The CLI printed the Phase 1 scaffold message.
- Test-enabled configuration exited one: pinned GoogleTest download failed
  with status 6, `Could not resolve host: github.com`. The existing empty
  download was correctly rejected by SHA-256 verification.
- CTest exited eight with `No tests were found!!!`; this is not a passing suite.
- Both supplementary checker runs exited zero and printed `Passed 5321
  dependency-free bus checks (not GoogleTest).` The temporary checker extends
  the earlier bus checker with the full-inbox invalid-payload regression and
  retains the fixed-seed queue-model comparison. The sanitized run emitted no
  AddressSanitizer or UndefinedBehaviorSanitizer reports.
- `git diff --check` exited zero for the tracked working-tree changes.

The temporary checker source is outside the repository and is supplementary
evidence only. At the end of that review, completing this phase still required
a successful test-enabled configuration, build, and GoogleTest/GoogleMock run
using network access or the trusted local dependency override described above.
No next-phase work started.

## Successful GoogleTest verification: 2026-10-10

GoogleTest 1.17.0 was available in the build dependency directory on retry.
The initial configuration succeeded, but CMake reset the Debug and architecture
options while refreshing its compiler cache. The final configuration used the
resolved compiler path to preserve the requested settings:

```sh
cmake -S . -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=/usr/bin/clang++ -DCMAKE_OSX_ARCHITECTURES=arm64 -DBUILD_TESTING=ON
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure --no-tests=error
./build/vaecu_sim
```

All four commands were executed and exited zero. The build compiled and linked
the actual GoogleTest/GoogleMock test executable without warnings. The cache
and generated compilation flags confirmed Debug, arm64, and `-std=c++17`.
CTest discovered and executed all 56 cases, including the strengthened bus
tests, with this result:

```text
100% tests passed, 0 tests failed out of 56

Total Test time (real) =   0.44 sec
```

The application printed `Virtual Automotive ECU Platform - Phase 1 scaffold`.
The previous GoogleTest/CTest verification blocker is resolved for this macOS
arm64 build. Windows, Linux, and concurrent behavior remain unverified. No
production source, test expectations, or CMake configuration was changed for
this successful rerun; no next-phase implementation started.
