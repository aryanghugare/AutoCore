# ADR 0006: Portable CMake targets and verification

Status: Accepted. Date: 2026-10-14.

## Context

The project must make toolchains, dependencies and verification reproducible,
without claiming cross-platform success from source-level portability alone.

## Decision

CMake requires C++17 without compiler extensions. `vaecu_core`, `vaecu_cli`,
`vaecu_sim` and optional bridge targets keep production code separate from tests.
`Threads::Threads` supplies toolchain thread requirements. Project targets use
compiler-specific warnings; GoogleTest/GoogleMock 1.17.0 is fetched from an
explicit archive with SHA-256 verification. CTest discovers unit/integration
cases and runs CLI process tests. ASan/UBSan and separate TSan builds are opt-in
and compile/link checked. The CI matrix targets macOS 15, Ubuntu 24.04 and Windows
Server 2022, builds Debug with the bridge, and runs CTest. Read-only checkout is
pinned to a verified action commit; there is no deploy step or secret requirement.

## Alternatives and consequences

System-installed test libraries would vary across machines. Vendoring would add
maintenance; a pinned FetchContent archive is small but needs first-use network
access. The documented trusted-source override permits offline builds but skips
archive verification at configuration. Warning-as-error is not forced across
unknown compiler versions. Sanitizers are useful defect detectors, not proofs.
MSVC sanitizer modes are not enabled by the current helper. The workflow is a
configuration until hosted jobs actually pass; local stage equivalence is not
hosted CI evidence. Runner labels do not freeze installed compiler versions.
Frontend tests and static-analysis enforcement remain outside this minimal C++ CI.

Sources: [checkout v6.1.0](https://github.com/actions/checkout/releases/tag/v6.1.0),
[runner image labels](https://github.com/actions/runner-images).
Evidence: `CMakeLists.txt`, `cmake/Sanitizers.cmake`, `.clang-tidy`,
`.github/workflows/cpp-ci.yml`, [final verification](../final-review.md).
