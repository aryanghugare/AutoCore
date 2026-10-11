# Optional local dashboard

The React/TypeScript dashboard uses the existing ECU, virtual bus, diagnostic
monitor, and fixed scenario runner. The CLI remains independently usable. Build
with `VAECU_BUILD_BRIDGE=ON` to add the C++ JSON adapter; the option defaults to
OFF. The core has no React, Node, JSON, HTTP, or WebSocket dependency.

## Communication design

Use a small loopback HTTP interface instead of adding a networking library to
C++. A Node server owns one `vaecu_bridge` child process and reads versioned JSON
lines from its stdout. React reads snapshots through Vite's same-origin `/api`
proxy. Node uses its built-in HTTP and child-process APIs, without Express or a
WebSocket dependency. HTTP polling is sufficient for these small fixed scenarios
and makes reconnection and commands straightforward. Its trade-offs are polling
latency and repeated snapshots; it is not a high-rate streaming transport.

```text
React components → typed HTTP client → Node HTTP/controller → C++ adapter
                                               ↑                   ↓
                                      bounded event state ← live JSON lines
                                                               ↑
                                              existing scenario / ECUs / bus
```

C++ invokes a synchronous observer as each real scenario event occurs. The host
pacing callback waits between cycles and can cancel before the next cycle.
Nothing replays a completed report as live telemetry. Default CLI execution
still has no waits. The adapter paces 250 ms of simulated time across roughly
one second of wall time (0.25×), giving two-second normal/sensor demonstrations
and six-second timeout/recovery demonstrations. These are finite runs; Start
creates a fresh session. This dashboard uses the deterministic scenario path,
not the separate four-worker `--concurrent` CLI demonstration. It does not attach
to another running CLI process.

## Build and launch

Requirements: the existing C++17/CMake tools, Node **22.12 or newer**, npm, and a
current browser supporting `AbortSignal.timeout` and `AbortSignal.any`.
`package.json` pins direct package versions; `package-lock.json` pins the full
resolved dependency graph and archive integrity. Use `npm ci`, retaining the
lockfile. Initial dependency installation normally needs registry access.
[Vite documents its Node requirements and proxy settings](https://vite.dev/guide/)
and [React documents TypeScript support](https://react.dev/learn/typescript).

From the repository root on macOS or Linux:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DVAECU_BUILD_BRIDGE=ON
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure --no-tests=error
cd dashboard
npm ci
npm run build
npm test
```

On Apple Silicon, Apple Clang's native arm64 default is suitable. The README's
explicit Apple Clang/architecture flags can also be used. GoogleTest's documented
trusted offline source override remains available when GitHub is inaccessible.

Terminal 1, from the repository root:

```sh
cd dashboard
npm run server
```

Expected successful startup:

```text
Virtual ECU API: http://127.0.0.1:8080 (loopback only)
```

Terminal 2, from the repository root:

```sh
cd dashboard
npm run dev
```

Open **http://127.0.0.1:5173**. Select a scenario and press Start. Stop cancels a
running scenario and retains its fault records. After completion or cancellation,
all ECU cards become offline and the summary appears. Historical last-received
values remain labeled as historical. Ctrl+C stops each local server; stopping the
HTTP adapter first also cancels and joins its C++ child.

The ports are fixed: API 8080, browser 5173. Vite uses `--strictPort`; resolve a
port conflict instead of allowing an unexpected origin. Both processes bind only
to `127.0.0.1`. The dashboard's API calls use the dev proxy. To view a production
bundle locally, after `npm run build`, use:

```sh
npx vite preview --host 127.0.0.1 --port 5173 --strictPort
```

Preview also proxies `/api`; the Node server must still be running.

For Windows/MSVC with a multi-configuration generator, use `cmake --build build
--config Debug` and `ctest --test-dir build -C Debug --output-on-failure`. Before
`npm run server` or `npm test`, set the executable location in PowerShell, from
inside `dashboard/`:

```powershell
$env:VAECU_BRIDGE_PATH = (Resolve-Path ../build/Debug/vaecu_bridge.exe).Path
```

`VAECU_BRIDGE_PATH` is also supported on macOS/Linux for alternate build
directories. Its value is an executable path, not a shell command. The server
uses `spawn` with `shell: false` and a fixed argument array. The default is
`build/vaecu_bridge` (`build/vaecu_bridge.exe` on Windows). Windows and Linux
launches have not been verified here.

The CLI continues to work without Node:

```sh
./build/vaecu_sim --scenario recovery --summary-only
```

## HTTP and event contracts

`dashboard/shared/protocol.ts` is the TypeScript wire schema and runtime validator.
`app/bridge_json.cpp` defines the C++ serialization, with GoogleTest assertions
for actual wire bytes. Existing IDs and byte encoding remain documented only in
[the message schema](messages.md); fault IDs remain in
[the diagnostic catalog](diagnostics.md). No browser-side payload decoding or ECU
business logic is duplicated.

| Endpoint | Request | Result |
| --- | --- | --- |
| `GET /api/state` | No body | `200`, current versioned snapshot |
| `POST /api/start` | `{"version":1,"scenario":"recovery"}` | `202`, new session snapshot |
| `POST /api/stop` | `{"version":1}` | `200`, snapshot after child termination |

Allowed scenarios: `normal`, `sensor-failure`, `timeout`, `recovery`. Extra command
fields, unknown scenarios, malformed JSON, and unsupported versions are rejected
with `400`; an active/stopping session rejects another Start with `409`. Oversized
commands get `413`; non-JSON commands get `415`; disallowed browser origins get
`403`; unknown routes/methods get `404`. Startup failure appears in snapshot
`run.state="error"` with a reason. A closing controller rejects Start with `503`.
Errors have `{"version":1,"error":"..."}`. Responses disable caching. Commands
never expose individual ECU state setters.

A snapshot carries server identity, revision, run ID/state/scenario/error,
latest ECU/display status, wall-clock status age, CAN history, fault records,
scenario actions, and an optional final summary. New runs clear prior histories.
A server restart changes `serverId`; clients accept a fresh server snapshot.

C++ emits these discriminated JSON line types, all with `version:1` and
nonnegative relative simulation `timeMs`:

- `action`: started/stopped and scenario fault/recovery transitions.
- `can`: source, numeric 11-bit ID, zero to eight numeric bytes, admission counts
  `routed` and `dropped`. This observes a real send; admission does not imply
  callback completion. Summary `processed` counts actual receiver dispatches.
- `status`: Engine/Brake/Dashboard online/offline, Engine/Brake freshness,
  nullable RPM/speed/brake state. Missing values are `null`, never made-up zeros.
- `diagnostic`: project-specific numeric fault code, catalog name, active/cleared.
- `summary`: duration via `timeMs`, cancellation, transmissions, routed/processed
  copies, dropped copies, raised/recovered/active faults, lost diagnostic events,
  and pending frames discarded at shutdown.

Example real Engine publication at simulation time zero:

```json
{"version":1,"type":"can","timeMs":0,"source":"Engine","id":256,"payload":[220,5,60,0],"routed":2,"dropped":0}
```

The backend rejects invalid schemas, backwards event time, recovery without a
fault occurrence, events after a summary, and inconsistent final transmission
or active-fault counts. It retains at most 200 CAN rows, 32 actions, and 11 fault
records. Each complete or unfinished JSON line is limited to 16 KiB; valid read
batches can contain multiple lines. UTF-8 characters split between reads are
preserved. Stderr retention is limited to 8192 characters, and HTTP
command bodies to 1 KiB. Core scenario schedules also bound the retained C++
trace. CAN routing remains FIFO; this does not introduce priority arbitration
or simulate electrical CAN behavior.

Fault aggregation is reporting only. Detection remains in the C++ diagnostic
monitor. Duplicate active notifications do not inflate occurrences; reactivation
increments the occurrence count. Recovery timestamps remain distinct from first
and last occurrence. Stop preserves active faults and never manufactures a
recovery. These are project-specific diagnostics, not UDS services.

## Ownership, cancellation, and UI freshness

Node's event loop owns the child handle and all retained event state; snapshot
responses are copies. Only one child is active at a time. The C++ scenario thread
owns all ECU behavior. One C++ input thread owns stdin and updates cancellation
under a mutex; the pacing thread waits on a condition variable using a predicate.
No callback from the reader accesses an ECU. Graceful Stop sends `stop\n` then
closes stdin. EOF also requests cancellation. Upon a natural final summary or an
error, Node closes stdin so the input thread can join. This executable is a
managed subprocess, not a standalone interactive console. If it fails to exit
within two seconds after cancellation, failure, or the final summary, the parent reports an error and
forces termination. Normal paths use cooperative cleanup; forced termination
cannot provide C++ RAII guarantees.

The browser polls every 100 ms after each completed request, avoiding overlapping
polls. Failures retry after one second; requests time out after 3.5 seconds and
are aborted on unmount. For the same server, snapshot revisions determine which
response is newer; request order resolves equal revisions and server restarts.
Status age uses monotonic elapsed clocks on both sides and conservatively includes
the full HTTP request/decoding time. It also advances locally while a request
hangs, independently of system-clock changes. Active ECU cards, including those
in startup/shutdown, become unknown when their status is too old. Telemetry's
"Observed at" timestamp describes the C++ display observation, not the original
publication/receipt time of an Engine frame. Current RPM/speed require a connected, running session, fresh
Engine data, online Engine, non-null values, no active Engine sensor fault, and a
status update within two seconds of wall time. Otherwise the prominent values are
`—`, and any last received values are explicitly historical. This wall-time
transport freshness rule is separate from the engine's injected simulation-time
stale/timeout rules. Sensor failure suppresses current metrics even when the
Engine intentionally continues publishing its last valid input.

Connection/loading/error states are visible, controls are disabled when their
operation is invalid, and the CAN panel offers auto-scroll with a bounded history.
Fault records show active and recovered status with occurrence/recovery times.

## Tests and verification

`npm test` runs Vitest with Node tests and jsdom React tests. It requires a built
C++ bridge for the subprocess integration tests. Tests cover schema boundaries,
invalid commands, HTTP conflicts/errors/limits, actual streaming through all four
C++ scenarios, cancellation with active faults, fresh run isolation, missing
executables, typed client failures, stale/disconnected values, display recovery,
CAN formatting, loading/controls, connection retry, local aging, and response
ordering. HTTP handlers are exercised with in-memory request/response streams;
they do not require listening on a socket.

Executed verification and environment limitations are recorded below. A real
browser-to-HTTP smoke test remains necessary on an unrestricted local machine:
launch both processes, run each scenario, observe growing CAN history before
completion, stop recovery before 1250 ms, verify active faults remain, restart,
and disconnect the HTTP server to verify unknown ECU status and historical
telemetry. This check has not passed merely because component tests passed.

### Executed on this workspace

On macOS arm64, Apple Clang 21.0.0, Node 22.22.0 and npm 10.9.4:

| Check | Actual result |
| --- | --- |
| Original baseline CMake build and CTest before dashboard edits | 186/186 passed |
| Bridge-enabled Debug build and CTest | 194/194 passed |
| `npm run build` in `dashboard/` | Type checking, Vite production bundle, and server compilation passed |
| `npm test` in `dashboard/` | 57/57 passed across seven files, including all four real C++ scenarios |
| Bridge-enabled ASan + UBSan build and CTest | 194/194 passed; no sanitizer reports |
| Bridge-enabled separate TSan build and CTest | 194/194 passed; no sanitizer reports |
| Controller/HTTP tests against each sanitized bridge | 17/17 passed in each configuration before the final bounded-input refinement |
| Final unsupported/oversized stdin tests against each sanitized bridge | 2/2 passed in each configuration |
| Core-only build with bridge OFF and tests OFF | Built; real CLI recovery summary ran successfully |
| `npm run server` | Failed to listen: `EPERM` on `127.0.0.1:8080` |
| `npm run dev` | Failed to listen: `EPERM` on `127.0.0.1:5173` |

The last two failures are workspace socket restrictions. Browser-to-HTTP
transport and visual browser rendering remain **unverified**. The completed
integration tests use actual C++ processes/stdio and HTTP handlers, with React
DOM behavior tested in jsdom; they do not claim a network or browser smoke test.
Other OS/CPU combinations and GitHub Actions are also unverified.

Registry DNS was unavailable in this workspace (`ENOTFOUND`), so initial normal
npm installation failed. Installation succeeded offline using existing official
npm archive cache entries and a task-local npm cache; archive integrity was
verified by npm. The executed install command was:

```sh
npm ci --offline --full-metadata --cache=/tmp/vaecu-offline-cache --ignore-scripts --no-audit --no-fund
```

The temporary cache path is specific to this verification environment. It is not
a project setting or a launch requirement. Use normal `npm ci` elsewhere. The
lockfile retains registry URLs, not temporary or machine-specific paths.
An incidental build command issued from the repository root failed because the
npm scripts live in `dashboard/`; the command was rerun there successfully.

Additional verification commands actually executed from the repository root:

```sh
cmake -S . -B build-dashboard-core-only -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=OFF -DVAECU_BUILD_BRIDGE=OFF
cmake --build build-dashboard-core-only --parallel 2
./build-dashboard-core-only/vaecu_sim --scenario recovery --summary-only
cmake -S . -B build-dashboard-sanitized -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DVAECU_BUILD_BRIDGE=ON -DVAECU_ENABLE_ASAN=ON -DVAECU_ENABLE_UBSAN=ON -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/Users/apple/Desktop/Embedded/build/_deps/googletest-src
cmake --build build-dashboard-sanitized --parallel 2
ctest --test-dir build-dashboard-sanitized --output-on-failure --no-tests=error
cmake -S . -B build-dashboard-tsan -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DVAECU_BUILD_BRIDGE=ON -DVAECU_ENABLE_TSAN=ON -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/Users/apple/Desktop/Embedded/build/_deps/googletest-src
cmake --build build-dashboard-tsan --parallel 2
ctest --test-dir build-dashboard-tsan --output-on-failure --no-tests=error
```

Additional commands actually executed from `dashboard/`:

```sh
VAECU_BRIDGE_PATH=/Users/apple/Desktop/Embedded/build-dashboard-sanitized/vaecu_bridge npm test -- tests/controller.test.ts tests/http.test.ts
VAECU_BRIDGE_PATH=/Users/apple/Desktop/Embedded/build-dashboard-tsan/vaecu_bridge npm test -- tests/controller.test.ts tests/http.test.ts
VAECU_BRIDGE_PATH=/Users/apple/Desktop/Embedded/build-dashboard-sanitized/vaecu_bridge npm test -- tests/controller.test.ts -t 'stdin'
VAECU_BRIDGE_PATH=/Users/apple/Desktop/Embedded/build-dashboard-tsan/vaecu_bridge npm test -- tests/controller.test.ts -t 'stdin'
```

The absolute paths above record this workspace's commands. Substitute your own
verified GoogleTest source and executable paths when repeating them.

Actual completed scenario results from the process integration tests:

| Scenario | CAN transmissions | Faults raised | Recoveries | Active at completion |
| --- | ---: | ---: | ---: | ---: |
| Normal | 8 | 0 | 0 | 0 |
| Sensor failure | 8 | 1 | 0 | 1 |
| Communication timeout | 10 | 2 | 0 | 2 |
| Recovery | 12 | 3 | 3 | 0 |

### Files for this addition

- `dashboard/src/`: React components, typed HTTP client, polling hook and styles.
- `dashboard/shared/protocol.ts`: event/snapshot/command types and validators.
- `dashboard/server/`: process ownership, bounded report aggregation, HTTP API.
- `dashboard/tests/`: protocol, HTTP, process, client, hook and component tests.
- `dashboard/package*.json`, TypeScript/Vite configuration and `index.html`.
- `app/bridge_main.cpp`, `app/bridge_json.hpp`, `app/bridge_json.cpp`: optional
  paced C++ host and JSON serialization.
- `include/vaecu/demo_scenario.hpp`, `src/demo_scenario.cpp`: generic observation
  and cancellation/pacing hooks; default behavior retained.
- `tests/demo_scenario_test.cpp`, `tests/bridge_json_test.cpp`: hook/cancellation
  regression cases and JSON contract assertions.
- `CMakeLists.txt`, `.gitignore`, `README.md`, `docs/dashboard.md`: optional build,
  generated-file ignores and documentation.

Existing ECU/bus/diagnostic modules, the concurrent runtime and CLI implementation
were not rewritten for the dashboard. Preexisting working-tree changes were
preserved; no commits or remote changes were made.

## Review follow-up

This review began with actual successful C++/dashboard builds and baseline
results of 194 CTest cases and 57 Vitest cases. Eight added regressions failed
against the previous behavior, followed by a separate reproduced synchronous
spawn failure. All were corrected without removing or weakening existing
assertions. The final Vitest suite has **73 passing tests** across eight files.

Corrections:

1. `Date.now()` made status age depend on system-clock adjustments. Node and
   React now measure elapsed age with monotonic `performance.now()` clocks.
2. The browser previously ignored time in transit and JSON decoding, allowing a
   delayed response to make an old sample look current. The client conservatively
   adds the request's full elapsed duration. This can hide values slightly early
   during slow requests; it does not overstate freshness.
3. Request start order could override a newer server revision. Within one
   `serverId`, revision now decides freshness; request order handles equal
   revisions and responses spanning server restarts.
4. Starting/stopping ECU cards previously bypassed the age check. All active
   lifecycle states now show unknown when status is missing or aged. The metric
   time label now says "Observed at" because it is a display observation time.
5. A final summary closed stdin without bounding the remaining process lifetime.
   Natural completion, errors, and Stop now share one two-second exit deadline.
   Concurrent Stop requests share that timer; termination clears it. Secondary
   failures preserve the first error rather than replacing the root cause.
6. The JSON parser treated an entire read batch as one bounded line, rejecting
   valid groups of short events. Limits now apply to individual complete/partial
   lines in bytes. Streaming UTF-8 decoding preserves split characters. Tests
   cover batches over 16 KiB, the exact line boundary, oversized lines, split
   reads, malformed events, backwards time, hanging children, and timer cleanup.
7. An empty executable configuration throws synchronously from Node `spawn`.
   It previously left a phantom starting session. It now produces an error
   snapshot with no pending child or unresolved completion promise.
8. The real-process test waited for two transmitted frames before asserting
   display status. Those sends precede receiver dispatch; readiness now requires
   both frames and a status. All existing telemetry assertions are retained.

The existing C++ ECU/bus/diagnostic code, CLI, CMake configuration, wire schema,
and dependency versions were unchanged during this review. Inspection found no
additional C++ ownership or lock-order defects. The bus retains bus-before-inbox
locking without callbacks under locks; workers own their ECU behavior; snapshots
and diagnostic records are copied under their existing synchronization. Unit
fault injection uses a process fixture with real streams and controlled exit;
it introduces no production abstraction or fabricated runtime telemetry.

Files modified during this review:

- `dashboard/server/controller.ts`
- `dashboard/src/api.ts`
- `dashboard/src/useSimulation.ts`
- `dashboard/src/components/EcuCards.tsx`
- `dashboard/src/components/Telemetry.tsx`
- `dashboard/tests/api.test.ts`
- `dashboard/tests/components.test.tsx`
- `dashboard/tests/useSimulation.test.tsx`
- `dashboard/tests/controller.test.ts`
- `docs/dashboard.md`

File added: `dashboard/tests/controller_failure.test.ts`.
Preexisting working-tree changes were preserved.

Commands executed from the repository root and actual results:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DVAECU_BUILD_BRIDGE=ON
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure --no-tests=error > /tmp/vaecu-review-baseline-cpp.log
./build/vaecu_sim --scenario recovery --summary-only
git diff --check
```

Configuration/build passed with project warnings enabled. CTest passed 194/194.
The CLI recovery demonstration exited successfully with 12 CAN transmissions,
three raised/three recovered faults, and all ECUs offline. Whitespace checks
passed. No C++ source changes followed that CTest run.

Commands executed inside `dashboard/` and actual results:

| Command | Actual result |
| --- | --- |
| `npm run build` | Type checking, production bundle, and server compilation passed, including after all fixes |
| `npm test` | Baseline 57/57; final 73/73 passed |
| `npm test -- tests/api.test.ts tests/components.test.tsx tests/useSimulation.test.tsx tests/controller_failure.test.ts` | Before fixes: 8 failed, 25 passed; after fixes: 33/33 passed |
| `npm test -- tests/controller_failure.test.ts` | Final 11/11 passed |
| `npm test -- tests/controller.test.ts -t 'synchronous'` | Failed before the spawn correction; passed afterward (one selected case, eight skipped) |
| `npm run server` | Server compiled; loopback listen failed with `EPERM` on port 8080 |
| `npm run dev` | Loopback listen failed with `EPERM` on port 5173 |

Also executed inside `dashboard/`:

```sh
VAECU_BRIDGE_PATH=/Users/apple/Desktop/Embedded/build-dashboard-tsan/vaecu_bridge npm test -- tests/controller.test.ts tests/http.test.ts
```

That run used the existing ThreadSanitizer-instrumented bridge and passed 18/18
process/HTTP tests with no sanitizer reports. It preceded the additional
synchronous-spawn regression, which does not launch a C++ process. The final
73-test run covers that regression and all four real scenario flows.

Outstanding verification: the workspace still prohibits loopback listeners.
Browser-to-HTTP integration and visual rendering remain unverified. Windows,
Linux, and Intel macOS builds remain unverified. The dashboard still runs the
fixed deterministic scenarios rather than the separate four-worker CLI runtime,
as documented above. No next phase was started.
