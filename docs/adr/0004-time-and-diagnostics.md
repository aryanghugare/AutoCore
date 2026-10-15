# ADR 0004: Time semantics and diagnostic separation

Status: Accepted. Date: 2026-10-14.

## Context

Sensor validity, usable message freshness and link activity are different facts.
Tests must exercise exact timeout boundaries without sleeping.

## Decision

`Timestamp` is a steady-clock time point passed into callbacks, not a UTC value
or CAN payload field. Behaviors reject backwards processing time. Explicit
byte codecs validate IDs, lengths and application limits; Engine speed is
0..9000 RPM and vehicle speed 0..300 whole km/h. Dashboard freshness expires at
publication time plus its timeout, default 500 ms. `DiagnosticMonitor` detects
raw sensor errors, missing/stale messages, invalid payloads and communication
timeouts. Its default startup grace/stale/timeout periods are 500/500/1000 ms.
Link activity uses reception time even for malformed payloads; malformed traffic
can clear a communication timeout while leaving stale/payload faults active.
The monitor sends transitions to `IDiagnosticSink`; `DiagnosticStore` retains
one record per catalog fault, occurrence/recovery times and occurrence counts.
Both suppress repeated identical states. Stop never manufactures recovery.

## Alternatives and consequences

Wall-clock reads inside ECUs would make boundary tests slow and fragile. A new
virtual clock object is unnecessary because callback timestamps already provide
injection. Mixing detection into each ECU would duplicate communication rules.
Project-specific fault identifiers are simpler than pretending to implement UDS.
A separate observer requires its owner to dispatch subscribed frames and ticks,
including during silence. Last valid data can remain stored; its explicit health
must be checked. Tests are deterministic logical-time tests, not timing benchmarks.

Evidence: `time.hpp`, `messages.hpp/.cpp`, `dashboard_ecu.cpp`, `diagnostics.cpp`;
`tests/messages_test.cpp`, `ecus_test.cpp`, `diagnostics_test.cpp`.
