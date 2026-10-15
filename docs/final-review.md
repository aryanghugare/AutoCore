# Final engineering and portfolio review

Review date: 2026-10-14. Assessment uses the checked-out implementation, not a
proposed architecture. No specific BMW TechWorks job description was provided;
the skill mapping is to a junior automotive/embedded C++ role in general.

The core/CLI are a credible portfolio demonstration. Cross-platform source intent
is supported by portable APIs and a new CI configuration, but hosted CI and other
OS builds are not proven. The optional browser dashboard is implemented and
component/process tested; socket restrictions still prevent end-to-end browser
verification. No production, hardware, certification or performance claim follows.

## Engineering conclusions

- Responsibilities are separated: ECUs own behavior; the bus validates envelopes,
  filters and queues; owners dispatch callbacks; codecs define byte layouts;
  diagnostics observe faults and a separate sink/store records transitions.
- Small interfaces allow GoogleMock tests without real receivers/threads.
  Producers borrow senders, the monitor borrows a sink, and those dependencies
  must outlive borrowers. No concrete ECU depends on another concrete ECU.
- Frames, snapshots and reports own values. Shared inbox handles can survive bus
  destruction in a closed state. Declaration order preserves runtime dependencies.
- Concurrent behavior has one worker owner per object; external reporting returns
  synchronized copies. Bus-before-inbox lock order, predicate waits, atomics,
  failure capture and joined shutdown are explicit. Tests exercise startup races,
  close with pending work, capacity, multiple producers and timeout/recovery.
- Resource growth is bounded by fixed receiver/event capacities, finite scenario
  schedules and dashboard history/body/line limits. Overflow is visible, not
  silently treated as successful receiver processing.
- FIFO software routing does not implement arbitration. Signal freshness uses
  publication time; communication timeout uses reception activity, a distinction
  exercised by malformed-message tests. Stop never manufactures fault recovery.
- The deterministic and four-worker paths share ECU/bus/diagnostic behavior but
  intentionally differ in scheduling. The dashboard uses the former path.

The C++ implementation was not rewritten. The one runtime correction in this
final pass was a demonstrated cross-language contract mismatch: C++ permits
0..9000 RPM while the TypeScript status validator accepted up to 12000. Two new
boundary cases failed before correction. The validator and display scale now
match 9000; a valid-limit case also passes. Existing assertions were retained.
The remaining changes are README/UML/ADRs/CI and this evidence/portfolio record.

## Skill review

| Skill area | Repository evidence | What remains outside the demonstrated scope |
| --- | --- | --- |
| C++17 | `std::array`, `optional`, `variant`, chrono, scoped enums, RAII, smart pointers; required standard with extensions OFF | Compiler/ABI behavior on other platforms; constrained firmware C/C++ practice |
| OOP | Small virtual interfaces, concrete final ECUs, composition and owner-managed lifecycles | No need to add pattern-heavy inheritance merely for a portfolio |
| Interfaces | `IEcu`, sender/receiver and diagnostic sink; lifetime and synchronization contracts | Hardware-driver interface experience and formal API compatibility policy |
| Stubs/mocks | GoogleMock senders/receivers/sinks, mock failures/partial delivery; controlled process fixture in dashboard tests | Mock success is not hardware or network integration evidence |
| Testing | Unit, negative, boundary, deterministic timeout/recovery, integration, real-thread and CLI golden-output cases; Vitest/jsdom and real C++ process tests | No measured line/branch coverage, fuzzing or browser end-to-end run; hosted OS jobs pending |
| OS/concurrency | Four workers, startup barrier, atomics, mutexes, CV deadline waits, cancellation, exception capture, joining and latest-value mailbox | No RTOS, interrupt priorities, hard real-time scheduling, WCET or CPU-budget evidence |
| CAN fundamentals | 11-bit identifiers, maximum eight-byte payload, explicit codecs, filtering, broadcast-style routing and loss policy | No electrical bus, bit-level arbitration, ACK/CRC, bus-off/error counters, CAN FD or hardware trace capture |
| Build systems | CMake production/test targets, pinned GoogleTest archive/hash, optional bridge/sanitizers, OS CI matrix configuration | Hosted runner results, release packaging and cross-compilation remain additional evidence |
| Debugging | Demonstrated failing regressions, precise fault/trace logs, injectable time and sanitizer checks | Debugger/static-analysis execution status is recorded below; no invented debugger expertise |
| Documentation | Runnable README, canonical message/fault contracts, source-traced UML, six ADRs and verified example outputs | Keep diagrams, protocol bounds and commands synchronized with subsequent code changes |

All tests here are regression evidence. Their count does not establish complete
coverage, absence of races or safety certification.

## Prioritized remaining gaps

1. **Obtain hosted cross-platform evidence.** Run the new workflow after the
   authorized GitHub push/PR, resolve real toolchain failures, retain results and
   confirm Windows x64/MSVC, Linux x64/GCC and macOS behavior. Do not display a
   green CI badge or say "cross-platform verified" beforehand. Intel macOS and
   actual Windows 10/11 desktop execution still need their own evidence.
2. **Complete the browser smoke test.** On an unrestricted local machine, start
   both servers; exercise all scenarios, cancellation during active faults,
   restart/reconnect and delayed data. Observe real incremental messages and
   historical/unavailable values. Add browser automation and optional Node/UI CI
   when the transport can actually be run.
3. **Measure quality instead of guessing.** Execute the available clang-tidy
   configuration, add compiler/tool coverage reporting if useful, and test a
   release/optimized build. Prioritize findings and fault/error branches over
   chasing a test-count or coverage target. No coverage percentage exists yet.
4. **Practice native debugging and target/toolchain work.** Inspect a real stack,
   locals and thread state in LLDB/GDB; explain a fault from evidence. A separate
   minimal cross-compiled/firmware exercise can add startup/linker, memory layout,
   interrupt and peripheral knowledge without pretending this host app is firmware.
5. **Deepen automotive protocol knowledge.** Study actual CAN arbitration/error
   handling and diagnostic transport/services, then validate against primary
   specifications and traces. UDS, ISO-TP, OBD, AUTOSAR and ISO 26262 concepts are
   additional study areas, not implemented/compliant capabilities of this project.
6. **Prevent schema drift.** Numeric bounds are mirrored in C++ and TypeScript.
   The RPM regression fixes the observed mismatch, but future codec/schema
   changes need explicit cross-language contract review/tests. Keep the C++ schema
   authoritative; avoid adding a generator until its maintenance value is clear.
7. **Improve reproducible repository evidence.** Capture a successful hosted run,
   code-review/PR history and a tagged demonstration snapshot when ready. C++ CI
   currently omits frontend jobs; static analysis and sanitizers are local opt-in
   checks. Durable scenario/diagnostic export and configurable traces are optional
   later enhancements, not prerequisites to explain the existing design.

Hardware experience is a separate genuine skills gap, not a dependency of this
software-only project. A physical bench or supervised lab exercise would be new
work and must not be claimed based on this simulation.

## Executed verification

Local environment: macOS arm64, Apple Clang 21.0.0, CMake 4.2.3, Node 22.22.0,
npm 10.9.4. Fresh C++ builds used a cached GoogleTest 1.17.0 source extraction
via the documented offline override. This does not count as verifying a fresh
remote download or a GitHub-hosted job.

| Check executed in this pass | Actual result |
| --- | --- |
| Fresh Debug C++ build, bridge ON | Built; 194/194 CTest cases passed |
| Fresh Debug C++ build, bridge OFF | Built; 189/189 CTest cases passed |
| Fresh ASan/UBSan C++ build, bridge ON | Built; 194/194 CTest cases passed, no sanitizer reports |
| Fresh separate TSan C++ build, bridge ON | Built; 194/194 CTest cases passed, no sanitizer reports |
| CLI-only build, testing and bridge OFF | Built; normal scenario exited successfully |
| All four CLI scenarios; help; recovery summary; concurrent demo | Exited successfully; logs agree with README/scenario table |
| Dashboard baseline build/tests | Built; 73/73 Vitest cases passed |
| RPM schema regression before correction | 9001 and 12000 wrongly accepted; two cases failed, valid 9000 passed |
| Dashboard after correction | TypeScript/bundle/server built; 76/76 Vitest cases passed |
| Workflow validation | YAML parsed and triggers/permissions/matrix/pin/stages structurally checked |
| UML validation | Relationships manually traced; explicit class/struct and member names matched to repository declarations |
| Debugger attempt | LLDB loaded Debug symbols and resolved Engine tick; process launch failed, so no stack/locals session completed |
| `npm run server`, `npm run dev` | Both failed to listen with `EPERM` on loopback ports 8080/5173 |

The same CMake configure/build/CTest stages used by CI were executed locally,
with separate build directories and the offline source override. This verifies
local stage behavior, not GitHub checkout/action execution or other OS toolchains.
The action's full SHA was checked against the
[official v6.1.0 release](https://github.com/actions/checkout/releases/tag/v6.1.0),
and labels against [the official runner image list](https://github.com/actions/runner-images).
`actionlint` and a Mermaid renderer are unavailable, so structural YAML and source
traceability checks are not described as actionlint/render validation.

Exact fresh C++ commands executed from the repository root:

```sh
cmake -S . -B build-final -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DVAECU_BUILD_BRIDGE=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/Users/apple/Desktop/Embedded/build/_deps/googletest-src
cmake --build build-final --config Debug --parallel 2
ctest --test-dir build-final -C Debug --output-on-failure --no-tests=error
cmake -S . -B build-final-default -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DVAECU_BUILD_BRIDGE=OFF -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/Users/apple/Desktop/Embedded/build/_deps/googletest-src
cmake --build build-final-default --parallel 2
ctest --test-dir build-final-default --output-on-failure --no-tests=error
./build-final/vaecu_sim --scenario normal
./build-final/vaecu_sim --scenario sensor-failure
./build-final/vaecu_sim --scenario timeout
./build-final/vaecu_sim --scenario recovery
./build-final/vaecu_sim --scenario recovery --summary-only
./build-final/vaecu_sim --concurrent
./build-final/vaecu_sim --help
```

Build/CTest and full scenario stdout were captured in `/tmp/vaecu-final-*.log`
and `/tmp/vaecu-final-<scenario>.txt`. These are local verification artifacts,
not required repository files. Root `build/` commands in README use the same
configuration with another directory name. Windows/Linux-specific variants were
reviewed against targets/generator paths but not executed on those platforms.

Inside `dashboard/`, executed:

```sh
npm run build
npm test
npm test -- tests/protocol.test.ts -t 'RPM limit'
npm run server
npm run dev
```

After the baseline, a clean offline `npm ci` installed 118 packages from cached
archives with the committed lockfile; the subsequent build and 76 tests passed.
This pass did not perform a fresh online registry installation. Prior cache preparation is recorded
in [the dashboard guide](dashboard.md). The temporary YAML validator used one
cached `yaml` package outside the repository; no production dependency was added.

## Technical interview walkthrough

A short explanation to adapt to your own words:

> I built a software-only C++17 simulation of Engine, Brake and Dashboard ECUs.
> Engine publishes a four-byte RPM/speed message, Brake publishes a one-byte
> state, and Dashboard consumes them through a virtual bus. The bus validates
> envelopes, filters by ID and copies messages into bounded FIFO inboxes. It
> doesn't implement physical CAN arbitration.
>
> ECU behavior is independent of transport and threads. I inject sender and
> diagnostic interfaces, so GoogleMock can verify publications and simulate
> failures. Diagnostics distinguishes invalid sensor data, old usable data and
> absent communication, stores fault transitions once, and records recovery.
>
> I have a deterministic scenario path for exact timeout tests and a separate
> four-worker runtime with explicit ownership, condition-variable waits and
> joined shutdown. For example, recovery pauses Engine publication, detects
> staleness at 500 ms and a communication timeout at 1000 ms, then restores it
> at 1250 ms. The CLI reports three faults and three recoveries.
>
> I verified the macOS builds and regression tests and configured a GitHub
> Actions OS matrix. An optional React dashboard consumes real C++ events through
> a local HTTP adapter. Browser networking and other OS results are still pending;
> I don't claim hardware, UDS or safety certification.

Suggested three-minute demonstration, after the README build:

1. Show [message IDs/byte encoding](../include/vaecu/messages.hpp), then explain
   `DC 05 3C 00` as 1500 RPM and 60 km/h using explicit little-endian fields.
2. Run `./build/vaecu_sim --scenario normal` and point out an Apply event reaching
   Brake, followed by Brake state reaching Dashboard. Two routed copies are
   Dashboard/diagnostic admission for telemetry, not two physical CAN sends.
3. Run `./build/vaecu_sim --scenario recovery --summary-only`, then the full
   recovery trace to show occurrence/recovery times. Explain why stop alone
   would not clear faults or make retained telemetry current.
4. Run `./build/vaecu_sim --concurrent` and describe four behavior owners, the
   startup barrier, bounded queues, atomics, snapshot locking and cancellation.
5. Show a boundary/mock regression and CTest results; finish with the documented
   limitations and the next verification step. Do not rely on browser networking
   for the interview until you have actually verified that local setup.

## Resume entry: verified claims only

**Virtual Automotive ECU Communication, Diagnostics & Testing Platform** —
C++17, CMake, GoogleTest/GoogleMock, Git; optional React/TypeScript.

- Developed a software-only three-ECU simulator with explicit CAN-inspired codecs,
  ID filtering, bounded FIFO queues and separate fault detection/storage.
- Implemented deterministic fault/recovery scenarios and a portable four-worker
  runtime with synchronized reporting, cancellation and joined shutdown.
- Verified macOS arm64 C++ builds with 194 bridge-enabled regression cases and
  76 dashboard/adapter tests; documented architecture and configured a GitHub
  Actions C++ OS matrix.

These bullets do not claim hosted CI success, hardware/firmware experience,
coverage percentages, benchmarks, UDS/AUTOSAR compliance or safety certification.
Explain AI-assisted implementation honestly if asked; demonstrate ownership by
walking through behavior, trade-offs, tests and failure handling in the code.

## Additional executed checks and boundaries

Sanitizer/application-only commands executed from the repository root:

```sh
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DVAECU_BUILD_BRIDGE=ON -DVAECU_ENABLE_ASAN=ON -DVAECU_ENABLE_UBSAN=ON -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/Users/apple/Desktop/Embedded/build/_deps/googletest-src
cmake --build build-asan --parallel 2
ctest --test-dir build-asan --output-on-failure --no-tests=error
cmake -S . -B build-tsan-check -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DVAECU_BUILD_BRIDGE=ON -DVAECU_ENABLE_TSAN=ON -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/Users/apple/Desktop/Embedded/build/_deps/googletest-src
cmake --build build-tsan-check --parallel 2
ctest --test-dir build-tsan-check --output-on-failure --no-tests=error
cmake -S . -B build-app -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=OFF -DVAECU_BUILD_BRIDGE=OFF
cmake --build build-app --parallel 2
./build-app/vaecu_sim --scenario normal
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DVAECU_BUILD_BRIDGE=ON
./build/vaecu_sim --scenario recovery --summary-only
```

Both sanitizer builds and their 194-test suites exited zero. No sanitizer
reports were observed. The CLI-only build/run and cached root configure/recovery
summary also passed. Results are not a proof of memory/race safety for all inputs.

The clean dependency installation executed inside `dashboard/` was:

```sh
npm ci --offline --full-metadata --cache=/tmp/vaecu-offline-cache --ignore-scripts --no-audit --no-fund
npm run build
npm test
```

It used a task-local cache and skipped install scripts; the bundle build verified
the installed native build dependencies on this arm64 machine. The temp path is
not committed and is not required for a normal `npm ci` elsewhere.

The debugger command executed was:

```sh
/usr/bin/lldb --batch -o 'breakpoint set -n vaecu::EngineEcu::on_tick' -o 'run --scenario normal --summary-only' -o 'bt' -o quit -- ./build-final/vaecu_sim
```

LLDB resolved the breakpoint to `src/engine_ecu.cpp:52`, then failed to launch
with `process exited with status -1 (no such process)`. Its exit status was 1.
`bt` did not complete. The cause was not established; debugger proficiency or a
successful debugging session is not inferred from symbol resolution.

The workflow YAML parsed with YAML 1.2 tooling and structural assertions checked
its three triggers, read-only permissions, three runner labels, full checkout SHA
and exact CMake/CTest stages. These checks passed. No hosted Actions job or
`actionlint` execution occurred.

Documentation checks passed for code fences, local links, 17 explicitly declared
UML class/struct names and 39 member names. Six Mermaid diagram blocks describe
two class views, one component view and three sequences. The separate PlantUML
component source supplies canonical UML notation. Sequence order/ownership was
cross-checked manually against the implementations; renderer/layout execution
remains unverified. The temporary checker initially distinguished only C++
`class` declarations and expected five blocks; correcting it to include value
`struct` classifiers and the actual six views resolved those audit-script
assumptions, not application failures.

Existing phase-specific verification sections remain historical records.
`docs/messages.md` current prose was updated to name the implemented concurrent
exception boundary and CLI modes without altering those historical results.
No C++ source, message bytes, interface signatures or dependency versions changed
in this pass. No Git commit, GitHub push or remote CI run was performed.
