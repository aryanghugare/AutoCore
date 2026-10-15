# ADR 0003: Execution modes and thread ownership

Status: Accepted. Date: 2026-10-14.

## Context

Tests need repeatable logical time, while the portfolio also needs real
producer/consumer scheduling, cancellation and joining.

## Decision

`run_demo_scenario` runs a fresh private Session on its caller with explicit
250 ms logical cycles. Optional synchronous `DemoHooks` observe real events and
allow a host to pace or cancel before a cycle. With no hooks it performs no waits.
`ConcurrentSimulation` instead owns four `std::thread` workers: Engine, Brake,
Dashboard and DiagnosticMonitor. Each behavior is accessed only by its worker.
Startup uses a predicate condition-variable barrier; worker failures are captured
and cancel the runtime. Inbox waits use deadlines, ticks cannot be starved by a
busy inbox, and missed periods are skipped rather than caught up in bursts.
Atomics carry publication/cancellation controls. A mutex protects copied display
snapshots; the store provides its own synchronized fault/event access. Stop
closes the bus, wakes waiters, joins all workers and closes diagnostic reporting.
The destructor stops; instances start once and are not restarted.

## Alternatives and consequences

One thread for all production behavior would hide scheduling concerns. An RTOS,
thread pool or detached worker scheme is unnecessary here. Standard C++17 threads
are portable, but have no real-time scheduling guarantee. Deterministic and
concurrent modes share behaviors, not identical scheduling. The fixed scenario
trace is bounded by its schedule; concurrent display reporting is a latest-value
mailbox rather than a telemetry history. No ECU getter is safe for arbitrary
cross-thread access. External callers must release the runtime's lifetime before
destruction. This is not a firmware scheduling architecture.

Evidence: `demo_scenario.hpp/.cpp`, `concurrent_simulation.hpp/.cpp`;
`tests/demo_scenario_test.cpp`, `concurrency_test.cpp`, `platform_integration_test.cpp`.
