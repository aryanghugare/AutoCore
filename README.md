# Virtual Automotive ECU Communication, Diagnostics & Testing Platform

A **software-only C++17 portfolio project** that simulates Engine, Brake and
Dashboard ECU behavior over a CAN-inspired virtual bus. It demonstrates explicit
message encoding, interface-based design, diagnostics, bounded queues, portable
threading, automated tests and reproducible command-line scenarios.

No ECU, microcontroller, sensor, CAN adapter or other automotive hardware is
required. This is a host application, not production firmware or a certified
vehicle system.

## What is implemented

- Engine publishes simulated RPM and vehicle speed; Brake consumes Apply/Release
  events and publishes state; Dashboard consumes telemetry with explicit freshness.
- Validated 11-bit IDs, fixed eight-byte frame storage, exact-ID filtering,
  serialized FIFO routing and per-receiver overflow reporting.
- A separate diagnostic monitor/storage pair records sensor, payload, missing,
  stale and communication-timeout faults, duplicate suppression and recovery.
- Deterministic scenarios use injected logical time. A separate concurrent mode
  runs three ECU workers and one diagnostic observer with coordinated startup,
  cancellation, blocking waits, synchronized reports and joined shutdown.
- CLI logs include relative time, hex IDs/payloads, availability and fault transitions.
- An optional React/TypeScript dashboard uses a managed C++ child and local HTTP
  adapter. The core has no frontend, JSON or networking dependency.

## Architecture

| Component | Responsibility |
| --- | --- |
| `CanId`, `CanFrame`, `messages` | Validated values and explicit little-endian signal codecs |
| `IEcu`, `ICanSender`, `ICanReceiver`, `IDiagnosticSink` | Small testable behavior/transport/storage interfaces |
| `VirtualCanBus`, `CanReceiver` | Synchronous fan-out, exact filters, owned queued copies and interruptible reception |
| `EngineEcu`, `BrakeEcu`, `DashboardEcu` | Independent behavior and display-ready state, without UI/thread ownership |
| `DiagnosticMonitor`, `DiagnosticStore` | Detection separated from synchronized records and bounded events |
| `run_demo_scenario` | Fresh deterministic Session, real event trace, optional synchronous host hooks |
| `ConcurrentSimulation` | Owns four workers, behavior objects, inbox handles and reporting synchronization |
| `vaecu_cli`, `vaecu_sim` | Argument parsing, presentation and CLI entry point |
| `vaecu_bridge`, optional `dashboard/` | Real JSON-line events, validated HTTP commands and React presentation |

Default inbox capacity is 64 frames and the bus permits four registrations.
Overflow rejects the newest copy for a full receiver without blocking other
receivers. Concurrent send admission is serialized; this is **routing, not CAN
arbitration**. Closing a CAN inbox discards pending frames and wakes waiters.
Diagnostic storage retains one record per known fault and the latest 64 events;
event overflow replaces the oldest and counts loss. Its closed feed remains
readable. Concrete ECU calls require one serialized owner; the runtime supplies
thread ownership rather than embedding locks/threads in every ECU.

Source-matched [UML class, component and sequence diagrams](docs/uml.md),
[architecture decision records](docs/adr/README.md), and
[the final engineering/portfolio review](docs/final-review.md) explain these choices.

The canonical [message schema](docs/messages.md) defines:

| ID | Payload | Flow |
| --- | --- | --- |
| `0x100` | 4 bytes: uint16 RPM, uint16 whole km/h, little-endian | Engine to Dashboard and diagnostics |
| `0x200` | 1 byte: released=0, applied=1 | Brake to Dashboard and diagnostics |
| `0x300` | 1 byte: Apply=1, Release=2 | Scenario to Brake and diagnostics |

RPM is limited to 0..9000 and speed to 0..300 km/h by this simulation's schema.
Timestamp is process-local publication time, not payload data or UTC.
See [diagnostic contracts](docs/diagnostics.md) for the project-specific fault catalog.

## Prerequisites

- CMake **3.25+**, a C++17 compiler and a compatible build tool.
- macOS Apple Silicon: Apple Clang, macOS SDK and Make from Xcode/Command Line
  Tools. Intel macOS is a source-level target, not locally verified here.
- Linux x64: GCC or Clang with C++17 support and Make or Ninja.
- Windows 10/11 x64: Visual Studio 2022 Build Tools, the Desktop development with
  C++ workload, MSVC and a Windows SDK. Use a developer terminal.
- First test-enabled configure normally needs GitHub access for GoogleTest.
- Optional dashboard only: Node **22.12+**, npm, registry access for `npm ci`,
  and a browser supporting `AbortSignal.timeout`/`AbortSignal.any`.

On macOS, install Command Line Tools with `xcode-select --install` if needed.
CMake and optional Node/npm must already be installed. Verify with:

```sh
cmake --version
clang++ --version
xcrun --show-sdk-path
make --version
```

Minimum C++ language support is enforced by CMake; no specific untested minimum
compiler version is claimed. Actual local verification used Apple Clang 21.0.0,
CMake 4.2.3 and Node 22.22.0. Windows/Linux commands below are provided from the
portable build configuration but have not been executed on those OSes here.

## Build, test and run the C++ CLI

From the repository root on macOS/Linux:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DVAECU_BUILD_BRIDGE=OFF
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure --no-tests=error
./build/vaecu_sim
```

The default compiler/generator follows the installed toolchain. On Apple
Silicon an explicit configure variant is:

```sh
cmake -S . -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=/usr/bin/clang++ -DCMAKE_OSX_ARCHITECTURES=arm64 -DBUILD_TESTING=ON -DVAECU_BUILD_BRIDGE=OFF
```

On Linux, optionally add `-DCMAKE_CXX_COMPILER=g++` or
`-DCMAKE_CXX_COMPILER=clang++`. Use a fresh build directory when changing
compiler, generator or architecture.

Windows PowerShell, from a Visual Studio developer terminal:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=ON -DVAECU_BUILD_BRIDGE=OFF
cmake --build build --config Debug --parallel 2
ctest --test-dir build -C Debug --output-on-failure --no-tests=error
.\build\Debug\vaecu_sim.exe --scenario recovery --summary-only
```

Expected successful test summary: **189 tests** with the bridge OFF; **194 tests**
with it ON. The bridge option is explicit above because CMake caches options.
These counts are regression cases, not coverage percentages. See the
[verification record](docs/final-review.md#executed-verification) for actual runs.

For a build without GoogleTest or Node:

```sh
cmake -S . -B build-app -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=OFF -DVAECU_BUILD_BRIDGE=OFF
cmake --build build-app --parallel 2
./build-app/vaecu_sim --scenario normal
```

## Run the four demonstration scenarios

Each command creates a fresh Session and completes automatically. No arguments
select Normal. These four CLI scenarios use logical time and **do not sleep**.

```sh
./build/vaecu_sim --scenario normal
./build/vaecu_sim --scenario sensor-failure
./build/vaecu_sim --scenario timeout
./build/vaecu_sim --scenario recovery
./build/vaecu_sim --scenario recovery --summary-only
./build/vaecu_sim --help
```

On Windows replace the executable prefix with `.\build\Debug\vaecu_sim.exe`.

| Scenario | What to observe | Logical duration | CAN transmissions | Raised / recovered / active |
| --- | --- | ---: | ---: | --- |
| `normal` | Healthy telemetry, Apply at 250 ms and Release at 500 ms | 500 ms | 8 | 0 / 0 / 0 |
| `sensor-failure` | Invalid raw RPM from 250 ms; last valid input retained | 500 ms | 8 | 1 / 0 / 1 |
| `timeout` | Engine silent from 250 ms; stale at 500 ms, timeout at 1000 ms | 1500 ms | 10 | 2 / 0 / 2 |
| `recovery` | Sensor failure and Engine silence; restored at 1250 ms | 1500 ms | 12 | 3 / 3 / 0 |

All durations are simulated timestamps, not benchmark measurements. Brake
continues publishing during Engine silence. Stop does not clear active faults.
Online/offline here indicates simulated communication availability, not a claim
about a physical ECU's electrical state.

Actual recovery summary from the verified CLI:

```text
Virtual Automotive ECU Platform | scenario=recovery | deterministic
Software CAN simulation | time=relative ms | routing=FIFO
Summary: duration=1500ms CAN=12 routed=24 processed=24 dropped=0
Diagnostics: raised=3 recovered=3 active=0 lost=0
Shutdown: all ECUs OFFLINE; pending_discarded=0
```

A normal telemetry line is:

```text
[0000 ms] CAN tx=Engine id=0x100 len=4 data=[DC 05 3C 00] routed=2 dropped=0
```

`routed` counts admitted receiver copies; `processed` counts actual dispatches.
See [the CLI guide](docs/cli.md) for complete logs, timelines and exit-code rules.

The separate real-thread demonstration is:

```sh
./build/vaecu_sim --concurrent
```

It waits for normal telemetry, applies Brake, pauses Engine publication, detects
its timeout, resumes publication and joins the four workers. Scheduling uses
real steady-clock time and is not deterministic. Successful output includes:

```text
concurrent shutdown: workers joined
```

## Optional React/TypeScript dashboard

Enable and build the managed C++ bridge first:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DVAECU_BUILD_BRIDGE=ON
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure --no-tests=error
cd dashboard
npm ci
npm run build
npm test
```

Then, in two terminals inside `dashboard/`:

```sh
npm run server
```

```sh
npm run dev
```

Open **http://127.0.0.1:5173**, select a scenario and press Start. Node serves
loopback port 8080; Vite proxies `/api`. Stop cancels the child; completed runs
retain a summary and clearly historical values. Ctrl+C shuts down the servers.
The dashboard shows ECU cards, RPM/speed, bounded CAN history and fault/recovery
records; current values require connected, fresh, valid data. Commands cannot
set arbitrary ECU internals. This runs real events from finite deterministic
scenarios paced at 0.25x, not a completed-report replay or the four-worker mode.

On Windows set `VAECU_BRIDGE_PATH` to `build/Debug/vaecu_bridge.exe` before server
or process tests. Alternate build directories also require that environment
variable. Exact setup, schema, HTTP status codes, limits and shutdown rules are
in [the dashboard guide](docs/dashboard.md). Browser-to-HTTP/visual verification
remains blocked in this workspace; successful component/process tests do not
establish browser end-to-end success.

## Dependencies, quality checks and CI

C++17 is required with extensions disabled. Project warnings include
`-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow
-Wnon-virtual-dtor -Woverloaded-virtual` for GCC/Clang and
`/W4 /permissive- /Zc:__cplusplus` for MSVC. Test libraries are separate targets.
GoogleTest/GoogleMock **1.17.0** is pinned through FetchContent with archive SHA-256:

```text
65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c
```

For an offline build, add
`-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/absolute/path/to/verified/googletest-1.17.0`.
Verify the pinned archive before extracting it: this override bypasses the
FetchContent archive check. Dashboard direct versions and archive integrity are
pinned by `package.json`/`package-lock.json`; use `npm ci`.

Optional compiler/runtime checks, from the root on a supported GCC/Clang mode:

```sh
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DVAECU_BUILD_BRIDGE=ON -DVAECU_ENABLE_ASAN=ON -DVAECU_ENABLE_UBSAN=ON
cmake --build build-asan --parallel 2
ctest --test-dir build-asan --output-on-failure --no-tests=error
cmake -S . -B build-tsan-check -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DVAECU_BUILD_BRIDGE=ON -DVAECU_ENABLE_TSAN=ON
cmake --build build-tsan-check --parallel 2
ctest --test-dir build-tsan-check --output-on-failure --no-tests=error
```

Use a separate TSan build; combining it with ASan/UBSan is rejected. Sanitizer
support is compile/link checked; MSVC modes are not supported by this helper.
`.clang-tidy` supplies focused analyzer/RAII/use-after-move checks. It does not
mean clang-tidy has run. [Testing notes](docs/testing.md) document coverage areas
and [concurrency notes](docs/concurrency.md) explain synchronization.

[The GitHub Actions workflow](.github/workflows/cpp-ci.yml) configures, builds and
tests the C++ project with the bridge on macOS, Ubuntu and Windows. It has no
publish/deploy step. It is configured, not yet a verified hosted CI run. Local
CI-equivalent stages and their offline dependency override are recorded in
[the final review](docs/final-review.md#executed-verification).

## Known limitations

- Software-only host simulation: **no physical CAN communication**, CAN adapter,
  electrical arbitration/signaling, ACK/CRC/bit-stuffing, bitrate model, bus-off
  behavior or full CAN protocol compliance. Only 11-bit data identifiers and up
  to eight payload bytes are modeled; no CAN FD/extended identifiers.
- **No real microcontroller firmware**, device drivers, interrupts, RTOS,
  cross-compilation, flashing, hardware-in-the-loop or physical sensor validation.
- Project-specific diagnostics: **no standards-compliant UDS/OBD/AUTOSAR claim**.
- **No production automotive safety certification**, ISO 26262/ASIL assessment,
  MISRA compliance evidence, real-time guarantee or vehicle-ready claim.
- Values/timelines are fixed demonstrations, not a physical vehicle model.
  Histories are bounded and not persisted; equal timestamps have no replay/authentication
  protection. Errors/loss policies are explicit, not guaranteed delivery.
- Local macOS arm64 verification is available. Other target OS/architectures,
  hosted CI and browser networking/visual rendering remain unverified here.
- No coverage percentage, latency/throughput benchmark, WCET or performance
  claim has been measured. Sanitizer/test passes cover executed paths only.

## Repository map and portfolio material

```text
app/                    CLI and optional C++ bridge applications
include/vaecu/          Public types, interfaces and runtime declarations
src/                    ECU, codec, bus, diagnostics and runtime implementations
tests/                  GoogleTest/GoogleMock and CLI process regression cases
cmake/                  Compiler sanitizer configuration
.github/workflows/      C++ OS-matrix CI configuration
dashboard/              Optional React, typed protocol, Node server and Vitest tests
docs/uml.md             Implementation-matched diagrams and source traceability
docs/adr/               Implemented decisions and trade-offs
docs/final-review.md    Verification, skill evidence, prioritized gaps and walkthrough
```

The [interview walkthrough and resume entry](docs/final-review.md#technical-interview-walkthrough)
use only verified project claims. The skill review is relevant to a junior
embedded/automotive C++ role; no specific BMW job-description text was supplied,
so no exact hiring-fit or experience claim is inferred.
