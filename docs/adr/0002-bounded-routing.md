# ADR 0002: Virtual bus routing and bounded queues

Status: Accepted. Date: 2026-10-14.

## Context

A software bus needs predictable ordering and bounded storage. Blocking a
producer until another ECU consumes data would complicate shutdown and make
faulty consumers stall healthy receivers.

## Decision

`VirtualCanBus` synchronously validates and copies a frame to every matching
open `CanReceiver`. Each receiver has an immutable exact-ID filter and a
preallocated ring of optional frame values. Default inbox capacity is 64; the
bus defaults to four registration slots. A full inbox rejects the newest copy;
other inboxes can still accept it. `SendResult.delivered` counts admitted copies,
not completed callbacks. Sends are serialized, so receivers observe the same
publication order. Neither CAN ID priority nor timestamp reorders admission.
The bus mutex is acquired before inbox mutexes; no ECU callback runs under them.
Close discards inbox frames, reports their count and wakes waiting consumers.
Unknown receiver lookups never create an inbox.

Diagnostic events use a different policy: `DiagnosticStore` retains the newest
64 events by overwriting the oldest and counts discards, while fault records
remain retained. Closing that feed retains pending events for reporting.

## Alternatives and consequences

Unbounded queues conceal overload. Blocking capacity waits risk coupled stalls.
A global bus dispatcher or priority queue would add scheduling policy absent
from the simulation's requirements. Reject-newest preserves queued work and
exposes loss, but no retransmission or guaranteed delivery is implemented.
FIFO routing is explicitly not electrical CAN arbitration. Different diagnostic
retention favors recent transitions; consumers must inspect discard counts.

Evidence: `virtual_can_bus.hpp/.cpp`, `diagnostics.hpp/.cpp`;
`tests/virtual_can_bus_test.cpp`, `concurrency_test.cpp`, `diagnostics_test.cpp`.
