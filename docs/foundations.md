# Communication foundations

The foundation phase defines values and behavioral boundaries only. The next
phase's bus and queues are documented in [virtual_can_bus.md](virtual_can_bus.md).
Concrete ECUs are now described in [messages.md](messages.md); there are no
worker threads yet.
The frame resembles an 11-bit, eight-byte classic CAN data frame; it does not
implement the full CAN standard, CAN FD, electrical signaling, or wire timing.

## Identifier: `CanId`

`CanId` is an explicitly constructed value with private storage. Its signed
64-bit constructor checks the input before narrowing to 16-bit storage, so
negative values and oversized identifiers cannot silently wrap into valid IDs.
The supported range is inclusive: `0` through `0x7FF`.

- `CanId(value)` throws `std::out_of_range` when the input is invalid.
- `is_valid(value)` is a nonthrowing predicate for external input validation.
- `value()` returns the numeric identifier without implicit conversion.
- Equality, inequality, and ordering compare identifier values. Ordering is
  useful for identifier collections; it does not implement CAN arbitration.

Identifiers are copyable and movable, with no default invalid state.

## Time: `Timestamp`

`Timestamp` aliases `std::chrono::steady_clock::time_point`. It denotes
monotonic process-local simulation time, not UTC, a serialized field, or CAN
hardware timing. Callers supply time explicitly; no clock interface is needed.

Tests can construct timestamps from a zero epoch and advance them with chrono
durations. Constructors do not query the system clock. Zero and negative
epoch offsets are valid; freshness and timestamp authority belong to later
runtime/transport policy, not envelope validation in this phase.

## Frame: `CanFrame`

The constructor takes a validated `CanId`, an eight-byte `Payload` array,
an active byte count, and a `Timestamp`. Counts from zero through eight are
valid; a larger count throws `std::length_error` before any payload indexing.
`is_valid_payload_size()` provides a nonthrowing precheck.

The frame owns its storage and normalizes unused bytes to zero. Its getters
return the identifier, active size, timestamp, and a const reference to the
array. Only `[0, payload_size())` constitutes the active payload. There are no
field setters; changing a frame means constructing or assigning another valid
value. Copy/move operations preserve the invariant and require no allocation.

The reference returned by `payload()` remains tied to that frame's lifetime.
Frame assignment replaces the whole value and is not a synchronized operation.
All values may be read concurrently only while nobody modifies or destroys
them. These types do not provide their own locks.

Envelope validity does not validate application signal encoding. Version
fields, signal ranges, and decoding will be implemented in a later phase.

## Transmission: `ICanSender`

`send(const CanFrame&)` returns `SendResult`. It must not wait for queue
capacity. It may need a short synchronization wait in a future implementation;
this is not a lock-free guarantee. Any retained frame must be copied, never
referenced through caller-owned storage.

`SendResult` contains `SendStatus`, `delivered`, and `dropped`:

- `Routed`: matching recipients were considered. Counts can describe complete
  delivery, partial delivery, all drops, or zero matching subscribers.
- `Closed`: admission is closed; both counts must be zero.
- `InvalidFrame`: envelope validation failed; both counts must be zero.
- For `Routed`, the number of matching recipients is `delivered + dropped`.

Typed frames enforce their invariants. The bus phase adds a raw-envelope
validation boundary that can return `InvalidFrame` before construction. Result
counts are an implementation contract; the plain record does not enforce them.
Unexpected implementation failures may throw and will need a future runtime
exception boundary. Automatic retries are not part of this interface.

## Reception: `ICanReceiver`

`try_receive()` returns a `ReceiveResult`, a variant containing either
an owned `CanFrame` or a `ReceiveStatus`:

- `Empty`: no frame is currently queued.
- `Closed`: the endpoint closed; closure takes precedence over queued frames.

Reception returns immediately. The bus phase replaces the originally proposed
timed receive operation to keep single-threaded behavior deterministic. Timed
waiting and concurrent-call support remain future runtime concerns.

The separate sender/receiver interfaces let ECU behavior depend only on
publication while a runtime handles reception and dispatches ECU callbacks.
There is no combined transport inheritance hierarchy.

## ECU behavior: `IEcu`

The user-requested ECU interface is a narrow behavior boundary:

- `start(now)`: initialize business state once per instance.
- `on_tick(now)`: advance periodic behavior while started.
- `on_message(frame, now)`: consume a const frame while started. `now` is the
  processing time; it can differ from the frame timestamp.
- `stop() noexcept`: release behavior resources without throwing. It must be
  idempotent and safe after a failed start or before start.

The owner calls `start()` at most once and never calls ticks/messages before a
successful start or after stop. It serializes all calls. These hooks do not
create threads, block for reception, join workers, or implement diagnostics.
The interface itself does not enforce lifecycle state. Concrete Engine, Brake,
and Dashboard implementations enforce it and have their own lifecycle tests;
their contracts are documented in [messages.md](messages.md).

All interfaces have virtual destructors and prohibit copying/moving identity.
The owner must call `stop()` before destroying an active ECU. Interface
destruction does not implicitly invoke the virtual stop hook. References are
non-owning dependencies; an owning polymorphic object can use `std::unique_ptr`.

## Tests

GoogleTest cases cover ID boundaries and extremes, all payload lengths,
oversized payload counts, normalized padding, independent storage, explicit
timestamps, and copy/move behavior. GoogleMock checks calls through sender,
receiver, and ECU interfaces, including partial delivery, no subscribers,
closure, empty inboxes, and lifecycle call ordering. Compile-time assertions check
strong typing, const payload access, virtual destruction, and `noexcept` stop.

Mock tests establish that the interfaces are usable and mockable. They do not
prove routing, lifecycle enforcement, or thread safety. Actual routing and queue
behavior are covered separately by the bus tests.

## Foundation-phase local verification (historical)

On macOS arm64 with Apple Clang 21.0.0 and CMake 4.2.3:

- CMake configured and built `vaecu_core` and `vaecu_sim` with
  `BUILD_TESTING=OFF`; the executable ran successfully.
- A separately compiled, dependency-free check program ran 113 assertions
  covering ID boundaries/extremes, all payload lengths, padding, storage
  ownership, timestamps, value copying/moving, and receive-result alternatives.
  It also compiled all public headers and their interface static assertions.
  This is supplementary verification, not a GoogleTest execution.
- The test-enabled configuration failed while fetching pinned GoogleTest:
  `Could not resolve host: github.com`. None of the 22 GoogleTest/GoogleMock
  cases has been compiled or executed in this environment.
- The Git whitespace check completed without errors.

To complete GoogleTest verification with network access, use the README's
test-enabled configure, build, and CTest commands. For an offline build, supply
`FETCHCONTENT_SOURCE_DIR_GOOGLETEST` pointing to a trusted extraction of the
pinned GoogleTest 1.17.0 source archive.
