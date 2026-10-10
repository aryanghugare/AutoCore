# Virtual Automotive ECU Platform

A software-only C++17 portfolio project for automotive communication,
diagnostics, threading, and testing. Engine, Brake, and Dashboard ECUs now
communicate through a single-threaded virtual CAN bus with exact-ID filters
and bounded FIFO inboxes. The CLI demonstrates telemetry, brake application,
stale data, and recovery. Diagnostics management and worker threads remain
later phases.

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
app/main.cpp                   Deterministic ECU communication demonstration
include/vaecu/                 Frame, transport, message schema, and ECU declarations
src/                          Frame, codecs, virtual bus, and ECU implementation
tests/                        GoogleTest and GoogleMock tests
docs/foundations.md            Public contracts and ownership rules
docs/virtual_can_bus.md        Routing, queue, closure, and interface changes
docs/messages.md               Message encoding, ECU behavior, and freshness
```

`vaecu_core` is a static library. `vaecu_sim` links it without test dependencies.
See [the public contracts](docs/foundations.md) and
[the bus contracts](docs/virtual_can_bus.md) for interface details. Message IDs,
byte layouts, units, and ECU behavior are defined in
[the simulation schema](docs/messages.md).

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
100% tests passed, 0 tests failed out of 98
```

The suite covers identifier/frame validation, interface mockability, routing,
filtering, FIFO ordering, malformed envelopes, overflow, unknown receivers, and
closure, explicit message codecs, concrete ECU lifecycle and behavior,
dashboard freshness, recovery, and communication between ECUs. It creates no
threads or real-time sleeps.
Expected executable output:

```text
normal: engine=fresh rpm=1500 speed_kph=60 brake=fresh applied=false
apply: engine=fresh rpm=2500 speed_kph=80 brake=fresh applied=true
stale: engine=stale rpm=2500 speed_kph=80 brake=stale applied=true
recovery: engine=fresh rpm=2500 speed_kph=80 brake=fresh applied=true
```

## Dependency strategy

Tests are enabled by default through CMake's `BUILD_TESTING` option.
`FetchContent` downloads GoogleTest 1.17.0 from its official release archive
and verifies SHA-256:

```text
65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c
```

GoogleTest and GoogleMock are built from that pinned release; the test target
links both. Dependencies stay in the build directory. No dependency
is linked into the application. Changing the dependency requires deliberately
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
-Wconversion -Wsign-conversion -Wshadow`; MSVC uses `/W4 /permissive-
/Zc:__cplusplus`.

The current verification target is macOS arm64 with Apple Clang. The 98-test
suite also passed with AddressSanitizer and UndefinedBehaviorSanitizer on that
platform. Windows, Linux, Intel macOS, and CI remain unverified. Build results
must be recorded separately from the expected output above.

Actual verification results for the current ECU phase are recorded in
[the schema and ECU notes](docs/messages.md). Earlier bus results remain in
[the bus notes](docs/virtual_can_bus.md).

## Git initialization

This directory is already a Git repository. This phase changes working-tree
files only; it does not create commits or change remotes.

