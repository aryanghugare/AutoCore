# Testing and code-quality guide

## Coverage review

This is a qualitative review of tested behaviors, not a measured line/branch
coverage percentage. The existing 155 cases were retained; seven cases were
added. Component timing tests use injected timestamps rather than sleeps.

| Area | Existing checks | Additional coverage in this pass |
| --- | --- | --- |
| Frames and codecs | ID bounds, lengths 0..8, rejected lengths, fixed-storage ownership, golden byte layouts, signal limits | Minimum/maximum telemetry through the complete producer/bus/consumer flow |
| CAN routing | Exact filters, fan-out, unknown names/IDs, FIFO, partial delivery, closure | Brake commands reach Brake/diagnostics; Dashboard changes only after Brake publishes status |
| ECU behavior | Lifecycle, invalid/unsupported messages, monotonic processing time, sender failures | Engine, Brake, Dashboard and diagnostics composed in normal and faulty scenarios |
| Diagnostics | Missing/stale/activity distinctions, inclusive thresholds, duplicate suppression, recovery, event overflow | Missing Engine with healthy Brake, malformed signals/commands, recovery timestamps, partially failed sink updates |
| Queues | Capacity one/default capacity, reject-newest overflow, wraparound, admitted/consumed/discarded accounting, concurrent producers | Shutdown discards six pending frame copies without applying their commands or telemetry |
| Lifecycle and concurrency | Stop before start, startup cancellation, concurrent stops/reporters, RAII join, wakeups | Complete-flow shutdown and sanitizer build checks; existing concurrency cases exercised under TSan |

The six cases in `tests/platform_integration_test.cpp` use real codecs, real ECU
behavior, and a real virtual bus. One case injects a strict mock sender into a
real Engine ECU: it verifies exact publication bytes/order, drops a publication,
throws on the next, then delegates to the real bus for recovery. Brake and
Dashboard continue operating. This models lost/faulty communication without
adding production test hooks. The additional strict mock sink test in
`tests/diagnostics_test.cpp` verifies that a successful transition is not repeated
when a later transition fails and is retried.

Future and replayed frames deliberately count as communication activity but
cannot replace accepted data or extend publication freshness. The integration
tests preserve this existing diagnostic policy rather than treating all invalid
traffic as silence.

## Ownership, exceptions, and synchronization review

- Frames, diagnostic records/events, and Dashboard snapshots are owned values.
  ECU sender/sink references are non-owning and must outlive their consumers.
  Receiver `shared_ptr`s preserve lifetime; receiver mutexes provide synchronization.
- Runtime construction starts no threads. Member declaration order preserves
  dependencies. Startup failure cancels and joins already-created workers;
  destruction uses idempotent stop. External reporters must be joined before
  destroying the runtime they access.
- ECU callbacks are confined to one worker. The bus locks before inboxes;
  receivers never acquire the bus lock. Routing does not call ECU code under
  queue locks. Store and snapshot reads return copies under their own mutexes.
  Worker paths do not acquire the lifecycle mutex held during joins.
- Condition-variable waits have predicates and closure notifications. Cancellation
  is coordinated with startup release; the diagnostic writer finishes before
  closing its event feed during normal execution. Memory remains bounded by
  fixed inboxes, the event ring, and the latest-value telemetry mailbox.
- Failed sends propagate exceptions and preserve the last completed send result.
  Diagnostic transitions commit individually only after the sink accepts them;
  a batch is not an atomic transaction. Processing time is not rolled back.
  The new partial-failure test exercises this distinction explicitly.

No new application correctness, ownership, race, or deadlock defect was confirmed
in this pass. Runtime code was not rewritten. OS mutex/join failures in noexcept
cleanup remain fatal, as documented by the existing runtime contract. Deliberate
thread-creation exhaustion/partial-start failure injection, cross-platform
execution, and schedule coverage beyond sampled runs remain verification gaps.
Per-ECU publication/drop counters are not exposed by the concurrent reporting API.

## Compiler and sanitizer options

Project targets use `/W4 /permissive- /Zc:__cplusplus` with MSVC. GCC and Clang
use `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow
-Wnon-virtual-dtor -Woverloaded-virtual`. These flags do not propagate to
GoogleTest/GoogleMock source builds.

`cmake/Sanitizers.cmake` provides three options, all off by default:

| Option | Purpose |
| --- | --- |
| `VAECU_ENABLE_ASAN` | AddressSanitizer |
| `VAECU_ENABLE_UBSAN` | UndefinedBehaviorSanitizer, with recovery disabled so violations fail tests |
| `VAECU_ENABLE_TSAN` | ThreadSanitizer, in a separate build directory |

ASan and UBSan may be combined. TSan cannot be combined with either option.
Enabling an option runs a compile/link probe for the requested sanitizer runtime;
unsupported configurations fail explicitly. The options use GCC/Clang GNU-style
flags, including Apple Clang. They do not implement MSVC or clang-cl sanitizer
flag support; leave them off in those compiler modes. A successful probe is not
evidence that the runtime works on a platform: execute CTest there to verify it.

Compile and link settings propagate from the core to the application and project
tests through an interface build target. Third-party test framework sources are
not instrumented by these options. Use fresh build directories rather than
mixing these options with cached manual `CMAKE_CXX_FLAGS` sanitizer settings.
GoogleTest discovery occurs at CTest time (`PRE_TEST`), so an instrumented test
program is not executed as part of linking. The existing 10-second per-case
CTest timeout remains in place.

The commands below are POSIX shell examples for macOS/Linux with a compatible
Clang/GCC toolchain. They reuse the existing trusted pinned GoogleTest source.
Omit the source override to use the normal hash-verified download.

```sh
cmake -S . -B build-quality-asan -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DVAECU_ENABLE_ASAN=ON -DVAECU_ENABLE_UBSAN=ON -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST="$PWD/build/_deps/googletest-src"
cmake --build build-quality-asan --parallel 2
ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-quality-asan --output-on-failure --no-tests=error

cmake -S . -B build-quality-tsan -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DVAECU_ENABLE_TSAN=ON -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST="$PWD/build/_deps/googletest-src"
cmake --build build-quality-tsan --parallel 2
TSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-quality-tsan --output-on-failure --no-tests=error
```

Sanitizers detect faults in executed code; successful runs do not prove that all
inputs or thread schedules are safe. If TSan is unavailable on another platform,
record that limitation and use stress tests, ASan/UBSan, and ownership/lock review
as partial verification, without claiming they replace race detection.

## Recommended clang-tidy pass (not executed here)

`.clang-tidy` selects a small set rather than every check:

- `clang-analyzer-core.*` and `clang-analyzer-cplusplus.*`: path-sensitive analysis
  of core operations and C++ lifetimes.
- `bugprone-use-after-move`: suspicious access to moved objects.
- `bugprone-unused-raii`: discarded RAII temporaries, particularly relevant to
  lock guards.
- `performance-unnecessary-value-param`: review expensive unnecessary copies.
- `modernize-use-override`: explicit virtual overrides.

These recommendations follow the [clang-tidy configuration documentation](https://clang.llvm.org/extra/clang-tidy/),
[use-after-move check](https://clang.llvm.org/extra/clang-tidy/checks/bugprone/use-after-move.html),
[unused-RAII check](https://clang.llvm.org/extra/clang-tidy/checks/bugprone/unused-raii.html),
and [value-parameter check](https://clang.llvm.org/extra/clang-tidy/checks/performance/unnecessary-value-param.html).
Review findings manually: small value types and intentional owned copies should
remain values. No automatic fixes or style-wide rewrites are configured.

The following commands are recommended, not reported as executed:

```sh
cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
clang-tidy --list-checks
clang-tidy -p build src/*.cpp app/main.cpp
```

Use Make or Ninja for the compilation database; check the installed clang-tidy
version for check availability. This pass found no `clang-tidy` executable on
PATH and did not install one or claim static-analysis results.

## Recommended additional verification (not executed)

- Build and test under Windows/MSVC, Linux/GCC or Clang, and Intel macOS; add
  those platforms to CI when implementing the CI phase.
- If startup-failure behavior changes, add controlled thread-creation failure
  injection rather than exhausting OS resources nondeterministically.
- Measure source/branch coverage with toolchain coverage instrumentation if
  a coverage percentage is needed; test counts are not coverage percentages.

## Executed verification: 2026-10-11

Environment: macOS arm64, Apple Clang 21.0.0, CMake 4.2.3. The baseline configure,
build and complete CTest run passed 155/155 cases in 1.80 seconds. Final complete
suite results after all test assertions were added:

| Configuration | Result | CTest elapsed time |
| --- | --- | --- |
| Debug | 162/162 passed | 3.31 s |
| ASan + UBSan options | 162/162 passed | 17.76 s |
| TSan option | 162/162 passed | 77.27 s |

The exact configure, build, and complete-suite commands executed were:

```sh
cmake -S . -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=/usr/bin/clang++ -DCMAKE_OSX_ARCHITECTURES=arm64 -DBUILD_TESTING=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure --no-tests=error

cmake -S . -B build-quality-asan -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=/usr/bin/clang++ -DCMAKE_OSX_ARCHITECTURES=arm64 -DBUILD_TESTING=ON -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/Users/apple/Desktop/Embedded/build/_deps/googletest-src -DVAECU_ENABLE_ASAN=ON -DVAECU_ENABLE_UBSAN=ON
cmake --build build-quality-asan --parallel 2
ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-quality-asan --output-on-failure --no-tests=error

cmake -S . -B build-quality-tsan -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=/usr/bin/clang++ -DCMAKE_OSX_ARCHITECTURES=arm64 -DBUILD_TESTING=ON -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/Users/apple/Desktop/Embedded/build/_deps/googletest-src -DVAECU_ENABLE_TSAN=ON
cmake --build build-quality-tsan --parallel 2
TSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-quality-tsan --output-on-failure --no-tests=error
```

All exited zero. Builds reported no compiler warnings. Generated compiler/link
settings were inspected to confirm instrumentation of the core, CLI, and project
tests, rather than relying only on successful option parsing. Test counts and
elapsed times are test-run observations, not benchmark or coverage claims.

Additional commands executed successfully:

```sh
./build/vaecu_tests --gtest_filter='Platform*IntegrationTest.*:DiagnosticMonitorTest.PartialSinkFailureRetriesOnlyTheUncommittedFault'
TSAN_OPTIONS=halt_on_error=1 ./build-quality-tsan/vaecu_tests --gtest_filter='Concurrent*' --gtest_repeat=25 --gtest_break_on_failure > /tmp/vaecu-quality-tsan-stress.log 2>&1
./build/vaecu_sim
./build/vaecu_sim --concurrent
ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./build-quality-asan/vaecu_sim --concurrent
TSAN_OPTIONS=halt_on_error=1 ./build-quality-tsan/vaecu_sim --concurrent
git diff --check
```

The targeted run passed all seven added tests. The stress log contains 25
successful repetitions of all 22 concurrency cases (550 case executions).
The normal deterministic CLI printed its 16 expected lines; the three concurrent
CLI runs printed the five expected scenario/shutdown lines and exited zero.
Complete CTest logs, stress logs and CLI runs reported no sanitizer errors.

Standalone sanitizer options were also configured, built, and exercised without
GoogleTest. These are application smoke runs, not complete automated test suites:

```sh
cmake -S . -B build-quality-asan-only -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Debug -DVAECU_ENABLE_ASAN=ON
cmake --build build-quality-asan-only --parallel 2
./build-quality-asan-only/vaecu_sim
cmake -S . -B build-quality-ubsan-only -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Debug -DVAECU_ENABLE_UBSAN=ON
cmake --build build-quality-ubsan-only --parallel 2
./build-quality-ubsan-only/vaecu_sim
```

All six commands exited zero. Both CLI runs retained the deterministic output.
Two negative configuration checks intentionally exited 1 with the explicit
"Use a separate build for TSan" error:

```sh
cmake -S . -B build-quality-invalid -DBUILD_TESTING=OFF -DVAECU_ENABLE_ASAN=ON -DVAECU_ENABLE_TSAN=ON
cmake -S . -B build-quality-invalid -DBUILD_TESTING=OFF -DVAECU_ENABLE_ASAN=OFF -DVAECU_ENABLE_UBSAN=ON -DVAECU_ENABLE_TSAN=ON
```

No unexpected test/build failure occurred in this pass and no existing assertion
was relaxed. clang-tidy analysis, deliberate OS resource failures, and builds on
Windows, Linux, or Intel macOS were not executed. The application remains a
software simulation without CAN hardware, UDS compliance, or safety certification.
