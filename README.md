# Virtual Automotive ECU Platform

A software-only C++17 portfolio project for automotive communication,
diagnostics, threading, and testing. Phase 2 adds validated communication values
and mockable interfaces. The CLI remains the Phase 1 placeholder. Concrete
ECUs, CAN routing, diagnostics, and worker threads are not implemented yet.

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
app/main.cpp                   Minimal executable
include/vaecu/                 Frame, identifier, time, ECU/transport interfaces
src/                          Validated identifier/frame implementation
tests/                        GoogleTest and GoogleMock tests
docs/foundations.md            Public contracts and ownership rules
```

`vaecu_core` is a static library. `vaecu_sim` links it without test dependencies.
See [the public contracts](docs/foundations.md) for interface details.

## Build, test, and run on macOS Apple Silicon

Run these commands from the repository root:

```sh
cmake -S . -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_OSX_ARCHITECTURES=arm64 -DBUILD_TESTING=ON
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure --no-tests=error
./build/vaecu_sim
```

Configuration should finish with `Configuring done`, `Generating done`, and
the build directory path. Build progress varies by CMake version. A successful
build includes `Built target vaecu_sim` and `Built target vaecu_tests`.

Expected test summary (after a successful test-enabled build):

```text
100% tests passed, 0 tests failed out of 22
```

The suite covers identifier/frame validation and interface mockability. It does
not test concrete ECUs or a bus, which are outside this phase.
Expected executable output:

```text
Virtual Automotive ECU Platform - Phase 1 scaffold
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
cmake -S . -B build-app -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_OSX_ARCHITECTURES=arm64 -DBUILD_TESTING=OFF
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

The initial verification target is macOS arm64 with Apple Clang. Windows,
Linux, Intel macOS, CI, and sanitizers will be verified in later phases. Build
results must be recorded separately from the expected output above.

Local verification results are recorded in [the foundation notes](docs/foundations.md).
Expected output above is not evidence that the GoogleTest suite has executed.

## Git initialization

This directory is already a Git repository. This phase changes working-tree
files only; it does not create commits or change remotes.

