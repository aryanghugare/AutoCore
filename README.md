# Virtual Automotive ECU Platform

A software-only C++17 portfolio project for automotive communication,
diagnostics, threading, and testing. Engine, Brake, and Dashboard ECUs now
communicate through a thread-safe virtual CAN bus with exact-ID filters
and bounded FIFO inboxes. The CLI demonstrates telemetry, brake application,
stale data, sensor faults, communication timeouts, and recovery. A
separate diagnostic observer stores bounded fault records and transition events.
The default mode uses deterministic callbacks; `--concurrent` runs four workers.

## Requirements

- CMake 3.25 or newer.
- A C++17 compiler and a compatible build tool.
- macOS Apple Silicon: Apple Clang and the macOS SDK from Xcode or Command Line
  Tools, plus Make (provided by the Apple tools).
- Network access to GitHub for the first test-enabled configuration.

If Apple Command Line Tools are missing, run `xcode-select --install` and
complete the installer. Verify the tools with:

```sh
cmake --version
clang++ --version
xcrun --show-sdk-path
make --version
```

## Structure

```text
CMakeLists.txt                 Core, executable, and test targets
cmake/Sanitizers.cmake         Optional sanitizer support checks and flags
.clang-tidy                   Recommended static-analysis checks
app/main.cpp                   Thin executable entry point
app/cli.cpp                    Argument parsing and trace formatting
app/concurrent_demo.cpp        Existing concurrent demonstration
app/bridge_main.cpp            Optional live JSON adapter (VAECU_BUILD_BRIDGE)
dashboard/                    Optional React/TypeScript UI and local HTTP adapter
include/vaecu/                 Frame, transport, message schema, and ECU declarations
src/                          Frame, codecs, virtual bus, ECUs, and scenario runner
tests/                        GoogleTest and GoogleMock tests
docs/foundations.md            Public contracts and ownership rules
docs/virtual_can_bus.md        Routing, queue, closure, and interface changes
docs/messages.md               Message encoding, ECU behavior, and freshness
docs/diagnostics.md            Fault identifiers, detection, records, and recovery
docs/concurrency.md            Worker ownership, waits, cancellation, and sanitizers
docs/testing.md                Coverage review, quality checks, and sanitizer options
docs/cli.md                    Scenario timelines, output, exit codes, and verification
docs/dashboard.md              Local dashboard launch, protocol, and verification
```

`vaecu_core` is a static library independent of terminal presentation.
`vaecu_cli` links the core and supplies argument parsing and formatting;
`vaecu_sim` links the CLI without test dependencies.
See [the public contracts](docs/foundations.md) and
[the bus contracts](docs/virtual_can_bus.md) for interface details. Message IDs,
byte layouts, units, and ECU behavior are defined in
[the simulation schema](docs/messages.md).
The [diagnostic contracts](docs/diagnostics.md) define the fault catalog and
deterministic timing policy. The diagnostics are project-specific and do not
implement UDS services.
The [concurrent runtime contracts](docs/concurrency.md) describe synchronization
and bounded reporting. ECU behavior objects still require serialized calls.

## Build, test, and run on macOS Apple Silicon

Run these commands from the repository root:

```sh
cmake -S . -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=/usr/bin/clang++ -DCMAKE_OSX_ARCHITECTURES=arm64 -DBUILD_TESTING=ON
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure --no-tests=error
./build/vaecu_sim
```

Configuration should finish with `Configuring done`, `Generating done`, and
the build directory path. Build progress varies by CMake version. A successful
build includes `Built target vaecu_sim` and `Built target vaecu_tests`.

Expected test summary (after a successful test-enabled build):

```text
100% tests passed, 0 tests failed out of 189
```

The suite covers identifier/frame validation, interface mockability, routing,
filtering, FIFO ordering, malformed envelopes, overflow, unknown receivers, and
closure, explicit message codecs, concrete ECU lifecycle and behavior,
dashboard freshness, recovery, communication between ECUs, diagnostic storage,
duplicate suppression, missing/stale data, sensor/payload faults, and timeout
boundaries. Component tests retain injected time; concurrency tests use actual
threads and bounded condition-variable waits. New tests cover fixed scenario
plans, repeatable logs, communication status, diagnostic recovery, argument
validation, output failures, and executable exit codes.

Select a scenario (no arguments selects `normal`):

```sh
./build/vaecu_sim --scenario normal
./build/vaecu_sim --scenario sensor-failure
./build/vaecu_sim --scenario timeout
./build/vaecu_sim --scenario recovery
./build/vaecu_sim --scenario recovery --summary-only
./build/vaecu_sim --help
```

Expected output for `--scenario recovery --summary-only`:

```text
Virtual Automotive ECU Platform | scenario=recovery | deterministic
Software CAN simulation | time=relative ms | routing=FIFO
Summary: duration=1500ms CAN=12 routed=24 processed=24 dropped=0
Diagnostics: raised=3 recovered=3 active=0 lost=0
Shutdown: all ECUs OFFLINE; pending_discarded=0
```

Detailed output includes relative timestamps, hex IDs and payloads, online/offline
status, and fault transitions. Scenarios run without wall-clock sleeps. Engine
becomes offline at the communication-timeout threshold, independently of sensor
validity and signal freshness. See [the CLI guide](docs/cli.md) for complete
normal output, all scenario outcomes, and their timing policies.

To run the concurrent demonstration:

```sh
./build/vaecu_sim --concurrent
```

It publishes telemetry, applies the brake, pauses Engine publication until a
communication timeout, resumes it, and joins the workers. A typical successful
run prints:

```text
concurrent normal: engine=fresh rpm=1500 speed_kph=60 brake=fresh applied=false
concurrent apply: engine=fresh rpm=1500 speed_kph=60 brake=fresh applied=true
concurrent timeout: engine=stale rpm=1500 speed_kph=60 brake=fresh applied=true
concurrent recovery: engine=fresh rpm=1500 speed_kph=60 brake=fresh applied=true
concurrent shutdown: workers joined
```

Scheduling is not deterministic; timeout detection uses actual steady-clock
time. No real-time deadline guarantee is implied. See the
[concurrency notes](docs/concurrency.md) for queue policies and sanitizer commands.

## Dependency strategy

Tests are enabled by default through CMake's `BUILD_TESTING` option.
`FetchContent` downloads GoogleTest 1.17.0 from its official release archive
and verifies SHA-256:

```text
65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c
```

GoogleTest and GoogleMock are built from that pinned release; the test target
links both. Dependencies stay in the build directory. No dependency
from GoogleTest is linked into the application. CMake's `Threads::Threads`
provides platform thread-link requirements. Changing the test dependency requires deliberately
updating both the URL and hash. An unavailable download fails configuration.

To build only the application without downloading GoogleTest:

```sh
cmake -S . -B build-app -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=/usr/bin/clang++ -DCMAKE_OSX_ARCHITECTURES=arm64 -DBUILD_TESTING=OFF
cmake --build build-app --parallel 2
./build-app/vaecu_sim
```

This does not build or execute tests.

For an offline test build, use an existing trusted extraction of GoogleTest
1.17.0 by adding `-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/absolute/path/to/googletest-1.17.0`
to the test-enabled configure command. This override skips archive hash
verification; verify the official archive before extracting it.

## Portability and verification

C++17 is required explicitly and compiler extensions are disabled. Warnings
apply to project targets only: GCC/Clang use `-Wall -Wextra -Wpedantic
-Wconversion -Wsign-conversion -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual`; MSVC uses `/W4 /permissive-
/Zc:__cplusplus`.

Concurrent-phase results and exact commands are recorded in
[the concurrency notes](docs/concurrency.md). On macOS arm64 with Apple Clang,
all 154 tests passed in Debug, ASan/UBSan, and ThreadSanitizer builds. The 21
concurrency cases also passed 25 repetitions under TSan, with no reported races.
The subsequent review added a cancellation regression: all 155 tests passed in
Debug, ASan/UBSan, and TSan builds. Both cancellation cases passed 25 repetitions
under TSan. Review corrections and exact executed commands are recorded in the
same concurrency notes.
The earlier testing pass added complete ECU flows and mock fault scenarios. See
[the testing guide](docs/testing.md) for the coverage review, opt-in
`VAECU_ENABLE_ASAN`, `VAECU_ENABLE_UBSAN`, and separate `VAECU_ENABLE_TSAN` builds,
recommended clang-tidy checks, and that pass's executed results.
The latest CLI-phase build/test results are recorded in [the CLI guide](docs/cli.md).
The optional dashboard build and verification are recorded in
[the dashboard guide](docs/dashboard.md).
Windows, Linux, Intel macOS, and CI
remain unverified. Executed results are separate from expected output above.
Earlier diagnostic-phase results remain in [the diagnostic notes](docs/diagnostics.md),
[the ECU notes](docs/messages.md) and [the bus notes](docs/virtual_can_bus.md).

## Optional React dashboard

The dashboard adds ECU cards, live RPM/speed, a bounded CAN table, fault and
recovery records, and validated start/stop/scenario controls. It uses a loopback
HTTP adapter managing the C++ JSON subprocess. Node/frontend dependencies are
optional; the CLI and core remain independent.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DVAECU_BUILD_BRIDGE=ON
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure --no-tests=error
cd dashboard
npm ci
npm run build
npm test
```

Then run `npm run server` and `npm run dev` in separate terminals inside
`dashboard/`, and open http://127.0.0.1:5173. Requires Node 22.12 or newer.
The bridge-enabled C++ suite contains 194 tests; the default bridge-disabled
suite contains 189. An existing CMake cache retains the bridge option until
explicitly changed. See [the dashboard guide](docs/dashboard.md) for architecture,
schemas, Windows executable paths, commands, actual test results, and local
transport verification limitations.

## Git initialization

This directory is already a Git repository. This phase changes working-tree
files only; it does not create commits or change remotes.

