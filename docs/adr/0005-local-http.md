# ADR 0005: Optional local dashboard protocol

Status: Accepted. Date: 2026-10-14.

## Context

A browser needs structured data and validated controls, without adding web
libraries or direct ECU mutation to the C++ core.

## Decision

An optional C++ application `vaecu_bridge` uses the existing scenario hooks,
paces logical cycles at 0.25x and flushes version-1 JSON-line events. A Node
controller owns one child, reads actual events and serves loopback HTTP
snapshots. React polls `/api/state`; `/api/start` accepts only defined scenario
names, and `/api/stop` accepts only the versioned stop command. The Vite proxy
keeps browser requests same-origin. The controller bounds histories, validates
messages and event order, preserves the first failure and bounds child exit
after completion, error or Stop. Cooperative cancellation uses stdin; the reader
thread joins after input is closed. Forceful termination is an error fallback.
The frontend uses revisions/server identities, monotonic status age and request
elapsed time to avoid presenting delayed or disconnected values as current.

## Alternatives and consequences

WebSockets would avoid polling but need another transport/lifecycle mechanism.
An HTTP library inside C++ would expand core dependencies. JSON replay after a
completed run would misrepresent the simulation as live. This adapter keeps Node
and React optional, but adds a managed process and two local servers. It drives
finite deterministic scenarios, not the four-worker concurrent runtime, and
does not attach to another CLI instance. It is a local demonstration interface,
not an authenticated remotely deployable service. Forced termination cannot
promise C++ cleanup. Browser transport verification is separate from mocked
HTTP-handler and real-process tests.

Evidence: `app/bridge_main.cpp`, `bridge_json.cpp`, `dashboard/shared/protocol.ts`,
`dashboard/server/`, `dashboard/src/useSimulation.ts`, [launch guide](../dashboard.md).
