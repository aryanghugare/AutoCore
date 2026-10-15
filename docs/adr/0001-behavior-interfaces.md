# ADR 0001: ECU behavior and dependency injection

Status: Accepted. Date: 2026-10-14.

## Context

ECU behavior must be unit-testable without a bus, threads, a terminal or a browser.
Three mutually dependent concrete ECUs would make isolated tests difficult.

## Decision

Use `IEcu` for `start`, `on_tick`, `on_message` and nonthrowing `stop`. Owners
serialize callbacks. Engine and Brake borrow `ICanSender&`; Dashboard receives
frames through callbacks and owns a `DashboardState`. `ICanReceiver` returns an
owned frame or an explicit Empty/Closed result. `IDiagnosticSink` allows the
monitor to be tested without storage. Interfaces have virtual destructors and
are noncopyable. Frames and report snapshots are values; the sender/sink must
outlive their borrower. No ECU creates its own thread or references another ECU.

## Alternatives and consequences

A common abstract ECU with bus/logging/thread members would couple behavior to
runtime policy. A generic event framework would add configuration and indirection
without helping these three ECUs. The small interfaces permit GoogleMock
interaction tests, but do not promise thread safety: concrete contracts and the
owner determine synchronization. Waiting reception stays on `CanReceiver` rather
than expanding the minimal mock receiver interface.

Evidence: `include/vaecu/ecu.hpp`, `can_transport.hpp`, ECU headers;
`tests/interfaces_test.cpp`, `ecus_test.cpp`, `diagnostics_test.cpp`.
